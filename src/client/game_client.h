#pragma once

// Networked client without any rendering: connection, join/reconnect, clock sync, prediction and
// rollback, input upload and desync detection. The raylib app and headless bots both use it.

#include "protocol.h"
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
	uint32_t maxRollbackTicks = 8;
	uint32_t inputRedundancy = 12; // each input packet repeats this many recent ticks
	uint32_t leadMarginTicks = 2;  // extra lead over the server beyond the measured latency
	double reconnectIntervalSeconds = 1.0;
	bool verbose = true;
	std::string logName = "client";
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

	// Null until the first Welcome.
	RollbackSession* Session()
	{
		return m_session.get();
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
	void SendInputs();
	void VerifyChecksums();
	void BeginReconnect( double now );
	void Log( const char* fmt, ... ) const;

	ClientOptions m_options;
	net::Transport m_transport;
	net::FrameCodec m_codec;
	std::unique_ptr<RollbackSession> m_session;
	SimConfig m_config;

	ClientState m_state = ClientState::Idle;
	std::string m_rejectReason;
	PlayerSlot m_slot = 0;
	uint64_t m_token = 0;
	bool m_connectPending = false;
	double m_nextConnectAttempt = 0.0;
	double m_lastNow = 0.0;

	// Clock sync
	uint32_t m_latestServerTick = 0;
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
