#pragma once

// The viewer's own player, a little ahead of the server.
//
// What a player's body plays is decided by the server: a mod sets a stance or emits an event, the
// character's state machine reacts, and the viewer sees it a round trip after the press. A look's
// prediction (CbPrediction) says what the server will answer; with that, the viewer runs the
// machine's upper layers forward for its own player, from the state the server sent, with the
// predicted stance or event put in at the moment of the press. The swing starts on the click.
//
//   lead = how far ahead the upper layers are shown (seconds)
//
//   press ........ the lead grows with the time since the press, the predicted input is put in
//   server's word  the state now has it by itself; the lead stays, so nothing jumps
//   afterwards ... the lead is given back slowly (the layers play a little slower until it is 0)
//
// Looks only: nothing is sent anywhere, and the server's state is what the lead starts from every
// frame, so a wrong guess corrects itself when the prediction expires. The base layer (the legs)
// is never led: it follows movement, which a simulating source already predicts.

#include "anim_graph.h"
#include "components.h"

#include <vector>

namespace cb::present
{

struct AnimLead
{
	uint32_t netId = 0;		   // whose (0: nobody is led)
	float seconds = 0.0f;	   // how far ahead
	float tickSeconds = 0.0f;  // the simulation's step
	// What predictions put in: a stance on a layer and / or an event at the player, `age` seconds ago.
	struct Input
	{
		float age = 0.0f;
		int layer = -1;	 // schema layer (-1: no stance)
		uint8_t stance = 0; // stance number in AnimState (schema index + 1)
		int event = -1;	 // schema event (-1: none)
	};
	std::vector<Input> inputs;
	// What the machine's conditions may read besides the state itself.
	Blackboard board;
	int32_t globalBoard[kBoardSlots] = {};
	std::vector<uint16_t> heldKinds;
};

// `shown` with the layers above the base run `lead.seconds` forward.
AnimState LeadAnimState( const AnimState& shown, const AnimGraph& graph, const AnimGraphPacks& packs, const AnimLead& lead );

} // namespace cb::present
