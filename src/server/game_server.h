#pragma once

// Authoritative server. Runs the one true Simulation at a fixed rate, collects client inputs, and
// streams the input frames it actually used. It never rolls back: a late input is replaced by the
// player's previous input.

#include "anim_graph.h"
#include "character_item.h"
#include "hit_test.h"
#include "map.h"
#include "mod_api.h"
#include "protocol.h"
#include "replay.h"
#include "simulation.h"
#include "transport.h"

#include <cstdint>
#include <map>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace cb
{

struct ServerOptions
{
	uint16_t port = net::kDefaultPort;
	SimConfig config;
	uint32_t maxClients = kMaxPlayers;
	uint32_t checksumInterval = 30; // ticks
	double reconnectGraceSeconds = 10.0;
	double resyncCooldownSeconds = 2.0;
	bool verbose = true;
	// Keep a hash for every tick (tests only; costs a serialization per tick).
	bool recordHashes = false;
	// Baked map to play (empty = the built-in sandbox).
	std::string mapPath;
	// Write a replay of the whole session (empty = off).
	std::string recordPath;
	// The workshop items clients need for the mods this server runs (announced, never sent).
	std::vector<ModItem> items;
	// The character everyone plays as (null: the built-in rig). Its item must be in `items`.
	std::shared_ptr<const CharacterAsset> character;
	// Reads a mod's animation pack (the baked files under anim/<pack>/ in its workshop item). Unset
	// or failing: the pack's swaps do nothing.
	std::function<std::shared_ptr<const anim::AnimSet>( const std::string& mod, const std::string& pack, std::string& error,
														std::string& warnings )>
		loadAnimPack;
	// Options mods read with Context::Option ("deathmatch.kills" -> "15").
	std::map<std::string, std::string> modOptions;
	uint32_t replayChecksumInterval = 60;
};

class GameServer
{
public:
	GameServer();
	~GameServer();

	// Gameplay mods, added before Start(). They tick in the order they were added.
	void AddMod( std::unique_ptr<mods::ServerMod> mod );

	bool Start( const ServerOptions& options );

	// What the mods declared (sent to clients on join).
	const ModSchema& Schema() const
	{
		return m_schema;
	}

	// Poll the network and run every tick that is due at `now` (seconds, monotonic).
	void Update( double now );

	// Seconds until the next tick is due (for sleeping).
	double TimeUntilNextTick( double now ) const;

	Simulation& Sim()
	{
		return *m_sim;
	}
	uint32_t Tick() const
	{
		return m_sim->Tick();
	}
	int ConnectedClients() const;
	bool GetRecordedHash( uint32_t tick, uint64_t& hash ) const;

	struct Stats
	{
		uint64_t lateInputs = 0; // ticks where a connected player's input had not arrived
		uint64_t inputTicks = 0; // ticks where a connected player's input was expected
		uint64_t snapshotsSent = 0; // welcomes: joins, reconnects and desync recoveries
		uint64_t batchesSent = 0;
		uint64_t framesSent = 0;	 // frames inside batches (resends included)
		uint64_t ackTooOld = 0;		 // clients that fell out of the frame history and got a snapshot
		uint64_t resyncRequests = 0;
		uint64_t joins = 0;
		uint64_t reconnects = 0;
		uint64_t ticks = 0;
		double tickMsTotal = 0.0; // simulation + snapshot + send work per tick
		double tickMsMax = 0.0;
		uint64_t bytesSent = 0;
		uint64_t bytesReceived = 0;
	};
	const Stats& GetStats() const
	{
		return m_stats;
	}
	void ResetTickTiming()
	{
		m_stats.ticks = 0;
		m_stats.tickMsTotal = 0.0;
		m_stats.tickMsMax = 0.0;
	}

	// Test hook: drop a player's connection without telling them.
	void DropClientHard( PlayerSlot slot, double now );

private:
	static constexpr uint32_t kInputBuffer = 128;
	static constexpr uint32_t kFrameHistory = 256; // frames kept for resending

	struct Client
	{
		bool used = false;
		bool connected = false;
		net::PeerId peer = 0;
		PlayerSlot slot = 0;
		std::string name;
		uint64_t token = 0;
		double disconnectedAt = 0.0;
		double lastResyncAt = -1e9;
		bool needsSnapshot = false; // send Welcome/Resync before the next tick
		bool welcomed = false;		// has received a Welcome for the current connection
		bool inWorld = false;		// Join event has been issued
		uint32_t ackTick = 0;		// the client has every frame with tick < ackTick
		PlayerInput lastInput{};
		struct Slot
		{
			uint32_t tick = UINT32_MAX;
			PlayerInput input{};
		};
		Slot inputs[kInputBuffer];
	};

	void HandleEvent( const net::NetEvent& ev, double now );
	void HandleHello( net::PeerId peer, const net::MsgHello& hello, double now );
	void HandleInput( Client& client, const net::MsgInput& msg );
	Client* FindByPeer( net::PeerId peer );
	void Reject( net::PeerId peer, const std::string& reason );
	void RunTick( double now );
	// Everyone's names, to every welcomed client (or only to `only`).
	void SendNames( net::PeerId only = 0 );
	std::string UniqueName( const std::string& wanted, const Client& self ) const;
	bool m_namesDirty = false;
	void RunMods( InputFrame& frame );
	void SendSnapshots();
	void SendFrames( double now );
	const InputFrame* HistoryFrame( uint32_t tick ) const;
	void Log( const char* fmt, ... ) const;

	ServerOptions m_options;
	std::unique_ptr<Simulation> m_sim;
	net::Transport m_transport;
	net::ReplayWriter m_replay;
	std::vector<InputFrame> m_history; // indexed by tick % kFrameHistory
	Client m_clients[kMaxPlayers];
	std::vector<PlayerEvent> m_pendingEvents;
	net::InputArray m_lastInputs{};

	double m_nextTickTime = -1.0;
	uint64_t m_tokenState = 0;
	Stats m_stats;

	std::vector<net::NetEvent> m_events;
	std::vector<uint8_t> m_buffer;
	std::vector<uint8_t> m_image;
	// The map as loaded, forwarded to every client on join.
	LevelLayout m_map;
	std::vector<uint8_t> m_mapBytes;
	uint64_t m_mapHash = 0;
	std::vector<uint64_t> m_hashes;

	std::vector<std::unique_ptr<mods::ServerMod>> m_mods;
	ModSchema m_schema;
	std::vector<uint8_t> m_schemaBytes;
	// The mods' own state lives here; the simulation never sees it.
	std::unique_ptr<flecs::world> m_modWorld;
	std::unique_ptr<HitTester> m_hits;
	std::shared_ptr<const AnimGraph> m_animGraph;
	AnimGraphPacks m_animPacks;
	uint64_t m_modRng = 0;
};

} // namespace cb
