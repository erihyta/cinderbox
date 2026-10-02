#pragma once

// A view source that plays on a server without simulating anything: the server sends the frames to
// draw (net/protocol.h, View), this sends the player's input up. No prediction, no rollback, no
// simulation in the process at all, and the server decides what this client is shown (its mods'
// Sees), which is what makes fog of war possible.
//
// The price: nothing is predicted, so the player's own character answers a round trip late.
//
// Frames arrive unreliably, each a delta against the newest frame this source said it has. It
// keeps the last few it decoded so the server can name any of them as the base; a frame whose base
// it no longer has is dropped and a whole one asked for.
//
// States: "connecting", "joining" (connected, waiting for the first frame), "playing",
// "reconnecting", "rejected" (see the "reject_reason" stat). It knows no controls.
//
// Stats: "reject_reason", "tick", "rtt_ms", "connect_failures", "frames" (decoded),
// "frames_dropped" (a base it no longer had, or bytes that were not a frame), "kbit_down" (the last
// second's, ENet's headers included), "kbit_down_total".

#include "source_thread.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace cb
{

struct StreamOptions
{
	std::string host = "127.0.0.1";
	uint16_t port = 7777;
	std::string playerName;
	double reconnectIntervalSeconds = 1.0;
	// How often the input goes up, a second.
	uint32_t inputRate = 60;
};

class StreamSource final : public ThreadedSource
{
public:
	explicit StreamSource( const StreamOptions& options );
	~StreamSource() override;

	bool TakesInput() const override
	{
		return true;
	}
	void SetInput( const PlayerInput& input ) override;

private:
	void Run() override;

	StreamOptions m_options;

	std::mutex m_inputMutex;
	PlayerInput m_input{};
	uint8_t m_latchedButtons = 0; // presses shorter than the gap between two sends still go up
	uint16_t m_latchedActions = 0;
};

} // namespace cb
