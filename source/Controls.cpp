#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace xenon
{
namespace
{
double clamp01( float v )
{
	return std::isfinite( v ) ? std::clamp( static_cast< double >( v ), 0.0, 1.0 ) : 0.0;
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}
double linear( float v, double low, double high )
{
	return low + ( high - low ) * clamp01( v );
}
float inverseLinear( double value, double low, double high )
{
	return static_cast< float >( std::clamp( ( value - low ) / ( high - low ), 0.0, 1.0 ) );
}

constexpr double kRechargeLow = 5e-3, kRechargeHigh = 5.0;
constexpr double kDecayLow = 0.2e-3, kDecayHigh = 0.5;
constexpr double kIntensityMost = 2.0;
constexpr double kWidthLow = 1.0, kWidthHigh = 100.0;

const char* const kPatternNames[] = { "Forward", "Reverse", "Bounce", "Build", "Random", "All" };
const char* const kTriggerNames[] = { "Bar", "Beat", "Half Beat", "Quarter Beat", "Eighth Beat", "Onset", "Fire Only" };
const char* const kLookNames[]    = { "Fill", "Outline", "Gate" };
const char* const kSlicesNames[]  = { "Map", "Grid" };
const char* const kOrderNames[]   = { "As Listed", "Left to Right", "Top to Bottom" };

ControlInfo standard( unsigned int id, const char* name, const char* group, float value, Kind kind = Kind::Standard )
{
	return { id, name, group, kind, value, nullptr, 0, 0.0f, 1.0f };
}
ControlInfo option( unsigned int id, const char* name, const char* group, float value, const char* const* names, int count )
{
	return { id, name, group, Kind::Option, value, names, count, 0.0f, static_cast< float >( count - 1 ) };
}
ControlInfo event( unsigned int id, const char* name, const char* group )
{
	return { id, name, group, Kind::Event, 0.0f, nullptr, 0, 0.0f, 1.0f };
}
ControlInfo integer( unsigned int id, const char* name, const char* group, int value, int low, int high )
{
	return { id, name, group, Kind::Integer, static_cast< float >( value ), nullptr, 0, static_cast< float >( low ), static_cast< float >( high ) };
}
ControlInfo buffer( unsigned int id, const char* name, const char* group )
{
	return { id, name, group, Kind::Buffer, 0.0f, nullptr, 0, 0.0f, 1.0f };
}
ControlInfo file( unsigned int id, const char* name, const char* group )
{
	return { id, name, group, Kind::File, 0.0f, nullptr, 0, 0.0f, 1.0f };
}

//---------------------------------------------------------------------------
// The defaults. The active map's slices stepped through one a beat, each a
// white strobe with a quarter-second bank (full again long before its turn
// comes round on any map of more than one slice at 120 BPM) and a 2 ms tube,
// so each flash lands in one frame; flashing over the clip at full room light.
//---------------------------------------------------------------------------
const ControlInfo kTable[] = {
	option( PT_PATTERN, "Pattern", "Chase", 0.0f, kPatternNames, static_cast< int >( Pattern::Count ) ),
	option( PT_TRIGGER, "Trigger", "Chase", 1.0f, kTriggerNames, static_cast< int >( Trigger::Count ) ),
	event( PT_FIRE, "Fire", "Chase" ),
	buffer( PT_AUDIO, "Audio", "Chase" ),
	standard( PT_THRESHOLD, "Threshold", "Chase", 0.4f ),
	integer( PT_SEED, "Seed", "Chase", 1, 0, kMaxSeed ),
	standard( PT_RECHARGE, "Recharge", "Strobe", ParamFromRecharge( 0.25 ) ),
	standard( PT_DECAY, "Decay", "Strobe", ParamFromDecay( 2e-3 ) ),
	standard( PT_INTENSITY, "Intensity", "Strobe", ParamFromIntensity( 1.0 ) ),
	standard( PT_COLOUR_R, "Colour", "Strobe", 1.0f, Kind::Red ),
	standard( PT_COLOUR_G, "Colour_Green", "Strobe", 1.0f, Kind::Green ),
	standard( PT_COLOUR_B, "Colour_Blue", "Strobe", 1.0f, Kind::Blue ),
	option( PT_LOOK, "Look", "Look", 0.0f, kLookNames, static_cast< int >( Look::Count ) ),
	standard( PT_WIDTH, "Width", "Look", ParamFromWidth( 12.0 ) ),
	standard( PT_ROOM, "Room Light", "Look", 1.0f ),
	option( PT_SLICES, "Slices", "Slices", 0.0f, kSlicesNames, static_cast< int >( Slices::Count ) ),
	file( PT_PRESET_FILE, "Preset File", "Slices" ),
	option( PT_ORDER, "Order", "Slices", 0.0f, kOrderNames, 3 ),
	integer( PT_ROWS, "Rows", "Slices", 2, kMinGrid, kMaxGrid ),
	integer( PT_COLUMNS, "Columns", "Slices", 4, kMinGrid, kMaxGrid ),
};
static_assert( sizeof( kTable ) / sizeof( kTable[ 0 ] ) == PT_ABOUT_FIRST, "every control needs a row in the table" );
} // namespace

const ControlInfo& InfoOf( unsigned int id )
{
	return kTable[ std::min< unsigned int >( id, PT_ABOUT_FIRST - 1 ) ];
}

int OptionIndex( float value, int count )
{
	if( !std::isfinite( value ) )
		return 0;
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double BeatsPerStep( Trigger trigger )
{
	switch( trigger )
	{
	case Trigger::Bar: return 4.0;
	case Trigger::Beat: return 1.0;
	case Trigger::HalfBeat: return 0.5;
	case Trigger::QuarterBeat: return 0.25;
	case Trigger::EighthBeat: return 0.125;
	default: return 0.0;
	}
}

double RechargeFromParam( float value )
{
	return geometric( value, kRechargeLow, kRechargeHigh );
}
float ParamFromRecharge( double seconds )
{
	return inverseGeometric( seconds, kRechargeLow, kRechargeHigh );
}
double DecayFromParam( float value )
{
	return geometric( value, kDecayLow, kDecayHigh );
}
float ParamFromDecay( double seconds )
{
	return inverseGeometric( seconds, kDecayLow, kDecayHigh );
}
double IntensityFromParam( float value )
{
	return linear( value, 0.0, kIntensityMost );
}
float ParamFromIntensity( double intensity )
{
	return inverseLinear( intensity, 0.0, kIntensityMost );
}
double WidthFromParam( float value )
{
	return geometric( value, kWidthLow, kWidthHigh );
}
float ParamFromWidth( double pixels )
{
	return inverseGeometric( pixels, kWidthLow, kWidthHigh );
}
double RoomFromParam( float value )
{
	return clamp01( value );
}
double SensitivityFromThreshold( float value )
{
	return 1.0 - clamp01( value );
}
int IntegerFromParam( float value, int low, int high, int fallback )
{
	if( !std::isfinite( value ) )
		return fallback;
	return std::clamp( static_cast< int >( std::lround( value ) ), low, high );
}

} // namespace xenon
