/**
    xntest -- render Xenon offline, and measure what the heads and the chase
    are doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock and a synthetic beat, in a headless CGL
    context, and reads back the host's output as floats. What it is measured
    against is independent of the code that made it: the corners Arena wrote
    into the preset files (worked out by hand), the capacitor's and the
    tube's closed forms evaluated here on their own, the host's own beat
    grid, the SPEC's pattern formulas, and a winding-number region test in
    double with nothing in common with the shader's.

        xntest --out /tmp/xenon.png   the defaults on unmaptest's slices, at 120 BPM
        xntest --list                 every parameter and its default
        xntest --pipe                 raw RGBA frames in on stdin, out on stdout
        xntest --film N               N frames of the harness's clip, raw RGBA on stdout
        xntest --offline              the checks that need no GL context (CI)

    The map: `--preset PATH` (default the committed fixture unmaptest.xml),
    `--preset grid` for Rows x Columns, `--preset active` for whatever this
    Mac's Arena has active. The checks never read this Mac's Documents.

    `--script` is the fleet's cue format: `frame  Parameter Name  value`
    lines, held before the first key and after the last. A STANDARD (0..1)
    control is linear between its keys; an option, a switch, an integer or an
    event STEPS.

    XNTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.
*/

#include "Audio.h"
#include "Controls.h"
#include "Rig.h"
#include "Shaders.h"
#include "SliceMap.h"
#include "Xenon.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#ifndef XENON_SOURCE_DIR
#error "XENON_SOURCE_DIR must name the source tree (CMakeLists.txt sets it)"
#endif

using namespace xenon;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 8192 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Tolerances, each with where it comes from.
//---------------------------------------------------------------------------
/// One float32 ULP at 1.0.
constexpr double kFloatEps = 1.0 / 8388608.0;
/// The light buffer is R32F: each head's light is one float32 rounding
/// into it, and each overlapping head one more (2^-24 each, relatively;
/// four allowed).
constexpr double kStoreRel = 4.0 / 16777216.0;
/// The sRGB encode in the composite and the decode here, in float32, with
/// GLSL's pow: a generous 64 ULPs.
constexpr double kCurveRel = 64.0 * kFloatEps;
/// Light below this the plugin does not draw (Xenon.cpp's kDark).
constexpr double kDarkLight = 1e-6;
/// Pixel centres this close to a region's boundary are left out of the
/// inside test: the shader's crossing is float32 in output pixels, four
/// ULPs at 2048 px.
constexpr double kEdgeMargin = 1e-3;

//---------------------------------------------------------------------------
// Fixtures.
//---------------------------------------------------------------------------
std::string Fixture( const std::string& name )
{
	return std::string( XENON_SOURCE_DIR ) + "/tests/presets/" + name;
}

/// A Documents folder that is never there, so a check that names no preset
/// gets the grid and never this Mac's Arena.
const char* const kNowhere = "/nonexistent/xntest-documents";

std::string ScratchDir()
{
	static std::string dir;
	if( dir.empty() )
	{
		const char* base = std::getenv( "TMPDIR" );
		char pattern[ 1024 ];
		std::snprintf( pattern, sizeof( pattern ), "%s/xntest.XXXXXX", base ? base : "/tmp" );
		const char* made = mkdtemp( pattern );
		dir              = made ? made : "/tmp";
	}
	return dir;
}

bool WriteFile( const std::string& path, const std::string& text )
{
	std::ofstream out( path, std::ios::binary | std::ios::trunc );
	out << text;
	return static_cast< bool >( out );
}

std::string ReadText( const std::string& path )
{
	std::string text;
	ReadWholeFile( path, text );
	return text;
}

/// Put a file's mtime `seconds` after the epoch, so a rewrite in the same
/// filesystem tick is still a new stamp.
void Touch( const std::string& path, double seconds )
{
	struct timeval times[ 2 ];
	times[ 0 ].tv_sec  = static_cast< time_t >( seconds );
	times[ 0 ].tv_usec = 0;
	times[ 1 ]         = times[ 0 ];
	utimes( path.c_str(), times );
}

void MakeDirs( const std::string& path )
{
	std::string so_far;
	std::stringstream parts( path );
	std::string part;
	while( std::getline( parts, part, '/' ) )
	{
		if( part.empty() )
		{
			so_far += "/";
			continue;
		}
		so_far += part + "/";
		mkdir( so_far.c_str(), 0755 );
	}
}

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written
/// top row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "XNTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "xntest: XNTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_FILE: return "file";
	case FF_TYPE_RED: return "red";
	case FF_TYPE_GREEN: return "green";
	case FF_TYPE_BLUE: return "blue";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch, a count or a press: cues STEP
/// between keys for these, and everything else ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == parts.back().c_str() || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// Clips. Every one is rows BOTTOM first (GL's order), as uploaded, and is a
// function of the host frame it is shown on.
//---------------------------------------------------------------------------
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double hash01( uint32_t a, uint32_t b = 0 )
{
	return pcg( a * 2654435761u ^ pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

using Clip = std::function< void( int frame, int width, int height, Floats& out ) >;

/// The look clip: a slow diagonal sweep of colour bars over a dark ground,
/// so a slice shows the clip under it and the strobe on top.
void lookClip( int frame, int width, int height, Floats& out )
{
	out.resize( static_cast< size_t >( width ) * height * 4 );
	const double t = frame / 60.0;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u    = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			const double band = std::fmod( 3.0 * ( u + 0.6 * v ) - 0.25 * t + 100.0, 1.0 );
			const int bar     = static_cast< int >( band * 6.0 );
			static const double colours[ 6 ][ 3 ] = { { 0.55, 0.12, 0.10 }, { 0.50, 0.38, 0.08 }, { 0.12, 0.42, 0.18 },
				                                     { 0.08, 0.30, 0.48 }, { 0.30, 0.14, 0.46 }, { 0.20, 0.20, 0.22 } };
			const double shade = 0.55 + 0.45 * v;
			float* o           = &out[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]             = static_cast< float >( colours[ bar ][ 0 ] * shade );
			o[ 1 ]             = static_cast< float >( colours[ bar ][ 1 ] * shade );
			o[ 2 ]             = static_cast< float >( colours[ bar ][ 2 ] * shade );
			o[ 3 ]             = 1.0f;
		}
}

Clip flatClip( float level, float alpha = 1.0f )
{
	return [ level, alpha ]( int, int width, int height, Floats& out ) {
		out.assign( static_cast< size_t >( width ) * height * 4, level * alpha );
		for( size_t i = 3; i < out.size(); i += 4 )
			out[ i ] = alpha;
	};
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	//Linear, as a host's clip texture is.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// A host: the real plugin, a float output framebuffer, a synthetic clock and
// beat, and a clip that is a function of the frame.
//---------------------------------------------------------------------------
struct Host
{
	XenonPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame  = 0;
	double fps = 60.0;
	double bpm = 120.0;///< 0: no beat sent
	Clip clip;         ///< empty: whatever was last uploaded stays
	std::function< void( int ) > beforeFrame;
	Floats scratch;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Host()
	{
		plugin.DeInitGL();
		release();
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO = outputFBO;
		Floats blank( static_cast< size_t >( width ) * height * 4, 0.0f );
		sourceTexture     = makeTexture( width, height, blank.data() );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}

	/// The preset: a path, "grid" for Rows x Columns, or "active" for this
	/// Mac's Arena. Never this Mac's Documents unless asked.
	bool Init( int w, int h, Clip c = flatClip( 0.0f ), const std::string& preset = "grid" )
	{
		clip                        = std::move( c );
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/xenon for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );
		UsePreset( preset );
		return attach( w, h );
	}

	void UsePreset( const std::string& preset )
	{
		if( preset == "active" )
		{
			plugin.SetDocumentsForTest( "" );
			plugin.SetTextParameter( PT_PRESET_FILE, "" );
			Set( PT_SLICES, static_cast< float >( Slices::Map ) );
			return;
		}
		plugin.SetDocumentsForTest( kNowhere );
		if( preset == "grid" )
		{
			plugin.SetTextParameter( PT_PRESET_FILE, "" );
			Set( PT_SLICES, static_cast< float >( Slices::Grid ) );
			return;
		}
		plugin.SetTextParameter( PT_PRESET_FILE, preset.c_str() );
		Set( PT_SLICES, static_cast< float >( Slices::Map ) );
	}

	/// The host's raster changes under a running instance: no InitGL.
	bool Resize( int w, int h )
	{
		release();
		return attach( w, h );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}

	double Seconds( int f ) const
	{
		return static_cast< double >( f ) / fps;
	}

	/// The host's bar phase at frame f, as a float (what SetBeatInfo carries).
	float PhaseAt( int f ) const
	{
		return static_cast< float >( std::fmod( Seconds( f ) * bpm / 240.0, 1.0 ) );
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			if( beforeFrame )
				beforeFrame( frame );
			if( clip )
			{
				clip( frame, width, height, scratch );
				Upload( scratch );
			}
			plugin.SetTime( Seconds( frame ) );
			if( bpm > 0.0 )
				plugin.SetBeatInfo( static_cast< float >( bpm ), PhaseAt( frame ) );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	/// Press Fire on the next frame and render it.
	bool Fire()
	{
		Set( PT_FIRE, 1.0f );
		const bool ok = Render( 1 );
		Set( PT_FIRE, 0.0f );
		return ok;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}
};

double linearOf( double code )
{
	code = std::max( code, 0.0 );
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}

/// The light (times Intensity) at an output pixel of a black, opaque clip
/// under Fill with white light: the red channel's code, decoded.
double lightAt( const Floats& out, int width, int x, int y )
{
	return linearOf( out[ ( static_cast< size_t >( y ) * width + x ) * 4 ] );
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( XenonPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

/// `Name=Value`. The value is a number, for an option the name of one of its
/// elements, for the Preset File a path; anything else is refused, never read
/// as 0 (polyhedral's trap: strtof( "Fixed" ) is 0, silently).
bool applySetting( XenonPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.type == FF_TYPE_FILE )
				return plugin.SetTextParameter( parameter.index, value.c_str() ) == FF_SUCCESS;
			if( parameter.type == FF_TYPE_TEXT || parameter.type == FF_TYPE_BUFFER )
			{
				error = name + " cannot be set from here";
				return false;
			}
			if( parameter.type == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == value.c_str() || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( XenonPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

/// The harness's music: a busy hi-hat (a fresh level every frame, so the
/// onset detector's floor is not zero) and a kick every half second, soft and
/// hard by turns, written into the Audio buffer the way the host writes it.
void feedKicks( XenonPlugin& plugin, int frame )
{
	const int since = frame % 30;
	const bool hard = ( frame / 30 ) % 2 == 1;
	const double hat = 0.3 * hash01( static_cast< uint32_t >( frame ), 11 );
	for( int bin = 0; bin < audio::kBins; ++bin )
	{
		double v = bin > 40 ? 0.02 + hat : 0.02;
		if( since < 4 && bin < 10 )
			v += ( hard ? 0.9 : 0.25 ) * std::exp( -since * 0.8 );
		plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), static_cast< float >( v ) );
	}
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock, with a
// beat at --bpm (120 unless told otherwise).
//===========================================================================
int runPipe( int width, int height, double fps, double bpm, const std::string& scriptPath, int count, bool readStdin,
             const std::vector< std::string >& settings, bool kicks, const std::string& preset )
{
	Host host;
	host.fps = fps;
	host.bpm = bpm;
	if( !host.Init( width, height, readStdin ? Clip() : Clip( lookClip ), preset ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( host.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( host.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			host.Upload( picture );
		}

		for( const auto& cue : automation )
			host.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( kicks )
			feedKicks( host.plugin, index );
		if( !host.Render( 1 ) )
			return 1;

		const Floats out = host.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is
			//EPIPE and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

#include "checks.inc"

} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/xenon.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 150;
	double fps = 60.0, bpm = 120.0;
	std::string mode, scriptPath, clipName = "look", preset = Fixture( "unmaptest.xml" );
	int filmFrames = -1;
	bool sizeGiven = false, kicks = false;
	std::map< int, std::vector< std::string > > later;//--at FRAME "Name=V"

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "xntest -- render Xenon offline and measure its heads and its chase\n\n"
			             "  --out PATH        render the clip and write a PNG (default /tmp/xenon.png)\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 150)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --bpm N           the synthetic beat (default 120; 0 sends none)\n"
			             "  --clip NAME       the harness's clip for --out and --film: look, black, white\n"
			             "  --preset P        a preset file, 'grid', or 'active' (this Mac's Arena); default the unmaptest fixture\n"
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name). Repeatable.\n"
			             "  --at N \"Name=V\"  (with --out) set it at frame N instead; an event is released the frame after\n"
			             "  --kicks           the harness's music in the Audio buffer: a kick every half second\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in on stdin, out on stdout\n"
			             "  --film N          N frames of the clip, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n\n"
			             "  checks: --reader --pointer --names --cues (no GL); --reread --charge --chase --inside --energy\n"
			             "          --audio --alpha --resize --state;  --negative  --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--bpm" && hasNext )
			bpm = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--clip" && hasNext )
			clipName = argv[ ++i ];
		else if( argument == "--preset" && hasNext )
			preset = argv[ ++i ];
		else if( argument == "--kicks" )
			kicks = true;
		else if( argument == "--at" && i + 2 < argc )
		{
			const int at = std::atoi( argv[ ++i ] );
			later[ at ].push_back( argv[ ++i ] );
		}
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	Clip chosen;
	if( clipName == "look" )
		chosen = lookClip;
	else if( clipName == "black" )
		chosen = flatClip( 0.0f );
	else if( clipName == "white" )
		chosen = flatClip( 1.0f );
	else
	{
		std::fprintf( stderr, "--clip %s: not one of look, black, white\n", clipName.c_str() );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "list" )
	{
		XenonPlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n",
		             failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( width, height, fps, bpm, scriptPath, 0, true, settings, kicks, preset );
	else if( mode == "film" )
		result = runPipe( width, height, fps, bpm, scriptPath, filmFrames, false, settings, kicks, preset );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Host host;
		host.fps = fps;
		host.bpm = bpm;
		if( !host.Init( width, height, chosen, preset ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( host.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			bool rendered = true;
			std::vector< std::pair< int, std::string > > release;
			for( int f = 0; f < std::max( frames, 1 ) && rendered; ++f )
			{
				if( kicks )
					feedKicks( host.plugin, f );
				for( const auto& r : release )
					if( r.first == f )
					{
						std::string ignored;
						applySetting( host.plugin, r.second + "=0", ignored );
					}
				const auto due = later.find( f );
				if( due != later.end() )
					for( const std::string& setting : due->second )
					{
						std::string error;
						if( !applySetting( host.plugin, setting, error ) )
						{
							std::fprintf( stderr, "--at %d %s: %s\n", f, setting.c_str(), error.c_str() );
							return 2;
						}
						//An event is a press: released on the next frame, as a host would.
						const std::string name = setting.substr( 0, setting.find( '=' ) );
						for( const NamedParameter& parameter : listParameters( host.plugin ) )
							if( parameter.name == name && parameter.type == FF_TYPE_EVENT )
								release.emplace_back( f + 1, name );
					}
				rendered = host.Render( 1 );
			}
			if( !rendered )
				result = 1;
			else if( writePng( outPath, width, height, host.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s), %d heads from %s, %llu flashes fired, %lld steps\n",
				             outPath.c_str(), width, height, frames, fps, frames / fps, host.plugin.RigState().HeadCount(),
				             host.plugin.Source().Map().source.c_str(), host.plugin.RigState().Fired(), host.plugin.RigState().Steps() );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
