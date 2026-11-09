//===========================================================================
// The checks. Included into main.cpp's anonymous namespace.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the stated law; `--negative` sets one field at a time to a
// deliberately wrong MODEL -- a hook in the plugin, so the shipped code
// computes the wrong thing -- and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	int hooks       = 0;    ///< shaders::Hook and PluginHook bits
	int rigHooks    = 0;    ///< RigHook bits
	int readerHooks = 0;    ///< ReaderHook bits
	bool unprimed   = false;///< the onset detector's first frame against zeroes
	bool cuesRamp   = false;///< --cues: every control ramps
};

using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

bool startHost( Host& host, const Raster& raster, Clip clip, const Perturb& perturb, const std::string& preset = "grid" )
{
	if( !host.Init( raster.w, raster.h, std::move( clip ), preset ) )
		return false;
	host.plugin.SetHooksForTest( perturb.hooks );
	host.plugin.SetRigHooksForTest( perturb.rigHooks );
	host.plugin.SetReaderHooksForTest( perturb.readerHooks );
	host.plugin.SetUnprimedForTest( perturb.unprimed );
	return true;
}

/// One output pixel, RGBA, read on its own (cheap enough to do every frame).
std::array< float, 4 > pixelOf( const Host& host, int x, int y )
{
	std::array< float, 4 > rgba = {};
	glBindFramebuffer( GL_FRAMEBUFFER, host.outputFBO );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( x, y, 1, 1, GL_RGBA, GL_FLOAT, rgba.data() );
	return rgba;
}

/// The light at an output pixel, Intensity divided out, for a black opaque
/// clip under Fill and white light.
double lightOf( const Host& host, int x, int y, double intensity )
{
	return linearOf( pixelOf( host, x, y )[ 0 ] ) / intensity;
}

/// What one measured light may be off by: its roundings into the R32F light
/// buffer and the float32 curve, relatively, plus the light the plugin does
/// not draw (kDark), absolutely.
double lightTolerance( double expected, double intensity )
{
	return std::fabs( expected ) * ( kStoreRel + kCurveRel ) + kDarkLight / intensity;
}

/// The pixel at the middle of a grid cell (row from the TOP), y up.
void cellPixel( int row, int column, int rows, int columns, int width, int height, int& x, int& y )
{
	x = static_cast< int >( ( column + 0.5 ) * width / columns );
	y = static_cast< int >( height - ( row + 0.5 ) * height / rows );
	x = std::clamp( x, 0, width - 1 );
	y = std::clamp( y, 0, height - 1 );
}

/// Setting a control in physical units, and reading back what the plugin
/// will actually use (the 0..1 the host stores, converted).
double setRecharge( Host& host, double seconds )
{
	const float p = ParamFromRecharge( seconds );
	host.Set( PT_RECHARGE, p );
	return RechargeFromParam( p );
}
double setDecay( Host& host, double seconds )
{
	const float p = ParamFromDecay( seconds );
	host.Set( PT_DECAY, p );
	return DecayFromParam( p );
}
double setIntensity( Host& host, double intensity )
{
	const float p = ParamFromIntensity( intensity );
	host.Set( PT_INTENSITY, p );
	return IntensityFromParam( p );
}

//===========================================================================
// The heads as SPEC-xenon.md states them, evaluated here on their own: the
// bank's closed form from the moment it last fired, E = q^2, and the tube's
// light integrated over a frame's own interval.
//===========================================================================
struct Bank
{
	double rc  = 0.25;
	double tau = 2e-3;
	std::vector< double > last;                                  ///< -inf: never fired
	std::vector< std::vector< std::pair< double, double > > > fl;///< ( t, E ) per head

	void reset( int heads )
	{
		last.assign( static_cast< size_t >( heads ), -1e300 );
		fl.assign( static_cast< size_t >( heads ), {} );
	}
	double fire( int head, double t )
	{
		double& l      = last[ static_cast< size_t >( head ) ];
		const double q = l < -1e299 ? 1.0 : 1.0 - std::exp( -( t - l ) / rc );
		const double e = q * q;
		fl[ static_cast< size_t >( head ) ].emplace_back( t, e );
		l = t;
		return e;
	}
	double exposure( int head, double t0, double t1 ) const
	{
		double x = 0.0;
		for( const auto& f : fl[ static_cast< size_t >( head ) ] )
			if( f.first <= t1 )
				x += f.second * ( std::exp( -std::max( 0.0, t0 - f.first ) / tau ) - std::exp( -( t1 - f.first ) / tau ) );
		return x;
	}
};

/// Step k of a chase over n heads, positions in the chase's order, as the
/// SPEC writes the patterns (Random has no formula: --chase reads it).
std::vector< int > specStep( Pattern pattern, long long k, int n )
{
	std::vector< int > out;
	switch( pattern )
	{
	case Pattern::Forward: out.push_back( static_cast< int >( k % n ) ); break;
	case Pattern::Reverse: out.push_back( static_cast< int >( n - 1 - k % n ) ); break;
	case Pattern::Bounce:
	{
		if( n == 1 )
		{
			out.push_back( 0 );
			break;
		}
		const long long p = k % ( 2 * n - 2 );
		out.push_back( static_cast< int >( p < n ? p : 2 * n - 2 - p ) );
		break;
	}
	case Pattern::Build:
		for( long long i = 0; i <= k % n; ++i )
			out.push_back( static_cast< int >( i ) );
		break;
	case Pattern::All:
		for( int i = 0; i < n; ++i )
			out.push_back( i );
		break;
	default: break;
	}
	return out;
}

//===========================================================================
// Regions, in output pixels (y up), tested with a winding number in double:
// nothing in common with the shader's float even-odd crossing count, and the
// same answer for every simple polygon.
//===========================================================================
struct Region
{
	std::vector< Point > outline, mask;
	int maskMode = shaders::kMaskNone;
	double x0, y0, x1, y1;
};

std::vector< Point > toPixels( const std::vector< Point >& in, int width, int height )
{
	std::vector< Point > out;
	for( const Point& p : in )
		out.push_back( { p.x * width, ( 1.0 - p.y ) * height } );
	return out;
}

Region regionOf( const Head& head, int width, int height )
{
	Region r;
	r.outline  = toPixels( head.outline, width, height );
	r.mask     = toPixels( head.mask, width, height );
	r.maskMode = head.mask.empty() ? shaders::kMaskNone : ( head.maskShowsInside ? shaders::kMaskInside : shaders::kMaskOutside );
	r.x0 = r.y0 = 1e300;
	r.x1 = r.y1 = -1e300;
	for( const Point& p : r.outline )
		r.x0 = std::min( r.x0, p.x ), r.x1 = std::max( r.x1, p.x ), r.y0 = std::min( r.y0, p.y ), r.y1 = std::max( r.y1, p.y );
	return r;
}

int windingNumber( const std::vector< Point >& p, double x, double y )
{
	int w          = 0;
	const size_t n = p.size();
	for( size_t i = 0; i < n; ++i )
	{
		const Point& a     = p[ i ];
		const Point& b     = p[ ( i + 1 ) % n ];
		const double cross = ( b.x - a.x ) * ( y - a.y ) - ( x - a.x ) * ( b.y - a.y );
		if( a.y <= y )
		{
			if( b.y > y && cross > 0.0 )
				++w;
		}
		else if( b.y <= y && cross < 0.0 )
			--w;
	}
	return w;
}

double segmentDistance( const Point& a, const Point& b, double x, double y )
{
	const double dx = b.x - a.x, dy = b.y - a.y;
	const double len2 = dx * dx + dy * dy;
	double s          = len2 > 0.0 ? ( ( x - a.x ) * dx + ( y - a.y ) * dy ) / len2 : 0.0;
	s                 = std::clamp( s, 0.0, 1.0 );
	const double ex = a.x + s * dx - x, ey = a.y + s * dy - y;
	return std::sqrt( ex * ex + ey * ey );
}

double boundaryDistance( const std::vector< Point >& p, double x, double y )
{
	double best = 1e300;
	for( size_t i = 0; i < p.size(); ++i )
		best = std::min( best, segmentDistance( p[ i ], p[ ( i + 1 ) % p.size() ], x, y ) );
	return best;
}

/// Inside the head's region, and its distance to the region's boundary
/// (the outline's and, when masked, the mask's: for a point inside both, the
/// nearer of the two is on the boundary of what is left).
bool insideRegion( const Region& r, double x, double y, double& distance )
{
	distance = boundaryDistance( r.outline, x, y );
	bool in  = windingNumber( r.outline, x, y ) != 0;
	if( r.maskMode != shaders::kMaskNone )
	{
		distance       = std::min( distance, boundaryDistance( r.mask, x, y ) );
		const bool inM = windingNumber( r.mask, x, y ) != 0;
		in             = in && ( r.maskMode == shaders::kMaskInside ? inM : !inM );
	}
	return in;
}

//===========================================================================
// --reader (no GL): the four presets, and Arena 7.28.1's Polygon and masks,
// against the corners Arena wrote, divided out by hand.
//===========================================================================
struct ExpectedHead
{
	const char* name;
	std::vector< Point > corners;///< composition pixels, as the file has them
};

struct ExpectedMap
{
	const char* file;
	double width, height;
	int seen, collapsed;
	std::vector< ExpectedHead > heads;///< in Arena's panel order
};

const std::vector< ExpectedMap >& expectedMaps()
{
	//Read off the XML by eye (tools/slicedump prints the same once the
	//reader agrees); the panel order is each screen's slices bottom-up in
	//the file.
	static const std::vector< ExpectedMap > maps = {
		{ "LPP slicer proof A.xml", 4920, 1920, 3, 0,
		  { { "S1 Out 1 s2 p2", { { 1920, 0 }, { 3840, 0 }, { 3840, 1080 }, { 1920, 1080 } } },
		    { "S1 Out 1", { { 0, 0 }, { 1920, 0 }, { 1920, 1080 }, { 0, 1080 } } },
		    { "S2 Out 5", { { 3840, 0 }, { 4920, 0 }, { 4920, 1920 }, { 3840, 1920 } } } } },
		{ "LPP slicer proof B.xml", 1920, 1080, 3, 0,
		  { { "S1 Out 1 s2 p2", { { 749.268, 165.434 }, { 1498.536, 165.434 }, { 1498.536, 586.821 }, { 749.268, 586.821 } } },
		    { "S1 Out 1", { { 0, 165.434 }, { 749.268, 165.434 }, { 749.268, 586.821 }, { 0, 586.821 } } },
		    { "S2 Out 5", { { 1498.537, 165.434 }, { 1920, 165.434 }, { 1920, 914.567 }, { 1498.537, 914.567 } } } } },
		{ "output_map_1.xml", 2944, 1280, 3, 0,
		  { { "trailer", { { 1024, 0 }, { 2944, 0 }, { 2944, 1080 }, { 1024, 1080 } } },
		    { "SR", { { 0, 640 }, { 1024, 640 }, { 1024, 1280 }, { 0, 1280 } } },
		    { "SL ", { { 0, 0 }, { 1024, 0 }, { 1024, 640 }, { 0, 640 } } } } },
		//Slice 3 is rotated by -3.107 rad: its first corner, its own top
		//left, is at the bottom right of the composition.
		{ "unmaptest.xml", 1920, 1080, 7, 3,
		  { { "Slice 3",
		      { { 1864.539794921875, 678.223388671875 },
		        { 873.3802490234375, 643.5595703125 },
		        { 889.460205078125, 183.7767486572265625 },
		        { 1880.619873046875, 218.4405670166015625 } } },
		    { "Slice 1", { { 142, 540 }, { 812, 540 }, { 812, 1004 }, { 142, 1004 } } },
		    { "Slice 2", { { 69, 56.00006103515625 }, { 812, 56.00006103515625 }, { 812, 431.00006103515625 }, { 69, 431.00006103515625 } } },
		    { "Slice", { { 0, 0 }, { 1920, 0 }, { 1920, 1080 }, { 0, 1080 } } } } },
	};
	return maps;
}

Point bezierAt( const Point& a, const Point& b, const Point& c, const Point& d, double t )
{
	const double u = 1.0 - t;
	return { u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
		     u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y };
}

double worstCorner( const std::vector< Point >& got, const std::vector< Point >& raw, double w, double h )
{
	if( got.size() != raw.size() )
		return 1e300;
	double worst = 0.0;
	for( size_t i = 0; i < raw.size(); ++i )
		worst = std::max( { worst, std::fabs( got[ i ].x - raw[ i ].x / w ), std::fabs( got[ i ].y - raw[ i ].y / h ) } );
	return worst;
}

int runReader( const Perturb& perturb )
{
	std::printf( "\n=== reader: every slice's region, normalised by the composition, in Arena's panel order, duplicates folded\n" );
	//Parsing a decimal and dividing it are two roundings in double.
	constexpr double kCorner = 1e-12;
	for( const ExpectedMap& e : expectedMaps() )
	{
		const SliceMap m = ReadPreset( Fixture( e.file ), perturb.readerHooks );
		std::string names;
		bool namesOk = m.heads.size() == e.heads.size();
		double worst = 0.0;
		for( size_t i = 0; i < m.heads.size(); ++i )
		{
			names += ( i ? ", [" : "[" ) + m.heads[ i ].name + "]";
			if( i < e.heads.size() )
			{
				namesOk = namesOk && m.heads[ i ].name == e.heads[ i ].name;
				worst   = std::max( worst, worstCorner( m.heads[ i ].outline, e.heads[ i ].corners, e.width, e.height ) );
			}
		}
		Check( m.ok && m.compWidth == e.width && m.compHeight == e.height && m.slicesSeen == e.seen && m.collapsed == e.collapsed && namesOk &&
		           worst <= kCorner,
		       fmt( "%s: %zu heads (want %zu) from %d slices (want %d), %d folded (want %d), composition %gx%g; %s; worst corner off by %.2g "
		            "(tolerance %.0e)",
		            e.file, m.heads.size(), e.heads.size(), m.slicesSeen, e.seen, m.collapsed, e.collapsed, m.compWidth, m.compHeight,
		            names.c_str(), worst, kCorner ) );
	}

	//Arena 7.28.1's save of unmaptest with a Polygon, a triangle Input Mask
	//on Slice 2 and a circle on Slice 1 (Bezier, CCCC).
	{
		const SliceMap m = ReadPreset( Fixture( "unmaptest-7.28.1-polygon-masks.xml" ), perturb.readerHooks );
		const char* order[] = { "Slice 3", "Slice 1", "Polygon", "Slice 2", "Slice" };
		bool namesOk        = m.heads.size() == 5;
		for( size_t i = 0; namesOk && i < 5; ++i )
			namesOk = m.heads[ i ].name == order[ i ];
		Check( m.ok && namesOk && m.masks == 2 && m.edges == 86,
		       fmt( "the 7.28.1 save: %zu heads (want 5: Slice 3, Slice 1, Polygon, Slice 2, Slice), %d masks (want 2), %d edges (want 4 + 4 + "
		            "64 + 3 + 4 + 3 + 4 = 86)",
		            m.heads.size(), m.masks, m.edges ) );
		if( namesOk )
		{
			const std::vector< Point > polygon = { { 960, 0 }, { 1583.5380859375, 1079.999755859375 }, { 336.4619140625, 1080 } };
			const double pw                    = worstCorner( m.heads[ 2 ].outline, polygon, 1920, 1080 );
			Check( m.heads[ 2 ].polygon && pw <= kCorner,
			       fmt( "the Polygon is its contour's three points, not its bounding box: off by %.2g", pw ) );
			const std::vector< Point > triangle = { { 441.5, 56.0000762939453125 }, { 658.00628662109375, 431 }, { 224.993682861328125, 431.00006103515625 } };
			const double tw                     = worstCorner( m.heads[ 3 ].mask, triangle, 1920, 1080 );
			Check( m.heads[ 3 ].maskShowsInside && tw <= kCorner,
			       fmt( "Slice 2's mask is the triangle, shown inside (Invert 1): off by %.2g", tw ) );
			//The circle: per segment an anchor and two controls, the next
			//anchor the next segment's (the last wraps to the first).
			const std::vector< Point > raw = { { 477, 540 },     { 605.1298828125, 540 },   { 709, 643.8701171875 },  { 709, 772 },
				                               { 709, 900.1298828125 }, { 605.1298828125, 1004 }, { 477, 1004 },       { 348.8701171875, 1004 },
				                               { 245, 900.1298828125 }, { 245, 772 },          { 245, 643.8701171875 }, { 348.8701171875, 540 } };
			std::vector< Point > circle;
			for( int s = 0; s < 4; ++s )
				for( int k = 0; k < kBezierSteps; ++k )
					circle.push_back( bezierAt( raw[ 3 * s ], raw[ 3 * s + 1 ], raw[ 3 * s + 2 ], raw[ ( 3 * s + 3 ) % 12 ], double( k ) / kBezierSteps ) );
			const double cw = worstCorner( m.heads[ 1 ].mask, circle, 1920, 1080 );
			Check( m.heads[ 1 ].maskShowsInside && cw <= kCorner,
			       fmt( "Slice 1's mask is the circle's four cubics, 16 lines each, from t = 0: %zu points (want 64), off by %.2g",
			            m.heads[ 1 ].mask.size(), cw ) );
		}
	}

	//A disabled slice is no head: LPP B with its first slice switched off.
	{
		std::string xml        = ReadText( Fixture( "LPP slicer proof B.xml" ) );
		const size_t named     = xml.find( "value=\"S1 Out 1\"/>" );
		const size_t enabled   = named == std::string::npos ? named : xml.find( "name=\"Enabled\"", named );
		const size_t value     = enabled == std::string::npos ? enabled : xml.find( "value=\"1\"", enabled );
		if( value != std::string::npos )
			xml.replace( value, 9, "value=\"0\"" );
		const SliceMap m = ParsePreset( xml, "LPP B, S1 Out 1 disabled", perturb.readerHooks );
		bool gone        = true;
		for( const Head& h : m.heads )
			gone = gone && h.name != "S1 Out 1";
		Check( value != std::string::npos && m.heads.size() == 2 && m.skippedDisabled == 1 && gone,
		       fmt( "LPP B with S1 Out 1 disabled: %zu heads (want 2), %d skipped as disabled (want 1)", m.heads.size(), m.skippedDisabled ) );
	}
	return Verdict();
}

//===========================================================================
// --pointer (no GL): which file is read, in a scratch Documents tree.
//===========================================================================
std::string pointerXml( const std::string& preset )
{
	return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<XmlState name=\"AdvancedOutput\">\n\t<versionInfo name=\"Resolume Arena\" "
	       "majorVersion=\"7\" minorVersion=\"28\" microVersion=\"1\" revision=\"0\"/>\n\t<ScreenSetup name=\"ScreenSetup\" presetFile=\"" +
	       preset + "\"/>\n</XmlState>\n";
}

int runPointer( const Perturb& perturb )
{
	std::printf( "\n=== pointer: Arena's AdvancedOutput.xml names the preset; the Preset File overrides it; anything missing is the grid\n" );
	const std::string docs    = ScratchDir() + "/pointer-docs";
	const std::string arena   = docs + "/Resolume Arena";
	const std::string presets = arena + "/Presets/Advanced Output";
	MakeDirs( arena + "/Preferences" );
	MakeDirs( presets );
	WriteFile( presets + "/two.xml", ReadText( Fixture( "LPP slicer proof A.xml" ) ) );
	WriteFile( presets + "/one.xml", ReadText( Fixture( "output_map_1.xml" ) ) );
	WriteFile( presets + "/empty.xml", "<?xml version=\"1.0\"?>\n<XmlState><ScreenSetup><CurrentCompositionTextureSize width=\"1920\" "
	                                   "height=\"1080\"/><screens/></ScreenSetup></XmlState>\n" );
	WriteFile( arena + "/Preferences/AdvancedOutput.xml", pointerXml( "two" ) );

	double now = 0.0;
	auto poll  = [ & ]( MapSource& s, const MapSource::Settings& settings ) {
		now += 1.0;
		s.Poll( now, settings );
	};
	auto ends = []( const std::string& s, const std::string& tail ) {
		return s.size() >= tail.size() && s.compare( s.size() - tail.size(), tail.size(), tail ) == 0;
	};
	MapSource::Settings follow;

	{
		MapSource s;
		s.SetDocumentsForTest( docs );
		s.SetReaderHooksForTest( perturb.readerHooks );
		poll( s, follow );
		std::string pointer;
		const std::string named = ActivePresetPath( docs, &pointer );
		Check( s.FromPreset() && ends( s.Map().source, "/two.xml" ) && s.Map().heads.size() == 3 && ends( pointer, "/Preferences/AdvancedOutput.xml" ) &&
		           named == presets + "/two.xml",
		       fmt( "presetFile=\"two\" reads %s: %zu heads (want 3, LPP A's)", s.Map().source.c_str(), s.Map().heads.size() ) );

		MapSource::Settings over = follow;
		over.presetFile          = presets + "/one.xml";
		poll( s, over );
		Check( s.FromPreset() && ends( s.Map().source, "/one.xml" ) && s.Map().heads.size() == 3 && s.Map().heads[ 0 ].name == "trailer",
		       fmt( "the Preset File wins over the pointer: %s, first head [%s] (want output_map_1's trailer)", s.Map().source.c_str(),
		            s.Map().heads.empty() ? "" : s.Map().heads[ 0 ].name.c_str() ) );

		MapSource::Settings grid = follow;
		grid.useMap              = false;
		grid.rows                = 3;
		grid.columns             = 5;
		poll( s, grid );
		Check( !s.FromPreset() && s.Map().heads.size() == 15, fmt( "Slices Grid, 3 x 5: %zu heads from %s", s.Map().heads.size(), s.Map().source.c_str() ) );
	}

	struct Fallback
	{
		const char* what;
		std::function< void() > arrange;
		const char* because;
	};
	const Fallback fallbacks[] = {
		{ "the pointer names a file that is not there", [ & ] { WriteFile( arena + "/Preferences/AdvancedOutput.xml", pointerXml( "gone" ) ); }, "no file" },
		{ "the preset has no slices", [ & ] { WriteFile( arena + "/Preferences/AdvancedOutput.xml", pointerXml( "empty" ) ); }, "" },
		{ "there is no pointer", [ & ] { std::remove( ( arena + "/Preferences/AdvancedOutput.xml" ).c_str() ); }, "no AdvancedOutput.xml" },
	};
	for( const Fallback& f : fallbacks )
	{
		f.arrange();
		MapSource s;
		s.SetDocumentsForTest( docs );
		s.SetReaderHooksForTest( perturb.readerHooks );
		poll( s, follow );
		const bool because = std::strlen( f.because ) == 0 || s.Map().source.find( f.because ) != std::string::npos;
		Check( !s.FromPreset() && s.Map().heads.size() == 8 && because,
		       fmt( "%s: the 2 x 4 grid (%zu heads), the reason in the source: %s", f.what, s.Map().heads.size(), s.Map().source.c_str() ) );
	}

	//Avenue's tree, when Arena's has no pointer.
	{
		const std::string avenue = docs + "/Resolume Avenue";
		MakeDirs( avenue + "/Preferences" );
		MakeDirs( avenue + "/Presets/Advanced Output" );
		WriteFile( avenue + "/Presets/Advanced Output/av.xml", ReadText( Fixture( "unmaptest.xml" ) ) );
		WriteFile( avenue + "/Preferences/AdvancedOutput.xml", pointerXml( "av" ) );
		MapSource s;
		s.SetDocumentsForTest( docs );
		poll( s, follow );
		Check( s.FromPreset() && ends( s.Map().source, "Resolume Avenue/Presets/Advanced Output/av.xml" ) && s.Map().heads.size() == 4,
		       fmt( "no Arena pointer, an Avenue one: %s, %zu heads (want unmaptest's 4)", s.Map().source.c_str(), s.Map().heads.size() ) );
	}
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	XenonPlugin plugin;
	std::map< std::string, int > seen, address;
	int longNames = 0, dupes = 0, clashes = 0, slashes = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( name.size() > 16 )
		{
			std::printf( "    too long: %s\n", name.c_str() );
			++longNames;
		}
		if( seen[ name ]++ > 0 )
			++dupes;
		if( name.find( '/' ) != std::string::npos )
			++slashes;
		std::string key;
		for( char c : name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( address[ key ]++ > 0 )
			++clashes;
	}
	const unsigned int aboutFirst = plugin.GetNumParams() - stoatworks::about::kParamCount;
	const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
	Check( longNames == 0 && dupes == 0 && clashes == 0 && slashes == 0 && aboutLast,
	       fmt( "SW Xenon: %u parameters, %d too long, %d duplicated, %d clashing addresses, %d with a '/'; the About block is last (%s)",
	            plugin.GetNumParams(), longNames, dupes, clashes, slashes, aboutLast ? "yes" : "NO" ) );
	return Verdict();
}

//===========================================================================
// --cues (no GL): options, switches, counts and events step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options, switches, counts and events, and ramps standard controls\n" );
	std::istringstream sheet( "0 Trigger 0\n60 Trigger 6\n0 Fire 0\n60 Fire 1\n0 Room Light 0\n60 Room Light 1\n0 Seed 4\n60 Seed 48\n0 Look 0\n60 Look 2\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	XenonPlugin plugin;
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b ) : ( mid == a && valueAt( found->second, 60, ramp ) == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0 && tracks.size() == 5, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --reread: Arena's Save & Close, picked up while the effect runs.
//===========================================================================
int runReread( const Perturb& perturb )
{
	std::printf( "\n=== reread: a preset rewritten in place is picked up within %.1f s of plugin time; nothing is read while neither file changes\n",
	             MapSource::kPollInterval );
	const Raster raster = kRasters.back();
	const double frameTime = 1.0 / 60.0;
	//The poll falls on the first frame at least kPollInterval after the last
	//one: 30 frames at 60 fps, or 31 when the subtraction of two frame times
	//rounds below 0.5; and the write can land just after a poll.
	const double limit = MapSource::kPollInterval + 2.0 * frameTime;

	//1. The Preset File, rewritten alone.
	{
		const std::string file = ScratchDir() + "/reread.xml";
		WriteFile( file, ReadText( Fixture( "output_map_1.xml" ) ) );
		Touch( file, 1000.0 );
		Host host;
		if( !startHost( host, raster, flatClip( 0.0f ), perturb, file ) )
			return 1;
		if( !host.Render( 30 ) )
			return 1;
		const int reads0 = host.plugin.Source().Reads(), changes0 = host.plugin.MapChanges();
		//127 frames: the write lands 7 frames after a poll, so the delay
		//measured is most of an interval, not a frame.
		if( !host.Render( 127 ) )
			return 1;
		const int idleReads = host.plugin.Source().Reads() - reads0;
		WriteFile( file, ReadText( Fixture( "unmaptest.xml" ) ) );
		Touch( file, 1010.0 );
		const int written = host.frame;
		int picked        = -1;
		for( int i = 0; i < 90 && picked < 0; ++i )
		{
			if( !host.Render( 1 ) )
				return 1;
			if( host.plugin.MapChanges() > changes0 )
				picked = host.frame - 1;
		}
		const double delay = picked < 0 ? 1e9 : ( picked - written + 1 ) * frameTime;
		Check( changes0 == 1 && idleReads == 0 && picked >= 0 && delay <= limit &&
		           host.plugin.RigState().HeadCount() == 4,
		       fmt( "Preset File: %d read in 2.1 s of an unchanged file; rewritten (output_map_1's 3 heads to unmaptest's 4), picked up after %.3f s "
		            "(limit %.3f), %d heads now",
		            idleReads, delay, limit, host.plugin.RigState().HeadCount() ) );
	}

	//2. Following the pointer, both files rewritten at once (Save & Close),
	//then a Save & Close that changes nothing.
	{
		const std::string docs    = ScratchDir() + "/reread-docs";
		const std::string arena   = docs + "/Resolume Arena";
		const std::string presets = arena + "/Presets/Advanced Output";
		MakeDirs( arena + "/Preferences" );
		MakeDirs( presets );
		const std::string pointer = arena + "/Preferences/AdvancedOutput.xml";
		const std::string live    = presets + "/live.xml";
		WriteFile( live, ReadText( Fixture( "LPP slicer proof A.xml" ) ) );
		WriteFile( pointer, pointerXml( "live" ) );
		Touch( live, 2000.0 );
		Touch( pointer, 2000.0 );
		Host host;
		if( !startHost( host, raster, flatClip( 0.0f ), perturb, "grid" ) )
			return 1;
		host.plugin.SetDocumentsForTest( docs );
		host.Set( PT_SLICES, static_cast< float >( Slices::Map ) );
		host.Set( PT_TRIGGER, static_cast< float >( Trigger::Beat ) );
		if( !host.Render( 37 ) )
			return 1;
		const std::string firstName = host.plugin.Source().Map().heads.empty() ? "" : host.plugin.Source().Map().heads[ 0 ].name;
		const int changes0          = host.plugin.MapChanges();

		WriteFile( live, ReadText( Fixture( "unmaptest.xml" ) ) );
		Touch( live, 2010.0 );
		Touch( pointer, 2010.0 );
		const int written = host.frame;
		int picked        = -1;
		for( int i = 0; i < 90 && picked < 0; ++i )
		{
			if( !host.Render( 1 ) )
				return 1;
			if( host.plugin.MapChanges() > changes0 )
				picked = host.frame - 1;
		}
		const double delay = picked < 0 ? 1e9 : ( picked - written + 1 ) * frameTime;
		Check( firstName == "S1 Out 1 s2 p2" && picked >= 0 && delay <= limit && host.plugin.RigState().HeadCount() == 4,
		       fmt( "following the pointer: Save & Close (both files) picked up after %.3f s, %d heads now (want 4)", delay,
		            host.plugin.RigState().HeadCount() ) );

		//Nothing changed: read again, but the same rig carries on.
		if( !host.Render( 60 ) )
			return 1;
		const int reads1 = host.plugin.Source().Reads(), changes1 = host.plugin.MapChanges();
		const long long steps1 = host.plugin.RigState().Steps();
		Touch( live, 2020.0 );
		Touch( pointer, 2020.0 );
		if( !host.Render( 60 ) )
			return 1;
		Check( host.plugin.Source().Reads() == reads1 + 1 && host.plugin.MapChanges() == changes1 && host.plugin.RigState().Steps() > steps1,
		       fmt( "a Save & Close that changes nothing: read %d time(s), %d map change(s), the chase on from step %lld to %lld (not back to 0)",
		            host.plugin.Source().Reads() - reads1, host.plugin.MapChanges() - changes1, steps1, host.plugin.RigState().Steps() ) );
	}
	return Verdict();
}

//===========================================================================
// --charge: a head's flash after an interval dt is ( 1 - e^( -dt / RC ) )^2
// of a full one.
//===========================================================================
int runCharge( const Perturb& perturb )
{
	std::printf( "\n=== charge: a flash's energy against the time since the head last fired is ( 1 - e^(-dt/RC) )^2, a full bank's 1\n" );
	for( const Raster& raster : kRasters )
	{
		Host host;
		if( !startHost( host, raster, flatClip( 0.0f ), perturb ) )
			return 1;
		host.Set( PT_ROWS, 1.0f );
		host.Set( PT_COLUMNS, 1.0f );
		host.Set( PT_TRIGGER, static_cast< float >( Trigger::FireOnly ) );
		Bank bank;
		bank.rc                = setRecharge( host, 0.25 );
		bank.tau               = setDecay( host, 2e-3 );
		const double intensity = setIntensity( host, 0.5 );
		bank.reset( 1 );
		int x = 0, y = 0;
		cellPixel( 0, 0, 1, 1, raster.w, raster.h, x, y );
		if( !host.Render( 5 ) )
			return 1;
		const double targets[] = { 0.0, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0 };
		double worst = 0.0, lowest = 1e9, highest = 0.0;
		int bad      = 0;
		std::string table;
		for( double target : targets )
		{
			if( target > 0.0 )
			{
				const int gap = std::max( 1, static_cast< int >( std::lround( target * bank.rc * host.fps ) ) ) - 1;
				if( !host.Render( gap ) )
					return 1;
			}
			const int f     = host.frame;
			const double t0 = host.Seconds( f - 1 ), t1 = host.Seconds( f );
			const double previous = bank.last[ 0 ];
			const double e        = bank.fire( 0, t0 );
			if( !host.Fire() )
				return 1;
			const double want = bank.exposure( 0, t0, t1 );
			const double got  = lightOf( host, x, y, intensity );
			const double err  = std::fabs( got - want );
			bad += err > lightTolerance( want, intensity );
			worst = std::max( worst, err / std::max( want, 1e-12 ) );
			if( target > 0.0 )
			{
				const double ratio = ( t0 - previous ) / bank.rc;
				lowest             = std::min( lowest, ratio );
				highest            = std::max( highest, ratio );
				table += fmt( " %.2f:%.4f", ratio, got / ( 1.0 - std::exp( -( t1 - t0 ) / bank.tau ) ) );
			}
			else
				table += fmt( " full:%.4f", got / ( 1.0 - std::exp( -( t1 - t0 ) / bank.tau ) ) );
			( void )e;
		}
		Check( bad == 0, fmt( "%dx%d: %zu presses, dt/RC %.2f to %.2f (RC %.4f s, tau %.2f ms); E read back (dt/RC:E)%s; worst relative "
		                      "error %.2g (tolerance %.2g: the float32 store and the curve)",
		                      raster.w, raster.h, std::size( targets ), lowest, highest, bank.rc, bank.tau * 1e3, table.c_str(), worst,
		                      kStoreRel + kCurveRel ) );
	}
	return Verdict();
}

//===========================================================================
// --energy: one flash's light summed over every frame it lands in is E,
// whatever the Decay, and each frame after the first catches e^(-T/tau) of
// the one before.
//===========================================================================
int runEnergy( const Perturb& perturb )
{
	std::printf( "\n=== energy: a flash's light over all the frames it lands in is its energy, whatever tau; frame to frame it falls by e^(-T/tau)\n" );
	const double taus[] = { 0.5e-3, 2e-3, 20e-3, 200e-3, 500e-3 };
	for( const Raster& raster : kRasters )
	{
		std::string table;
		int bad = 0;
		for( double wantTau : taus )
		{
			Host host;
			if( !startHost( host, raster, flatClip( 0.0f ), perturb ) )
				return 1;
			host.Set( PT_ROWS, 1.0f );
			host.Set( PT_COLUMNS, 1.0f );
			host.Set( PT_TRIGGER, static_cast< float >( Trigger::FireOnly ) );
			setRecharge( host, 0.25 );
			const double tau       = setDecay( host, wantTau );
			const double intensity = setIntensity( host, 0.5 );
			int x = 0, y = 0;
			cellPixel( 0, 0, 1, 1, raster.w, raster.h, x, y );
			if( !host.Render( 5 ) || !host.Fire() )
				return 1;
			const double T = 1.0 / host.fps;
			const double r = std::exp( -T / tau );
			//The plugin keeps a flash for 20 tau and draws no light below
			//kDark: what that leaves out, bounded.
			const int frames    = static_cast< int >( std::ceil( 21.0 * tau / T ) ) + 2;
			double sum          = lightOf( host, x, y, intensity );
			double previous     = sum;
			double worstRatio   = 0.0;
			int ratios          = 0;
			for( int i = 1; i < frames; ++i )
			{
				if( !host.Render( 1 ) )
					return 1;
				const double got = lightOf( host, x, y, intensity );
				sum += got;
				//Where both frames are well above kDark and the curve.s
				//range, the ratio is a measurement.
				if( previous * intensity > 1e-3 && got * intensity > 1e-3 )
				{
					worstRatio = std::max( worstRatio, std::fabs( got / previous - r ) / r );
					++ratios;
				}
				previous = got;
			}
			const double tail      = ( kDarkLight / intensity ) / std::max( 1.0 - r, 1e-12 ) + std::exp( -20.0 );
			const double tolerance = kStoreRel + kCurveRel + tail;
			const double err       = std::fabs( sum - 1.0 );
			const double ratioTol  = 2.0 * ( kStoreRel + kCurveRel );
			const bool ok          = err <= tolerance && worstRatio <= ratioTol;
			bad += !ok;
			table += fmt( "\n      tau %7.2f ms: summed %.6f over %d frames (want 1, tolerance %.2g); %d frame-to-frame ratios, worst off e^(-T/tau) "
			              "by %.2g (tolerance %.2g)%s",
			              tau * 1e3, sum, frames, tolerance, ratios, worstRatio, ratioTol, ok ? "" : "  <-- wrong" );
		}
		Check( bad == 0, fmt( "%dx%d, a full flash:%s", raster.w, raster.h, table.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --chase: the step on the frame that holds its beat, at the moment inside it
// where the beat fell, lighting the heads the pattern names.
//===========================================================================
struct ChaseCase
{
	const char* name;
	Pattern pattern;
	HeadOrder order;
	int seed;
	std::vector< int > orderHeads;///< position -> head, worked out by hand for the 2 x 4 grid
};

int runChase( const Perturb& perturb )
{
	constexpr int kRows = 2, kColumns = 4, kHeads = kRows * kColumns;
	constexpr double kBpm = 128.0;
	constexpr int kBeats  = 24;
	std::printf( "\n=== chase: Trigger Beat at %g BPM (28.125 frames a beat, so beats fall inside frames) over a %d x %d grid: every head's light on "
	             "every frame against the SPEC's chase, the bank and the tube\n",
	             kBpm, kRows, kColumns );
	const std::vector< int > listed = { 0, 1, 2, 3, 4, 5, 6, 7 };
	//Left to right: by centroid x, then y (y down): each column top then bottom.
	const std::vector< int > across = { 0, 4, 1, 5, 2, 6, 3, 7 };
	const std::vector< ChaseCase > cases = {
		{ "Forward", Pattern::Forward, HeadOrder::AsListed, 1, listed },
		{ "Reverse", Pattern::Reverse, HeadOrder::AsListed, 1, listed },
		{ "Bounce", Pattern::Bounce, HeadOrder::AsListed, 1, listed },
		{ "Build", Pattern::Build, HeadOrder::AsListed, 1, listed },
		{ "All", Pattern::All, HeadOrder::AsListed, 1, listed },
		{ "Forward, Left to Right", Pattern::Forward, HeadOrder::LeftToRight, 1, across },
		{ "Random, Seed 1", Pattern::Random, HeadOrder::AsListed, 1, listed },
		{ "Random, Seed 7", Pattern::Random, HeadOrder::AsListed, 7, listed },
		{ "Random, Seed 1 again", Pattern::Random, HeadOrder::AsListed, 1, listed },
	};
	for( const Raster& raster : kRasters )
	{
		std::map< int, std::vector< int > > randomOrders;//seed -> heads, step by step
		std::string table;
		int bad = 0;
		for( const ChaseCase& c : cases )
		{
			Host host;
			host.bpm = kBpm;
			if( !startHost( host, raster, flatClip( 0.0f ), perturb ) )
				return 1;
			host.Set( PT_ROWS, static_cast< float >( kRows ) );
			host.Set( PT_COLUMNS, static_cast< float >( kColumns ) );
			host.Set( PT_TRIGGER, static_cast< float >( Trigger::Beat ) );
			host.Set( PT_PATTERN, static_cast< float >( c.pattern ) );
			host.Set( PT_ORDER, static_cast< float >( c.order ) );
			host.Set( PT_SEED, static_cast< float >( c.seed ) );
			Bank bank;
			bank.rc                = setRecharge( host, 0.25 );
			bank.tau               = setDecay( host, 5e-3 );
			const double intensity = setIntensity( host, 0.5 );
			bank.reset( kHeads );
			int px[ kHeads ], py[ kHeads ];
			for( int h = 0; h < kHeads; ++h )
				cellPixel( h / kColumns, h % kColumns, kRows, kColumns, raster.w, raster.h, px[ h ], py[ h ] );

			//The beats, at their own times: beat b (b >= 1) at b * 60 / BPM is
			//step b - 1 (the phase starts at 0, which no frame crosses).
			const double beatTime = 60.0 / kBpm;
			const int frames      = static_cast< int >( kBeats * beatTime * host.fps ) + 6;
			std::vector< std::vector< double > > got( static_cast< size_t >( frames ), std::vector< double >( kHeads, 0.0 ) );
			for( int f = 0; f < frames; ++f )
			{
				if( !host.Render( 1 ) )
					return 1;
				for( int h = 0; h < kHeads; ++h )
					got[ static_cast< size_t >( f ) ][ static_cast< size_t >( h ) ] = lightOf( host, px[ h ], py[ h ], intensity );
			}

			//Who fired at each step: the SPEC's formula, or for Random the head
			//the picture shows (the light of a beat lands in its frame and the
			//next).
			std::vector< std::vector< int > > steps;
			for( int b = 1; b * beatTime < ( frames - 1 ) / host.fps; ++b )
			{
				std::vector< int > heads;
				if( c.pattern == Pattern::Random )
				{
					const int f = static_cast< int >( std::ceil( b * beatTime * host.fps - 1e-9 ) );
					int best     = -1;
					double most  = 0.0;
					for( int h = 0; h < kHeads; ++h )
					{
						double l = 0.0;
						for( int g = f; g <= f + 1 && g < frames; ++g )
							l += got[ static_cast< size_t >( g ) ][ static_cast< size_t >( h ) ];
						if( l > most )
							most = l, best = h;
					}
					if( best >= 0 )
						heads.push_back( best );
				}
				else
					for( int position : specStep( c.pattern, b - 1, kHeads ) )
						heads.push_back( c.orderHeads[ static_cast< size_t >( position ) ] );
				steps.push_back( heads );
				for( int h : heads )
					bank.fire( h, b * beatTime );
			}

			//Every head on every frame. The beat's moment comes from the
			//host's float bar phase: each end of the frame's phase is within
			//2^-25 of the truth, which moves the interpolated moment by up to
			//2^-24 * 240 / BPM s and the light by up to E / tau of that.
			const double phaseTime = std::ldexp( 1.0, -24 ) * 240.0 / kBpm;
			int wrong              = 0;
			double worst           = 0.0;
			for( int f = 1; f < frames; ++f )
				for( int h = 0; h < kHeads; ++h )
				{
					const double want = bank.exposure( h, host.Seconds( f - 1 ), host.Seconds( f ) );
					const double err  = std::fabs( got[ static_cast< size_t >( f ) ][ static_cast< size_t >( h ) ] - want );
					const double tol  = lightTolerance( want, intensity ) + phaseTime / bank.tau;
					if( err > tol )
						++wrong;
					worst = std::max( worst, err / std::max( tol, 1e-30 ) );
				}

			std::string sequence;
			for( size_t k = 0; k < steps.size() && k < 16; ++k )
			{
				sequence += k ? " " : "";
				for( size_t i = 0; i < steps[ k ].size(); ++i )
					sequence += ( i ? "+" : "" ) + std::to_string( steps[ k ][ i ] );
			}
			bool property = true;
			std::string note;
			if( c.pattern == Pattern::Random )
			{
				//Every head once a cycle, the order fixed by the Seed.
				std::vector< int > flat;
				for( const auto& s : steps )
					flat.push_back( s.empty() ? -1 : s[ 0 ] );
				for( size_t cycle = 0; ( cycle + 1 ) * kHeads <= flat.size(); ++cycle )
				{
					std::set< int > once( flat.begin() + static_cast< long >( cycle * kHeads ), flat.begin() + static_cast< long >( ( cycle + 1 ) * kHeads ) );
					property = property && once.size() == kHeads && *once.begin() >= 0;
				}
				const auto earlier = randomOrders.find( c.seed );
				if( earlier != randomOrders.end() )
				{
					property = property && earlier->second == flat;
					note     = flat == earlier->second ? ", the same as before" : ", NOT the same as before";
				}
				else
				{
					for( const auto& other : randomOrders )
						if( other.second == flat )
						{
							property = false;
							note     = ", the same as another Seed's";
						}
					randomOrders[ c.seed ] = flat;
				}
				note = ( property ? ", every head once a cycle" : ", NOT every head once a cycle" ) + note;
			}
			const bool ok = wrong == 0 && property && !steps.empty();
			bad += !ok;
			table += fmt( "\n      %-22s steps %s ...%s; %d of %d head-frames off (worst %.2f of its tolerance)%s", c.name, sequence.c_str(), note.c_str(),
			              wrong, ( frames - 1 ) * kHeads, worst, ok ? "" : "  <-- wrong" );
		}
		Check( bad == 0, fmt( "%dx%d, %d beats each:%s", raster.w, raster.h, kBeats, table.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --inside: a head's light lands inside its region and nowhere else, on every
// map; Outline only within Width of the region's edge.
//===========================================================================
int runInside( const Perturb& perturb )
{
	std::printf( "\n=== inside: each head lit alone, every pixel against its region (winding number in double); Outline within Width of the edge\n" );
	const std::vector< std::string > presets = { "LPP slicer proof A.xml", "LPP slicer proof B.xml", "output_map_1.xml", "unmaptest.xml",
		                                         "unmaptest-7.28.1-polygon-masks.xml", "grid" };
	constexpr double kWidth = 12.0;
	for( const Raster& raster : kRasters )
	{
		std::string table;
		int bad = 0;
		for( Look look : { Look::Fill, Look::Outline } )
			for( const std::string& name : presets )
			{
				const bool grid = name == "grid";
				Host host;
				if( !startHost( host, raster, flatClip( 0.0f ), perturb, grid ? "grid" : Fixture( name ) ) )
					return 1;
				host.Set( PT_TRIGGER, static_cast< float >( Trigger::FireOnly ) );
				host.Set( PT_PATTERN, static_cast< float >( Pattern::Forward ) );
				host.Set( PT_ORDER, static_cast< float >( HeadOrder::AsListed ) );
				host.Set( PT_LOOK, static_cast< float >( look ) );
				const float widthParam = ParamFromWidth( kWidth );
				host.Set( PT_WIDTH, widthParam );
				//A short tube, so a head is dark again by the next press.
				setDecay( host, 1e-3 );
				setIntensity( host, 0.5 );
				if( !host.Render( 3 ) )
					return 1;
				//The expected regions from an unhooked read of the same file.
				const SliceMap map = grid ? GridMap( 2, 4 ) : ReadPreset( Fixture( name ) );
				const double band  = WidthFromParam( widthParam ) * ( grid ? 1.0 : raster.h / map.compHeight );
				int outside = 0, dark = 0, skipped = 0, lit = 0;
				for( size_t h = 0; h < map.heads.size(); ++h )
				{
					if( !host.Fire() )
						return 1;
					const Floats out    = host.Output();
					const Region region = regionOf( map.heads[ h ], raster.w, raster.h );
					for( int y = 0; y < raster.h; ++y )
						for( int x = 0; x < raster.w; ++x )
						{
							const double cx = x + 0.5, cy = y + 0.5;
							const bool on   = out[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ] > 0.0f;
							lit += on;
							bool want = false;
							if( cx >= region.x0 - 2.0 && cx <= region.x1 + 2.0 && cy >= region.y0 - 2.0 && cy <= region.y1 + 2.0 )
							{
								double d     = 0.0;
								const bool in = insideRegion( region, cx, cy, d );
								if( d < kEdgeMargin || ( look == Look::Outline && std::fabs( d - band ) < kEdgeMargin ) )
								{
									++skipped;
									continue;
								}
								want = in && ( look == Look::Fill || d <= band );
							}
							if( on && !want )
								++outside;
							else if( !on && want )
								++dark;
						}
					if( !host.Render( 1 ) )
						return 1;
				}
				const bool ok = outside == 0 && dark == 0 && lit > 0;
				bad += !ok;
				table += fmt( "\n      %-8s %-36s %3zu heads: %8d lit, %d outside, %d dark inside, %d within %.0e px of an edge left out%s",
				              look == Look::Fill ? "Fill" : "Outline", name.c_str(), map.heads.size(), lit, outside, dark, skipped, kEdgeMargin,
				              ok ? "" : "  <-- wrong" );
			}
		Check( bad == 0, fmt( "%dx%d, Width %g composition px:%s", raster.w, raster.h, kWidth, table.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --audio: an onset steps the chase on the frame it is heard; nothing fires
// from the cold start.
//===========================================================================
int runAudio( const Perturb& perturb )
{
	std::printf( "\n=== audio: Trigger Onset steps the chase on the frame a kick is heard, that frame lit; nothing fires from the cold start\n" );
	const Raster raster = kRasters.back();
	constexpr int kHeads = 4;
	auto music = [ & ]( XenonPlugin& plugin, int f, bool loudStart ) {
		const int since = ( f - 20 ) % 30;
		const bool hard = ( ( f - 20 ) / 30 ) % 2 == 1;
		for( int bin = 0; bin < audio::kBins; ++bin )
		{
			double v = bin > 40 ? 0.05 : 0.02;
			if( f >= 20 && since < 4 && bin < 10 )
				v += ( hard ? 0.9 : 0.25 ) * std::exp( -since * 0.8 );
			if( loudStart && f < 2 )
				v += 0.3;
			plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), static_cast< float >( v ) );
		}
	};
	Host host;
	if( !startHost( host, raster, flatClip( 0.0f ), perturb ) )
		return 1;
	host.Set( PT_ROWS, 1.0f );
	host.Set( PT_COLUMNS, static_cast< float >( kHeads ) );
	host.Set( PT_TRIGGER, static_cast< float >( Trigger::Onset ) );
	host.Set( PT_THRESHOLD, 0.4f );
	setDecay( host, 2e-3 );
	const double intensity = setIntensity( host, 0.5 );
	host.beforeFrame       = [ & ]( int f ) { music( host.plugin, f, false ); };
	int px[ kHeads ], py[ kHeads ];
	for( int h = 0; h < kHeads; ++h )
		cellPixel( 0, h, 1, kHeads, raster.w, raster.h, px[ h ], py[ h ] );
	std::vector< int > kicks, fired, litRight;
	for( int f = 0; f < 220; ++f )
	{
		if( f >= 20 && ( f - 20 ) % 30 == 0 )
			kicks.push_back( f );
		const unsigned long long before = host.plugin.RigState().Fired();
		if( !host.Render( 1 ) )
			return 1;
		if( host.plugin.RigState().Fired() > before )
			fired.push_back( f );
		//Step k lights head k mod 4, and the frame that heard it shows it.
		const int expectHead = static_cast< int >( ( fired.empty() ? 0 : fired.size() - 1 ) % kHeads );
		if( !fired.empty() && fired.back() == f && lightOf( host, px[ expectHead ], py[ expectHead ], intensity ) > 0.4 )
			litRight.push_back( f );
	}
	Host cold;
	if( !startHost( cold, raster, flatClip( 0.0f ), perturb ) )
		return 1;
	cold.Set( PT_ROWS, 1.0f );
	cold.Set( PT_COLUMNS, static_cast< float >( kHeads ) );
	cold.Set( PT_TRIGGER, static_cast< float >( Trigger::Onset ) );
	cold.Set( PT_THRESHOLD, 0.4f );
	cold.beforeFrame = [ & ]( int f ) { music( cold.plugin, f, true ); };
	if( !cold.Render( 20 ) )
		return 1;
	const unsigned long long coldFlashes = cold.plugin.RigState().Fired();
	Check( fired == kicks && litRight == kicks,
	       fmt( "%zu kicks, %zu steps fired (%s on the kicks' own frames), %zu of them lit the next head on the frame that heard it", kicks.size(), fired.size(),
	            fired == kicks ? "all" : "NOT all", litRight.size() ) );
	Check( coldFlashes == 0, fmt( "a fresh instance with music already loud on its first frame: %llu flash%s in the first 20 frames (none before the first kick)",
	                              coldFlashes, coldFlashes == 1 ? "" : "es" ) );
	return Verdict();
}

//===========================================================================
// --alpha: the output alpha the SPEC decides, over opaque, half and clear
// clip.
//===========================================================================
double codeOf( double linear )
{
	linear = std::max( linear, 0.0 );
	return std::clamp( linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow( linear, 1.0 / 2.4 ) - 0.055, 0.0, 1.0 );
}

int runAlpha( const Perturb& perturb )
{
	std::printf( "\n=== alpha: Fill and Outline make a lit pixel opaque as far as its light goes; Gate keeps the clip's alpha; premultiplied out\n" );
	const Raster raster = kRasters.back();
	//Three bands of the clip, premultiplied: grey 0.5 at alpha 1, 0.5 and 0.
	const double alphas[ 3 ] = { 1.0, 0.5, 0.0 };
	constexpr double kGrey   = 0.5;
	Clip bands               = [ & ]( int, int width, int height, Floats& out ) {
        out.assign( static_cast< size_t >( width ) * height * 4, 0.0f );
        for( int y = 0; y < height; ++y )
            for( int x = 0; x < width; ++x )
            {
                const double a = alphas[ std::min( 2, x * 3 / width ) ];
                float* o       = &out[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
                o[ 0 ] = o[ 1 ] = o[ 2 ] = static_cast< float >( kGrey * a );
                o[ 3 ]                   = static_cast< float >( a );
            }
	};
	std::string table;
	int bad = 0;
	for( Look look : { Look::Fill, Look::Gate, Look::Outline } )
	{
		Host host;
		if( !startHost( host, raster, bands, perturb ) )
			return 1;
		host.Set( PT_ROWS, 1.0f );
		host.Set( PT_COLUMNS, 1.0f );
		host.Set( PT_TRIGGER, static_cast< float >( Trigger::FireOnly ) );
		host.Set( PT_LOOK, static_cast< float >( look ) );
		host.Set( PT_ROOM, 0.6f );
		const double room      = RoomFromParam( 0.6f );
		const double tau       = setDecay( host, 2e-3 );
		const double intensity = setIntensity( host, 0.25 );
		if( !host.Render( 5 ) )
			return 1;
		for( int pass = 0; pass < 2; ++pass )
		{
			//Pass 0 before any flash, pass 1 on a press of a full bank.
			if( pass == 1 ? !host.Fire() : !host.Render( 1 ) )
				return 1;
			const double light = pass == 1 ? intensity * ( 1.0 - std::exp( -( 1.0 / host.fps ) / tau ) ) : 0.0;
			for( int b = 0; b < 3; ++b )
			{
				const int x = ( 2 * b + 1 ) * raster.w / 6, y = raster.h / 2;
				//Outline's band (Width 12 output px on a grid) is nowhere near
				//the middle of the frame: no light there.
				const double l       = look == Look::Outline ? 0.0 : light;
				const double a       = alphas[ b ];
				const double surface = std::pow( ( kGrey + 0.055 ) / 1.055, 2.4 );
				const auto expect    = [ & ]( double L, double& rgb, double& alpha ) {
                    if( look == Look::Gate )
                    {
                        alpha = a;
                        rgb   = codeOf( surface * ( room + L ) ) * a;
                    }
                    else
                    {
                        alpha = std::max( a, std::min( 1.0, L ) );
                        rgb   = alpha > 0.0 ? codeOf( ( surface * room * a + L ) / alpha ) * alpha : 0.0;
                    }
				};
				//The light is stored as float32: anything within its rounding
				//of it, and float32 on top.
				double rgbLo, alphaLo, rgbHi, alphaHi;
				expect( l * ( 1.0 - kStoreRel ), rgbLo, alphaLo );
				expect( l * ( 1.0 + kStoreRel ), rgbHi, alphaHi );
				const auto got   = pixelOf( host, x, y );
				const double eps = 1e-5;
				const bool ok    = got[ 0 ] >= std::min( rgbLo, rgbHi ) - eps && got[ 0 ] <= std::max( rgbLo, rgbHi ) + eps &&
				                got[ 3 ] >= std::min( alphaLo, alphaHi ) - eps && got[ 3 ] <= std::max( alphaLo, alphaHi ) + eps && got[ 0 ] <= got[ 3 ] + eps;
				bad += !ok;
				table += fmt( "\n      %-7s %-11s clip alpha %.1f: out (%.5f, a %.5f), want (%.5f..%.5f, a %.5f..%.5f)%s",
				              look == Look::Fill ? "Fill" : look == Look::Gate ? "Gate" : "Outline", pass ? "flash" : "no flash", a, got[ 0 ], got[ 3 ],
				              std::min( rgbLo, rgbHi ), std::max( rgbLo, rgbHi ), std::min( alphaLo, alphaHi ), std::max( alphaLo, alphaHi ),
				              ok ? "" : "  <-- wrong" );
			}
		}
	}
	Check( bad == 0, fmt( "Room Light %.1f, light %.3f at the press:%s", 0.6, 0.25, table.c_str() ) );
	return Verdict();
}

//===========================================================================
// --resize: a new raster under a running instance keeps every bank's charge.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: the host's raster changes between two presses; the bank's charge carries over and the head fills the new raster\n" );
	std::string table;
	int bad = 0;
	for( const Raster& raster : kRasters )
	{
		const Raster next = { raster.w / 2 + 16, raster.h / 2 + 8 };
		Host host;
		if( !startHost( host, raster, flatClip( 0.0f ), perturb ) )
			return 1;
		host.Set( PT_ROWS, 1.0f );
		host.Set( PT_COLUMNS, 1.0f );
		host.Set( PT_TRIGGER, static_cast< float >( Trigger::FireOnly ) );
		Bank bank;
		bank.rc                = setRecharge( host, 0.25 );
		bank.tau               = setDecay( host, 2e-3 );
		const double intensity = setIntensity( host, 0.5 );
		bank.reset( 1 );
		if( !host.Render( 5 ) )
			return 1;
		bank.fire( 0, host.Seconds( host.frame - 1 ) );
		if( !host.Fire() || !host.Render( 9 ) || !host.Resize( next.w, next.h ) || !host.Render( 5 ) )
			return 1;
		const int f     = host.frame;
		const double e  = bank.fire( 0, host.Seconds( f - 1 ) );
		if( !host.Fire() )
			return 1;
		const double want = bank.exposure( 0, host.Seconds( f - 1 ), host.Seconds( f ) );
		const Floats out  = host.Output();
		int lit           = 0;
		for( size_t i = 0; i < out.size(); i += 4 )
			lit += out[ i ] > 0.0f;
		int x = 0, y = 0;
		cellPixel( 0, 0, 1, 1, next.w, next.h, x, y );
		const double got = lightOf( host, x, y, intensity );
		const bool ok    = std::fabs( got - want ) <= lightTolerance( want, intensity ) && lit == next.w * next.h;
		bad += !ok;
		table += fmt( "\n      %dx%d -> %dx%d: the second press %.4f (want %.4f, E %.4f after %.3f s; a forgotten bank gives ~1), %d of %d pixels lit%s",
		              raster.w, raster.h, next.w, next.h, got, want, e, 15.0 / host.fps, lit, next.w * next.h, ok ? "" : "  <-- wrong" );
	}
	Check( bad == 0, fmt( "a head fired, the raster changed, the head fired again:%s", table.c_str() ) );
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& perturb )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back\n" );
	int problems = 0;
	std::string what;
	struct Config
	{
		const char* name;
		std::string preset;
		Look look;
	};
	const Config configs[] = {
		{ "Fill on the 7.28.1 save (masks, a polygon)", Fixture( "unmaptest-7.28.1-polygon-masks.xml" ), Look::Fill },
		{ "Outline on a 4 x 8 grid", "grid", Look::Outline },
		{ "Gate on unmaptest", Fixture( "unmaptest.xml" ), Look::Gate },
	};
	for( const Config& config : configs )
	{
		Host host;
		if( !startHost( host, { 320, 180 }, flatClip( 0.4f ), perturb, config.preset ) )
			return 1;
		host.Set( PT_LOOK, static_cast< float >( config.look ) );
		host.Set( PT_TRIGGER, static_cast< float >( Trigger::EighthBeat ) );
		host.Set( PT_PATTERN, static_cast< float >( Pattern::All ) );
		host.Set( PT_ROWS, 4.0f );
		host.Set( PT_COLUMNS, 8.0f );
		setDecay( host, 50e-3 );
		GLuint hostArray = 0, hostBuffer = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		Floats picture;
		for( int frame = 0; frame < 40; ++frame )
		{
			flatClip( 0.4f )( frame, 320, 180, picture );
			host.Upload( picture );
			host.Set( PT_FIRE, frame % 7 == 3 ? 1.0f : 0.0f );
			glBindFramebuffer( GL_FRAMEBUFFER, host.outputFBO );
			glViewport( 0, 0, 320, 180 );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, 320, 180 );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			host.plugin.SetTime( frame / 60.0 );
			host.plugin.SetBeatInfo( 120.0f, static_cast< float >( std::fmod( frame / 60.0 * 0.5, 1.0 ) ) );
			if( host.plugin.ProcessOpenGL( &host.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0, srcA = 0, dstA = 0;
			GLfloat clear[ 4 ]   = {};
			GLboolean mask[ 4 ]  = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetIntegerv( GL_BLEND_SRC_ALPHA, &srcA );
			glGetIntegerv( GL_BLEND_DST_ALPHA, &dstA );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			glGetBooleanv( GL_COLOR_WRITEMASK, mask );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					if( what.find( name ) == std::string::npos )
						what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 0 && viewport[ 1 ] == 0 && viewport[ 2 ] == 320 && viewport[ 3 ] == 180, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( host.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA && srcA == GL_ONE && dstA == GL_ZERO, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
			for( int u = 0; u < 8; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
			expect( glGetError() == GL_NO_ERROR, "gl-error" );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		if( host.plugin.RigState().Fired() == 0 )
		{
			++problems;
			what += std::string( " (" ) + config.name + ": nothing fired)";
		}
	}
	Check( problems == 0, fmt( "40 frames each of Fill with masks, Outline on a grid and Gate, chasing on the eighth beat with presses: viewport, vertex "
	                           "array, array buffer, program, active unit, framebuffer, blend, scissor, clear colour, colour mask, eight texture "
	                           "units, no GL error (%d wrong:%s)",
	                           problems, what.empty() ? " none" : what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --bench: GPU time per frame from GL_TIME_ELAPSED.
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	struct Case
	{
		const char* what;
		std::string preset;
		std::function< void( Host& ) > set;
	};
	const Case cases[] = {
		{ "unmaptest's 4 heads, defaults, 120 BPM", Fixture( "unmaptest.xml" ), []( Host& ) {} },
		{ "the 7.28.1 save (5 heads, 86 edges), All", Fixture( "unmaptest-7.28.1-polygon-masks.xml" ),
		  []( Host& h ) { h.Set( PT_PATTERN, static_cast< float >( Pattern::All ) ); } },
		{ "256 heads (16 x 16 grid), All, Eighth Beat", "grid", []( Host& h ) {
			 h.Set( PT_ROWS, 16.0f );
			 h.Set( PT_COLUMNS, 16.0f );
			 h.Set( PT_PATTERN, static_cast< float >( Pattern::All ) );
			 h.Set( PT_TRIGGER, static_cast< float >( Trigger::EighthBeat ) );
			 h.Set( PT_DECAY, ParamFromDecay( 0.5 ) );
		 } },
		{ "256 heads, Outline, All, Eighth Beat", "grid", []( Host& h ) {
			 h.Set( PT_ROWS, 16.0f );
			 h.Set( PT_COLUMNS, 16.0f );
			 h.Set( PT_PATTERN, static_cast< float >( Pattern::All ) );
			 h.Set( PT_TRIGGER, static_cast< float >( Trigger::EighthBeat ) );
			 h.Set( PT_DECAY, ParamFromDecay( 0.5 ) );
			 h.Set( PT_LOOK, static_cast< float >( Look::Outline ) );
		 } },
	};
	std::printf( "\n=== bench: GPU time per frame (GL_TIME_ELAPSED), after 120 frames of warm-up, median and worst of 120\n" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Host host;
			if( !host.Init( size.w, size.h, Clip(), c.preset ) )
				return 1;
			Floats still;
			lookClip( 0, size.w, size.h, still );
			host.Upload( still );
			c.set( host );
			if( !host.Render( 120 ) )
				return 1;
			constexpr int kTimed = 120;
			std::vector< double > times, cpu;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				glBeginQuery( GL_TIME_ELAPSED, query );
				if( !host.Render( 1 ) )
					return 1;
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				times.push_back( static_cast< double >( ns ) * 1e-6 );
				cpu.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			std::sort( times.begin(), times.end() );
			std::sort( cpu.begin(), cpu.end() );
			std::printf( "  %-46s %-6s GPU median %6.2f ms, worst %6.2f  (%4.1f%% of a 60 fps frame); wall %5.2f ms\n", c.what, size.name,
			             times[ kTimed / 2 ], times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ), cpu[ kTimed / 2 ] );
		}
	glDeleteQueries( 1, &query );
	return 0;
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "reread", runReread, false },
		{ "charge", runCharge, false },
		{ "chase", runChase, false },
		{ "inside", runInside, false },
		{ "energy", runEnergy, false },
		{ "audio", runAudio, false },
		{ "alpha", runAlpha, false },
		{ "resize", runResize, false },
		{ "state", runState, false },
		{ "reader", runReader, true },
		{ "pointer", runPointer, true },
		{ "cues", runCues, true },
		{ "names", runNames, true },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	add( "reader", runReader, "every duplicate region kept (unmaptest gives 7)", []( Perturb& p ) { p.readerHooks = kHookNoCollapse; } );
	add( "reader", runReader, "a slice's corners replaced by their bounding box", []( Perturb& p ) { p.readerHooks = kHookBoundingBox; } );
	add( "reader", runReader, "the heads in the file's order, not Arena's panel's", []( Perturb& p ) { p.readerHooks = kHookFileOrder; } );
	add( "reread", runReread, "only the pointer watched, so a preset rewritten alone is missed", []( Perturb& p ) { p.readerHooks = kHookWatchPointerOnly; } );
	add( "charge", runCharge, "E = 1 - e^(-dt/RC): the voltage, not the energy", []( Perturb& p ) { p.rigHooks = kHookEnergyUnsquared; } );
	add( "charge", runCharge, "the bank charging through 2 RC", []( Perturb& p ) { p.rigHooks = kHookRechargeDoubled; } );
	add( "charge", runCharge, "a press's flash at the end of its frame", []( Perturb& p ) { p.rigHooks = kHookStepAtFrameEnd; } );
	add( "chase", runChase, "a beat's step at the start of its frame, not where the beat fell", []( Perturb& p ) { p.rigHooks = kHookBeatAtFrameStart; } );
	add( "chase", runChase, "step k lights what step k - 1 should", []( Perturb& p ) { p.rigHooks = kHookPatternLate; } );
	add( "inside", runInside, "the head's whole quad lit: its bounding box", []( Perturb& p ) { p.hooks = shaders::kHookQuadUnclipped; } );
	add( "energy", runEnergy, "a frame sees the light at its end times its length, not the integral", []( Perturb& p ) { p.rigHooks = kHookSampledLight; } );
	add( "audio", runAudio, "the onset detector's first frame measured against zeroes (deaf for the first kicks)", []( Perturb& p ) { p.unprimed = true; } );
	add( "audio", runAudio, "an onset's flash at the end of the frame that heard it", []( Perturb& p ) { p.rigHooks = kHookStepAtFrameEnd; } );
	add( "alpha", runAlpha, "the clip's alpha passed through under Fill and Outline", []( Perturb& p ) { p.hooks = shaders::kHookClipAlpha; } );
	add( "resize", runResize, "a new raster empties every bank", []( Perturb& p ) { p.hooks = kHookResizeForgets; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
