#include "Rig.h"

#include <algorithm>
#include <cmath>

namespace xenon
{
namespace
{
/// The most steps one frame may fire. A frame that crossed more was a seek
/// or a stall, not music.
constexpr int kMaxPerFrame = 64;

/// A bar is four beats (Resolume's bar phase runs 0..1 over a 4/4 bar).
constexpr double kBeatsPerBar = 4.0;

/// A flash is dropped once its light has fallen below e^-20 of its start.
constexpr double kKeepTaus = 20.0;
} // namespace

uint32_t HashInt( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

void HeadsForStep( Pattern pattern, long long step, int count, uint32_t seed, std::vector< int >& out )
{
	out.clear();
	if( count <= 0 || step < 0 )
		return;
	const long long n = count;
	switch( pattern )
	{
	case Pattern::Forward: out.push_back( int( step % n ) ); break;
	case Pattern::Reverse: out.push_back( int( n - 1 - step % n ) ); break;
	case Pattern::Bounce:
	{
		if( n == 1 )
		{
			out.push_back( 0 );
			break;
		}
		const long long period = 2 * n - 2;
		const long long p      = step % period;
		out.push_back( int( p < n ? p : period - p ) );
		break;
	}
	case Pattern::Build:
		for( long long i = 0; i <= step % n; ++i )
			out.push_back( int( i ) );
		break;
	case Pattern::Random:
	{
		//Every head once a cycle: Fisher-Yates over the cycle's own hash, so
		//the order is fixed by ( Seed, cycle ) and nothing else.
		const uint32_t cycle = uint32_t( step / n );
		std::vector< int > perm( static_cast< size_t >( n ) );
		for( int i = 0; i < count; ++i )
			perm[ size_t( i ) ] = i;
		const uint32_t base = HashInt( seed * 0x9E3779B9u ^ HashInt( cycle + 0x632BE5ABu ) );
		for( int i = count - 1; i > 0; --i )
		{
			const uint32_t j = HashInt( base ^ uint32_t( i ) * 0x85EBCA6Bu ) % uint32_t( i + 1 );
			std::swap( perm[ size_t( i ) ], perm[ j ] );
		}
		out.push_back( perm[ size_t( step % n ) ] );
		break;
	}
	case Pattern::All:
		for( int i = 0; i < count; ++i )
			out.push_back( i );
		break;
	default: break;
	}
}

void Rig::Reset( int headCount )
{
	headCount = std::max( headCount, 0 );
	charge.assign( size_t( headCount ), 1.0 );
	chargedAt.assign( size_t( headCount ), 0.0 );
	flashes.clear();
	fresh.clear();
	step = 0;
}

void Rig::crossings( double p0, double p1, double stepSize, double frameStart, double dt, std::vector< double >& at ) const
{
	if( !( stepSize > 0.0 ) || !( p1 > p0 ) )
		return;
	double boundary = ( std::floor( p0 / stepSize + 1e-9 ) + 1.0 ) * stepSize;
	for( int k = 0; boundary <= p1 + 1e-12 && k < kMaxPerFrame; ++k, boundary += stepSize )
	{
		const double inside = ( hooks & kHookBeatAtFrameStart ) != 0 ? 0.0 : ( boundary - p0 ) / ( p1 - p0 );
		at.push_back( frameStart + std::clamp( inside, 0.0, 1.0 ) * dt );
	}
}

void Rig::fireStep( double at, const RigInputs& in, const std::vector< int >& order )
{
	const int count = HeadCount();
	if( count == 0 )
	{
		++step;
		return;
	}
	const long long k = ( hooks & kHookPatternLate ) != 0 ? std::max( 0LL, step - 1 ) : step;
	HeadsForStep( in.pattern, k, count, in.seed, lit );
	const double rc = std::max( in.recharge, 1e-6 ) * ( ( hooks & kHookRechargeDoubled ) != 0 ? 2.0 : 1.0 );
	for( int position : lit )
	{
		const int head = ( position >= 0 && position < int( order.size() ) ) ? order[ size_t( position ) ] : position;
		if( head < 0 || head >= count )
			continue;
		double& v      = charge[ size_t( head ) ];
		double& since  = chargedAt[ size_t( head ) ];
		//The bank charges on from where it was to the moment it fires.
		const double gap = std::max( at - since, 0.0 );
		const double q   = 1.0 - ( 1.0 - v ) * std::exp( -gap / rc );
		const double e   = ( hooks & kHookEnergyUnsquared ) != 0 ? q : q * q;
		const Flash f{ head, at, e, step };
		flashes.push_back( f );
		fresh.push_back( f );
		++fired;
		v     = 0.0;
		since = at;
	}
	++step;
}

void Rig::Advance( double now, double dt, const RigInputs& in, const std::vector< int >& order )
{
	fresh.clear();
	const double frameStart = now - dt;
	longestTau              = std::max( in.decay, 1e-3 );

	//The bar phase: the host's, or a clock of the plugin's own at its BPM, so
	//a host that never moves it still has beats.
	const double tempo    = in.bpm > 1.0 ? in.bpm : 120.0;
	const double phaseNow = in.hostBeat ? std::clamp( in.barPhase, 0.0, 1.0 ) : std::fmod( now * tempo / 240.0, 1.0 );
	double p0 = lastPhase, p1 = phaseNow;
	const bool phaseValid = lastPhase >= 0.0 && dt > 0.0;
	if( phaseValid && p1 < p0 )
		p1 += 1.0;
	lastPhase = phaseNow;

	std::vector< double > at;
	const double beats = BeatsPerStep( in.trigger );
	if( beats > 0.0 && phaseValid )
		crossings( p0, p1, beats / kBeatsPerBar, frameStart, dt, at );
	//An onset or a press fires at the start of the frame that heard it.
	const double unannounced = ( hooks & kHookStepAtFrameEnd ) != 0 ? now : frameStart;
	if( in.trigger == Trigger::Onset && in.onset )
		at.push_back( unannounced );
	for( int i = 0; i < in.presses && i < kMaxPerFrame; ++i )
		at.push_back( unannounced );
	std::sort( at.begin(), at.end() );
	for( double t : at )
		fireStep( t, in, order );

	//Every bank on to the end of the frame, with this frame's RC.
	const double rc = std::max( in.recharge, 1e-6 ) * ( ( hooks & kHookRechargeDoubled ) != 0 ? 2.0 : 1.0 );
	for( size_t h = 0; h < charge.size(); ++h )
	{
		const double gap = now - chargedAt[ h ];
		if( gap > 0.0 )
		{
			charge[ h ]    = 1.0 - ( 1.0 - charge[ h ] ) * std::exp( -gap / rc );
			chargedAt[ h ] = now;
		}
		else if( gap < -1.0 )//the clock ran backwards: a clip restart
			chargedAt[ h ] = now;
	}

	//Flashes whose light no frame will see again.
	while( !flashes.empty() && ( now - flashes.front().time > kKeepTaus * longestTau || flashes.front().time > now + 1.0 ) )
		flashes.pop_front();
}

void Rig::Exposure( double now, double dt, double tau, std::vector< float >& out ) const
{
	out.assign( charge.size(), 0.0f );
	tau = std::max( tau, 1e-7 );
	const double t0 = now - std::max( dt, 0.0 );
	std::vector< double > sum( charge.size(), 0.0 );
	for( const Flash& f : flashes )
	{
		if( f.head < 0 || f.head >= int( sum.size() ) || f.time > now )
			continue;
		double x;
		if( ( hooks & kHookSampledLight ) != 0 )
			x = f.energy / tau * std::exp( -( now - f.time ) / tau ) * ( now - t0 );
		else
			x = f.energy * ( std::exp( -std::max( 0.0, t0 - f.time ) / tau ) - std::exp( -( now - f.time ) / tau ) );
		sum[ size_t( f.head ) ] += x;
	}
	for( size_t h = 0; h < sum.size(); ++h )
		out[ h ] = static_cast< float >( sum[ h ] );
}

} // namespace xenon
