#include "anim_lead.h"

#include <utility>

#include "events.h"

#include <algorithm>
#include <cmath>

namespace cb::present
{

namespace
{
// Never further ahead than this many ticks, whatever the latency.
constexpr int kMaxLeadTicks = 60;
} // namespace

AnimState LeadAnimState( const AnimState& shown, const AnimGraph& graph, const AnimGraphPacks& packs, const AnimLead& lead )
{
	if ( lead.tickSeconds <= 0.0f || lead.netId == 0 || graph.layers.size() < 2 )
	{
		return shown;
	}
	const float dt = lead.tickSeconds;
	int ticks = std::clamp( int( std::floor( lead.seconds / dt + 0.5f ) ), 0, kMaxLeadTicks );
	// In the simulation a command takes effect in the tick it arrives in, and the state's clock
	// starts after it. So an input the state does not have yet gets a tick of its own, before the
	// lead: the state it leads to is then as old as the press, exactly as the server's will be.
	// (A stance tells whether the state has it; an event does not, so on the frame its answer
	// arrives the clock is a tick ahead, once.)
	bool ownTick = false;
	for ( const AnimLead::Input& input : lead.inputs )
	{
		ownTick |= input.event >= 0;
		ownTick |= input.layer >= 0 && input.layer < kLayerLimit && shown.stances[input.layer] != input.stance;
	}
	ticks += ownTick ? 1 : 0;
	if ( ticks == 0 )
	{
		return shown;
	}

	// What the machine reads about movement: what the state says, held still over the lead.
	AnimGraphInputs in;
	bool grounded = shown.mode == AnimMode::Locomotion || shown.mode == AnimMode::Land;
	in.builtins[AnimExpr::Speed] = shown.groundSpeed;
	in.builtins[AnimExpr::ForwardSpeed] = shown.legsBackward != 0 ? -shown.groundSpeed : shown.groundSpeed;
	in.builtins[AnimExpr::VerticalSpeed] = 0.0f;
	in.builtins[AnimExpr::Grounded] = grounded ? 1.0f : 0.0f;
	in.builtins[AnimExpr::AirborneTime] = grounded ? 0.0f : shown.modeTime;
	in.builtins[AnimExpr::Jumped] = 0.0f;
	in.builtins[AnimExpr::Aiming] = shown.aiming != 0 ? 1.0f : 0.0f;
	in.builtins[AnimExpr::Backward] = shown.legsBackward != 0 ? 1.0f : 0.0f;
	in.builtins[AnimExpr::MoveForward] = shown.moveForward;
	in.builtins[AnimExpr::MoveRight] = shown.moveRight;
	in.board = &lead.board.values;
	in.globalBoard = &lead.globalBoard;
	in.netId = lead.netId;
	in.heldKinds = lead.heldKinds.data();
	in.heldCount = uint32_t( lead.heldKinds.size() );

	AnimState s = shown;
	ModEventRecord events[kModEventHistory];
	std::vector<int> markers;
	for ( int i = 0; i < ticks; ++i )
	{
		// This tick is `ago` seconds before now; a press is in it, or before it, or still to come.
		float ago = float( ticks - i ) * dt;
		uint32_t eventCount = 0;
		s.stances = shown.stances;
		for ( const AnimLead::Input& input : lead.inputs )
		{
			// A press older than the lead counts from the first tick.
			float age = std::min( input.age + ( ownTick ? dt : 0.0f ), float( ticks ) * dt );
			if ( age <= ago - dt )
			{
				continue; // not pressed yet at this tick
			}
			if ( input.layer >= 0 && input.layer < kLayerLimit )
			{
				s.stances[input.layer] = input.stance;
			}
			// The event is at the tick of the press, like a command's.
			if ( input.event >= 0 && age <= ago && eventCount < kModEventHistory )
			{
				ModEventRecord& e = events[eventCount++];
				e = ModEventRecord{};
				e.type = uint16_t( input.event );
				e.netIdA = lead.netId;
				e.tick = uint32_t( i + 1 );
			}
		}
		in.events = events;
		in.eventCount = eventCount;
		in.tick = uint32_t( i + 1 );
		markers.clear();
		UpdateAnimGraph( s, graph, packs, in, dt, markers );
	}

	AnimState out = shown;
	for ( size_t l = 1; l < graph.layers.size(); ++l )
	{
		out.graph[l] = std::as_const( s ).graph[l];
	}
	return out;
}

} // namespace cb::present
