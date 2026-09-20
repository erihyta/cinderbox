#pragma once

// Networked client without any rendering: connection, join/reconnect, clock sync, prediction and
// rollback, input upload and desync detection. The raylib app and headless bots both use it.

#include "protocol.h"
#include "map.h"
#include "rollback.h"
#include "transport.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cb
{

struct ClientOptions
{
	std::string host = "127.0.0.1";
	uint16_t port = net::kDefaultPort;
	// The prediction window follows the measured latency within [min, max]. Equal values fix it.
	uint32_t minRollbackTicks = 8;
	uint32_t maxRollbackTicks = 20;
	uint32_t inputRedundancy = 12; // each input packet repeats this many recent ticks
	uint32_t leadMarginTicks = 2;  // extra lead over the server beyond the measured latency
	double reconnectIntervalSeconds = 1.0;
	bool verbose = true;
	std::string logName = "client";
	// false: "lite" client that keeps pace with the server and sends input without simulating
	// (cheap load for stress tests).
	bool simulate = true;
};

enum class ClientState
{
	Idle,
	Connecting,
	AwaitingWelcome, // connected, waiting for the server state (also while resuming)
	Playing,
	Reconnecting, // time is frozen until the server takes us back
	Rejected,
};

const char* ToString( ClientState state );

class GameClient
{
public:
	// Called once per simulated tick to sample the local player's input.
	using InputSampler = std::function<PlayerInput( uint32_t tick )>;

	struct Stats
	{
		uint64_t welcomes = 0;
		uint64_t disconnects = 0;
		uint64_t checksumsVerified = 0;
		uint64_t desyncs = 0;
		uint32_t rttMs = 0;
		uint32_t ticksLastFrame = 0;
		bool rolledBackLastFrame = false;
		double tickError = 0.0; // target tick - current tick
		double rateScale = 1.0;
		double simMsLastFrame = 0.0; // reconcile + predicted ticks this frame
		double simMsMax = 0.0;
		uint64_t bytesSent = 0;
		uint64_t bytesReceived = 0;
		uint32_t rollbackWindow = 0;
		uint64_t batchesReceived = 0;
		uint64_t batchesIgnored = 0; // stale, or older than the frames we still know
		double stalledSeconds = 0.0; // time the prediction window was full and the clock was held
		double playingSeconds = 0.0;
	};

	GameClient();
	~GameClient();

	bool Start( const ClientOptions& options, double now );
	void Update( double now, const InputSampler& sampleInput );

	ClientState State() const
	{
		return m_state;
	}
	const std::string& RejectReason() const
	{
		return m_rejectReason;
	}

	// Tick the next Update will simulate (or, for a lite client, send input for).
	uint32_t CurrentTick() const
	{
		return m_session ? m_session->CurrentTick() : m_liteTick;
	}

	void ResetMaxStats()
	{
		m_stats.simMsMax = 0.0;
	}

	// Null until the first Welcome, and always null for a lite client.
	RollbackSession* Session()
	{
		return m_session.get();
	}

	// The map the server sent. Presentation uses its name to find the map's visuals.
	const LevelLayout& Map() const
	{
		return m_map;
	}
	uint64_t MapHash() const
	{
		return m_mapHash;
	}
	PlayerSlot Slot() const
	{
		return m_slot;
	}
	// True once we have been in the game; reconnects resume the same player.
	bool HasJoined() const
	{
		return m_token != 0;
	}

	// Fraction of the way to the next tick, for render interpolation.
	float TickAlpha() const;

	// Increments whenever the world was replaced wholesale (welcome / resync), so presentation can
	// snap instead of smoothing.
	uint64_t ResetGeneration() const
	{
		return m_resetGeneration;
	}

	const Stats& GetStats() const
	{
		return m_stats;
	}

	// Test hook: lose the connection without telling the server.
	void DropConnectionHard();

private:
	void HandleEvent( const net::NetEvent& ev, double now );
	void HandleWelcome( net::MsgWelcome& msg, double now );
	void Advance( double now, const InputSampler& sampleInput );
	void OnServerTick( uint32_t tickAfter, double now );
	void HandleFrameBatch( net::ByteReader& r, double now );
	void UpdateRollbackWindow( double rttTicks, double frameDt );
	uint32_t AckTick() const;
	void SendInputs();
	void VerifyChecksums();
	void BeginReconnect( double now );
	void Log( const char* fmt, ... ) const;

	ClientOptions m_options;
	net::Transport m_transport;
	// Inputs of recent authoritative frames (delta bases for batches), indexed by tick % size.
	struct KnownInputs
	{
		uint32_t tick = UINT32_MAX;
		net::InputArray inputs{};
	};
	std::vector<KnownInputs> m_knownInputs;
	std::vector<InputFrame> m_batchScratch;
	uint32_t m_liteConfirmed = 0;
	double m_windowShrinkTimer = 0.0;
	std::unique_ptr<RollbackSession> m_session;
	SimConfig m_config;
	// The map the server sent on join. The session is rebuilt when it changes.
	LevelLayout m_map;
	uint64_t m_mapHash = 0;

	ClientState m_state = ClientState::Idle;
	std::string m_rejectReason;
	PlayerSlot m_slot = 0;
	uint64_t m_token = 0;
	bool m_connectPending = false;
	double m_nextConnectAttempt = 0.0;
	double m_lastNow = 0.0;

	// Clock sync
	uint32_t m_latestServerTick = 0;
	uint32_t m_liteTick = 0;
	double m_clockOffset = 0.0; // server tick ~= now * rate + offset, at the moment a message arrives
	bool m_haveClock = false;
	double m_latestFrameTime = 0.0;
	double m_accumulator = 0.0;
	double m_lastUpdate = -1.0;

	// Sent-input history, indexed by tick % size.
	struct SentInput
	{
		uint32_t tick = UINT32_MAX;
		PlayerInput input{};
	};
	std::vector<SentInput> m_inputHistory;
	uint32_t m_newestInputTick = 0;
	bool m_haveInput = false;

	std::vector<net::MsgChecksum> m_pendingChecksums;
	uint64_t m_resetGeneration = 0;
	Stats m_stats;

	std::vector<net::NetEvent> m_events;
	std::vector<uint8_t> m_buffer;
};

} // namespace cb
