#include "Xenon.h"

#include "Diag.h"
#include "GLState.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

using namespace ffglex;

namespace xenon
{
namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame may advance by. A stalled host must
/// not fire a minute of chase in one frame.
constexpr double kMaxFrameDelta = 0.25;

/// Below this a head's light is not drawn.
constexpr float kDark = 1e-6f;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

double toLinear( double code )
{
	code = std::clamp( code, 0.0, 1.0 );
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}

std::string describe( const SliceMap& m )
{
	return m.source + ": " + std::to_string( m.heads.size() ) + " heads (" + std::to_string( m.slicesSeen ) + " slices, "
	       + std::to_string( m.skippedDisabled ) + " disabled, " + std::to_string( m.collapsed ) + " collapsed, "
	       + std::to_string( m.masks ) + " masked, " + std::to_string( m.dropped ) + " dropped, " + std::to_string( m.edges ) + " edges)";
}
} // namespace

//---------------------------------------------------------------------------
static_assert( PT_COUNT - PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
XenonPlugin::XenonPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Flashes fire on the host's beat and decay in seconds, so the plugin has
	//to know what a second is.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Declaration, from the table in Controls.cpp. Every ranged STANDARD
	// parameter is a plain 0..1 float: SetParamInfo clamps its default into
	// 0..1 before a range could be attached (SDK b1afaf9). FF_TYPE_INTEGER
	// is exempt, so Seed, Rows and Columns are declared with real ranges.
	//---------------------------------------------------------------------
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
	{
		const ControlInfo& info = InfoOf( id );
		params[ id ]            = info.defaultValue;
		switch( info.kind )
		{
		case Kind::Standard: SetParamInfo( id, info.name, FF_TYPE_STANDARD, info.defaultValue ); break;
		case Kind::Red: SetParamInfo( id, info.name, FF_TYPE_RED, info.defaultValue ); break;
		case Kind::Green: SetParamInfo( id, info.name, FF_TYPE_GREEN, info.defaultValue ); break;
		case Kind::Blue: SetParamInfo( id, info.name, FF_TYPE_BLUE, info.defaultValue ); break;
		case Kind::Integer:
			SetParamInfo( id, info.name, FF_TYPE_INTEGER, info.defaultValue );
			SetParamRange( id, info.minimum, info.maximum );
			break;
		case Kind::Option:
			SetOptionParamInfo( id, info.name, static_cast< unsigned int >( info.optionCount ), info.defaultValue );
			for( int i = 0; i < info.optionCount; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), info.options[ i ], static_cast< float >( i ) );
			break;
		case Kind::Event: SetParamInfo( id, info.name, FF_TYPE_EVENT, false ); break;
		case Kind::Buffer:
			//An FFT buffer: Resolume shows it as an audio-source picker and
			//writes the spectrum into it every frame. With nothing routed,
			//Trigger Onset never fires.
			SetBufferParamInfo( id, info.name, audio::kBins, FF_USAGE_FFT );
			for( int i = 0; i < audio::kBins; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), "", 0.0f );
			break;
		case Kind::File:
			//A path, with the host's file picker. Empty follows Arena's own
			//pointer to the active preset. Only stored here: the file is read
			//on the render thread, never in a setter or the constructor
			//(Arena constructs every plugin at its startup scan).
			SetFileParamInfo( id, info.name, { "xml" }, "" );
			break;
		}
		SetParamGroup( id, info.group );
	}

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );
}

//---------------------------------------------------------------------------
FFResult XenonPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string quadVertex  = std::string( shaders::kVersion ) + shaders::kQuadVertex;
	const std::string lightVertex = std::string( shaders::kVersion ) + shaders::kLightVertex;
	struct Stage
	{
		FFGLShader* shader;
		const std::string* vertex;
		shaders::Pass pass;
		const char* name;
	};
	const Stage stages[] = {
		{ &lightShader, &lightVertex, shaders::Pass::Light, "light" },
		{ &compositeShader, &quadVertex, shaders::Pass::Composite, "composite" },
	};
	for( const Stage& stage : stages )
	{
		const std::string fragment = shaders::Assemble( stage.pass );
		if( stage.shader->Compile( stage.vertex->c_str(), fragment.c_str() ) )
			continue;
		//Returning FF_FAIL is invisible to the operator: the plugin simply
		//does nothing in Resolume. These lines are the only record of which
		//pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Xenon: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//The light pass's quads: a stream buffer refilled every frame with the lit
	//heads' bounding boxes. The VAO holds the layout; the host's VAO and
	//array buffer come back through ScopedGLState.
	GLint previousArray = 0, previousBuffer = 0;
	glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &previousArray );
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &previousBuffer );
	glGenVertexArrays( 1, &vertexArray );
	glGenBuffers( 1, &vertexBuffer );
	glBindVertexArray( vertexArray );
	glBindBuffer( GL_ARRAY_BUFFER, vertexBuffer );
	const GLsizei stride = shaders::kVertexFloats * sizeof( float );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast< const void* >( 0 ) );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast< const void* >( 2 * sizeof( float ) ) );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast< const void* >( 6 * sizeof( float ) ) );
	glBindVertexArray( static_cast< GLuint >( previousArray ) );
	glBindBuffer( GL_ARRAY_BUFFER, static_cast< GLuint >( previousBuffer ) );
	glGenTextures( 1, &edgeTexture );
	geometryDirty = true;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void XenonPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	//Resolume has been seen sending both seconds and milliseconds through
	//SetTime, and nothing in the call says which. Vote on it against the wall
	//clock for a few frames, then stop asking. (Rosette's code, by way of
	//millpond, schlieren and zoetrope.)
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				//The clock is about to jump from wall time to the host's own,
				//which can be hours in: no dt across the jump.
				settledJump = true;
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );
}

//---------------------------------------------------------------------------
void XenonPlugin::UpdateAudio( double dt )
{
	float bins[ audio::kBins ] = {};
	int binCount               = 0;
	if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
	{
		binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
		for( int i = 0; i < binCount; ++i )
			bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
	}

	audio::Settings settings;
	settings.sensitivity = static_cast< float >( SensitivityFromThreshold( params[ PT_THRESHOLD ] ) );
	analyser.Update( bins, binCount, static_cast< float >( dt ), settings );
}

//---------------------------------------------------------------------------
bool XenonPlugin::rebuildGeometry( int width, int height )
{
	//The map's 0..1, y-down regions into output pixels, y up, and every edge
	//into the edge texture. Only when the map or the raster changes.
	const SliceMap& map = source.Map();
	pixels.assign( map.heads.size(), HeadPixels() );
	std::vector< float > texels;
	int edgeCount = 0;
	auto toPixels = [ & ]( const std::vector< Point >& in, std::vector< Point >& out ) {
		out.clear();
		for( const Point& p : in )
			out.push_back( { p.x * width, ( 1.0 - p.y ) * height } );
	};
	auto addRun = [ & ]( const std::vector< Point >& p, int& start, int& count ) {
		start = edgeCount;
		count = static_cast< int >( p.size() );
		for( size_t i = 0; i < p.size(); ++i )
		{
			const Point& a = p[ i ];
			const Point& b = p[ ( i + 1 ) % p.size() ];
			texels.push_back( static_cast< float >( a.x ) );
			texels.push_back( static_cast< float >( a.y ) );
			texels.push_back( static_cast< float >( b.x ) );
			texels.push_back( static_cast< float >( b.y ) );
		}
		edgeCount += count;
	};
	for( size_t h = 0; h < map.heads.size(); ++h )
	{
		const Head& head = map.heads[ h ];
		HeadPixels& px   = pixels[ h ];
		toPixels( head.outline, px.outline );
		toPixels( head.mask, px.mask );
		px.maskMode = head.mask.empty() ? shaders::kMaskNone : ( head.maskShowsInside ? shaders::kMaskInside : shaders::kMaskOutside );
		double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
		for( const Point& p : px.outline )
			x0 = std::min( x0, p.x ), x1 = std::max( x1, p.x ), y0 = std::min( y0, p.y ), y1 = std::max( y1, p.y );
		//A pixel's centre is what is tested, so a pixel of padding is all
		//the quad needs; clipped to the frame.
		px.x0 = std::clamp( std::floor( x0 ) - 1.0, 0.0, double( width ) );
		px.x1 = std::clamp( std::ceil( x1 ) + 1.0, 0.0, double( width ) );
		px.y0 = std::clamp( std::floor( y0 ) - 1.0, 0.0, double( height ) );
		px.y1 = std::clamp( std::ceil( y1 ) + 1.0, 0.0, double( height ) );
		addRun( px.outline, px.outlineStart, px.outlineCount );
		addRun( px.mask, px.maskStart, px.maskCount );
	}
	const int rows = std::max( 1, ( edgeCount + shaders::kEdgeRow - 1 ) / shaders::kEdgeRow );
	texels.resize( static_cast< size_t >( rows ) * shaders::kEdgeRow * 4, 0.0f );

	GLint previous = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previous );
	glBindTexture( GL_TEXTURE_2D, edgeTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, shaders::kEdgeRow, rows, 0, GL_RGBA, GL_FLOAT, texels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previous ) );

	geometryWidth  = width;
	geometryHeight = height;
	geometryDirty  = false;
	return glGetError() == GL_NO_ERROR;
}

//---------------------------------------------------------------------------
FFResult XenonPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;
	const FFGLTextureStruct& input = *( pgl->inputTextures[ 0 ] );

	//The host's viewport and blend state, put back however this returns.
	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	const int width           = hostViewport[ 2 ];
	const int height          = hostViewport[ 3 ];
	if( width <= 0 || height <= 0 )
		return FF_FAIL;
	glDisable( GL_BLEND );

	//---------------------------------------------------------------------
	// Time, the audio and the beat.
	//---------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		lastNow     = -1.0;
		settledJump = false;
	}
	const double dt = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, kMaxFrameDelta ) : 0.0;
	lastNow         = now;
	++frameCount;
	UpdateAudio( dt );

	if( barPhase != lastBarPhase && lastBarPhase >= 0.0f )
		hostBeatSeen = true;
	lastBarPhase = barPhase;

	//---------------------------------------------------------------------
	// The slices: the active map, re-read when Arena saves it.
	//---------------------------------------------------------------------
	MapSource::Settings settings;
	settings.useMap = static_cast< Slices >( OptionIndex( params[ PT_SLICES ], static_cast< int >( Slices::Count ) ) ) == Slices::Map;
	{
		std::lock_guard< std::mutex > lock( textMutex );
		settings.presetFile = presetFile;
	}
	settings.rows    = IntegerFromParam( params[ PT_ROWS ], kMinGrid, kMaxGrid, 2 );
	settings.columns = IntegerFromParam( params[ PT_COLUMNS ], kMinGrid, kMaxGrid, 4 );
	if( source.Poll( now, settings ) )
	{
		rig.Reset( static_cast< int >( source.Map().heads.size() ) );
		geometryDirty = true;
		++mapChanges;
		diag::info( "slices: " + describe( source.Map() ) );
	}
	order = OrderHeads( source.Map(), static_cast< HeadOrder >( OptionIndex( params[ PT_ORDER ], 3 ) ) );

	//---------------------------------------------------------------------
	// The rig: the chase's steps this frame, the banks, and what this frame
	// catches of every head's light.
	//---------------------------------------------------------------------
	RigInputs in;
	in.trigger  = static_cast< Trigger >( OptionIndex( params[ PT_TRIGGER ], static_cast< int >( Trigger::Count ) ) );
	in.pattern  = static_cast< Pattern >( OptionIndex( params[ PT_PATTERN ], static_cast< int >( Pattern::Count ) ) );
	in.presses  = firePresses;
	in.onset    = analyser.Fired();
	in.barPhase = barPhase;
	in.bpm      = bpm > 1.0f ? static_cast< double >( bpm ) : 120.0;
	in.hostBeat = hostBeatSeen;
	in.recharge = RechargeFromParam( params[ PT_RECHARGE ] );
	in.decay    = DecayFromParam( params[ PT_DECAY ] );
	in.seed     = static_cast< uint32_t >( IntegerFromParam( params[ PT_SEED ], 0, kMaxSeed, 1 ) );
	firePresses = 0;
	rig.Advance( now, dt, in, order );
	rig.Exposure( now, dt, in.decay, exposure );
	const float intensity = static_cast< float >( IntensityFromParam( params[ PT_INTENSITY ] ) );
	light.resize( exposure.size() );
	for( size_t h = 0; h < exposure.size(); ++h )
		light[ h ] = exposure[ h ] * intensity;

	//---------------------------------------------------------------------
	// Buffers and geometry, all before anything is bound: every
	// ffglex::Scoped* binding clears to 0 on exit rather than restoring, and
	// FFGLFBO::Initialise binds under one.
	//---------------------------------------------------------------------
	if( width != geometryWidth || height != geometryHeight )
	{
		if( geometryWidth > 0 && ( hooks & kHookResizeForgets ) != 0 )
			rig.Reset( rig.HeadCount() );
		geometryDirty = true;
	}
	if( geometryDirty && !rebuildGeometry( width, height ) )
		diag::error( "the edge texture could not be filled" );
	if( !lightBuffer.Ensure( width, height, GL_R32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the light buffer" );
		return FF_FAIL;
	}

	//Outline's Width is in composition pixels (the units Arena shows a slice
	//in); a grid has no composition, so its pixels are the output's.
	const SliceMap& map = source.Map();
	const double scale  = source.FromPreset() && map.compHeight > 0.0 ? double( height ) / map.compHeight : 1.0;
	band                = WidthFromParam( params[ PT_WIDTH ] ) * scale;
	const auto look     = static_cast< Look >( OptionIndex( params[ PT_LOOK ], static_cast< int >( Look::Count ) ) );

	//---------------------------------------------------------------------
	// 1. The light: every lit head's quad, additively.
	//---------------------------------------------------------------------
	vertices.clear();
	for( size_t h = 0; h < pixels.size() && h < light.size(); ++h )
	{
		if( !( light[ h ] > kDark ) )
			continue;
		const HeadPixels& px = pixels[ h ];
		if( !( px.x1 > px.x0 ) || !( px.y1 > px.y0 ) )
			continue;
		const float runs[ 6 ] = { float( px.outlineStart ), float( px.outlineCount ), float( px.maskStart ), float( px.maskCount ),
			                      float( px.maskMode ), light[ h ] };
		const float corners[ 6 ][ 2 ] = { { float( px.x0 ), float( px.y0 ) }, { float( px.x1 ), float( px.y0 ) }, { float( px.x1 ), float( px.y1 ) },
			                              { float( px.x0 ), float( px.y0 ) }, { float( px.x1 ), float( px.y1 ) }, { float( px.x0 ), float( px.y1 ) } };
		for( const auto& c : corners )
		{
			vertices.push_back( c[ 0 ] );
			vertices.push_back( c[ 1 ] );
			vertices.insert( vertices.end(), runs, runs + 6 );
		}
	}
	{
		ScopedFBOBinding fbo( lightBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( !vertices.empty() )
		{
			glEnable( GL_BLEND );
			glBlendFunc( GL_ONE, GL_ONE );
			ScopedShaderBinding shader( lightShader.GetGLID() );
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, edgeTexture );
			lightShader.Set( "Edges", 0 );
			lightShader.Set( "Size", static_cast< float >( width ), static_cast< float >( height ) );
			lightShader.Set( "Outline", look == Look::Outline ? 1 : 0 );
			lightShader.Set( "Band", static_cast< float >( band ) );
			lightShader.Set( "Hooks", hooks & 0xff );
			glBindVertexArray( vertexArray );
			glBindBuffer( GL_ARRAY_BUFFER, vertexBuffer );
			glBufferData( GL_ARRAY_BUFFER, static_cast< GLsizeiptr >( vertices.size() * sizeof( float ) ), vertices.data(), GL_STREAM_DRAW );
			glDrawArrays( GL_TRIANGLES, 0, static_cast< GLsizei >( vertices.size() / shaders::kVertexFloats ) );
			glBindVertexArray( 0 );
			glBindBuffer( GL_ARRAY_BUFFER, 0 );
			glBindTexture( GL_TEXTURE_2D, 0 );
			glDisable( GL_BLEND );
		}
	}

	//---------------------------------------------------------------------
	// 2. The clip and the light, to the host.
	//---------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
		ScopedShaderBinding shader( compositeShader.GetGLID() );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, input.Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, lightBuffer.TextureID() );
		compositeShader.Set( "InputTexture", 0 );
		compositeShader.Set( "Light", 1 );
		compositeShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		glUniform2i( compositeShader.FindUniform( "ViewOrigin" ), hostViewport[ 0 ], hostViewport[ 1 ] );
		compositeShader.Set( "Colour", static_cast< float >( toLinear( params[ PT_COLOUR_R ] ) ), static_cast< float >( toLinear( params[ PT_COLOUR_G ] ) ),
		                     static_cast< float >( toLinear( params[ PT_COLOUR_B ] ) ) );
		compositeShader.Set( "Room", static_cast< float >( RoomFromParam( params[ PT_ROOM ] ) ) );
		compositeShader.Set( "Look", static_cast< int >( look ) );
		compositeShader.Set( "Hooks", hooks & 0xff );
		quad.Draw();
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult XenonPlugin::DeInitGL()
{
	lightShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();
	lightBuffer.Destroy();
	if( vertexBuffer != 0 )
		glDeleteBuffers( 1, &vertexBuffer );
	if( vertexArray != 0 )
		glDeleteVertexArrays( 1, &vertexArray );
	if( edgeTexture != 0 )
		glDeleteTextures( 1, &edgeTexture );
	vertexBuffer = vertexArray = edgeTexture = 0;
	geometryWidth = geometryHeight = 0;
	geometryDirty                  = true;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult XenonPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void XenonPlugin::SetBeatInfo( float newBpm, float newBarPhase )
{
	CFFGLPlugin::SetBeatInfo( newBpm, newBarPhase );
	if( newBpm > 0.0f && std::isfinite( newBpm ) )
		bpm = newBpm;
	if( std::isfinite( newBarPhase ) )
		barPhase = newBarPhase - std::floor( newBarPhase );
}

char* XenonPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	if( index == PT_PRESET_FILE )
	{
		std::lock_guard< std::mutex > lock( textMutex );
		const size_t length = std::min( presetFile.size(), sizeof( textReturn ) - 1 );
		std::memcpy( textReturn, presetFile.data(), length );
		textReturn[ length ] = '\0';
		return textReturn;
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult XenonPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	if( index == PT_PRESET_FILE )
	{
		std::lock_guard< std::mutex > lock( textMutex );
		presetFile = value != nullptr ? value : "";
		return FF_SUCCESS;
	}
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult XenonPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	//Fire acts on the press, once: an event arrives as 1 on press and 0 on
	//release, and a host is free to restate either.
	if( index == PT_FIRE )
	{
		const bool down = value >= 0.5f;
		if( down && !fireHeld )
			++firePresses;
		fireHeld = down;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float XenonPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

} // namespace xenon
