#pragma once

// Authoritative server. Runs the one true Simulation at a fixed rate, collects client inputs, and
// streams the input frames it actually used. It never rolls back: a late input is replaced by the
// player's previous input.

#include "look_item.h"
#include "anim_graph.h"
#include "character_item.h"
#include "hit_test.h"
#include "navigation.h"
#include "map.h"
#include "mod_api.h"
#include "protocol.h"
#include "replay.h"
#include "view_file.h"
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
	// The physics step's tasks go to the process's physics threads (PhysicsThreads::Start).
	bool physicsThreaded = false;
	double reconnectGraceSeconds = 10.0;
	double resyncCooldownSeconds = 2.0;
	bool verbose = true;
	// Keep a hash for every tick (tests only; costs a serialization per tick).
	bool recordHashes = false;
	// Baked map to play (empty = the built-in sandbox).
	std::string mapPath;
	// Write a replay of the whole session (empty = off).
	std::string recordPath;
	// Write the session as a view file: what a viewer would be shown, playable without a
	// simulation (present/view_file.h; empty = off).
	std::string recordViewPath;
	// Frames a second in the view file (0: one per tick), and compact packets instead of exact
	// ones (present/view_codec.h): a much smaller file.
	uint32_t recordViewRate = 0;
	bool recordViewCompact = false;
	// The workshop items clients need for the mods this server runs (announced, never sent).
	std::vector<ModItem> items;
	// The character everyone plays as (null: the built-in rig). Its item must be in `items`.
	std::shared_ptr<const CharacterAsset> character;
	// Reads a character a mod declared (the baked files under characters/<name>/ in its workshop
	// item). Unset or failing: its bodies are the built-in rig.
	std::function<std::shared_ptr<const CharacterAsset>( const std::string& mod, const std::string& name, std::string& error,
														 std::string& warnings )>
		loadCharacter;
	// A character item's manifest by its name (a map's CbCharacterBody names one): announced to
	// players like a mod's item. Unset or false: not announced.
	std::function<bool( const std::string& name, ModItem& item )> characterItem;
	// Reads a mod's animation pack (the baked files under anim/<pack>/ in its workshop item). Unset
	// or failing: the pack's swaps do nothing.
	std::function<std::shared_ptr<const anim::AnimSet>( const std::string& mod, const std::string& pack, std::string& error,
														std::string& warnings )>
		loadAnimPack;
	// Reads a mod's motion set (motions/<set>.cfg in its workshop item) into `text`. Unset or false:
	// the set does nothing.
	std::function<bool( const std::string& mod, const std::string& set, std::string& text, std::string& error )> loadMotions;
	// Every item of a mod's look (items/*.cfg of its workshop item): for a mod that takes its
	// items from its scenes (Declarations::ItemsOfLook). Without it such a mod has none.
	std::function<std::vector<LookItem>( const std::string& mod )> lookItems;
	// Reads an item kind's body from a mod's workshop item (items/<kind>.cfg, baked from its scene).
	// Unset or false: the kind keeps the body its mod declared, or a small box.
	std::function<bool( const std::string& mod, const std::string& kind, ItemShape& shape, std::string& error,
						std::map<std::string, float>& properties )>
		loadItemShape;
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
	// A mod's look was published again and changes nothing the simulation reads: the item players
	// need for it from now on. Everyone connected is told (they load it over the old one), and
	// whoever joins later is told in the schema. False: no item of that mod is announced.
	bool ReplaceItem( const ModItem& item );
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

	// Called after every tick the server runs (an update can run several): for tests and tools that
	// must see the state of each one.
	void SetTickObserver( std::function<void()> observer )
	{
		m_tickObserver = std::move( observer );
	}
	// A player's private fields, as the server keeps them (tests).
	const Blackboard& Privates( PlayerSlot slot ) const
	{
		return m_privates[slot];
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
		uint64_t framesResent = 0;	 // frames sent again from the oldest unacknowledged one, after a wait
		uint64_t ackTooOld = 0;		 // clients that fell out of the frame history and got a snapshot
		uint64_t resyncRequests = 0;
		uint64_t joins = 0;
		uint64_t reconnects = 0;
		uint64_t infoRequests = 0; // asked what the server is (a menu, before joining)
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
		// Frames go out as they are made, each repeated in the next kFrameRepeats packets (a lost
		// packet or two costs nothing). Only when the oldest one the client has not acknowledged
		// has been out for longer than an answer takes is everything from it sent again.
		uint32_t sentThrough = 0;	// frames with tick < sentThrough have been sent at least once
		// What sentThrough was before each of the last two packets: where the packet after them
		// starts, so that a frame is in three packets however many ticks one packet brought.
		uint32_t sentBefore[2] = {};
		double roundTrip = 0.1;		// seconds from sending a frame to hearing it acknowledged
		double frameSentAt[256] = {}; // by tick % 256: when it last went out in full
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
	void HandleInput( Client& client, const net::MsgInput& msg, double now );
	Client* FindByPeer( net::PeerId peer );
	void Reject( net::PeerId peer, const std::string& reason );
	void RunTick( double now );
	std::vector<uint8_t> m_itemsBuffer;
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
	present::ViewFileWriter m_viewFile;
	present::ViewFrame m_viewFrame;
	// The state after the tick just simulated, as a frame of the view file.
	void RecordView( const InputFrame& frame );
	std::vector<InputFrame> m_history; // indexed by tick % kFrameHistory
	Client m_clients[kMaxPlayers];
	std::vector<PlayerEvent> m_pendingEvents;
	std::function<void()> m_tickObserver;
	net::InputArray m_lastInputs{};

	double m_nextTickTime = -1.0;
	uint64_t m_tokenState = 0;
	Stats m_stats;

	std::vector<net::NetEvent> m_events;
	std::vector<uint8_t> m_buffer;
	std::vector<uint8_t> m_image;
	// The map as loaded, forwarded to every client on join.
	LevelLayout m_map;
	bool m_mapBodiesPlaced = false; // its characters' bodies spawned (the first tick)
	std::vector<uint8_t> m_mapBytes;
	uint64_t m_mapHash = 0;
	std::vector<uint64_t> m_hashes;

	std::vector<std::unique_ptr<mods::ServerMod>> m_mods;
	ModSchema m_schema;
	std::vector<uint8_t> m_schemaBytes;
	// The mods' own state lives here; the simulation never sees it.
	std::unique_ptr<flecs::world> m_modWorld;
	std::unique_ptr<HitTester> m_hits;
	NavMesh m_nav; // where the server's own character can walk (built at the start)
	std::vector<std::shared_ptr<const AnimGraph>> m_graphs; // each character's (ModSchema::characters)
	// The characters as the simulation has them: each its state machine, its capsule, its movement.
	std::vector<Simulation::CharacterBody> CharacterBodies() const;
	AnimGraphPacks m_animPacks;
	std::shared_ptr<const Motions> m_motions;
	std::map<std::pair<int, std::string>, float> m_itemProperties; // what mods declared about item kinds
	std::map<int, int> m_itemLayers;							   // item kind -> the animation pack it brings
	mods::LayerWishes m_layerWishes;
	// Private fields (BoardScope::Private), by player slot: never in the simulation, sent to their
	// owner alone (SendPrivates), cleared when the slot is given up.
	std::array<Blackboard, kMaxPlayers> m_privates{};
	std::array<bool, kMaxPlayers> m_privatesChanged{};
	void SendPrivates( const Client& client );
	uint64_t m_modRng = 0;
	// Typed text: the channels mods declared, what players typed since the last tick (for the
	// mods), and what the mods said (sent after they ran).
	std::vector<std::string> m_textChannels;
	std::vector<mods::TypedText> m_typed;
	std::vector<mods::SaidText> m_said;
	void HandleText( const Client& client, const net::MsgText& text );
	void SendSaid();
};

} // namespace cb
