#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
    The operator's slices, read from Resolume's saved Advanced Output preset.

    FFGL never tells an effect about Advanced Output, so this is the whole
    route: follow `Preferences/AdvancedOutput.xml`'s `presetFile` to the
    preset Arena saved, and read the shape of every slice in it. Plain C++,
    no GL, so `xntest --reader` checks it without a context.

    What the file looks like was read off the four presets on the Mac this was
    written on and off an Arena 7.28.1 save with a Polygon and two masks
    (2026-11-09; SPEC-xenon.md has the details):

      <XmlState><ScreenSetup>
        <CurrentCompositionTextureSize width height/>
        <screens><Screen> <Params>Enabled</Params> <layers>
          <Slice>    Common/Name, Common/Enabled,
                     <InputRect orientation=rad> four <v x y>, ALREADY rotated,
                     optional <SliceMask> (Enabled, Invert, a Contour)
          <Polygon>  <InputRect> is its bounding box only; the shape is
                     <InputContour><points/><segments>LLC…</segments>

    Coordinates in the file are composition pixels, y down. A Head holds its
    region normalised by the composition size, still y down (0,0 top left,
    1,1 bottom right); the plugin turns that into output pixels.
*/
namespace xenon
{
struct Point
{
	double x = 0.0;
	double y = 0.0;
};

/// One strobe head: a closed region of the composition.
struct Head
{
	std::string name;
	std::vector< Point > outline;///< the slice's own shape, normalised, y down
	std::vector< Point > mask;   ///< empty for none
	bool maskShowsInside = true; ///< Arena's Invert 1: the slice is seen inside the mask
	Point centroid;              ///< of the outline's vertices' polygon (area centroid)
	double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;///< outline's bounds
	int screen = 0;              ///< index of the screen it came from
	int listed = 0;              ///< position in Arena's panel order
	bool polygon = false;        ///< from a <Polygon>, not a <Slice>
};

/// What a preset read found, and what it left out.
struct SliceMap
{
	bool ok = false;           ///< false: nothing usable (the caller falls back to the grid)
	std::string source;        ///< the file read, or "grid R x C"
	std::string error;         ///< why not ok
	double compWidth  = 0.0;   ///< CurrentCompositionTextureSize
	double compHeight = 0.0;
	std::vector< Head > heads; ///< in Arena's panel order
	int slicesSeen      = 0;   ///< every <Slice> and <Polygon> met
	int skippedDisabled = 0;   ///< disabled slices, or slices of disabled screens
	int collapsed       = 0;   ///< identical regions folded into an earlier head
	int skippedDegenerate = 0; ///< fewer than three points, or no area
	int masks           = 0;   ///< enabled masks applied
	int dropped         = 0;   ///< over kMaxHeads or kMaxEdges
	int edges           = 0;   ///< outline + mask edges of the heads kept
};

/// The order the chase walks the heads in.
enum class HeadOrder : int
{
	AsListed    = 0,///< Arena's panel: screens in order, each screen's list top down
	LeftToRight = 1,///< by centroid x, then y
	TopToBottom = 2 ///< by centroid y, then x
};

constexpr int kMaxHeads = 256;
constexpr int kMaxEdges = 8192;
/// A cubic Bezier segment becomes this many straight edges.
constexpr int kBezierSteps = 16;
/// Two regions closer than this, point for point, in composition pixels, are one.
constexpr double kSameRegion = 0.01;

/// Hooks for the negative controls. A shipped reader passes 0.
enum ReaderHook : int
{
	kHookNoCollapse   = 1 << 0,///< keep every duplicate region
	kHookBoundingBox  = 1 << 1,///< a slice's quad replaced by its axis-aligned bounds
	kHookFileOrder    = 1 << 2,///< heads in the file's order, not the panel's
	kHookWatchPointerOnly = 1 << 3///< MapSource stats only the pointer, so a preset rewritten alone is missed
};

/// Parse one preset's text. `source` is only recorded.
SliceMap ParsePreset( const std::string& xml, const std::string& source, int hooks = 0 );

/// Read and parse a preset file.
SliceMap ReadPreset( const std::string& path, int hooks = 0 );

/// A rows x columns grid of rectangles, row-major from the top left.
SliceMap GridMap( int rows, int columns );

/// The heads' indices in the chase's order.
std::vector< int > OrderHeads( const SliceMap& map, HeadOrder order );

/// The user's Documents folder: $HOME/Documents on macOS, the Documents known
/// folder on Windows. Empty if it cannot be found.
std::string DocumentsFolder();

/// The preset Arena names as active, from `<documents>/Resolume Arena/
/// Preferences/AdvancedOutput.xml` (then Avenue's). `pointer` receives the
/// pointer file that named it. Empty if there is none.
std::string ActivePresetPath( const std::string& documents, std::string* pointer = nullptr );

/// The text of a file, or false.
bool ReadWholeFile( const std::string& path, std::string& out );

/// A file's modification time (ns) and size, or false if it does not exist.
bool StatFile( const std::string& path, int64_t& mtime, int64_t& size );

/// Signed area of a closed polygon (positive anticlockwise in a y-up frame).
double PolygonArea( const std::vector< Point >& p );

/// Two maps with the same heads, point for point (names, outlines, masks).
bool SameHeads( const SliceMap& a, const SliceMap& b );

/// Even-odd point in polygon, with the half-open rule the shader uses.
bool InsidePolygon( const std::vector< Point >& p, double x, double y );

/// Distance from a point to the nearest edge of a closed polygon.
double DistanceToEdges( const std::vector< Point >& p, double x, double y );

/**
    The live source of the plugin's map: the active preset (or the Preset
    File override) re-read whenever Arena saves it, or the grid.

    Arena writes the preset and its pointer only at Save & Close in the
    Advanced Output window (measured on 7.28.1, SPEC-xenon.md), so Poll stats
    both files every `kPollInterval` of the plugin's clock and re-parses when
    either changes. Called on the render thread; a stat is microseconds and a
    parse of a 60 KB preset about a millisecond, and only on a change.
*/
class MapSource
{
public:
	static constexpr double kPollInterval = 0.5;///< seconds of plugin time

	struct Settings
	{
		bool useMap = true;      ///< Slices: Map (else Grid)
		std::string presetFile;  ///< the override; empty follows Arena's pointer
		int rows = 2, columns = 4;
	};

	/// Bring the map up to date. Returns true when the heads changed: a
	/// re-read that finds the same regions returns false.
	bool Poll( double now, const Settings& settings );

	const SliceMap& Map() const
	{
		return current;
	}
	/// Files parsed since construction (the --reread check counts them).
	int Reads() const
	{
		return reads;
	}
	/// True when the map came from a preset, false for the grid.
	bool FromPreset() const
	{
		return fromPreset;
	}

	/// The harness points the reader at a scratch Documents tree.
	void SetDocumentsForTest( const std::string& folder )
	{
		documentsOverride = folder;
		forced            = true;
	}
	void SetReaderHooksForTest( int bits )
	{
		readerHooks = bits;
		forced      = true;
	}

private:
	struct Stamp
	{
		std::string path;
		int64_t mtime = -1, size = -1;
		bool operator==( const Stamp& o ) const
		{
			return path == o.path && mtime == o.mtime && size == o.size;
		}
	};
	static Stamp stampOf( const std::string& path );

	SliceMap current;
	Settings last;
	Stamp pointerStamp, presetStamp;
	double lastPoll = -1e300;
	bool forced     = true;
	bool fromPreset = false;
	int reads       = 0;
	int readerHooks = 0;
	std::string documentsOverride;
};

} // namespace xenon
