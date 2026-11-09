#pragma once

#include "Controls.h"

#include <cstdint>
#include <deque>
#include <vector>

/**
    The rig: one xenon strobe per head and the desk chase that fires them.
    Plain C++, no GL, so `xntest` checks it without a context.

    **A head** is a capacitor bank charged through a resistor from a fixed
    supply and dumped through its tube when its step fires. After a dump to
    zero the bank's voltage is V_s ( 1 - e^( -dt / RC ) ); a capacitor holds
    C V^2 / 2, so a flash carries

        E = E0 ( 1 - e^( -dt / RC ) )^2

    (IDEAS.md wrote the unsquared form: that is the voltage, not the energy).
    A head that has never fired is full. The charge is advanced every frame
    with the frame's RC, v <- 1 - ( 1 - v ) e^( -T / RC ), which is the closed
    form exactly while RC holds still and follows a dragged Recharge.

    **The tube** dumps the bank with its own time constant tau (Decay): the
    light of a flash at t_f is ( E / tau ) e^( -( t - t_f ) / tau ).

    **A frame** sees the light that fell in its own interval ( t0, t1 ], in
    closed form,

        X = sum E [ e^( -max( 0, t0 - t_f ) / tau ) - e^( -( t1 - t_f ) / tau ) ],

    so a flash caught whole is E and one flash summed over every frame it
    lands in is exactly E, whatever tau: the camera neither loses nor invents
    light.

    **The chase** steps on the host's bar phase at the trigger's subdivision
    (a boundary crossed between two frames fires at the moment inside the
    frame where the phase crossed it, interpolated: zoetrope's rule), on an
    onset, or on a press of Fire. An onset or a press carries no moment of
    its own: its step fires at the start of the frame that heard it, so that
    frame shows it. Step k lights heads by Pattern (Rig.cpp, HeadsForStep).
*/
namespace xenon
{
/// One flash, kept while its light still reaches a frame.
struct Flash
{
	int head;     ///< index into the map's heads
	double time;  ///< host seconds
	double energy;///< E / E0, 0..1
	long long step;
};

/// What one frame knows about the beat, the audio and the controls.
struct RigInputs
{
	Trigger trigger  = Trigger::Beat;
	Pattern pattern  = Pattern::Forward;
	int presses      = 0;   ///< Fire, since the last frame
	bool onset       = false;
	double barPhase  = 0.0; ///< 0..1
	double bpm       = 120.0;
	bool hostBeat    = false;///< the host has been moving barPhase
	double recharge  = 0.25; ///< RC, seconds
	double decay     = 2e-3; ///< tau, seconds (how long a flash is kept)
	uint32_t seed    = 1;
};

/// Hooks for the negative controls. A shipped rig sets 0.
enum RigHook : int
{
	kHookEnergyUnsquared  = 1 << 0,///< E = E0 ( 1 - e^( -dt / RC ) ): the voltage, not the energy
	kHookRechargeDoubled  = 1 << 1,///< the bank charges through 2 RC
	kHookBeatAtFrameStart = 1 << 2,///< a beat's step at the start of its frame, not where it fell
	kHookPatternLate      = 1 << 3,///< step k lights what step k - 1 should
	kHookSampledLight     = 1 << 4,///< a frame sees the light at its end times its length, not the integral
	kHookStepAtFrameEnd   = 1 << 5 ///< a press's flash at the END of its frame: the frame that heard it shows nothing
};

/// The heads step k of a chase over `count` heads lights, by position in
/// the chase's order (0 .. count - 1). Pure: the harness checks against it.
void HeadsForStep( Pattern pattern, long long step, int count, uint32_t seed, std::vector< int >& out );

/// A 32-bit integer hash (PCG's output permutation of an LCG step).
uint32_t HashInt( uint32_t v );

class Rig
{
public:
	/// A new set of heads: every bank full, the chase back at step 0.
	void Reset( int headCount );

	/// One host frame ( now - dt, now ]. Fires every step due in it, in time
	/// order, through `order` (chase position -> head index), then advances
	/// every bank's charge to `now`.
	void Advance( double now, double dt, const RigInputs& in, const std::vector< int >& order );

	/// Each head's light over ( now - dt, now ], a full flash caught whole = 1.
	void Exposure( double now, double dt, double tau, std::vector< float >& out ) const;

	int HeadCount() const
	{
		return static_cast< int >( charge.size() );
	}
	/// A head's bank, 0..1 of the supply, as of the last frame.
	double Charge( int head ) const
	{
		return head >= 0 && head < HeadCount() ? charge[ static_cast< size_t >( head ) ] : 0.0;
	}
	/// Steps taken since the last Reset.
	long long Steps() const
	{
		return step;
	}
	/// The flashes fired on the last frame.
	const std::vector< Flash >& LastFlashes() const
	{
		return fresh;
	}
	/// Flashes fired since the plugin loaded.
	unsigned long long Fired() const
	{
		return fired;
	}

	void SetHooksForTest( int bits )
	{
		hooks = bits;
	}
	int Hooks() const
	{
		return hooks;
	}

private:
	void fireStep( double at, const RigInputs& in, const std::vector< int >& order );
	void crossings( double p0, double p1, double stepSize, double frameStart, double dt, std::vector< double >& at ) const;

	std::vector< double > charge;   ///< v / V_s per head
	std::vector< double > chargedAt;///< the time `charge` holds for
	std::deque< Flash > flashes;    ///< still lighting frames
	std::vector< Flash > fresh;
	std::vector< int > lit;
	double lastPhase = -1.0;
	long long step   = 0;
	unsigned long long fired = 0;
	double longestTau = 0.5;
	int hooks         = 0;
};

} // namespace xenon
