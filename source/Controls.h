#pragma once

#include "StoatworksAboutLinks.h"

/**
    The host's parameters, and what they mean in physical units.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control, each with its inverse where a default or the harness needs it.
    Seed, Rows and Columns are real integers (FF_TYPE_INTEGER is exempt from
    the clamp); options hold the element index; Fire is a press; Audio is
    Resolume's 64-bin FFT buffer; Preset File is a path (FF_TYPE_FILE, a text
    value with Arena's file picker); Colour is three FF_TYPE_RED/GREEN/BLUE
    parameters, which Arena shows as one colour picker.

    Units: seconds for Recharge (the bank's RC) and Decay (the tube's time
    constant), composition pixels for Width.
*/
namespace xenon
{
/// Every control, in the order Resolume shows them. The About block is
/// LAST, so the day a user guide adds a button no control moves.
enum ParamId : unsigned int
{
	// -- Chase: who fires, and when ---------------------------------------------
	PT_PATTERN = 0,
	PT_TRIGGER,
	PT_FIRE,
	PT_AUDIO,
	PT_THRESHOLD,
	PT_SEED,

	// -- Strobe: the heads ------------------------------------------------------
	PT_RECHARGE,
	PT_DECAY,
	PT_INTENSITY,
	PT_COLOUR_R,
	PT_COLOUR_G,
	PT_COLOUR_B,

	// -- Look: what the light does to the picture ---------------------------------
	PT_LOOK,
	PT_WIDTH,
	PT_ROOM,

	// -- Slices: where the heads are ------------------------------------------------
	PT_SLICES,
	PT_PRESET_FILE,
	PT_ORDER,
	PT_ROWS,
	PT_COLUMNS,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 1 + stoatworks::about::kButtonCount
};

enum class Kind
{
	Standard,///< 0..1, converted in Controls.cpp
	Integer, ///< a real integer with a real range
	Option,  ///< an element index
	Event,   ///< a press
	Buffer,  ///< the FFT
	File,    ///< a path, with the host's file picker
	Red,     ///< the three parts of a colour picker
	Green,
	Blue
};

/// One control as the host is told about it.
struct ControlInfo
{
	unsigned int id;
	const char* name; ///< at most 16 characters: FFGL's name field is not terminated
	const char* group;
	Kind kind;
	float defaultValue;///< in the host's units: 0..1, the integer, or the index
	const char* const* options;
	int optionCount;
	float minimum;///< an Integer's range
	float maximum;
};

/// The table, indexed by ParamId, for every id below PT_ABOUT_FIRST.
const ControlInfo& InfoOf( unsigned int id );

//---------------------------------------------------------------------------
// Options.
//---------------------------------------------------------------------------
enum class Pattern
{
	Forward = 0,///< one head a step, in order
	Reverse,    ///< the same, backwards
	Bounce,     ///< forwards then back, the ends once each
	Build,      ///< heads 0..k together on step k, then again from one
	Random,     ///< every head once a cycle, in an order fixed by the Seed
	All,        ///< every head every step
	Count
};

enum class Trigger
{
	Bar = 0,
	Beat,
	HalfBeat,   ///< every quaver
	QuarterBeat,///< every semiquaver
	EighthBeat, ///< every demisemiquaver
	Onset,      ///< every onset the audio analyser finds
	FireOnly,   ///< only the Fire button
	Count
};

enum class Look
{
	Fill = 0,///< the slice is a lamp in front of the clip
	Outline, ///< a tube bent round the slice's edge
	Gate,    ///< the clip lit by the strobe
	Count
};

enum class Slices
{
	Map = 0,///< Arena's active Advanced Output preset, or the Preset File
	Grid,   ///< Rows x Columns
	Count
};

/// An option's element index from the host's value. Resolume reads an
/// option's range back as 0..1 whatever its count, but writes the index.
int OptionIndex( float value, int count );

/// Beats per step for a beat-type trigger; 0 for Onset and Fire Only.
double BeatsPerStep( Trigger trigger );

double RechargeFromParam( float value );///< seconds, RC
float ParamFromRecharge( double seconds );
double DecayFromParam( float value );   ///< seconds, tau
float ParamFromDecay( double seconds );
double IntensityFromParam( float value );
float ParamFromIntensity( double intensity );
double WidthFromParam( float value );   ///< composition pixels
float ParamFromWidth( double pixels );
double RoomFromParam( float value );
double SensitivityFromThreshold( float value );
int IntegerFromParam( float value, int low, int high, int fallback );

constexpr int kMinGrid = 1;
constexpr int kMaxGrid = 32;
constexpr int kMaxSeed = 9999;

} // namespace xenon
