#pragma once

// A view source that plays on a server: the networked client (connection, prediction, rollback) on
// its own thread, publishing the predicted world every tick. The viewer's input drives the local
// player.
//
// States: "connecting", "joining" (waiting for the server's state), "playing", "reconnecting"
// (time is frozen until the server takes us back), "rejected" (see the "reject_reason" stat).
// It knows no controls.

#include "game_client.h"
#include "source_thread.h"

#include <mutex>

namespace cb
{

class LiveSource final : public ThreadedSource
{
public:
	explicit LiveSource( const ClientOptions& options );
	~LiveSource() override;

	bool TakesInput() const override
	{
		return true;
	}
	// The latest input, sampled by the simulation on its next tick.
	void SetInput( const PlayerInput& input ) override;

private:
	void Run() override;
	void Fill( GameClient& client, uint64_t fingerprint );

	ClientOptions m_options;

	std::mutex m_inputMutex;
	PlayerInput m_input{};
	uint8_t m_latchedButtons = 0; // presses shorter than a tick still reach the simulation
	ActionBits m_latchedActions = 0;
};

} // namespace cb
