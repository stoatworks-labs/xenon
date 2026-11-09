// slicedump: print the strobe heads xenon reads from an Advanced Output preset.
//
//   slicedump <preset.xml> [--order listed|across|down] [--points]
//   slicedump --active            (follow Arena's AdvancedOutput.xml pointer)
//
// No GL: the same SliceMap code the plugin runs, so what this prints is what
// the plugin chases.
#include "SliceMap.h"

#include <cstdio>
#include <cstring>
#include <string>

int main( int argc, char** argv )
{
	std::string path;
	bool points = false;
	xenon::HeadOrder order = xenon::HeadOrder::AsListed;
	for( int i = 1; i < argc; ++i )
	{
		if( std::strcmp( argv[ i ], "--points" ) == 0 )
			points = true;
		else if( std::strcmp( argv[ i ], "--active" ) == 0 )
		{
			std::string pointer;
			path = xenon::ActivePresetPath( xenon::DocumentsFolder(), &pointer );
			std::printf( "pointer: %s\npreset:  %s\n", pointer.c_str(), path.c_str() );
		}
		else if( std::strcmp( argv[ i ], "--order" ) == 0 && i + 1 < argc )
		{
			const std::string o = argv[ ++i ];
			order = o == "across" ? xenon::HeadOrder::LeftToRight : o == "down" ? xenon::HeadOrder::TopToBottom : xenon::HeadOrder::AsListed;
		}
		else
			path = argv[ i ];
	}
	if( path.empty() )
	{
		std::fprintf( stderr, "usage: slicedump <preset.xml> [--order listed|across|down] [--points] | --active\n" );
		return 2;
	}
	const xenon::SliceMap map = xenon::ReadPreset( path );
	std::printf( "%s\n  ok %d  composition %.0f x %.0f  heads %zu  seen %d  disabled %d  collapsed %d  degenerate %d  masks %d  dropped %d  edges %d\n",
	             map.source.c_str(), map.ok ? 1 : 0, map.compWidth, map.compHeight, map.heads.size(), map.slicesSeen,
	             map.skippedDisabled, map.collapsed, map.skippedDegenerate, map.masks, map.dropped, map.edges );
	if( !map.ok )
		std::printf( "  error: %s\n", map.error.c_str() );
	int step = 0;
	for( int i : xenon::OrderHeads( map, order ) )
	{
		const xenon::Head& h = map.heads[ size_t( i ) ];
		std::printf( "  %2d  %-24s screen %d  %s %3zu pts  centroid (%.6f, %.6f)  bounds [%.6f %.6f]-[%.6f %.6f]%s\n", step++, h.name.c_str(),
		             h.screen, h.polygon ? "polygon" : "slice  ", h.outline.size(), h.centroid.x, h.centroid.y, h.minX, h.minY, h.maxX, h.maxY,
		             h.mask.empty() ? "" : ( h.maskShowsInside ? "  mask(inside)" : "  mask(outside)" ) );
		if( points )
		{
			for( const xenon::Point& p : h.outline )
				std::printf( "        (%.9f, %.9f)\n", p.x, p.y );
			if( !h.mask.empty() )
			{
				std::printf( "      mask %zu pts\n", h.mask.size() );
				for( const xenon::Point& p : h.mask )
					std::printf( "        (%.9f, %.9f)\n", p.x, p.y );
			}
		}
	}
	return map.ok ? 0 : 1;
}
