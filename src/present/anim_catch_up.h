#pragma once

// Another player's animation, seen from its start.
//
// Behind latency, what another player does reaches the viewer late: a swing the server started
// 0.25 s ago is first seen 0.25 s in (a rollback brings its state to where the server has it), and
// drawn as it is, the body would jump from standing to mid-swing in one frame. Instead the viewer
// draws such a clip from its start and plays it faster until it is level with the state, within
// kCatchUpSeconds: the wind-up is short, but it is there, and nothing pops.
//
//   first seen      a layer's state is new (or its clip started over) and already some way in
//   lag             how far behind the state the drawn clip is: the time it was first seen at
//   catching up     the lag shrinks to 0 over kCatchUpSeconds (the clip plays a few times faster)
//
// Only a clip that does not loop, first seen more than a tick and a half and at most kMaxLateStart
// in. A join, a loop or a blend space is drawn as it is. A correction that leaves a clip and comes
// straight back to the same play of it goes on where it was, never back to the start.
//
// Looks only: the state itself (what the server hit-tests) is untouched; what the character's
// animations do besides bones (CbTrackPlayer) follows the drawn time, so a keyed sound or flame
// plays with the pose it was keyed on.

#include "anim_graph.h"
#include "components.h"

#include <cstdint>
#include <vector>

namespace cb::present
{

constexpr float kMaxLateStart = 0.5f;
constexpr float kCatchUpSeconds = 0.1f;

// What the catch-up remembers of one player between frames.
struct AnimCatchUp
{
	struct Layer
	{
		bool known = false;
		uint8_t state = 0;
		uint8_t source = 0;
		float stateTime = 0.0f; // the state's own, last frame
		float lag = 0.0f;		// how far behind it the drawn clip is
		float pace = 0.0f;		// how fast the lag shrinks (seconds a second)
		// The play it left last, and how far behind it was then.
		bool left = false;
		uint8_t leftState = 0;
		uint8_t leftSource = 0;
		float leftStateTime = 0.0f;
		float leftLag = 0.0f;
		float leftPace = 0.0f;
	};
	std::vector<Layer> layers;
	int64_t started = 0; // clips first seen late and drawn from their start, so far
};

// `shown` with every layer that is catching up drawn `lag` behind. `seconds`: since the last call.
AnimState CatchUpAnimState( const AnimState& shown, const AnimGraph& graph, const AnimGraphPacks& packs, AnimCatchUp& memory,
							float seconds, float tickSeconds );

} // namespace cb::present
