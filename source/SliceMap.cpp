#include "SliceMap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <windows.h>
#	include <shlobj.h>
#	include <sys/stat.h>
#	include <sys/types.h>
#else
#	include <pwd.h>
#	include <sys/stat.h>
#	include <unistd.h>
#endif

namespace xenon
{
namespace
{
//---------------------------------------------------------------------------
// A small XML reader: elements, attributes, text, the five entities and
// numeric references, comments, the prolog and CDATA. Enough for Resolume's
// own files, which are plain machine-written XML; nothing here validates.
//---------------------------------------------------------------------------
struct Element
{
	std::string name;
	std::vector< std::pair< std::string, std::string > > attrs;
	std::vector< Element > children;
	std::string text;

	const std::string* attr( const char* key ) const
	{
		for( const auto& a : attrs )
			if( a.first == key )
				return &a.second;
		return nullptr;
	}
	const Element* child( const char* tag ) const
	{
		for( const Element& c : children )
			if( c.name == tag )
				return &c;
		return nullptr;
	}
	/// The child `<tag name="key">`.
	const Element* childNamed( const char* tag, const char* key ) const
	{
		for( const Element& c : children )
		{
			const std::string* n = c.attr( "name" );
			if( c.name == tag && n != nullptr && *n == key )
				return &c;
		}
		return nullptr;
	}
};

void appendUtf8( std::string& out, unsigned long cp )
{
	if( cp < 0x80 )
		out += char( cp );
	else if( cp < 0x800 )
		out += char( 0xC0 | ( cp >> 6 ) ), out += char( 0x80 | ( cp & 0x3F ) );
	else if( cp < 0x10000 )
		out += char( 0xE0 | ( cp >> 12 ) ), out += char( 0x80 | ( ( cp >> 6 ) & 0x3F ) ), out += char( 0x80 | ( cp & 0x3F ) );
	else if( cp < 0x110000 )
		out += char( 0xF0 | ( cp >> 18 ) ), out += char( 0x80 | ( ( cp >> 12 ) & 0x3F ) ),
			out += char( 0x80 | ( ( cp >> 6 ) & 0x3F ) ), out += char( 0x80 | ( cp & 0x3F ) );
}

std::string unescape( const char* s, size_t n )
{
	std::string out;
	out.reserve( n );
	for( size_t i = 0; i < n; ++i )
	{
		if( s[ i ] != '&' )
		{
			out += s[ i ];
			continue;
		}
		size_t end = i + 1;
		while( end < n && end - i < 12 && s[ end ] != ';' )
			++end;
		if( end >= n || s[ end ] != ';' )
		{
			out += '&';
			continue;
		}
		const std::string ent( s + i + 1, end - i - 1 );
		if( ent == "amp" )
			out += '&';
		else if( ent == "lt" )
			out += '<';
		else if( ent == "gt" )
			out += '>';
		else if( ent == "quot" )
			out += '"';
		else if( ent == "apos" )
			out += '\'';
		else if( ent.size() > 1 && ent[ 0 ] == '#' )
		{
			const bool hex       = ent[ 1 ] == 'x' || ent[ 1 ] == 'X';
			const unsigned long cp = std::strtoul( ent.c_str() + ( hex ? 2 : 1 ), nullptr, hex ? 16 : 10 );
			appendUtf8( out, cp );
		}
		else
		{
			out.append( s + i, end - i + 1 );
		}
		i = end;
	}
	return out;
}

class XmlReader
{
public:
	explicit XmlReader( const std::string& text ) : s( text ), n( text.size() )
	{
	}

	bool Document( Element& root )
	{
		skipMisc();
		if( i >= n || s[ i ] != '<' )
			return fail( "no root element" );
		if( !element( root, 0 ) )
			return false;
		return true;
	}

	std::string error;

private:
	bool fail( const char* why )
	{
		if( error.empty() )
			error = std::string( why ) + " at byte " + std::to_string( i );
		return false;
	}
	bool startsWith( const char* lit ) const
	{
		const size_t len = std::strlen( lit );
		return i + len <= n && s.compare( i, len, lit ) == 0;
	}
	void skipSpace()
	{
		while( i < n && ( s[ i ] == ' ' || s[ i ] == '\t' || s[ i ] == '\r' || s[ i ] == '\n' ) )
			++i;
	}
	bool skipPast( const char* lit )
	{
		const size_t at = s.find( lit, i );
		if( at == std::string::npos )
		{
			i = n;
			return false;
		}
		i = at + std::strlen( lit );
		return true;
	}
	/// The prolog, comments, doctype and processing instructions between elements.
	void skipMisc()
	{
		for( ;; )
		{
			skipSpace();
			if( startsWith( "<?" ) )
				skipPast( "?>" );
			else if( startsWith( "<!--" ) )
				skipPast( "-->" );
			else if( startsWith( "<!DOCTYPE" ) )
				skipPast( ">" );
			else
				return;
		}
	}
	static bool nameChar( char c )
	{
		return !( c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>' || c == '=' || c == '<' );
	}
	std::string name()
	{
		const size_t start = i;
		while( i < n && nameChar( s[ i ] ) )
			++i;
		return s.substr( start, i - start );
	}
	bool element( Element& e, int depth )
	{
		if( depth > 200 )
			return fail( "nesting too deep" );
		++i;//'<'
		e.name = name();
		if( e.name.empty() )
			return fail( "empty element name" );
		for( ;; )
		{
			skipSpace();
			if( i >= n )
				return fail( "unterminated start tag" );
			if( s[ i ] == '/' )
			{
				if( i + 1 < n && s[ i + 1 ] == '>' )
				{
					i += 2;
					return true;
				}
				return fail( "stray '/' in a tag" );
			}
			if( s[ i ] == '>' )
			{
				++i;
				break;
			}
			std::string key = name();
			if( key.empty() )
				return fail( "bad attribute" );
			skipSpace();
			if( i >= n || s[ i ] != '=' )
				return fail( "attribute without '='" );
			++i;
			skipSpace();
			if( i >= n || ( s[ i ] != '"' && s[ i ] != '\'' ) )
				return fail( "unquoted attribute" );
			const char quote = s[ i++ ];
			const size_t end = s.find( quote, i );
			if( end == std::string::npos )
				return fail( "unterminated attribute" );
			e.attrs.emplace_back( std::move( key ), unescape( s.data() + i, end - i ) );
			i = end + 1;
		}
		//Content.
		for( ;; )
		{
			const size_t lt = s.find( '<', i );
			if( lt == std::string::npos )
				return fail( "unterminated element" );
			if( lt > i )
				e.text += unescape( s.data() + i, lt - i );
			i = lt;
			if( startsWith( "</" ) )
			{
				i += 2;
				const std::string closing = name();
				skipSpace();
				if( closing != e.name || i >= n || s[ i ] != '>' )
					return fail( "mismatched end tag" );
				++i;
				return true;
			}
			if( startsWith( "<!--" ) )
			{
				if( !skipPast( "-->" ) )
					return fail( "unterminated comment" );
				continue;
			}
			if( startsWith( "<![CDATA[" ) )
			{
				const size_t start = i + 9;
				const size_t end   = s.find( "]]>", start );
				if( end == std::string::npos )
					return fail( "unterminated CDATA" );
				e.text.append( s, start, end - start );
				i = end + 3;
				continue;
			}
			if( startsWith( "<?" ) )
			{
				skipPast( "?>" );
				continue;
			}
			e.children.emplace_back();
			if( !element( e.children.back(), depth + 1 ) )
				return false;
		}
	}

	const std::string& s;
	size_t n;
	size_t i = 0;
};

//---------------------------------------------------------------------------
// Reading the preset.
//---------------------------------------------------------------------------
double number( const std::string* text, double fallback )
{
	if( text == nullptr || text->empty() )
		return fallback;
	char* end      = nullptr;
	const double v = std::strtod( text->c_str(), &end );
	return ( end != text->c_str() && std::isfinite( v ) ) ? v : fallback;
}

/// `<Params name=group><Param name=key value=…/>` → value, or null.
const std::string* paramValue( const Element& owner, const char* group, const char* key )
{
	const Element* params = group != nullptr ? owner.childNamed( "Params", group ) : owner.child( "Params" );
	if( params == nullptr )
		return nullptr;
	const Element* p = params->childNamed( "Param", key );
	return p != nullptr ? p->attr( "value" ) : nullptr;
}

bool isOff( const std::string* v )
{
	return v != nullptr && ( *v == "0" || *v == "false" );
}

std::vector< Point > vertices( const Element* list )
{
	std::vector< Point > out;
	if( list == nullptr )
		return out;
	for( const Element& v : list->children )
		if( v.name == "v" )
			out.push_back( { number( v.attr( "x" ), 0.0 ), number( v.attr( "y" ), 0.0 ) } );
	return out;
}

Point bezier( const Point& a, const Point& b, const Point& c, const Point& d, double t )
{
	const double u = 1.0 - t;
	const double w0 = u * u * u, w1 = 3.0 * u * u * t, w2 = 3.0 * u * t * t, w3 = t * t * t;
	return { w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x, w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y };
}

/// A `<Contour>`/`<InputContour>`: its points and its segment letters, one a
/// segment. L is a line from its one point; C a cubic Bezier from its start
/// through two controls to the next segment's start. A count that does not
/// add up falls back to the raw points.
std::vector< Point > flattenContour( const Element* contour )
{
	std::vector< Point > out;
	if( contour == nullptr )
		return out;
	const std::vector< Point > pts = vertices( contour->child( "points" ) );
	std::string segs;
	if( const Element* s = contour->child( "segments" ) )
		for( char c : s->text )
			if( c != ' ' && c != '\t' && c != '\r' && c != '\n' )
				segs += c;
	const size_t n = pts.size();
	if( segs.empty() || n == 0 )
		return pts;
	size_t idx = 0;
	for( char c : segs )
	{
		if( idx >= n )
			return pts;
		if( c == 'C' || c == 'c' )
		{
			if( idx + 2 >= n )
				return pts;
			const Point& a = pts[ idx ];
			const Point& b = pts[ idx + 1 ];
			const Point& d = pts[ idx + 2 ];
			const Point& e = pts[ ( idx + 3 ) % n ];
			out.push_back( a );
			for( int k = 1; k < kBezierSteps; ++k )
				out.push_back( bezier( a, b, d, e, double( k ) / kBezierSteps ) );
			idx += 3;
		}
		else
		{
			out.push_back( pts[ idx ] );
			idx += 1;
		}
	}
	return idx == n ? out : pts;
}

/// Drop a closing point that repeats the first, and consecutive repeats.
void tidy( std::vector< Point >& p )
{
	std::vector< Point > out;
	for( const Point& q : p )
		if( out.empty() || std::fabs( q.x - out.back().x ) > 1e-9 || std::fabs( q.y - out.back().y ) > 1e-9 )
			out.push_back( q );
	while( out.size() > 1 && std::fabs( out.front().x - out.back().x ) <= 1e-9 && std::fabs( out.front().y - out.back().y ) <= 1e-9 )
		out.pop_back();
	p.swap( out );
}

bool sameRegion( const std::vector< Point >& a, const std::vector< Point >& b )
{
	if( a.size() != b.size() )
		return false;
	for( size_t i = 0; i < a.size(); ++i )
		if( std::fabs( a[ i ].x - b[ i ].x ) > kSameRegion || std::fabs( a[ i ].y - b[ i ].y ) > kSameRegion )
			return false;
	return true;
}

/// A head as read, in composition pixels, before it is normalised.
struct Raw
{
	Head head;
	std::vector< Point > outline;
	std::vector< Point > mask;
};

void finishHead( Head& h )
{
	h.minX = h.minY = 1e300;
	h.maxX = h.maxY = -1e300;
	for( const Point& p : h.outline )
	{
		h.minX = std::min( h.minX, p.x ), h.maxX = std::max( h.maxX, p.x );
		h.minY = std::min( h.minY, p.y ), h.maxY = std::max( h.maxY, p.y );
	}
	//Area centroid; the vertex mean if it has no area.
	double a = 0.0, cx = 0.0, cy = 0.0;
	const size_t n = h.outline.size();
	for( size_t i = 0; i < n; ++i )
	{
		const Point& p = h.outline[ i ];
		const Point& q = h.outline[ ( i + 1 ) % n ];
		const double c = p.x * q.y - q.x * p.y;
		a += c, cx += ( p.x + q.x ) * c, cy += ( p.y + q.y ) * c;
	}
	if( std::fabs( a ) > 1e-15 )
		h.centroid = { cx / ( 3.0 * a ), cy / ( 3.0 * a ) };
	else if( n > 0 )
	{
		double sx = 0.0, sy = 0.0;
		for( const Point& p : h.outline )
			sx += p.x, sy += p.y;
		h.centroid = { sx / n, sy / n };
	}
}
} // namespace

double PolygonArea( const std::vector< Point >& p )
{
	double a       = 0.0;
	const size_t n = p.size();
	for( size_t i = 0; i < n; ++i )
	{
		const Point& u = p[ i ];
		const Point& v = p[ ( i + 1 ) % n ];
		a += u.x * v.y - v.x * u.y;
	}
	return 0.5 * a;
}

bool InsidePolygon( const std::vector< Point >& p, double x, double y )
{
	//The same test, in the same form and the same edge order, as the light
	//pass's `scanEdges`: edge i runs from point i to point i + 1, it counts
	//when it straddles y with the half-open rule (one end strictly above),
	//and the crossing is to the right of x.
	bool inside    = false;
	const size_t n = p.size();
	for( size_t i = 0; i < n; ++i )
	{
		const Point& a = p[ i ];
		const Point& b = p[ ( i + 1 ) % n ];
		if( ( a.y > y ) != ( b.y > y ) )
		{
			const double t = ( y - a.y ) / ( b.y - a.y );
			if( x < a.x + t * ( b.x - a.x ) )
				inside = !inside;
		}
	}
	return inside;
}

double DistanceToEdges( const std::vector< Point >& p, double x, double y )
{
	double best    = 1e300;
	const size_t n = p.size();
	for( size_t i = 0; i < n; ++i )
	{
		const Point& a  = p[ i ];
		const Point& b  = p[ ( i + 1 ) % n ];
		const double dx = b.x - a.x, dy = b.y - a.y;
		const double len2 = dx * dx + dy * dy;
		double t = len2 > 0.0 ? ( ( x - a.x ) * dx + ( y - a.y ) * dy ) / len2 : 0.0;
		t        = std::clamp( t, 0.0, 1.0 );
		const double ex = a.x + t * dx - x, ey = a.y + t * dy - y;
		best = std::min( best, ex * ex + ey * ey );
	}
	return std::sqrt( best );
}

SliceMap ParsePreset( const std::string& xml, const std::string& source, int hooks )
{
	SliceMap map;
	map.source = source;
	Element root;
	XmlReader reader( xml );
	if( !reader.Document( root ) )
	{
		map.error = "not XML: " + reader.error;
		return map;
	}
	const Element* setup = root.name == "ScreenSetup" ? &root : root.child( "ScreenSetup" );
	if( setup == nullptr )
	{
		map.error = "no <ScreenSetup>";
		return map;
	}
	const Element* size = setup->child( "CurrentCompositionTextureSize" );
	map.compWidth       = size != nullptr ? number( size->attr( "width" ), 0.0 ) : 0.0;
	map.compHeight      = size != nullptr ? number( size->attr( "height" ), 0.0 ) : 0.0;
	if( !( map.compWidth > 0.0 ) || !( map.compHeight > 0.0 ) )
	{
		map.error = "no <CurrentCompositionTextureSize>";
		return map;
	}
	const Element* screens = setup->child( "screens" );
	if( screens == nullptr )
	{
		map.error = "no <screens>";
		return map;
	}

	std::vector< Raw > kept;
	int screenIndex = -1;
	for( const Element& screen : screens->children )
	{
		if( screen.name != "Screen" )
			continue;
		++screenIndex;
		const bool screenOn  = !isOff( paramValue( screen, "Params", "Enabled" ) );
		const Element* layers = screen.child( "layers" );
		if( layers == nullptr )
			continue;
		std::vector< Raw > these;
		for( const Element& layer : layers->children )
		{
			const bool isSlice   = layer.name == "Slice";
			const bool isPolygon = layer.name == "Polygon";
			if( !isSlice && !isPolygon )
				continue;
			++map.slicesSeen;
			if( !screenOn || isOff( paramValue( layer, "Common", "Enabled" ) ) )
			{
				++map.skippedDisabled;
				continue;
			}
			Raw r;
			const std::string* nm = paramValue( layer, "Common", "Name" );
			r.head.name           = nm != nullptr ? *nm : layer.name;
			r.head.screen         = screenIndex;
			r.head.polygon        = isPolygon;
			if( isPolygon )
				r.outline = flattenContour( layer.child( "InputContour" ) );
			if( r.outline.size() < 3 )
				r.outline = vertices( layer.child( "InputRect" ) );
			if( ( hooks & kHookBoundingBox ) != 0 && !r.outline.empty() )
			{
				double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
				for( const Point& p : r.outline )
					x0 = std::min( x0, p.x ), x1 = std::max( x1, p.x ), y0 = std::min( y0, p.y ), y1 = std::max( y1, p.y );
				r.outline = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
			}
			tidy( r.outline );
			if( const Element* m = layer.child( "SliceMask" ) )
			{
				const Element* contour = nullptr;
				if( const Element* so = m->child( "ShapeObject" ) )
					if( const Element* sh = so->child( "Shape" ) )
						contour = sh->child( "Contour" );
				std::vector< Point > mk = flattenContour( contour );
				tidy( mk );
				if( !isOff( paramValue( *m, "Input Mask", "Enabled" ) ) && mk.size() >= 3 && std::fabs( PolygonArea( mk ) ) > 1e-6 )
				{
					r.mask                   = mk;
					r.head.maskShowsInside   = !isOff( paramValue( *m, "Input Mask", "Invert" ) );
				}
			}
			if( r.outline.size() < 3 || !( std::fabs( PolygonArea( r.outline ) ) > 1e-6 ) )
			{
				++map.skippedDegenerate;
				continue;
			}
			these.push_back( std::move( r ) );
		}
		//The file lists a screen's slices bottom up; Arena's panel shows them
		//top down. The chase walks the panel.
		if( ( hooks & kHookFileOrder ) == 0 )
			std::reverse( these.begin(), these.end() );
		for( Raw& r : these )
		{
			bool duplicate = false;
			if( ( hooks & kHookNoCollapse ) == 0 )
				for( const Raw& k : kept )
					if( k.head.maskShowsInside == r.head.maskShowsInside && sameRegion( k.outline, r.outline ) && sameRegion( k.mask, r.mask ) )
					{
						duplicate = true;
						break;
					}
			if( duplicate )
			{
				++map.collapsed;
				continue;
			}
			const int edges = int( r.outline.size() + r.mask.size() );
			if( int( kept.size() ) >= kMaxHeads || map.edges + edges > kMaxEdges )
			{
				++map.dropped;
				continue;
			}
			map.edges += edges;
			if( !r.mask.empty() )
				++map.masks;
			kept.push_back( std::move( r ) );
		}
	}

	for( Raw& r : kept )
	{
		Head h = std::move( r.head );
		for( const Point& p : r.outline )
			h.outline.push_back( { p.x / map.compWidth, p.y / map.compHeight } );
		for( const Point& p : r.mask )
			h.mask.push_back( { p.x / map.compWidth, p.y / map.compHeight } );
		finishHead( h );
		h.listed = int( map.heads.size() );
		map.heads.push_back( std::move( h ) );
	}
	map.ok = !map.heads.empty();
	if( !map.ok && map.error.empty() )
		map.error = "no enabled slices";
	return map;
}

SliceMap ReadPreset( const std::string& path, int hooks )
{
	std::string text;
	if( !ReadWholeFile( path, text ) )
	{
		SliceMap map;
		map.source = path;
		map.error  = "cannot read " + path;
		return map;
	}
	return ParsePreset( text, path, hooks );
}

SliceMap GridMap( int rows, int columns )
{
	SliceMap map;
	rows       = std::clamp( rows, 1, 32 );
	columns    = std::clamp( columns, 1, 32 );
	map.source = "grid " + std::to_string( rows ) + " x " + std::to_string( columns );
	map.compWidth = map.compHeight = 1.0;
	for( int r = 0; r < rows; ++r )
		for( int c = 0; c < columns; ++c )
		{
			Head h;
			const double x0 = double( c ) / columns, x1 = double( c + 1 ) / columns;
			const double y0 = double( r ) / rows, y1 = double( r + 1 ) / rows;
			h.name    = "Cell " + std::to_string( r + 1 ) + "," + std::to_string( c + 1 );
			h.outline = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
			finishHead( h );
			h.listed = int( map.heads.size() );
			map.heads.push_back( std::move( h ) );
			map.edges += 4;
		}
	map.slicesSeen = rows * columns;
	map.ok         = true;
	return map;
}

std::vector< int > OrderHeads( const SliceMap& map, HeadOrder order )
{
	std::vector< int > idx( map.heads.size() );
	for( size_t i = 0; i < idx.size(); ++i )
		idx[ i ] = int( i );
	if( order == HeadOrder::AsListed )
		return idx;
	//Quantised keys, so a column of grid cells sorts as one column whatever
	//the last bit of each centroid says, and the order is a strict weak one.
	auto key = []( double v ) { return static_cast< long long >( std::llround( v * 1e6 ) ); };
	std::stable_sort( idx.begin(), idx.end(), [ & ]( int a, int b ) {
		const Head& p = map.heads[ size_t( a ) ];
		const Head& q = map.heads[ size_t( b ) ];
		const long long px = key( p.centroid.x ), py = key( p.centroid.y );
		const long long qx = key( q.centroid.x ), qy = key( q.centroid.y );
		if( order == HeadOrder::LeftToRight )
			return px != qx ? px < qx : py < qy;
		return py != qy ? py < qy : px < qx;
	} );
	return idx;
}

//---------------------------------------------------------------------------
// Files.
//---------------------------------------------------------------------------
#ifdef _WIN32
namespace
{
std::wstring widen( const std::string& s )
{
	if( s.empty() )
		return {};
	const int len = MultiByteToWideChar( CP_UTF8, 0, s.data(), int( s.size() ), nullptr, 0 );
	std::wstring w( size_t( len ), L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, s.data(), int( s.size() ), w.data(), len );
	return w;
}
std::string narrow( const wchar_t* w )
{
	const int len = WideCharToMultiByte( CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr );
	if( len <= 1 )
		return {};
	std::string s( size_t( len - 1 ), '\0' );
	WideCharToMultiByte( CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr );
	return s;
}
} // namespace
#endif

bool ReadWholeFile( const std::string& path, std::string& out )
{
	if( path.empty() )
		return false;
#ifdef _WIN32
	FILE* f = _wfopen( widen( path ).c_str(), L"rb" );
#else
	FILE* f = std::fopen( path.c_str(), "rb" );
#endif
	if( f == nullptr )
		return false;
	out.clear();
	char buffer[ 65536 ];
	size_t got = 0;
	while( ( got = std::fread( buffer, 1, sizeof( buffer ), f ) ) > 0 )
	{
		out.append( buffer, got );
		if( out.size() > ( size_t( 64 ) << 20 ) )//no preset is 64 MB
		{
			std::fclose( f );
			return false;
		}
	}
	std::fclose( f );
	return true;
}

bool StatFile( const std::string& path, int64_t& mtime, int64_t& size )
{
	if( path.empty() )
		return false;
#ifdef _WIN32
	struct _stat64 st;
	if( _wstat64( widen( path ).c_str(), &st ) != 0 )
		return false;
	mtime = int64_t( st.st_mtime ) * 1000000000LL;
	size  = int64_t( st.st_size );
#else
	struct stat st;
	if( ::stat( path.c_str(), &st ) != 0 )
		return false;
#	ifdef __APPLE__
	mtime = int64_t( st.st_mtimespec.tv_sec ) * 1000000000LL + int64_t( st.st_mtimespec.tv_nsec );
#	else
	mtime = int64_t( st.st_mtim.tv_sec ) * 1000000000LL + int64_t( st.st_mtim.tv_nsec );
#	endif
	size = int64_t( st.st_size );
#endif
	return true;
}

std::string DocumentsFolder()
{
#ifdef _WIN32
	PWSTR folder = nullptr;
	std::string out;
	if( SUCCEEDED( SHGetKnownFolderPath( FOLDERID_Documents, 0, nullptr, &folder ) ) && folder != nullptr )
		out = narrow( folder );
	if( folder != nullptr )
		CoTaskMemFree( folder );
	return out;
#else
	const char* home = std::getenv( "HOME" );
	std::string base = home != nullptr ? home : "";
	if( base.empty() )
		if( const passwd* pw = getpwuid( getuid() ) )
			if( pw->pw_dir != nullptr )
				base = pw->pw_dir;
	return base.empty() ? std::string() : base + "/Documents";
#endif
}

std::string ActivePresetPath( const std::string& documents, std::string* pointer )
{
	if( documents.empty() )
		return {};
	for( const char* app : { "Resolume Arena", "Resolume Avenue" } )
	{
		const std::string root = documents + "/" + app;
		const std::string ptr  = root + "/Preferences/AdvancedOutput.xml";
		std::string text;
		if( !ReadWholeFile( ptr, text ) )
			continue;
		if( pointer != nullptr )
			*pointer = ptr;
		Element doc;
		XmlReader reader( text );
		if( !reader.Document( doc ) )
			continue;
		const Element* setup = doc.name == "ScreenSetup" ? &doc : doc.child( "ScreenSetup" );
		const std::string* name = setup != nullptr ? setup->attr( "presetFile" ) : nullptr;
		if( name == nullptr || name->empty() )
			continue;
		std::string file = *name;
		const bool absolute = file[ 0 ] == '/' || file[ 0 ] == '\\' || ( file.size() > 2 && file[ 1 ] == ':' );
		const bool hasXml   = file.size() > 4 && ( file.compare( file.size() - 4, 4, ".xml" ) == 0 || file.compare( file.size() - 4, 4, ".XML" ) == 0 );
		if( !hasXml )
			file += ".xml";
		return absolute ? file : root + "/Presets/Advanced Output/" + file;
	}
	return {};
}

//---------------------------------------------------------------------------
// The live source.
//---------------------------------------------------------------------------
MapSource::Stamp MapSource::stampOf( const std::string& path )
{
	Stamp s;
	s.path = path;
	if( !StatFile( path, s.mtime, s.size ) )
		s.mtime = s.size = -1;
	return s;
}

bool SameHeads( const SliceMap& a, const SliceMap& b )
{
	if( a.heads.size() != b.heads.size() )
		return false;
	auto samePoints = []( const std::vector< Point >& p, const std::vector< Point >& q ) {
		if( p.size() != q.size() )
			return false;
		for( size_t i = 0; i < p.size(); ++i )
			if( p[ i ].x != q[ i ].x || p[ i ].y != q[ i ].y )
				return false;
		return true;
	};
	for( size_t h = 0; h < a.heads.size(); ++h )
	{
		const Head& x = a.heads[ h ];
		const Head& y = b.heads[ h ];
		if( x.name != y.name || x.maskShowsInside != y.maskShowsInside || !samePoints( x.outline, y.outline ) || !samePoints( x.mask, y.mask ) )
			return false;
	}
	return true;
}

bool MapSource::Poll( double now, const Settings& settings )
{
	const bool settingsChanged = forced || settings.useMap != last.useMap || settings.presetFile != last.presetFile ||
	                             settings.rows != last.rows || settings.columns != last.columns;
	//A clock that ran backwards (a clip restarted) polls at once.
	if( !settingsChanged && now >= lastPoll && now - lastPoll < kPollInterval )
		return false;
	lastPoll = now;

	if( !settings.useMap )
	{
		if( !settingsChanged )
			return false;
		current    = GridMap( settings.rows, settings.columns );
		fromPreset = false;
		last       = settings;
		forced     = false;
		pointerStamp = presetStamp = Stamp();
		return true;
	}

	std::string pointerPath, preset;
	if( !settings.presetFile.empty() )
		preset = settings.presetFile;
	else
		preset = ActivePresetPath( documentsOverride.empty() ? DocumentsFolder() : documentsOverride, &pointerPath );
	const Stamp ps = stampOf( pointerPath );
	//The negative control for --reread: only the pointer watched.
	const Stamp ss = ( readerHooks & kHookWatchPointerOnly ) != 0 && !settingsChanged ? presetStamp : stampOf( preset );
	if( !settingsChanged && ps == pointerStamp && ss == presetStamp )
		return false;
	pointerStamp = ps;
	presetStamp  = ss;
	last         = settings;
	forced       = false;

	SliceMap m;
	if( preset.empty() )
		m.error = pointerPath.empty() ? "no AdvancedOutput.xml" : "no presetFile in " + pointerPath;
	else if( ss.mtime < 0 )
		m.error = "no file " + preset;
	else
	{
		m = ReadPreset( preset, readerHooks );
		++reads;
	}
	if( m.ok )
	{
		//Arena rewrites both files on every Save & Close, changed or not: a
		//save that leaves the regions as they were is not a new rig, and the
		//chase and the banks carry on.
		const bool same = fromPreset && SameHeads( current, m );
		current         = std::move( m );
		fromPreset      = true;
		return !same;
	}
	else
	{
		SliceMap g = GridMap( settings.rows, settings.columns );
		g.error    = m.error;
		g.source   = g.source + " (no map: " + m.error + ")";
		current    = std::move( g );
		fromPreset = false;
	}
	return true;
}

} // namespace xenon
