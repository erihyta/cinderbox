#pragma once

// Authoritative server. Runs the one true Simulation at a fixed rate, collects client inputs, and
// keeps its clients' worlds by one of two network models (net::NetModel): it streams the input
// frames it used (inputs: every client simulates the whole world), or the frames' physics inputs
// and its characters' states every few ticks (hybrid: a client simulates the physics from those,
// predicts its own character, and places the rest from the states). It never rolls back: a late
// input is replaced by the player's previous input.

#include "look_item.h"
#include "anim_graph.h"
#include "bot_brain.h"
#include "crowd_bench.h"
#include "character_item.h"
#include "hit_test.h"
#include "navigation.h"
#include "map.h"
#include "mod_api.h"
#include "protocol.h"
#include "replay.h"
#include "state_codec.h"
#include "view_file.h"
#include "simulation.h"
#include "transport.h"
#include "worker_pool.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cb
{

struct ServerOptions
{
	uint16_t port = net::kDefaultPort;
	// How clients' worlds are kept (protocol.h). The hybrid sends the characters' state every
	// `stateEvery` ticks, and the physics checksum every `checksumInterval`.
	net::NetModel netModel = net::NetModel::Inputs;
	uint32_t stateEvery = 2;
	// The hybrid (M250): each client is sent the characters by what matters to it most (M257: how
	// near, in its view, changing, aiming at it), within a budget; off, every one in every state (one
	// encoding for every client, but every character to everyone).
	bool statesByDistance = true;
	// Threads besides the server's own that encode those states, a client each (0: none).
	int stateThreads = 0;
	// The hybrid: how far back a player's hits may take the others, as it saw them (lag
	// compensation, M250), in milliseconds (0: as they are now). A hybrid game shows the others a
	// round trip and its physics' buffer in the past: about 330 ms at a 100 ms round trip.
	uint32_t lagCompensationMs = 500;
	uint32_t LagCompensationTicks() const
	{
		return uint32_t( uint64_t( lagCompensationMs ) * config.tickRate / 1000 );
	}
	// The most entities a client is sent in one state by distance, its own character and what it
	// holds aside: the most overdue first, a character with what it holds (a crowd's come round in
	// turn).
	uint32_t stateBudget = 512;
	// And the most bytes (M257): what the others' updates may take of a client's states, in kbit/s
	// (before packing; its own character and the head aside). 0: no limit but stateBudget.
	uint32_t clientBudgetKbits = 256;
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
	// Fake players (M251, the crowd bench): the first `fakePlayers` slots are played by bot brains
	// in the server, and each is also a client that is sent what a player would be (frames, states,
	// names) and acknowledges it a round trip later. Nothing goes on the network; what would have is
	// counted (Bench()). The same seed plays the same session.
	uint32_t fakePlayers = 0;
	uint64_t fakeSeed = 1;
	float fakeRadius = 0.0f;		 // metres from the map's spawn centre they walk back within (0: anywhere)
	uint32_t fakeRoundTripMs = 100;
	bool fakeFight = false;			 // they take out the first slot's item and use it on whoever is near
	float fakeWarmupSeconds = 5.0f;	 // what they are sent is counted from then on (not the joins' burst)
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
		uint64_t commandsLeftOut = 0; // the hybrid: commands its clients skip, not sent (M256)
		double stateBudgeted = 0.0;	  // the hybrid: the bytes its plans thought the others' updates took (M257)
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
		// The hybrid.
		uint64_t statesSent = 0;	 // messages, to every client
		uint64_t statesWhole = 0;	 // of them, against no base
		uint64_t updatesCarried = 0; // entities' updates against the record a client has, their base too old (M257)
		uint64_t stateBytes = 0;	 // their bytes
		double stateMsTotal = 0.0; // saving and encoding them
		// Where a tick's time goes (since the last ResetTickTiming): the mods, the simulation's step,
		// saving the characters' state (the hybrid), and sending frames and states (not in tickMs).
		double modsMs = 0.0;
		double stepMs = 0.0;
		double saveMs = 0.0;
		double sendMs = 0.0;
	};
	// The inputs of the tick just run (tests).
	const net::InputArray& LastInputs() const
	{
		return m_lastInputs;
	}
	const Stats& GetStats() const
	{
		return m_stats;
	}
	void ResetTickTiming()
	{
		m_stats.ticks = 0;
		m_stats.tickMsTotal = 0.0;
		m_stats.tickMsMax = 0.0;
		m_stats.modsMs = 0.0;
		m_stats.stepMs = 0.0;
		m_stats.saveMs = 0.0;
		m_stats.sendMs = 0.0;
	}

	// Test hook: drop a player's connection without telling them.
	void DropClientHard( PlayerSlot slot, double now );

	// The crowd bench (M251): what the fake players would have been sent.
	const CrowdBench& Bench() const
	{
		return m_bench;
	}

private:
	static constexpr uint32_t kInputBuffer = 128;
	static constexpr uint32_t kFrameHistory = 256; // frames kept for resending
	static constexpr size_t kStateHistory = 64; // states kept as bases (the state model)

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
		uint32_t framesFrom = 0;	// its first frame's tick (its snapshot's): it has none before
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
		// The hybrid: the newest characters' state the client has (its base for the next one), and
		// the newest sent to it.
		uint32_t stateAck = net::kNoStateBase;
		uint32_t stateSent = 0;
		// By distance (M250): every entity it may have, by NetId, with the state of the last update
		// of it it acknowledged (0: none yet); one gone is named in every state until one that
		// named it is acknowledged. And the states sent to it: which entities each updated, and
		// which it named gone.
		struct Known
		{
			uint32_t netId = 0;
			uint32_t acked = 0;
			bool removing = false;
			uint32_t sentAt = 0; // the state of the last update of it sent (acknowledged or not), or found it had
			// M257: how much it is owed an update (it grows each state by what it matters to this client;
			// sent, 0), and what its last update took (bytes before packing; 0: never sent).
			float priority = 0.0f;
			uint32_t bytes = 0;
			// M253: the record (with its puppet) it acknowledged, hashed, and whether that was as a
			// puppet (its view: the base for the next is the same kind); the one last sent, hashed. One
			// both are of is not sent again.
			bool ackedPuppet = false;
			uint64_t ackedHash = 0, sentHash = 0;
			// M257: the record alone it acknowledged (as a puppet's client has it, without the puppet),
			// hashed; and the state of the last update of it sent (0: none). One sent nothing since
			// it acknowledged one has that record in every state since: a base too old to be kept
			// is no matter if the record is still that one (net::EntityUpdate::carried).
			uint64_t ackedViewHash = 0;
			uint32_t updatedAt = 0;
			// M257: a character: the state through whose tick it has the events at it (acknowledged),
			// and through whose they were sent (each goes once; again from the acknowledged one when a
			// state that had them is lost: 0).
			uint32_t eventsAcked = 0, eventsSent = 0;
		};
		std::vector<Known> known;
		struct Sent
		{
			uint32_t tick = 0;
			std::vector<uint32_t> updated, removed;
			std::vector<uint64_t> hashes;	  // (by updated)
			std::vector<uint64_t> viewHashes; // (by updated) the record alone (M257: Known::ackedViewHash)
			std::vector<uint8_t> puppets;	  // (by updated)
			// M257: the characters whose events it said, all there were from a tick on, and the state
			// through which the client must have had them before for it to have them all after (0:
			// none: what was older is no longer sent).
			struct Covered
			{
				uint32_t netId = 0;
				uint32_t needs = 0;
			};
			std::vector<Covered> covered;
		};
		std::deque<Sent> sent;
		struct Slot
		{
			uint32_t tick = UINT32_MAX;
			PlayerInput input{};
		};
		Slot inputs[kInputBuffer];
		// A fake player (ServerOptions::fakePlayers): its inputs from a brain, what it is sent
		// counted, its acknowledgements a round trip after each send.
		bool fake = false;
		BotBrain brain = BotBrain( 1 );
		uint8_t intents = 0;
		struct Ack
		{
			uint32_t due = 0; // the tick it arrives at
			uint32_t ackTick = 0;
			uint32_t stateAck = 0;
		};
		std::deque<Ack> acks;
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
	std::vector<std::string> m_sentNames; // by slot: as the last change to everyone said (M255)
	void RunMods( InputFrame& frame );
	void SendSnapshots();
	void SendFrames( double now );
	uint32_t m_framesSentAt = 0; // the tick frames last went out (the hybrid's go with its states)
	// The hybrid: the newest characters' state to everyone, each against the one it has.
	void SendStates();
	const net::StateFrame* KeptState( uint32_t tick ) const;
	std::deque<net::StateFrame> m_states; // the newest last
	// Each kept state read (its entities' parts), and where each of its entities was when it was
	// saved (a held item: its holder; for how often a client is sent it). Alongside m_states.
	struct Placed
	{
		b3Vec3 at = {};
		uint32_t holder = 0;
	};
	struct KeptLayout
	{
		net::StateLayout layout;
		std::vector<Placed> placed; // by layout.entities
		// Each entity's group (by layout.entities): a character with what it holds, sent together
		// (their changes are much alike, and compress well side by side). The index of its head.
		std::vector<uint32_t> head;
		// Puppets (M253; sim/puppet.h): whether each is one or what one holds (its record goes as
		// its view to every client but the one that drives it), the views and the puppets the
		// layout's spans point into, and each one's record as each kind of client is sent it, hashed
		// (what a client acknowledged having, so one that has it is not sent it again).
		std::vector<uint8_t> puppetGroup;
		std::vector<uint8_t> viewBytes;
		std::vector<Puppet> puppets;
		std::vector<uint64_t> fullHash, viewHash;
		std::vector<uint64_t> viewRecordHash; // M257: a puppet's view and bodies without the puppet (0: none)
		// M257: a character whose layers' states or flags are not what they were in the state before
		// (it did something: its update matters more); what each would cost sent whole to a puppet's
		// client and to its own (its record's bytes that are not zeros, and its bodies': about what
		// packing leaves of them).
		std::vector<uint8_t> changed;
		std::vector<uint32_t> wholeView, wholeFull;
		// M257: by layout.entities, a character's newest event's tick and 1 (0: none kept).
		std::vector<uint32_t> newestEvent;
		// M257: by layout.entities, where a puppet looks, flat (x, z: a unit; zeros for the rest).
		std::vector<float> lookX, lookZ;
	};
	std::deque<KeptLayout> m_layouts;
	// M257: the last few records each puppet (or what one holds) had, as puppets' clients have them
	// (their views, by KeptLayout::viewRecordHash): one a client acknowledged long ago is still a
	// base for its next update, carried (net::EntityUpdate::carried). By NetId; dropped once no
	// state has had it for a while.
	struct RecordVersion
	{
		uint64_t hash = 0;
		std::vector<uint8_t> view;
		net::StateSpan span; // (into view: its bodies' size and hash, not the bodies; no puppet)
	};
	struct RecordVersions
	{
		uint32_t seenAt = 0;
		uint32_t next = 0;
		std::array<RecordVersion, 8> ring;
	};
	std::unordered_map<uint32_t, RecordVersions> m_recordVersions;
	// M257: the events at each character (Simulation::StepEvents) of the last kEventWindow ticks, by
	// its NetId, oldest first: each client is sent those at the characters it is sent, once.
	struct KeptEvent
	{
		ModEventRecord record;
		bool command = false; // (a mod's: its own game hears it from the server too)
		uint32_t serial = 0;  // (net::CharacterEvent::serial)
	};
	uint32_t m_eventSerial = 0;
	// A state sent that the client will not have (a later one acknowledged first, or too old): its
	// characters' events go again, from those it acknowledged.
	void EventsLost( Client& c, const Client::Sent& sent );
	std::unordered_map<uint32_t, std::vector<KeptEvent>> m_characterEvents;
	// Those too old to send dropped; each character's newest in the state kept (KeptLayout::newestEvent).
	void KeepEvents( KeptLayout& kept, uint32_t tick );
	uint32_t m_recordVersionsSwept = 0;
	void KeepRecordVersions( const KeptLayout& kept, uint32_t tick );
	const net::StateSpan* RecordVersionOf( uint32_t netId, uint64_t hash ) const;
	// A kept state's puppets, their views and the hashes (M253), as the world is when it is saved.
	void KeepPuppets( KeptLayout& kept ) const;
	// Sending states by distance, a step at a time (SendStates): each client's plan (what it is sent),
	// the messages they make (one for clients whose plans say the same), and a cache of shared
	// parts for each thread that encodes them.
	struct Plan
	{
		bool active = false;
		const net::StateFrame* base = nullptr;
		uint32_t ownId = 0;
		bool hasOwn = false;
		b3Vec3 ownAt = {};
		float viewX = 0.0f, viewZ = 1.0f; // where its camera looks, flat (M257: Matters)
		std::vector<net::EntityUpdate> updates;
		std::vector<PlayerSlot> inputSlots;
		std::vector<uint32_t> says; // what the message says, as numbers: the same for the same message
		uint64_t hash = 0;
		Client::Sent sent;
		std::vector<Client::Known> next;
		int message = -1;
		// (Scratch: each entity of the newest state, how it stands for this client.)
		struct Entry
		{
			const Client::Known* had = nullptr;
			uint32_t acked = 0;
			float score = 0.0f; // 0: not due
			bool chosen = false;
			bool puppet = false; // sent as a puppet's view (M253)
			bool same = false;	 // what it has of it already (acknowledged, and nothing else sent since)
			float owed = 0.0f;	 // M257: a group head's priority with this state's share (Known::priority)
			uint32_t bytes = 0;	 // what its update would take, as last time or whole
			// M257: its base too old, the record the client has (net::EntityUpdate::carried; null: none).
			const net::StateSpan* carried = nullptr;
		};
		std::vector<Entry> entries;
		std::vector<float> groupScore; // by head (the most overdue of its group's)
		std::vector<uint8_t> groupChosen;
		std::vector<std::pair<float, uint32_t>> due; // heads
		double budgeted = 0.0; // M257: the bytes the others' updates were thought to take
		std::vector<net::CharacterEvent> events; // M257: the events at the characters it is sent
	};
	struct Message
	{
		size_t plan = 0; // the first client's whose plan it is
		bool ok = false;
		std::vector<uint8_t> bytes;
		std::vector<uint32_t> sizes; // each update's bytes (M257)
		size_t unpacked = 0;		 // its bytes before packing
	};
	// M257: what packing leaves of a state's bytes, lately (the budget is of bytes sent; what an
	// update takes is known before packing).
	double m_packed = 0.5;
	std::vector<Plan> m_plans;
	std::vector<Message> m_messages;
	std::vector<net::UpdateCache> m_updateCaches;
	WorkerPool m_stateWorkers;
	// Lag compensation (M250): the characters as they were on each of the last ticks (the newest
	// last), and each slot's view of them for this tick's mods.
	std::deque<PastPoses> m_past;
	std::array<const PastPoses*, kMaxPlayers> m_seenBy{};
	void RecordPast();
	void PlanState( Plan& plan, Client& c, const net::StateFrame& newest, const KeptLayout& now,
					const std::vector<std::pair<uint32_t, PlayerSlot>>& players );
	const KeptLayout* KeptLayoutOf( uint32_t tick ) const;
	// What a character (a kept state's entity `i`) matters to a client, a state's share of its
	// priority (M257): near, in its view, doing something, looking at it.
	float Matters( const Plan& plan, const KeptLayout& now, size_t i ) const;
	static const net::StateLayout* LayoutOfTick( const void* server, uint32_t tick );
	// A client acknowledged the state of `tick` (by distance: what it updated is what it has).
	void StateAcknowledged( Client& c, uint32_t tick );
	// The hybrid: the physics checksum after each tick of the history (0: none), by tick % kFrameHistory.
	std::vector<uint64_t> m_physicsSums;
	const InputFrame* HistoryFrame( uint32_t tick ) const;
	// A hybrid frame as its clients are sent it (M256): without what they skip (the commands about
	// characters, the joins and leaves), its physics events where they then go.
	void LeaveOutSkipped( InputFrame& frame, const std::vector<uint8_t>& skipped );
	void Log( const char* fmt, ... ) const;

	// A message to a client: through the transport (reliable: in order on ChannelReliable;
	// otherwise unordered on ChannelInput), or, to a fake player, counted (M251) as `kind`.
	void Deliver( const Client& c, const std::vector<uint8_t>& bytes, bool reliable, CrowdBench::Kind CrowdBench::*kind );
	// The fake players (M251): made when the server starts; each tick's input from its brain;
	// their acknowledgements, a round trip after each update sent; the bench's parts.
	void AddFakePlayers();
	PlayerInput FakeInput( Client& c );
	void FakeAcknowledgements( double now );
	void FakeSent();
	void CountFrame( const InputFrame& frame );
	void SampleState( const Plan& plan, PlayerSlot slot, const net::StateFrame& newest, const KeptLayout& now, const std::vector<uint8_t>& message );
	CrowdBench m_bench;
	// Whether what the fake players get is counted now (there are some, and the warm-up is over).
	bool BenchCounts() const
	{
		return m_bench.fakes > 0 && m_sim->Tick() >= m_bench.fromTick;
	}
	std::vector<std::vector<std::pair<uint32_t, bool>>> m_outbox; // by slot: what a fake player got this update
	std::vector<uint32_t> m_fakeTargets;							// by slot: who it fights (a NetId, 0: none)

	ServerOptions m_options;
	std::unique_ptr<Simulation> m_sim;
	net::Transport m_transport;
	net::ReplayWriter m_replay;
	present::ViewFileWriter m_viewFile;
	present::ViewFrame m_viewFrame;
	// The state after the tick just simulated, as a frame of the view file.
	void RecordView( const InputFrame& frame );
	std::vector<InputFrame> m_history; // indexed by tick % kFrameHistory
	std::vector<Client> m_clients = std::vector<Client>( kMaxPlayers ); // (1023 of them: too big for a stack)
	std::vector<PlayerEvent> m_pendingEvents;
	// The mods' commands a frame had no room for, first in the next (M251: 255 players joining at
	// once had most of their start items dropped).
	std::vector<SimCommand> m_overflow;
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
