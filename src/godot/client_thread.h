#pragma once

// Runs the networked client (connection, prediction, rollback) on its own thread and hands
// presentation frames to Godot's main thread.
//
// A dedicated thread keeps rollback spikes off the render thread, and it lets us own the
// floating-point environment the simulation runs in: whatever Godot or other libraries do to
// the main thread's FP control register cannot change simulation results.

#include "frame.h"
#include "game_client.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cb::gd
{

struct PublishedFrame
{
	present::PresentationFrame frame;
	GameClient::Stats stats;
	RollbackSession::Stats rollback;
	ClientState state = ClientState::Idle;
	std::string rejectReason;
	uint32_t currentTick = 0;
	uint32_t confirmedTick = 0;
	uint32_t rollbackWindow = 0;
	uint64_t fingerprint = 0;
	// Identity and name of the server's map, for loading its visuals.
	uint64_t mapHash = 0;
	std::string mapName;
	// Prefab each map template draws as, indexed by template. Only refreshed with the map.
	std::vector<std::string> templateVisuals;
	bool fpEnvironmentOk = true;
	bool hasSimulation = false;
	double publishedAt = 0.0; // seconds on the thread clock
	float alphaAtPublish = 0.0f;
	uint64_t serial = 0;
};

class ClientThread
{
public:
	~ClientThread();

	void Start( const ClientOptions& options );
	void Stop();
	bool Running() const
	{
		return m_thread.joinable();
	}

	// Main thread: latest input, sampled by the simulation on its next tick.
	void SetInput( const PlayerInput& input );

	// Main thread: copies the newest frame into `out` if it is newer than `out.serial`.
	bool TakeFrame( PublishedFrame& out );

	// Seconds since Start(), shared clock for interpolation.
	double Now() const;

private:
	void Run( ClientOptions options );
	void Publish( GameClient& client, double now, bool rolledBack );

	std::thread m_thread;
	std::atomic<bool> m_stop{ false };
	std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();

	std::mutex m_inputMutex;
	PlayerInput m_input{};
	uint8_t m_latchedButtons = 0; // presses shorter than a tick still reach the simulation

	std::mutex m_frameMutex;
	PublishedFrame m_latest;
	bool m_latestTaken = true;

	PublishedFrame m_building; // thread-owned scratch
	ClientState m_lastState = ClientState::Idle;
};

} // namespace cb::gd
