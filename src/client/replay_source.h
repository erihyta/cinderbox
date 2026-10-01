#pragma once

// A view source that plays a server recording (cb_server --record): the session re-simulated on
// its own thread, with the recorded checksums checked on the way. The player it follows is the
// frame's local player, so a viewer shows what that player saw: its HUD, its hits, its presses.
//
// States: "playing" for as long as the file plays (paused and ended included: see the stats),
// "rejected" when it cannot (the "reject_reason" stat says why).
//
// Controls:
//   "pause"        1 pauses, 0 plays on
//   "speed"        times real time, 0.125 to 16
//   "seek"         to this many seconds from the start
//   "skip"         by this many seconds from where it is (negative: back)
//   "step"         by this many ticks (negative: back), and pauses
//   "follow"       the player in this slot; -1: nobody (no local player)
//   "follow_next"  the next player in the world
// Until a "follow" it follows the first player there is.
//
// Stats: "reject_reason", "tick", "checksums_verified", "desyncs" (recorded checksums the playback
// did not reproduce), "fingerprint", "build_matches" (recorded by this build of the simulation),
// "fp_environment_ok", "replay_seconds", "replay_length_seconds", "replay_speed", "replay_paused",
// "replay_ended", "replay_follow" (the slot, or -1).

#include "source_thread.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cb
{

class ReplayPlayer;

class ReplaySource final : public ThreadedSource
{
public:
	explicit ReplaySource( const std::string& path );
	~ReplaySource() override;

	void Control( const std::string& name, double value ) override;

private:
	void Run() override;
	void Fill( ReplayPlayer& player, float alpha );

	std::string m_path;

	std::mutex m_controlMutex;
	std::vector<std::pair<std::string, double>> m_controls;

	// Thread only.
	double m_speed = 1.0;
	bool m_paused = false;
	int m_follow = -1;
	bool m_autoFollow = true;
};

} // namespace cb
