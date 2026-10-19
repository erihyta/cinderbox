// End-to-end tests over real UDP loopback: a GameServer and headless GameClients in one process.
//
//   cb_net_tests [name]

#include "server_query.h"
#include "detmath.h"
#include "bot_brain.h"
#include "anim_graph.h"
#include "character_item.h"
#include "joint_math.h"
#include "level_query.h"
#include "live_source.h"
#include "pose.h"
#include "sha256.h"
#include "registry.h"
#include "fingerprint.h"
#include "game_client.h"
#include "game_server.h"
#include "map.h"
#include "netsim.h"
#include "replay.h"
#include "replay_source.h"
#include "simulation.h"
#include "box3d/box3d.h"
#include "util.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <thread>

#include "miniz.h"

#if defined( _WIN32 )
#include <windows.h>
#include <timeapi.h>
#endif

using namespace cb;

namespace
{

int g_failures = 0;

// Real-time thresholds (late inputs) only hold when the simulation runs at optimized speed: in a
// Debug build the server and four simulating clients cannot keep up on one thread. Nor on a machine
// too slow or too busy to: CB_NO_TIMING_CHECKS=1 turns them off (CI's shared macOS runner, where
// they failed one run in three with nothing wrong).
bool TimingChecks()
{
#if defined( NDEBUG )
	const char* off = std::getenv( "CB_NO_TIMING_CHECKS" );
	return off == nullptr || off[0] == '\0' || off[0] == '0';
#else
	return false;
#endif
}
const bool kTimingChecks = TimingChecks();

#define CHECK( cond )                                                                                                            \
	do                                                                                                                           \
	{                                                                                                                            \
		if ( !( cond ) )                                                                                                         \
		{                                                                                                                        \
			std::printf( "    CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond );                                            \
			++g_failures;                                                                                                        \
			return;                                                                                                              \
		}                                                                                                                        \
	}                                                                                                                            \
	while ( 0 )

using Clock = std::chrono::steady_clock;

// What a test script calls the number keys: the top bits of the actions, which no mod's action has
// (the mods' come from the bottom), and which Bot::Sample turns into slot intents.
// And the use button (the left mouse button), which is the engine's too.
constexpr ActionBits kKeyUse = ActionBits( 1 ) << 58;
constexpr ActionBits kKeySlot4 = ActionBits( 1 ) << 59;
constexpr ActionBits kKeySlot1 = ActionBits( 1 ) << 60;
constexpr ActionBits kKeySlot2 = ActionBits( 1 ) << 61;
constexpr ActionBits kKeySlot3 = ActionBits( 1 ) << 62;
constexpr ActionBits kKeyHands = ActionBits( 1 ) << 63;
constexpr ActionBits kSlotKeys = kKeySlot1 | kKeySlot2 | kKeySlot3 | kKeySlot4 | kKeyHands;

// An item kind's own key ("H" for the frag): the mod action "<kind>.use" its scene declares.
ActionBits UseKey( const ModSchema& schema, const std::string& kind )
{
	return schema.ActionMask( kind + ".use" );
}

struct Bot
{
	std::unique_ptr<GameClient> client;
	BotBrain brain;
	// Replaces the brain when set.
	std::function<PlayerInput( uint32_t )> script;

	// The slot key the bot pressed last (kKeySlot1 ..): its intent stays in its input, as a player's
	// does, so it is carried out once.
	ActionBits slotKey = 0;
	ActionBits slotKeyDown = 0;
	uint8_t slotPresses = 0;

	PlayerInput Sample( uint32_t tick )
	{
		if ( script )
		{
			PlayerInput in = script( tick );
			// Slots are the engine's now: a script still says "the pistol's key" as a bit, and this
			// turns it into the intent a number key sends.
			ActionBits key = in.actions & kSlotKeys;
			if ( key != 0 && key != slotKeyDown )
			{
				// A press: one more intent. (Held, it is one press, as a key is.)
				slotKey = key;
				slotPresses = uint8_t( slotPresses + 1 );
			}
			slotKeyDown = key;
			if ( in.actions & kKeyUse )
			{
				in.buttons |= BtnUse;
			}
			in.actions = in.actions & ~( kSlotKeys | kKeyUse );
			if ( slotKey != 0 )
			{
				uint8_t slot = slotKey & kKeySlot1 ? 0 : slotKey & kKeySlot2 ? 1 : slotKey & kKeySlot3 ? 2 : slotKey & kKeySlot4 ? 3 : kNoSlot;
				in.intent = uint8_t( SlotIntent::Select );
				in.intentA = slot;
				in.intentSeq = slotPresses;
			}
			return in;
		}
		// The spawn button is a mod action; its bit comes from the server's schema.
		brain.spawnAction = client->Schema().ActionMask( "spawn_prop" );
		return brain.Next();
	}
};

struct Harness
{
	GameServer server;
	std::vector<Bot> bots;
	Clock::time_point start = Clock::now();
	uint16_t port = 0;
	uint16_t clientPort = 0; // port bots connect to (the proxy's, when there is one)
	std::unique_ptr<net::NetSimProxy> proxy;

	// Runs every compiled server mod, like cb_server does by default.
	explicit Harness( uint16_t p, const std::string& recordPath = {}, const std::string& mapPath = {},
					  const std::map<std::string, std::string>& modOptions = {},
					  const std::function<void( ServerOptions& )>& configure = {} )
		: port( p )
		, clientPort( p )
	{
		// Every mod that runs by default; a disabled one (a "disabled" file: the impact grenade) only
		// when the test names it, in the mod option "test.mods" (a, b), as cb_server --mods would.
		// The brawler's kit runs by default too, but its skills take the hotbar's first slots, where
		// the tests expect the pistol, the bat and the rifle: it runs only when named as well.
		std::string named;
		if ( auto it = modOptions.find( "test.mods" ); it != modOptions.end() )
		{
			named = "," + it->second + ",";
		}
		for ( const mods::ModInfo& info : mods::CompiledMods() )
		{
			const bool asked = named.find( "," + std::string( info.name ) + "," ) != std::string::npos;
			if ( ( info.onByDefault && std::string( info.name ) != "brawl" ) || asked )
			{
				server.AddMod( info.create() );
			}
		}
		ServerOptions options;
		options.port = port;
		options.mapPath = mapPath;
		options.recordHashes = true;
		options.verbose = false;
		options.config.physicsArenaMB = 64;
		options.reconnectGraceSeconds = 10.0;
		options.recordPath = recordPath;
		options.modOptions = modOptions;
		// (Tests run every mod; chests stand only where a test asks for them.)
		options.modOptions.emplace( "chest.count", "0" );
		options.lookItems = []( const std::string& mod ) {
			std::vector<LookItem> items;
			ListLookItemsFolder( std::string( CB_SOURCE_DIR ) + "/server_mods/" + mod + "/client", items );
			return items;
		};
		options.loadItemShape = []( const std::string& mod, const std::string& kind, ItemShape& shape, std::string& error,
									std::map<std::string, float>& properties ) {
			return LoadItemShapeFolder( std::string( CB_SOURCE_DIR ) + "/server_mods/" + mod + "/client", kind, shape, error, &properties );
		};
		options.loadMotions = []( const std::string& mod, const std::string& set, std::string& text, std::string& error ) {
			return LoadMotionsFolder( std::string( CB_SOURCE_DIR ) + "/server_mods/" + mod + "/client", set, text, error );
		};
		if ( configure )
		{
			configure( options );
		}
		if ( server.Start( options ) == false )
		{
			std::printf( "    server failed to start on port %u (is another test on it?)\n", port );
			std::fflush( stdout );
			std::exit( 1 );
		}
	}

	// Route bots added from now on through a degraded link.
	void AddNetSim( uint16_t proxyPort, const net::NetSimConfig& config )
	{
		proxy = std::make_unique<net::NetSimProxy>();
		if ( proxy->Start( proxyPort, "127.0.0.1", port, config ) == false )
		{
			std::printf( "    netsim failed to start on port %u\n", proxyPort );
		}
		clientPort = proxyPort;
	}

	double Now() const
	{
		return std::chrono::duration<double>( Clock::now() - start ).count();
	}

	Bot& AddBot( uint32_t rollbackWindow = 8, const std::string& name = {} )
	{
		Bot bot;
		bot.client = std::make_unique<GameClient>();
		bot.brain = BotBrain( 1000 + bots.size() );
		ClientOptions options;
		options.port = clientPort;
		options.minRollbackTicks = rollbackWindow;
		options.maxRollbackTicks = std::max<uint32_t>( rollbackWindow, 20 );
		options.verbose = false;
		options.logName = "bot" + std::to_string( bots.size() );
		options.playerName = name.empty() ? options.logName : name;
		// On a machine too slow for the real-time limits (CB_NO_TIMING_CHECKS), a bot sends its input
		// further ahead, so it still reaches the server in time: tests that say on which tick the
		// server saw a press hold there too.
		if ( kTimingChecks == false )
		{
			options.leadMarginTicks = 6;
		}
		bot.client->Start( options, Now() );
		bots.push_back( std::move( bot ) );
		return bots.back();
	}

	void RunUntil( double until, const std::function<void( double )>& onStep = {} )
	{
		while ( Now() < until )
		{
			double now = Now();
			server.Update( now );
			if ( proxy )
			{
				proxy->Update( now );
			}
			// Each client on a thread of its own for this step, as each is on a machine of its own in
			// a real session: a client resimulates its whole rollback window for every frame, and
			// several of them one after another take longer than a tick, which makes every input
			// late and says nothing about the netcode. They share nothing but the sockets, and all
			// are done before the server runs again or the test looks at them. Only once all of them
			// play: while one is still joining they go one after another, so they join in the order
			// they were added and tests can tell which slot is whose.
			bool allPlaying = bots.size() > 1;
			for ( const Bot& b : bots )
			{
				allPlaying = allPlaying && b.client->State() == ClientState::Playing;
			}
			if ( allPlaying )
			{
				std::vector<std::thread> stepping;
				for ( Bot& b : bots )
				{
					stepping.emplace_back( [&b, now] { b.client->Update( now, [&b]( uint32_t tick ) { return b.Sample( tick ); } ); } );
				}
				for ( std::thread& t : stepping )
				{
					t.join();
				}
			}
			else
			{
				for ( Bot& b : bots )
				{
					b.client->Update( now, [&b]( uint32_t tick ) { return b.Sample( tick ); } );
				}
			}
			if ( onStep )
			{
				onStep( now );
			}
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
	}

	// Every confirmed client state we can still look up must equal the server's.
	int CompareWithServer( Bot& bot, int& compared )
	{
		RollbackSession* s = bot.client->Session();
		if ( s == nullptr )
		{
			return 1;
		}
		int mismatches = 0;
		uint32_t confirmed = s->ConfirmedTick();
		uint32_t from = confirmed > 400 ? confirmed - 400 : 0;
		for ( uint32_t t = from; t <= confirmed; ++t )
		{
			uint64_t clientHash, serverHash;
			if ( s->GetConfirmedHash( t, clientHash ) && server.GetRecordedHash( t, serverHash ) )
			{
				++compared;
				if ( clientHash != serverHash )
				{
					++mismatches;
				}
			}
		}
		return mismatches;
	}

	void Report()
	{
		const auto& ss = server.GetStats();
		std::printf( "    server tick %u, %d connected, late inputs %llu, snapshots %llu, reconnects %llu\n", server.Tick(),
					 server.ConnectedClients(), (unsigned long long)ss.lateInputs, (unsigned long long)ss.snapshotsSent,
					 (unsigned long long)ss.reconnects );
		for ( size_t i = 0; i < bots.size(); ++i )
		{
			GameClient& c = *bots[i].client;
			RollbackSession* s = c.Session();
			std::printf( "    bot%zu: %s slot %u tick %u confirmed %u rollbacks %llu checksums ok %llu desyncs %llu welcomes %llu\n", i,
						 ToString( c.State() ), c.Slot(), s ? s->CurrentTick() : 0, s ? s->ConfirmedTick() : 0,
						 s ? (unsigned long long)s->GetStats().rollbacks : 0ull,
						 (unsigned long long)c.GetStats().checksumsVerified, (unsigned long long)c.GetStats().desyncs,
						 (unsigned long long)c.GetStats().welcomes );
		}
	}
};

size_t CountPlayers( Simulation& sim )
{
	size_t n = 0;
	for ( int i = 0; i < kMaxPlayers; ++i )
	{
		n += sim.IsPlayerActive( PlayerSlot( i ) ) ? 1 : 0;
	}
	return n;
}

// A client must play the server's map, not its own idea of a level. The server sends the baked
// bytes on join; if that were skipped, entities created later (players spawning) would differ.
void TestMapSync()
{
	// A small map that is clearly not the built-in sandbox.
	LevelLayout authored;
	authored.name = "net_test_arena";
	authored.spawnCenter = { -6.0f, 1.5f, -9.0f };
	authored.spawnRadius = 3.0f;
	authored.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 25.0f, 0.5f, 25.0f }, 0.0f, 0.0f } );
	authored.statics.push_back( { { 0.0f, 1.0f, -14.0f }, { 25.0f, 1.0f, 0.5f }, 0.0f, 0.0f } );
	authored.props.push_back( { ShapeKind::Box, { -6.0f, 0.5f, -5.0f }, { 0.5f, 0.5f, 0.5f } } );

	// A template the spawn button creates, so the bots exercise it over the network.
	EntityTemplate ball;
	ball.name = "ball";
	ball.visual = "prop_bouncy";
	AuthoredComponent shape;
	shape.id = Fnv32( "Shape" );
	shape.fields.push_back( { Fnv32( "kind" ), { int32_t( ShapeKind::Sphere ), 0, 0 } } );
	shape.fields.push_back( { Fnv32( "radius" ), { MapQuantize( 0.3f, kMapPositionScale ), 0, 0 } } );
	AuthoredComponent material;
	material.id = Fnv32( "Material" );
	material.fields.push_back( { Fnv32( "restitution" ), { MapQuantize( 0.8f, kMapPositionScale ), 0, 0 } } );
	AuthoredComponent prop;
	prop.id = Fnv32( "Prop" );
	prop.fields.push_back( { Fnv32( "lifetime_seconds" ), { MapQuantize( 4.0f, kMapPositionScale ), 0, 0 } } );
	ball.components = { shape, material, prop };
	authored.templates.push_back( ball );
	authored.instances.push_back( { 0, { -6.0f, 4.0f, -6.0f }, 0.0f, 0.0f } );
	authored.spawnTemplate = 0;

	QuantizeLayout( authored );

	std::vector<uint8_t> bytes;
	SerializeMap( authored, bytes );
	std::filesystem::path mapPath = std::filesystem::temp_directory_path() / "cb_net_test_arena.cbmap";
	std::string error;
	CHECK( WriteMapFile( mapPath.string(), bytes, error ) );

	Harness h( 17836, {}, mapPath.string() );
	h.AddBot();
	h.AddBot();
	h.RunUntil( 3.0 );
	h.AddBot(); // late joiner: gets the map with its snapshot
	h.RunUntil( 6.0 );
	h.Report();

	CHECK( CountPlayers( h.server.Sim() ) == 3 );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->State() == ClientState::Playing );
		CHECK( b.client->GetStats().desyncs == 0 );
		// The client rebuilt its simulation from the server's map, not from the built-in one.
		CHECK( b.client->Map().name == authored.name );
		CHECK( b.client->Map().statics.size() == authored.statics.size() );
		// The map's, and the mods' templates after them (the chest mod's): the map the server runs.
		std::vector<uint8_t> served;
		SerializeMap( h.server.Sim().Map(), served );
		CHECK( b.client->MapHash() == MapHash( served.data(), served.size() ) && b.client->MapHash() != MapHash( bytes.data(), bytes.size() ) );
		// Templates travel with the map, so clients can create the same entities the server does.
		CHECK( b.client->Map().templates.size() == 2 && b.client->Map().templates[1].name == "chest" );
		CHECK( b.client->Map().templates[0].visual == "prop_bouncy" );
		CHECK( b.client->Map().spawnTemplate == 0 );
		int compared = 0;
		CHECK( h.CompareWithServer( b, compared ) == 0 );
		CHECK( compared > 20 );
	}

	// Players really did spawn where the map says, not where the sandbox would have put them.
	bool sawPlayer = false;
	Simulation& sim = h.server.Sim();
	for ( const auto& r : sim.Entities() )
	{
		flecs::entity e( sim.World(), r.entity );
		if ( e.has<Character>() == false )
		{
			continue;
		}
		sawPlayer = true;
		const Transform& t = e.get<Transform>();
		CHECK( t.position.z < 0.0f );
	}
	CHECK( sawPlayer );

	std::filesystem::remove( mapPath );
}

void TestLoopbackSession()
{
	Harness h( 17801 );
	for ( int i = 0; i < 4; ++i )
	{
		h.AddBot();
	}
	h.RunUntil( 3.0 );
	uint64_t lateAtJoin = h.server.GetStats().lateInputs;
	uint32_t tickAtJoin = h.server.Tick();
	h.RunUntil( 6.0 );
	h.Report();

	// Once settled, inputs must reach the server in time (loopback has no real latency).
	uint64_t lateSteady = h.server.GetStats().lateInputs - lateAtJoin;
	uint64_t playerTicks = uint64_t( h.server.Tick() - tickAtJoin ) * 4;
	std::printf( "    steady state: %llu late inputs of %llu player-ticks\n", (unsigned long long)lateSteady,
				 (unsigned long long)playerTicks );
	CHECK( kTimingChecks == false || lateSteady * 100 < playerTicks );

	CHECK( CountPlayers( h.server.Sim() ) == 4 );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->State() == ClientState::Playing );
		CHECK( b.client->GetStats().desyncs == 0 );
		CHECK( b.client->GetStats().checksumsVerified >= 5 );
		int compared = 0;
		CHECK( h.CompareWithServer( b, compared ) == 0 );
		CHECK( compared > 100 );
		// The client runs ahead of the server (prediction), but not by much on loopback.
		RollbackSession* s = b.client->Session();
		CHECK( s->CurrentTick() + 10 > h.server.Tick() );
		CHECK( s->CurrentTick() < h.server.Tick() + 10 );
	}

	// Slots are distinct.
	for ( size_t i = 0; i < h.bots.size(); ++i )
	{
		for ( size_t j = i + 1; j < h.bots.size(); ++j )
		{
			CHECK( h.bots[i].client->Slot() != h.bots[j].client->Slot() );
		}
	}
}

void TestLateJoin()
{
	Harness h( 17802 );
	h.AddBot();
	h.AddBot();
	h.RunUntil( 3.0 );
	h.AddBot();
	h.RunUntil( 6.0 );
	h.Report();

	CHECK( CountPlayers( h.server.Sim() ) == 3 );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->State() == ClientState::Playing );
		CHECK( b.client->GetStats().desyncs == 0 );
		CHECK( b.client->GetStats().welcomes == 1 );
		int compared = 0;
		CHECK( h.CompareWithServer( b, compared ) == 0 );
		CHECK( compared > 50 );
	}
	// The late joiner started from a snapshot well after tick 0.
	uint64_t unused;
	CHECK( h.bots[2].client->Session()->GetConfirmedHash( 10, unused ) == false );
}

void TestReconnect()
{
	Harness h( 17803 );
	h.AddBot();
	h.AddBot();
	h.RunUntil( 2.0 );

	PlayerSlot slot0 = h.bots[0].client->Slot();
	PlayerSlot slot1 = h.bots[1].client->Slot();

	// Client notices first (e.g. its network interface changed) and comes straight back.
	h.bots[0].client->DropConnectionHard();
	uint32_t frozenAt = h.bots[0].client->Session()->CurrentTick();
	bool sawFrozen = false;
	h.RunUntil( 2.5, [&]( double ) {
		if ( h.bots[0].client->State() != ClientState::Playing )
		{
			sawFrozen = sawFrozen || h.bots[0].client->Session()->CurrentTick() == frozenAt;
		}
	} );
	CHECK( sawFrozen );

	// Server drops the other one silently; the client has to time out first.
	h.server.DropClientHard( slot1, h.Now() );
	h.RunUntil( 10.0 );
	h.Report();

	CHECK( CountPlayers( h.server.Sim() ) == 2 );
	CHECK( h.server.GetStats().reconnects == 2 );
	CHECK( h.bots[0].client->Slot() == slot0 );
	CHECK( h.bots[1].client->Slot() == slot1 );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->State() == ClientState::Playing );
		CHECK( b.client->GetStats().welcomes == 2 );
		CHECK( b.client->GetStats().desyncs == 0 );
		int compared = 0;
		CHECK( h.CompareWithServer( b, compared ) == 0 );
		CHECK( compared > 50 );
	}
}

void TestGraceExpiry()
{
	Harness h( 17804 );
	h.AddBot();
	h.AddBot();
	h.RunUntil( 1.5 );
	CHECK( CountPlayers( h.server.Sim() ) == 2 );

	// Stop updating bot 1 entirely: its connection times out and the grace period expires.
	Bot gone = std::move( h.bots[1] );
	h.bots.pop_back();
	gone.client.reset();
	h.RunUntil( 15.0 );
	h.Report();
	CHECK( CountPlayers( h.server.Sim() ) == 1 );
	CHECK( h.bots[0].client->GetStats().desyncs == 0 );
}

void TestLossySession()
{
	// A bad connection: ~70 ms RTT with jitter, 3% loss and duplicates each way.
	std::filesystem::path replayPath = std::filesystem::temp_directory_path() / "cinderbox_net_test.cbr";
	{
		Harness h( 17805, replayPath.string() );
		net::NetSimConfig bad;
		bad.latencyMs = 30;
		bad.jitterMs = 10;
		bad.lossPercent = 3.0f;
		bad.duplicatePercent = 1.0f;
		bad.seed = 7;
		h.AddNetSim( 17806, bad );
		for ( int i = 0; i < 4; ++i )
		{
			h.AddBot();
		}
		h.RunUntil( 4.0 );
		uint64_t lateBefore = h.server.GetStats().lateInputs;
		uint64_t expectedBefore = h.server.GetStats().inputTicks;
		h.RunUntil( 10.0 );
		h.Report();

		// Once settled, lost packets must not make inputs late (no head-of-line blocking, and the
		// prediction window grows with the latency).
		uint64_t late = h.server.GetStats().lateInputs - lateBefore;
		uint64_t expected = h.server.GetStats().inputTicks - expectedBefore;
		std::printf( "    steady state: %llu late inputs of %llu (%.2f%%), windows", (unsigned long long)late,
					 (unsigned long long)expected, expected ? 100.0 * double( late ) / double( expected ) : 0.0 );
		for ( Bot& b : h.bots )
		{
			std::printf( " %u", b.client->GetStats().rollbackWindow );
		}
		std::printf( "; stalled" );
		for ( Bot& b : h.bots )
		{
			std::printf( " %.2fs (%llu of %llu batches no use)", b.client->GetStats().stalledSeconds,
						 (unsigned long long)b.client->GetStats().batchesIgnored, (unsigned long long)b.client->GetStats().batchesReceived );
		}
		std::printf( "; frames sent again %llu\n", (unsigned long long)h.server.GetStats().framesResent );
		CHECK( kTimingChecks == false || late * 50 < expected ); // under 2%

		auto ps = h.proxy->GetStats();
		std::printf( "    netsim: %u links, %llu forwarded, %llu dropped, %llu duplicated\n", ps.links,
					 (unsigned long long)ps.forwarded, (unsigned long long)ps.dropped, (unsigned long long)ps.duplicated );
		CHECK( ps.dropped > 0 );
		CHECK( CountPlayers( h.server.Sim() ) == 4 );
		for ( Bot& b : h.bots )
		{
			CHECK( b.client->State() == ClientState::Playing );
			CHECK( b.client->GetStats().desyncs == 0 );
			CHECK( b.client->GetStats().rttMs >= 50 );
			CHECK( b.client->Session()->GetStats().rollbacks > 0 );
			int compared = 0;
			CHECK( h.CompareWithServer( b, compared ) == 0 );
			CHECK( compared > 100 );
		}
	}

	// The server recorded the session; replaying it reproduces every recorded checksum.
	net::ReplayReader replay;
	std::string error;
	CHECK( replay.Open( replayPath.string(), error ) );
	CHECK( replay.Fingerprint() == BuildFingerprint() );
	CHECK( replay.Frames().size() > 500 );
	CHECK( replay.Checksums().size() > 5 );
	Simulation sim( replay.Config() );
	sim.SetAnimGraph( replay.Graph() ); // what the server ran: the character's state machine
	sim.SetAnimPacks( replay.Packs() );
	sim.SetMotions( replay.MotionSets() );
	sim.SetItemShapes( replay.Schema().itemShapes ); // where each kind is stowed
	size_t next = 0;
	size_t verified = 0;
	for ( const InputFrame& frame : replay.Frames() )
	{
		while ( next < replay.Checksums().size() && replay.Checksums()[next].tick == sim.Tick() )
		{
			CHECK( sim.ComputeHash() == replay.Checksums()[next].hash );
			++verified;
			++next;
		}
		sim.Step( frame );
	}
	std::printf( "    replay: %zu ticks, %zu checksums verified\n", replay.Frames().size(), verified );
	CHECK( verified > 5 );
	std::filesystem::remove( replayPath );
}

// About 18 degrees down (65536 = a full turn): from one spawn point's eye to the next one's chest.
constexpr int16_t kAimAtChest = -3300;

// Server mods end to end: one player picks the pistol and shoots another until it dies. The rules
// run only on the server; the clients only receive commands and must agree with it on everything,
// ragdoll included, while seeing the board values and events the mods published.
void TestModsSession()
{
	Harness h( 47790 );
	const ModSchema& schema = h.server.Schema();
	CHECK( schema.FindField( "combat.health" ) != nullptr );
	CHECK( schema.FindEvent( "item.use" ) >= 0 );
	CHECK( kKeyUse != 0 );
	ActionBits fire = kKeyUse;
	ActionBits pistol = kKeySlot1;
	uint16_t spawn = schema.ActionMask( "spawn_prop" );

	// Slot 0 stands at x = -5.25 and slot 1 at x = -3.75 (the sandbox spawn grid): slot 0 looks
	// toward +X at slot 1, 1.5 m away, and a little down from its eye to the chest (players are hit
	// by their hitboxes, and the eye is level with the top of the head).
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraYaw = 16384;
		in.cameraPitch = kAimAtChest;
		uint32_t t = tick % 1000;
		if ( t >= 100 && t < 110 )
		{
			in.actions = pistol;
		}
		else if ( t >= 150 && t < 400 && ( t % 20 ) < 3 )
		{
			// Held for a few ticks like a real click, so one late input cannot swallow it.
			in.actions = fire;
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.actions = ( tick % 40 ) < 3 ? spawn : 0;
		return in;
	};
	Bot& shooter = h.bots[0];
	Bot& target = h.bots[1];

	bool sawDeadOnClient = false;
	bool sawRagdollOnClient = false;
	uint32_t targetEventsSeen = 0;
	int killedEvent = schema.FindEvent( "combat.killed" );
	const BoardField* deadField = schema.FindField( "combat.dead" );
	h.RunUntil( 9.0, [&]( double ) {
		RollbackSession* s = target.client->Session();
		if ( s == nullptr || target.client->Schema().fields.empty() )
		{
			return;
		}
		Simulation& sim = s->Sim();
		uint32_t me = sim.PlayerNetId( target.client->Slot() );
		if ( me != 0 && sim.BoardValue( me, deadField->slot ) == 1 )
		{
			sawDeadOnClient = true;
		}
		for ( const auto& r : sim.Entities() )
		{
			sawRagdollOnClient |= flecs::entity( sim.World(), r.entity ).has<Ragdoll>();
		}
		const SimGlobals& g = sim.Globals();
		for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
		{
			const ModEventRecord& e = g.modEvents[i];
			if ( int( e.type ) == killedEvent && e.netIdB == me )
			{
				++targetEventsSeen;
			}
		}
	} );
	h.Report();

	Simulation& server = h.server.Sim();
	uint32_t shooterId = server.PlayerNetId( shooter.client->Slot() );
	uint32_t targetId = server.PlayerNetId( target.client->Slot() );
	const BoardField* kills = schema.FindField( "combat.kills" );
	const BoardField* deaths = schema.FindField( "combat.deaths" );
	std::printf( "    shooter kills %d, target deaths %d, target health %d\n", server.BoardValue( shooterId, kills->slot ),
				 server.BoardValue( targetId, deaths->slot ), server.BoardValue( targetId, schema.FindField( "combat.health" )->slot ) );
	CHECK( server.BoardValue( shooterId, kills->slot ) >= 1 );
	CHECK( server.BoardValue( targetId, deaths->slot ) >= 1 );
	// The pistol out means aiming, in the pose itself: the shooter aims, the target does not.
	CHECK( server.EntityAnimState( shooterId ) != nullptr && server.EntityAnimState( shooterId )->aiming == 1 );
	CHECK( server.EntityAnimState( targetId ) != nullptr && server.EntityAnimState( targetId )->aiming == 0 );
	CHECK( sawDeadOnClient );
	CHECK( sawRagdollOnClient );
	CHECK( targetEventsSeen > 0 );
	// Respawned by the time the session ends.
	CHECK( server.PlayerCharacter( target.client->Slot() )->dead == 0 );

	// The target's spawn presses made props through the props mod.
	uint32_t owned = 0;
	for ( const auto& r : server.Entities() )
	{
		const Prop* p = flecs::entity( server.World(), r.entity ).try_get<Prop>();
		owned += p != nullptr && p->owner == targetId ? 1 : 0;
	}
	std::printf( "    target owns %u props\n", owned );
	CHECK( owned > 0 );

	for ( Bot& b : h.bots )
	{
		int compared = 0;
		CHECK( h.CompareWithServer( b, compared ) == 0 );
		CHECK( compared > 100 );
		CHECK( b.client->GetStats().desyncs == 0 );
		CHECK( b.client->GetStats().checksumsVerified > 0 );
	}
}

// A client that stalls for a moment (a hitch, a dragged window) comes back behind the server. It must
// catch up quickly: until it does, every input it sends arrives late and the server drops presses.
void TestStallRecovery()
{
	Harness h( 47795 );
	h.AddBot();
	h.AddBot();
	h.RunUntil( 2.0 );

	// Stall the second bot for 0.4 s: the server keeps ticking, the bot does nothing.
	Bot& stalled = h.bots[1];
	double resumeAt = h.Now() + 0.4;
	while ( h.Now() < resumeAt )
	{
		double now = h.Now();
		h.server.Update( now );
		h.bots[0].client->Update( now, [&]( uint32_t tick ) { return h.bots[0].Sample( tick ); } );
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}

	// Let it notice, then count what arrives late over the next second.
	h.RunUntil( h.Now() + 0.25 );
	uint64_t lateBefore = h.server.GetStats().lateInputs;
	h.RunUntil( h.Now() + 1.0 );
	uint64_t late = h.server.GetStats().lateInputs - lateBefore;
	std::printf( "    after a 0.4 s stall: %llu late inputs in the next second, clock error %.1f ticks\n",
				 (unsigned long long)late, stalled.client->GetStats().tickError );
	h.Report();
	if ( kTimingChecks )
	{
		CHECK( late < 10 );
	}
	CHECK( stalled.client->GetStats().desyncs == 0 );
}

// Names: cleaned up by the server, made unique, and known to every client by slot.
void TestNames()
{
	using namespace net;
	CHECK( SanitizeName( "  Sam  ", 0 ) == "Sam" );
	CHECK( SanitizeName( "a\tb\nc", 0 ) == "abc" );
	CHECK( SanitizeName( "", 4 ) == "Player 5" );
	CHECK( SanitizeName( std::string( 40, 'x' ), 0 ).size() == kMaxPlayerName );
	// A cut never lands inside a UTF-8 character ("\xc3\xa9" is one).
	std::string accents;
	for ( int i = 0; i < 20; ++i )
	{
		accents += "\xc3\xa9";
	}
	std::string cut = SanitizeName( accents, 0 );
	CHECK( cut.size() % 2 == 0 && cut.size() <= kMaxPlayerName );

	Harness h( 47797 );
	h.AddBot( 8, "Sam" );
	h.AddBot( 8, "Sam" );
	h.AddBot( 8, "\x01" );
	h.RunUntil( 2.0 );
	for ( Bot& b : h.bots )
	{
		const auto& names = b.client->Names();
		std::printf( "    bot sees: [%s] [%s] [%s]\n", names[0].c_str(), names[1].c_str(), names[2].c_str() );
		CHECK( names[0] == "Sam" );
		CHECK( names[1] == "Sam (2)" );
		CHECK( names[2] == "Player 3" );
	}

	// A player leaving is gone from everyone's roster.
	h.bots[1].client->DropConnectionHard();
	h.bots.erase( h.bots.begin() + 1 );
	h.RunUntil( h.Now() + 18.0 ); // up to 6 s to notice the drop, then the 10 s reconnect grace
	CHECK( h.bots[0].client->Names()[1].empty() );
}

// A look published again while the server runs: the item players need changes for everyone
// connected (their schema says the new one, and says it changed) and for whoever joins later; the
// simulation does not notice. An item of a mod the server does not announce is not taken.
void TestItemsReplaced()
{
	using namespace net;
	const std::string first( 64, 'a' ), second( 64, 'b' );
	{
		MsgItems msg, back;
		msg.items = { { "melee", first }, { "pistol", second } };
		std::vector<uint8_t> bytes;
		Encode( msg, bytes );
		ByteReader r( bytes.data(), bytes.size() );
		CHECK( ReadType( r ) == MsgType::Items && Decode( r, back ) && back.items == msg.items );
	}
	Harness h( 47891, {}, {}, {}, [&]( ServerOptions& options ) { options.items = { { "melee", first }, { "pistol", first } }; } );
	h.AddBot();
	h.RunUntil( 1.5 );
	uint64_t before = h.bots[0].client->SchemaGeneration();
	CHECK( h.bots[0].client->Schema().items.size() == 2 && h.bots[0].client->Schema().items[0].sha256 == first );
	CHECK( h.server.ReplaceItem( { "nothing", second } ) == false );
	CHECK( h.server.ReplaceItem( { "melee", second } ) );
	h.RunUntil( h.Now() + 1.0 );
	CHECK( h.bots[0].client->SchemaGeneration() == before + 1 );
	CHECK( h.bots[0].client->Schema().items[0].sha256 == second && h.bots[0].client->Schema().items[1].sha256 == first );
	h.AddBot();
	h.RunUntil( h.Now() + 1.5 );
	CHECK( h.bots[1].client->Schema().items[0].sha256 == second );
	h.Report();
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// Deathmatch rounds end to end: kills score, the limit ends the round, everyone is frozen through
// the intermission, and the next round starts clean. Clients agree with the server throughout.
// SHA-256 test vectors, and a character item read the way the server reads one: from the zip
// players have, checked against its hash.
void TestCharacterItem()
{
	CHECK( Sha256Hex( "", 0 ) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" );
	CHECK( Sha256Hex( "abc", 3 ) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" );
	const char* twoBlocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; // 56 bytes: padding spills over
	CHECK( Sha256Hex( twoBlocks, std::strlen( twoBlocks ) ) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" );

	namespace fs = std::filesystem;
	fs::path dir = fs::temp_directory_path() / "cinderbox_character_test";
	fs::remove_all( dir );
	fs::create_directories( dir / "baked" );
	CHECK( anim::AnimSet::CreateProcedural()->Save( ( dir / "baked" ).string() ) );

	auto makeZip = [&]( bool withHitboxes ) {
		mz_zip_archive zip = {};
		mz_zip_writer_init_heap( &zip, 0, 0 );
		for ( const auto& entry : fs::directory_iterator( dir / "baked" ) )
		{
			std::string bytes;
			anim::DiskReader( ( dir / "baked" ).string() )( entry.path().filename().string(), bytes );
			std::string name = "characters/robot/" + entry.path().filename().string();
			mz_zip_writer_add_mem( &zip, name.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION );
		}
		if ( withHitboxes )
		{
			std::string text = anim::FormatHitboxes( anim::DefaultHitboxes() );
			mz_zip_writer_add_mem( &zip, "characters/robot/hitboxes.cfg", text.data(), text.size(), MZ_DEFAULT_COMPRESSION );
		}
		mz_zip_writer_add_mem( &zip, "characters/robot/character.tscn", "[gd_scene]", 10, 0 );
		void* data = nullptr;
		size_t size = 0;
		mz_zip_writer_finalize_heap_archive( &zip, &data, &size );
		std::string bytes( static_cast<const char*>( data ), size );
		mz_zip_writer_end( &zip );
		return bytes;
	};
	auto install = [&]( const std::string& bytes, ModItem& item ) {
		item.mod = "robot";
		item.sha256 = Sha256Hex( bytes.data(), bytes.size() );
		fs::path path = dir / "workshop" / "robot" / ( item.sha256 + ".zip" );
		fs::create_directories( path.parent_path() );
		std::ofstream( path, std::ios::binary ).write( bytes.data(), std::streamsize( bytes.size() ) );
		return path.string();
	};

	ModItem item;
	std::string path = install( makeZip( true ), item );
	std::string error, warnings;
	auto character = LoadCharacterItem( path, item, error, warnings );
	CHECK( character != nullptr );
	if ( character != nullptr )
	{
		CHECK( character->name == "robot" );
		CHECK( character->hitboxes.boxes.size() == anim::DefaultHitboxes().boxes.size() );
		CHECK( character->animations->Skeleton().num_joints() == anim::AnimSet::CreateProcedural()->Skeleton().num_joints() );
	}

	// Not the file the manifest names: refused.
	ModItem wrong = item;
	wrong.sha256[0] = wrong.sha256[0] == '0' ? '1' : '0';
	CHECK( LoadCharacterItem( path, wrong, error, warnings ) == nullptr );
	CHECK( error.find( "not the item" ) != std::string::npos );

	// No hitboxes: refused.
	ModItem bare;
	std::string barePath = install( makeZip( false ), bare );
	CHECK( LoadCharacterItem( barePath, bare, error, warnings ) == nullptr );
	CHECK( error.find( "hitboxes.cfg" ) != std::string::npos );

	fs::remove_all( dir );
}

// Hitboxes in a session: aiming at the head does the pistol's head damage (x2 by default), so the
// target falls in two hits instead of four.
// The sneak mod: holding the crouch key swaps the player's Base layer for the mod's pack (loaded
// from its client project, fitted to the mannequin), in the server's simulation and so in its hit
// tests; letting go restores it. Bots follow without desyncs.
// Items in the world, end to end: the pickup mod drops one of every item kind around the spawn;
// slot 0 takes out the bat, throws it (G), walks after it and presses E until it holds something
// again. The bat takes holding (its mod's "pickup.hold_seconds"): a tap does nothing, a held key
// fills "pickup.progress" and then takes it. The thrown bat's physics runs on the server and on
// both clients, which must agree.
void TestPickup()
{
	Harness h( 47804, {}, {}, { { "pickup.spawn_each", "1" } } );
	const ModSchema& schema = h.server.Schema();
	ActionBits bat = kKeySlot2;
	uint16_t pickup = schema.ActionMask( "pickup" );
	uint16_t drop = schema.ActionMask( "drop" );
	const BoardField* target = schema.FindField( "pickup.target" );
	const BoardField* progress = schema.FindField( "pickup.since" );
	const BoardField* hold = schema.FindField( "pickup.hold" );
	CHECK( bat != 0 && pickup != 0 && drop != 0 && target != nullptr && progress != nullptr && hold != nullptr &&
		   schema.itemKinds.size() >= 2 );
	if ( target == nullptr || progress == nullptr || hold == nullptr )
	{
		return;
	}
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		if ( tick >= 100 && tick < 110 )
		{
			in.actions = bat;
		}
		else if ( tick >= 170 && tick < 175 )
		{
			in.actions = drop;
		}
		else if ( tick >= 200 && tick < 420 )
		{
			in.moveForward = tick < 272 ? 127 : 0; // up to where the thrown bat came to lie
			// Taps first (too short for a bat), then holds.
			in.actions = tick < 320 ? ( ( tick % 20 ) < 2 ? pickup : 0 ) : ( ( tick % 60 ) < 50 ? pickup : 0 );
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; };

	Simulation& server = h.server.Sim();
	auto lying = [&]() {
		int n = 0;
		for ( const Simulation::EntityRef& r : server.Entities() )
		{
			const HeldItem* item = flecs::entity( server.World(), r.entity ).try_get<HeldItem>();
			n += item != nullptr && item->holder == 0 ? 1 : 0;
		}
		return n;
	};
	int mostLying = 0;
	bool heldBat = false;
	bool thrown = false;
	bool targeted = false;
	bool heldAgain = false;
	bool tappedInVain = false;
	bool halfway = false;
	bool needsHolding = false;
	int sinceChanges = 0;
	int32_t lastSince = 0;
	h.RunUntil( 8.0, [&]( double ) {
		uint32_t me = server.PlayerNetId( h.bots[0].client->Slot() );
		uint32_t inHand = server.HeldItemOf( me, kSocketRightHand );
		mostLying = std::max( mostLying, lying() );
		heldBat |= inHand != 0 && server.Tick() < 170;
		thrown |= heldBat && inHand == 0 && server.Tick() > 175 && server.Tick() < 200;
		targeted |= server.BoardValue( me, target->slot ) != 0;
		heldAgain |= thrown && inHand != 0 && server.Tick() > 200;
		// Taps (before tick 320) never take the bat; a held key shows progress on the way.
		tappedInVain |= server.Tick() > 300 && server.Tick() < 320 && inHand == 0 && server.BoardValue( me, target->slot ) != 0;
		// What a look draws: from the tick the hold began, over the time it takes.
		int32_t began = server.BoardValue( me, progress->slot );
		float takes = BoardToFloat( server.BoardValue( me, hold->slot ) ) * float( server.Config().tickRate );
		float fill = began > 0 && takes > 0.0f ? float( int32_t( server.Tick() ) - began ) / takes : 0.0f;
		sinceChanges += began != lastSince ? 1 : 0;
		lastSince = began;
		halfway |= fill > 0.2f && fill < 0.9f;
		needsHolding |= BoardToFloat( server.BoardValue( me, hold->slot ) ) > 0.4f;
	} );
	h.Report();
	{
		// Where the thrown bats came to lie from the player (the pickup's reach is 1.5 m along the ground).
		uint32_t me = server.PlayerNetId( h.bots[0].client->Slot() );
		const Transform* at = server.EntityTransform( me );
		for ( const Simulation::EntityRef& r : server.Entities() )
		{
			flecs::entity e( server.World(), r.entity );
			const HeldItem* item = e.try_get<HeldItem>();
			const Transform* where = e.try_get<Transform>();
			if ( item != nullptr && item->holder == 0 && where != nullptr && at != nullptr && int( item->kind ) == schema.FindItemKind( "melee.bat" ) )
			{
				std::printf( "    a bat lies %.2f m ahead and %.2f m to the side of the player, %.2f m up\n", where->position.z - at->position.z,
							 where->position.x - at->position.x, where->position.y );
			}
		}
	}
	std::printf( "    most lying %d, bat held %d, thrown %d, targeted %d, held again %d\n", mostLying, int( heldBat ), int( thrown ),
				 int( targeted ), int( heldAgain ) );
	// (One of every kind but the two grenades, which are not for picking up and are not scattered.)
	CHECK( mostLying >= int( schema.itemKinds.size() ) - 2 );
	// The bat's body is the one authored in its scene and baked (not the default small box).
	int batKind = schema.FindItemKind( "melee.bat" );
	CHECK( batKind >= 0 && std::max( schema.itemShapes[size_t( batKind )].half.x, schema.itemShapes[size_t( batKind )].half.z ) > 0.3f && schema.itemShapes[size_t( batKind )].mass > 1.0f );
	CHECK( heldBat && thrown && targeted && heldAgain );
	std::printf( "    taps did nothing %d, \"pickup.hold\" said so %d, progress seen on the way %d\n", int( tappedInVain ),
				 int( needsHolding ), int( halfway ) );
	CHECK( tappedInVain && needsHolding && halfway );
	// The board changes when a hold starts and ends, not while it runs.
	std::printf( "    \"pickup.since\" changed %d times\n", sinceChanges );
	CHECK( sinceChanges >= 2 && sinceChanges < 40 );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// The inventory: a life starts with a pistol (slot 1) and a bat (slot 2), carried but put away.
// Switching slots moves items between the hand and stowed and never drops or makes one. A slot is a
// place, not a kind's: picking up a bat that lies in the world while carrying one puts it in the
// free slot and into the hand, and the first stays in its own. Each bat comes out by its own slot's
// key, the one asked for. A thrown bat leaves its slot empty and the other where it was. The number
// of items never changes. (What a life starts with is counted, not assumed: the rifle is local, and
// a checkout without it starts with one item fewer.)
void TestInventory()
{
	// (A grid of 8 x 1, the server's options: room for the bat picked up.)
	Harness h( 47805, {}, {}, { { "pickup.spawn_each", "1" }, { "inventory.columns", "8" }, { "inventory.rows", "1" } } );
	const ModSchema& schema = h.server.Schema();
	CHECK( h.server.Sim().Config().slots == 8 );
	ActionBits gunSlot = kKeySlot1;
	ActionBits batSlot = kKeySlot2;
	// The slot the picked-up bat went to (the first free one), as its key is in a player's input.
	std::atomic<int> pickedSlot{ -1 };
	ActionBits pickup = schema.ActionMask( "pickup" );
	ActionBits drop = schema.ActionMask( "drop" );
	int gunKind = schema.FindItemKind( "gun.pistol" );
	int batKind = schema.FindItemKind( "melee.bat" );
	CHECK( gunSlot != 0 && batSlot != 0 && pickup != 0 && drop != 0 && gunKind >= 0 && batKind >= 0 );
	if ( gunKind < 0 || batKind < 0 )
	{
		return;
	}
	Simulation& server = h.server.Sim();
	struct Count
	{
		int lying = 0;
		int carried = 0; // by `holder`
		int stowed = 0;	 // of those
		int total = 0;
	};
	auto count = [&]( uint32_t holder ) {
		Count c;
		for ( const Simulation::EntityRef& r : server.Entities() )
		{
			const HeldItem* item = flecs::entity( server.World(), r.entity ).try_get<HeldItem>();
			if ( item == nullptr )
			{
				continue;
			}
			c.total += 1;
			c.lying += item->holder == 0 ? 1 : 0;
			c.carried += holder != 0 && item->holder == holder ? 1 : 0;
			c.stowed += holder != 0 && item->holder == holder && item->stowed != 0 ? 1 : 0;
		}
		return c;
	};
	auto kindOf = [&]( uint32_t netId ) {
		const HeldItem* item = netId != 0 ? server.FindEntity( netId ).try_get<HeldItem>() : nullptr;
		return item != nullptr ? int( item->kind ) : -1;
	};
	// The bat the pickup mod left lying at the start.
	uint32_t lyingBat = 0;
	bool reached = false;
	h.AddBot().script = [&, gunSlot, batSlot, pickup, drop]( uint32_t tick ) {
		PlayerInput in;
		uint32_t me = server.PlayerNetId( h.bots[0].client->Slot() );
		if ( ( tick >= 100 && tick < 110 ) || ( tick >= 560 && tick < 570 ) )
		{
			in.actions = batSlot;
		}
		else if ( ( tick >= 150 && tick < 160 ) || ( tick >= 520 && tick < 530 ) || ( tick >= 670 && tick < 680 ) )
		{
			in.actions = gunSlot;
		}
		else if ( tick >= 600 && tick < 605 && pickedSlot >= 0 )
		{
			in.slotKeys = uint16_t( 1u << pickedSlot );
		}
		else if ( tick >= 640 && tick < 645 )
		{
			in.actions = drop;
		}
		else if ( tick >= 200 && tick < 500 && reached == false && me != 0 && lyingBat != 0 )
		{
			// Walk to the lying bat, looking at it, pressing E in pulses (a bat takes holding).
			const Transform* at = server.EntityTransform( me );
			const Transform* to = server.EntityTransform( lyingBat );
			if ( at != nullptr && to != nullptr )
			{
				float dx = to->position.x - at->position.x;
				float dz = to->position.z - at->position.z;
				in.cameraYaw = detmath::RadiansToYaw( std::atan2( dx, dz ) );
				in.moveForward = dx * dx + dz * dz > 0.8f * 0.8f ? 127 : 0;
				in.actions = ( tick % 60 ) < 50 ? pickup : 0;
			}
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; };

	bool startsStowed = false;
	bool batOut = false;
	bool gunOut = false;
	bool droppedBySwitching = false;
	bool bothKept = false;
	bool awayAndKept = false;
	bool ownBatOut = false;
	bool pickedOut = false;
	bool thrown = false;
	bool gunAfterThrow = false;
	bool slotsAtStart = false;
	bool slotsWithTwo = false;
	bool slotsAfterThrow = false;
	uint32_t ownBat = 0;
	int start = 0;		 // what a life starts with
	int lyingAtStart = 0; // what the pickup mod left lying: one of each kind
	int startTotal = 0;
	int leastTotal = 1000;
	int mostTotal = 0;
	h.RunUntil( 12.5, [&]( double ) {
		uint32_t tick = server.Tick();
		uint32_t me = server.PlayerNetId( h.bots[0].client->Slot() );
		if ( me == 0 )
		{
			return;
		}
		uint32_t inHand = server.HeldItemOf( me, kSocketRightHand );
		Count c = count( me );
		if ( tick > 80 )
		{
			startTotal = startTotal == 0 ? c.total : startTotal;
			leastTotal = std::min( leastTotal, c.total );
			mostTotal = std::max( mostTotal, c.total );
		}
		if ( lyingBat == 0 )
		{
			for ( const Simulation::EntityRef& r : server.Entities() )
			{
				const HeldItem* item = flecs::entity( server.World(), r.entity ).try_get<HeldItem>();
				lyingBat = item != nullptr && item->holder == 0 && int( item->kind ) == batKind ? r.netId : lyingBat;
			}
		}
		if ( tick > 80 && tick < 100 && inHand == 0 && c.carried > 0 && c.stowed == c.carried )
		{
			// The items of every mod the checkout has (tests run every mod), and one of each lying about.
			startsStowed = c.carried >= 2 && c.carried <= 7 && c.lying >= 2;
			start = c.carried;
			lyingAtStart = c.lying;
		}
		if ( tick > 130 && tick < 150 && kindOf( inHand ) == batKind )
		{
			batOut = c.carried == start && c.stowed == start - 1;
			ownBat = inHand;
		}
		// What is in each slot is the simulation's to say (the look reads it from there).
		uint32_t pistolSlot = server.SlotItem( me, 0 );
		uint32_t batsSlot = server.SlotItem( me, 1 );
		const HeldItem* picked = lyingBat != 0 ? server.FindEntity( lyingBat ).try_get<HeldItem>() : nullptr;
		if ( picked != nullptr && picked->holder == me && picked->slot != kNoSlot )
		{
			pickedSlot = int( picked->slot );
		}
		uint32_t freeSlot = pickedSlot >= 0 ? server.SlotItem( me, uint8_t( pickedSlot.load() ) ) : 0;
		slotsAtStart |= tick > 130 && tick < 150 && ownBat != 0 && batsSlot == ownBat && kindOf( pistolSlot ) == gunKind;
		slotsWithTwo |= reached && tick < 640 && batsSlot == ownBat && freeSlot == lyingBat && pickedSlot > 1 && kindOf( pistolSlot ) == gunKind;
		slotsAfterThrow |= tick > 700 && batsSlot == ownBat && pickedSlot > 1 && freeSlot == 0 && kindOf( pistolSlot ) == gunKind;
		gunOut |= tick > 180 && tick < 200 && kindOf( inHand ) == gunKind && c.carried == start && c.stowed == start - 1;
		droppedBySwitching |= tick > 100 && tick < 200 && c.lying != lyingAtStart;
		// The lying bat is in the hand: picked up, into the free slot; the first bat is still carried.
		if ( inHand == lyingBat && lyingBat != 0 && tick < 520 )
		{
			reached = true;
			const HeldItem* old = ownBat != 0 ? server.FindEntity( ownBat ).try_get<HeldItem>() : nullptr;
			// (By the bats themselves: with more than one free slot the bot may pick up something else too.)
			bothKept |= old != nullptr && old->holder == me && old->stowed != 0 && c.carried > start;
		}
		// The pistol's slot: the picked-up bat is put away and kept.
		awayAndKept |= tick > 545 && tick < 560 && kindOf( inHand ) == gunKind && picked != nullptr && picked->holder == me && picked->stowed != 0;
		// The bats' own keys: each brings out its own.
		ownBatOut |= tick > 585 && tick < 600 && inHand == ownBat && ownBat != 0 && picked != nullptr && picked->stowed != 0;
		pickedOut |= tick > 625 && tick < 640 && inHand == lyingBat && reached;
		thrown |= tick > 655 && tick < 670 && inHand == 0 && picked != nullptr && picked->holder == 0 && ownBat != 0 &&
				  server.FindEntity( ownBat ).get<HeldItem>().holder == me;
		gunAfterThrow |= tick > 690 && tick < 710 && kindOf( inHand ) == gunKind;
	} );
	h.Report();
	std::printf( "    a life starts with %d items (%d lying about); starts stowed %d, bat out %d, pistol out %d, dropped by switching %d\n", start,
				 lyingAtStart, int( startsStowed ), int( batOut ), int( gunOut ), int( droppedBySwitching ) );
	std::printf( "    picked up the lying bat %d, both bats kept %d; put away and kept %d; the first bat's key %d, the second's %d\n", int( reached ),
				 int( bothKept ), int( awayAndKept ), int( ownBatOut ), int( pickedOut ) );
	std::printf( "    thrown %d, pistol after %d; items %d at the start, %d to %d throughout\n", int( thrown ), int( gunAfterThrow ), startTotal,
				 leastTotal, mostTotal );
	CHECK( startsStowed && batOut && gunOut );
	CHECK( droppedBySwitching == false );
	CHECK( reached && bothKept );
	CHECK( awayAndKept && ownBatOut && pickedOut );
	CHECK( thrown && gunAfterThrow );
	std::printf( "    the slots: at the start %d, with two bats %d, after the throw %d\n", int( slotsAtStart ), int( slotsWithTwo ),
				 int( slotsAfterThrow ) );
	CHECK( slotsAtStart && slotsWithTwo && slotsAfterThrow );
	CHECK( startTotal == lyingAtStart + 2 * start && leastTotal == startTotal && mostTotal == startTotal ); // none made, none lost
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// An item's body as its scene's bake writes it (items/<kind>.cfg): parsed, and refused when it is
// not a body the physics can build.
void TestItemShapes()
{
	ItemShape shape;
	std::string error;
	CHECK( ParseItemShape( "# baked\nshape box\nhalf 0.035 0.035 0.41\ncenter 0 0 -0.31\nmass 1.1\n", shape, error ) );
	CHECK( shape.kind == 0 && shape.half.z == 0.41f && shape.center.z == -0.31f && shape.mass == 1.1f );
	CHECK( ParseItemShape( "shape sphere\r\nhalf 0.2 0.2 0.2\r\n", shape, error ) && shape.kind == 1 && shape.mass == 1.0f );
	CHECK( shape.friction == 0.6f && shape.restitution == 0.0f ); // the usual, when the file does not say
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nfriction 0.05\nbounce 0.7\n", shape, error ) && shape.friction == 0.05f &&
		   shape.restitution == 0.7f );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nbounce 1.5\n", shape, error ) == false );
	CHECK( ParseItemShape( "shape sphere\nhalf 0.1 0.1 0.1\nrolling 0.08\ndrag 1.5\n", shape, error ) && shape.rolling == 0.08f &&
		   shape.drag == 1.5f );
	CHECK( ParseItemShape( "shape sphere\nhalf 0.1 0.1 0.1\nrolling 3\n", shape, error ) == false );
	CHECK( ParseItemShape( "shape sphere\nhalf 0.1 0.1 0.1\ndrag -1\n", shape, error ) == false );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nfriction -1\n", shape, error ) == false );
	CHECK( ParseItemShape( "shape capsule\nhalf 0.1 0.1 0.1\n", shape, error ) == false && error.empty() == false );
	CHECK( ParseItemShape( "shape box\n", shape, error ) == false );				   // no size
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1\n", shape, error ) == false );	   // two numbers
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 9\n", shape, error ) == false );   // 18 m long
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nmass 0\n", shape, error ) == false );
	CHECK( ParseItemShape( "shape box\nhalf nan 0.1 0.1\n", shape, error ) == false );
	// The ones the mods ship.
	const std::string root = CB_SOURCE_DIR;
	CHECK( LoadItemShapeFolder( root + "/server_mods/melee/client", "melee.bat", shape, error ) && std::max( shape.half.x, shape.half.z ) > 0.3f ); // long, along how it is carried
	CHECK( LoadItemShapeFolder( root + "/server_mods/gun/client", "gun.pistol", shape, error ) && shape.mass < 1.0f );
	CHECK( LoadItemShapeFolder( root + "/server_mods/melee/client", "no.such", shape, error ) == false );
	// Properties authored on the body ride along: the bat's hold time is in its scene, not in its mod.
	ItemProperties properties;
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nproperty pickup.hold_seconds 0.75\nproperty a.b 2\n", shape, error, &properties ) );
	CHECK( properties.size() == 2 && properties["pickup.hold_seconds"] == 0.75f && properties["a.b"] == 2.0f );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nproperty lonely\n", shape, error, &properties ) == false );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nproperty x nan\n", shape, error, &properties ) == false );
	// A grip: where the other hand holds the item, baked from the marker its CbItem names.
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\n", shape, error ) && shape.grip == 0 );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\ngrip -0.05 0.02 0.1 0 0 0 1 0\n", shape, error ) );
	CHECK( shape.grip == 1 && shape.gripPosition.x == -0.05f && shape.gripPosition.z == 0.1f && shape.gripRotation[3] == 1.0f );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\ngrip 0 0 0.1 0 0.7071 0 0.7071 1\n", shape, error ) && shape.grip == 2 );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\ngrip 0 0 0 0 0 0 1 2\n", shape, error ) && shape.grip == 3 ); // as animated
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\ngrip 0 0 0 0 0 0 1 3\n", shape, error ) == false );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\ngrip 0 0 0.1\n", shape, error ) == false );			   // no rotation
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\ngrip 0 0 5 0 0 0 1 0\n", shape, error ) == false );   // out of reach
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\ngrip 0 0 0.1 0 0 0 3 0\n", shape, error ) == false ); // not a rotation
	// The body's turn in the frame the item is carried in: a carrying grip may be turned any way.
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\n", shape, error ) && shape.turn[3] == 1.0f );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.4\nturn 0 0.70711 0 0.70711\n", shape, error ) && shape.turn[1] > 0.7f && shape.turn[3] > 0.7f );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nturn 0 0 0\n", shape, error ) == false );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nturn 0 0 0 5\n", shape, error ) == false );
	// It travels in the schema, so every client poses the same arms.
	{
		ModSchema schema;
		schema.itemKinds = { "test.rifle" };
		ItemShape rifle;
		CHECK( ParseItemShape( "shape box\nhalf 0.03 0.08 0.4\nturn 0.5 0.5 0.5 0.5\ngrip 0 -0.02 -0.3 0 0.7071 0 0.7071 1\n", rifle, error ) );
		schema.itemShapes = { rifle };
		std::vector<uint8_t> bytes;
		EncodeSchema( schema, bytes );
		ModSchema back;
		CHECK( DecodeSchema( bytes.data(), bytes.size(), back ) );
		CHECK( back.itemShapes.size() == 1 && back.itemShapes[0] == rifle && back.itemShapes[0].grip == 2 && back.itemShapes[0].turn[0] == 0.5f );
	}
	// The shipped pistol and bat are held with both hands.
	CHECK( LoadItemShapeFolder( root + "/server_mods/gun/client", "gun.pistol", shape, error ) && shape.grip == 3 ); // as its animations have the hands
	CHECK( LoadItemShapeFolder( root + "/server_mods/melee/client", "melee.bat", shape, error ) && shape.grip != 0 );
	properties.clear();
	CHECK( LoadItemShapeFolder( root + "/server_mods/melee/client", "melee.bat", shape, error, &properties ) );
	CHECK( properties.count( "pickup.hold_seconds" ) == 1 && properties["pickup.hold_seconds"] == 0.5f );
}

// An item's body of several shapes (items/<kind>.cfg "part" lines, as the bake writes them for an
// item with more than one CollisionShape3D, or one a box or ball cannot say): parsed, its one shape
// the box that holds them; refused when a part is not a body; through the schema unchanged; and in
// the world one body of all of them, weighing what the file says, whatever part a ray meets.
// The camera against the level as it is (LevelQuery, what a live or replay source answers the
// viewer's camera with): a terrain hill and a triangle-mesh wall stop the camera's ball where their
// surfaces are, not at their bounds; open sky lets it go its whole way; a prop never stops it;
// there is no answer before a level, and a new level replaces the old.
void TestLevelQuery()
{
	LevelLayout map;
	map.name = "camera";
	map.spawnCenter = { 0.0f, 1.5f, 20.0f };
	map.spawnRadius = 2.0f;
	LevelHeightField hill;
	hill.countX = 9;
	hill.countZ = 9;
	for ( int z = 0; z < 9; ++z )
	{
		for ( int x = 0; x < 9; ++x )
		{
			hill.heights.push_back( 4.0f - 0.25f * float( ( x - 4 ) * ( x - 4 ) + ( z - 4 ) * ( z - 4 ) ) );
		}
	}
	map.heightFields.push_back( hill );
	map.statics.push_back( { { -4.0f, 0.0f, -4.0f }, { 8.0f, 4.0f, 8.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::HeightField, 0 } );
	// A wall of two triangles at z 10, facing -Z, 6 m wide and 4 m high.
	LevelMesh wall;
	wall.vertices = { { -3.0f, 0.0f, 0.0f }, { 3.0f, 0.0f, 0.0f }, { 3.0f, 4.0f, 0.0f }, { -3.0f, 4.0f, 0.0f } };
	wall.indices = { 0, 2, 1, 0, 3, 2 };
	map.meshes.push_back( wall );
	map.statics.push_back( { { 0.0f, 0.0f, 10.0f }, { 3.0f, 4.0f, 0.001f }, 0.0f, 0.0f, 0.0f, ShapeKind::Mesh, 0 } );
	map.props.push_back( { ShapeKind::Box, { 0.0f, 1.0f, -10.0f }, { 1.0f, 1.0f, 1.0f } } ); // in the way of the last look
	QuantizeLayout( map );

	LevelQuery query;
	CHECK( query.FreeDistance( { 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f, 0.2f ) < 0.0f ); // no level yet
	query.SetLevel( map, 1 );
	const float onHill = query.FreeDistance( { 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f, 0.2f );			 // down onto its top (4 m)
	const float onSlope = query.FreeDistance( { 2.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f, 0.2f );		 // its side, 3 m up
	const float atWall = query.FreeDistance( { 0.0f, 2.0f, 5.0f }, { 0.0f, 0.0f, 1.0f }, 20.0f, 0.2f );			 // the wall 5 m ahead
	const float sky = query.FreeDistance( { 0.0f, 10.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 20.0f, 0.2f );				 // up: nothing
	const float pastProp = query.FreeDistance( { 0.0f, 1.0f, -5.0f }, { 0.0f, 0.0f, -1.0f }, 20.0f, 0.2f );		 // through the box
	std::printf( "    the camera's ball stops %.2f m down on the hill's top, %.2f m on its side, %.2f m at the wall; up %.2f m; through a prop %.2f m\n",
				 onHill, onSlope, atWall, sky, pastProp );
	CHECK( std::fabs( onHill - 5.8f ) < 0.05f );  // 10 - 4 - 0.2
	CHECK( std::fabs( onSlope - 6.8f ) < 0.15f ); // 10 - 3 - 0.2 (the cells are flat between points)
	CHECK( std::fabs( atWall - 4.8f ) < 0.02f );
	CHECK( sky == 20.0f && pastProp == 20.0f );
	// A new level: the old one's hill is gone.
	LevelLayout flat;
	flat.name = "flat";
	flat.spawnCenter = { 0.0f, 1.5f, 0.0f };
	flat.spawnRadius = 2.0f;
	flat.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
	QuantizeLayout( flat );
	query.SetLevel( flat, 2 );
	const float onFlat = query.FreeDistance( { 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 20.0f, 0.2f );
	CHECK( std::fabs( onFlat - 9.8f ) < 0.02f );
}

void TestItemParts()
{
	// A hammer: a handle along the item's -Z (a capsule turned from Y), a head, and a wedge (a hull).
	const std::string hammer = "# baked\n"
							   "part capsule 0.03 0.3 0 0 0 -0.3 0.70711 0 0 0.70711\n"
							   "part box 0.12 0.05 0.05 0 0 -0.62 0 0 0 1\n"
							   "part hull 0.06 0.05 0.05 0.18 0 -0.62 0 0 0 1 4 0 -0.05 -0.05 0 0.05 -0.05 0 0 0.05 0.06 0 0\n"
							   "mass 2\n";
	ItemShape shape;
	std::string error;
	CHECK( ParseItemShape( hammer, shape, error ) );
	std::printf( "    the hammer: %zu parts, held in a box of %.2f x %.2f x %.2f\n", shape.parts.size(), shape.half.x, shape.half.y,
				 shape.half.z );
	CHECK( shape.parts.size() == 3 && shape.parts[2].kind == 3 && shape.parts[2].points.size() == 4 && shape.mass == 2.0f );
	CHECK( shape.kind == 0 && shape.center.z == 0.0f && shape.turn[3] == 1.0f && shape.half.z > 0.66f && shape.half.x > 0.23f );
	// Refused: a flat hull, a turn that is no rotation, an unknown kind, too many parts.
	CHECK( ParseItemShape( "part hull 0.1 0.1 0.1 0 0 0 0 0 0 1 4 0 0 0 0.1 0 0 0 0 0.1 0.1 0 0.1\n", shape, error ) == false );
	CHECK( ParseItemShape( "part box 0.1 0.1 0.1 0 0 0 0 0 0 3\n", shape, error ) == false );
	CHECK( ParseItemShape( "part cone 0.1 0.1 0.1 0 0 0 0 0 0 1\n", shape, error ) == false );
	std::string many;
	for ( int i = 0; i < 17; ++i )
	{
		many += "part sphere 0.05 0 0 0 0 0 0 0 0 1\n";
	}
	CHECK( ParseItemShape( many, shape, error ) == false );
	CHECK( ParseItemShape( hammer, shape, error ) );

	// Through the schema as it was written.
	ModSchema schema;
	schema.itemKinds = { "test.hammer" };
	schema.itemShapes = { shape };
	std::vector<uint8_t> bytes;
	EncodeSchema( schema, bytes );
	ModSchema back;
	CHECK( DecodeSchema( bytes.data(), bytes.size(), back ) && back.itemShapes.size() == 1 && back.itemShapes[0] == shape );

	// In the world: dropped from a metre, it lands and lies; it weighs 2 kg; head and handle are it.
	LevelLayout map;
	map.name = "items";
	map.spawnCenter = { 0.0f, 1.5f, 8.0f };
	map.spawnRadius = 2.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 10.0f, 0.5f, 10.0f }, 0.0f, 0.0f } );
	QuantizeLayout( map );
	SimConfig config;
	Simulation sim( config, map );
	sim.SetItemShapes( { shape } );
	InputFrame f;
	SimCommand c;
	c.type = CommandType::SpawnItem;
	c.target = 0;
	c.index = 0;
	c.mode = kNoSocket;
	c.value = 1;
	c.a = { 0.0f, 1.0f, 0.0f };
	f.commands.push_back( c );
	for ( int i = 0; i < 180; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		f.commands.clear();
	}
	uint32_t item = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		item = flecs::entity( sim.World(), r.entity ).has<HeldItem>() ? r.netId : item;
	}
	CHECK( item != 0 );
	flecs::entity e = sim.FindEntity( item );
	CHECK( e.has<PhysicsBody>() );
	const float mass = b3Body_GetMass( sim.BodyOf( e.get<PhysicsBody>() ) );
	const Transform at = *sim.EntityTransform( item );
	b3Vec3 head = b3Add( at.position, b3RotateVector( at.rotation, { 0.0f, 0.0f, -0.62f } ) );
	b3Vec3 grip = b3Add( at.position, b3RotateVector( at.rotation, { 0.0f, 0.0f, -0.1f } ) );
	RayHit onHead, onHandle;
	bool headHit = sim.CastRay( { head.x, 3.0f, head.z }, { 0.0f, -4.0f, 0.0f }, 0, onHead );
	bool handleHit = sim.CastRay( { grip.x, 3.0f, grip.z }, { 0.0f, -4.0f, 0.0f }, 0, onHandle );
	std::printf( "    lying: %.3f kg, its grip %.3f m up; down onto the head %.3f m, onto the handle %.3f m\n", mass, at.position.y,
				 headHit ? onHead.point.y : -1.0f, handleHit ? onHandle.point.y : -1.0f );
	CHECK( std::fabs( mass - 2.0f ) < 0.01f );
	CHECK( at.position.y > 0.0f && at.position.y < 0.2f );
	CHECK( headHit && onHead.netId == item && handleHit && onHandle.netId == item );
	CHECK( onHead.point.y > onHandle.point.y ); // the head stands higher than the handle
}

// An item's pose: a pack's layer the character does not have is an overlay, laid over the body
// (or its bone filter) while its weight says so. The boots and the hook are carried, out of sight:
// their packs take the overlay slots, one for each such layer (the boots' Dash, the hook's left-arm
// Grapple), for as long as the player has them. The boots' dash pose shows while the Dash motion
// is on, on the client the tick the server has it (predicted), and is gone after. The hook's is a
// throw of the left arm: in while its Throwing motion is on (predicted too), out a second later
// (whatever the line does: the pull needs no hands), and at nothing before.
void TestItemPoses()
{
	const std::string root = CB_SOURCE_DIR;
	std::shared_ptr<const CharacterAsset> mannequin;
	Harness h( 47895, {}, {}, {}, [&]( ServerOptions& options ) {
		std::string error, warnings;
		mannequin = LoadCharacterFolder( root + "/godot/characters/mannequin", "mannequin", error, warnings );
		options.character = mannequin;
		options.loadAnimPack = [root]( const std::string& mod, const std::string& pack, std::string& error, std::string& warnings ) {
			return LoadAnimPackFolder( root + "/server_mods/" + mod + "/client", pack, error, warnings );
		};
	} );
	const ModSchema& schema = h.server.Schema();
	// The boots' own key (V): used where they are.
	ActionBits dash = UseKey( schema, "dash.boots" );
	int boots = -1, hook = -1;
	for ( size_t i = 0; i < schema.animPacks.size(); ++i )
	{
		boots = schema.animPacks[i].name == "dash.boots_dash" && schema.animPacks[i].graph.empty() == false ? int( i ) : boots;
		hook = schema.animPacks[i].name == "grapple.hook_arm" && schema.animPacks[i].graph.empty() == false ? int( i ) : hook;
	}
	CHECK( mannequin != nullptr && dash != 0 && boots >= 0 && hook >= 0 );
	if ( mannequin == nullptr || dash == 0 || boots < 0 || hook < 0 )
	{
		return;
	}
	const uint32_t dashAt = 240, throwAt = 300;
	const ActionBits grapple = UseKey( schema, "grapple.hook" ); // the hook's own key (Q)
	CHECK( grapple != 0 );
	Bot& bot = h.AddBot();
	bot.script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.moveForward = 127;
		if ( tick >= dashAt && tick < dashAt + 4 )
		{
			in.actions = dash;
		}
		if ( tick >= throwAt && tick < throwAt + 4 )
		{
			in.actions = grapple;
		}
		return in;
	};

	// The overlay slots of the player, after the character's own layers: which pack, how much.
	struct Seen
	{
		uint8_t source[kOverlaySlots] = {};
		float weight[kOverlaySlots] = {};
	};
	std::map<uint32_t, Seen> truth, predicted;
	auto look = [&]( const Simulation& sim, PlayerSlot slot, std::map<uint32_t, Seen>& into ) {
		uint32_t netId = sim.PlayerNetId( slot );
		AnimStateCopy anim = netId != 0 ? sim.EntityAnimState( netId ) : AnimStateCopy();
		size_t own = sim.Graph() != nullptr ? sim.Graph()->layers.size() : 0;
		if ( anim == nullptr || own == 0 || into.count( sim.Tick() ) != 0 )
		{
			return;
		}
		Seen seen;
		for ( size_t i = 0; i < kOverlaySlots && own + i < anim->graph.size(); ++i )
		{
			seen.source[i] = anim->graph[own + i].source;
			seen.weight[i] = anim->graph[own + i].weight;
		}
		into[sim.Tick()] = seen;
	};
	Simulation& server = h.server.Sim();
	h.server.SetTickObserver( [&] {
		if ( bot.client->State() == ClientState::Playing )
		{
			look( server, bot.client->Slot(), truth );
		}
	} );
	auto script = bot.script;
	bot.script = [&, script]( uint32_t tick ) {
		if ( bot.client->Session() != nullptr && bot.client->State() == ClientState::Playing )
		{
			look( bot.client->Session()->Sim(), bot.client->Slot(), predicted );
		}
		return script( tick );
	};
	h.RunUntil( 7.0 );
	h.server.SetTickObserver( {} );
	h.Report();
	CHECK( bot.client->GetStats().desyncs == 0 );

	const uint32_t before = dashAt - 10, during = dashAt + 6, after = dashAt + 40;
	const uint32_t throwing = throwAt + 20, thrown = throwAt + 75;
	bool have = truth.count( before ) && truth.count( during ) && truth.count( after ) && predicted.count( during ) && truth.count( throwing ) &&
				predicted.count( throwing ) && truth.count( thrown );
	CHECK( have );
	if ( have == false )
	{
		return;
	}
	// Which slot is whose: both packs are in, one each, whichever order.
	int bootsSlot = -1, hookSlot = -1;
	for ( size_t i = 0; i < kOverlaySlots; ++i )
	{
		bootsSlot = truth[before].source[i] == uint8_t( boots + 1 ) ? int( i ) : bootsSlot;
		hookSlot = truth[before].source[i] == uint8_t( hook + 1 ) ? int( i ) : hookSlot;
	}
	std::printf( "    overlays: the boots' pack in slot %d, the hook's in slot %d\n", bootsSlot, hookSlot );
	CHECK( bootsSlot >= 0 && hookSlot >= 0 && bootsSlot != hookSlot );
	if ( bootsSlot < 0 || hookSlot < 0 )
	{
		return;
	}
	std::printf( "    the dash pose: %.2f before, %.2f during (predicted %.2f), %.2f after; the grapple's arm: %.2f\n", truth[before].weight[bootsSlot],
				 truth[during].weight[bootsSlot], predicted[during].weight[bootsSlot], truth[after].weight[bootsSlot], truth[during].weight[hookSlot] );
	CHECK( truth[before].weight[bootsSlot] == 0.0f );
	CHECK( truth[during].weight[bootsSlot] > 0.2f && predicted[during].weight[bootsSlot] == truth[during].weight[bootsSlot] );
	CHECK( truth[after].weight[bootsSlot] == 0.0f );
	CHECK( truth[during].weight[hookSlot] == 0.0f );
	std::printf( "    the grapple's throw: %.2f while throwing (predicted %.2f), %.2f a second later\n", truth[throwing].weight[hookSlot],
				 predicted[throwing].weight[hookSlot], truth[thrown].weight[hookSlot] );
	CHECK( truth[throwing].weight[hookSlot] > 0.9f && predicted[throwing].weight[hookSlot] == truth[throwing].weight[hookSlot] );
	CHECK( truth[thrown].weight[hookSlot] == 0.0f );
	int compared = 0;
	CHECK( h.CompareWithServer( bot, compared ) == 0 );
}

// A held item brings its layers: with the bat out, the player's "UpperBody" layer plays from the
// melee mod's pack (how it is held and swung), and the legs stay the player's own; crouching (the
// sneak mod's swap of "FullBody") plays under it, the bat still held its way; standing up gives the
// player's own legs back; throwing the bat away gives its own upper body back. Clients agree
// throughout.
void TestItemLayers()
{
	const std::string root = CB_SOURCE_DIR;
	std::shared_ptr<const CharacterAsset> mannequin;
	Harness h( 47806, {}, {}, { { "test.mods", "sneak" } }, [&]( ServerOptions& options ) {
		std::string error, warnings;
		mannequin = LoadCharacterFolder( root + "/godot/characters/mannequin", "mannequin", error, warnings );
		options.character = mannequin;
		options.loadAnimPack = [root]( const std::string& mod, const std::string& pack, std::string& error, std::string& warnings ) {
			return LoadAnimPackFolder( root + "/server_mods/" + mod + "/client", pack, error, warnings );
		};
	} );
	const ModSchema& schema = h.server.Schema();
	ActionBits bat = kKeySlot2;
	uint16_t crouch = schema.ActionMask( "crouch" );
	uint16_t drop = schema.ActionMask( "drop" );
	int carry = -1;
	int sneak = -1;
	for ( size_t i = 0; i < schema.animPacks.size(); ++i )
	{
		carry = schema.animPacks[i].name == "melee.hold" && schema.animPacks[i].graph.empty() == false ? int( i ) : carry;
		sneak = schema.animPacks[i].name == "sneak.crouch" && schema.animPacks[i].graph.empty() == false ? int( i ) : sneak;
	}
	CHECK( mannequin != nullptr && bat != 0 && crouch != 0 && drop != 0 && carry >= 0 && sneak >= 0 );
	if ( mannequin == nullptr || carry < 0 || sneak < 0 )
	{
		return;
	}
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		if ( tick >= 100 && tick < 110 )
		{
			in.actions = bat;
		}
		else if ( tick >= 200 && tick < 260 )
		{
			in.actions = crouch;
		}
		else if ( tick >= 320 && tick < 325 )
		{
			in.actions = drop;
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; };

	Simulation& server = h.server.Sim();
	bool sawCarry = false;
	bool sawCrouch = false;
	bool carryAgain = false;
	bool ownAgain = false;
	bool wrong = false;
	h.RunUntil( 7.0, [&]( double ) {
		uint32_t tick = server.Tick();
		AnimStateCopy a = server.EntityAnimState( server.PlayerNetId( h.bots[0].client->Slot() ) );
		if ( a == nullptr )
		{
			return;
		}
		if ( a->graph.size() < 2 )
		{
			return;
		}
		// The mannequin's layers: "FullBody", then "UpperBody".
		uint8_t legs = a->graph[0].source;
		uint8_t upper = a->graph[1].source;
		sawCarry |= tick > 130 && tick < 195 && upper == uint8_t( carry + 1 ) && legs == 0;
		sawCrouch |= tick > 215 && tick < 255 && legs == uint8_t( sneak + 1 ) && upper == uint8_t( carry + 1 );
		carryAgain |= tick > 275 && tick < 315 && upper == uint8_t( carry + 1 ) && legs == 0;
		ownAgain |= tick > 345 && upper == 0 && legs == 0;
		// Never the bat's layer once the bat is gone, never the player's own upper body while it is held.
		wrong |= ( tick > 345 && upper != 0 ) || ( tick > 275 && tick < 315 && upper != uint8_t( carry + 1 ) );
	} );
	h.Report();
	std::printf( "    the bat's upper body %d, a crouch under it %d, standing again %d, its own after the throw %d\n", int( sawCarry ),
				 int( sawCrouch ), int( carryAgain ), int( ownAgain ) );
	CHECK( sawCarry && sawCrouch && carryAgain && ownAgain );
	CHECK( wrong == false );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

void TestSneak()
{
	const std::string root = CB_SOURCE_DIR;
	std::shared_ptr<const CharacterAsset> mannequin;
	Harness h( 47803, {}, {}, { { "test.mods", "sneak" } }, [&]( ServerOptions& options ) {
		std::string error, warnings;
		mannequin = LoadCharacterFolder( root + "/godot/characters/mannequin", "mannequin", error, warnings );
		options.character = mannequin;
		options.loadAnimPack = [root]( const std::string& mod, const std::string& pack, std::string& error, std::string& warnings ) {
			return LoadAnimPackFolder( root + "/server_mods/" + mod + "/client", pack, error, warnings );
		};
	} );
	CHECK( mannequin != nullptr );
	const ModSchema& schema = h.server.Schema();
	uint16_t crouch = schema.ActionMask( "crouch" );
	// The sneak mod's pack, among whatever packs other mods ship.
	int sneakPack = -1;
	for ( size_t i = 0; i < schema.animPacks.size(); ++i )
	{
		sneakPack = schema.animPacks[i].name == "sneak.crouch" ? int( i ) : sneakPack;
	}
	CHECK( crouch != 0 && sneakPack >= 0 && schema.animPacks[size_t( sneakPack )].graph.empty() == false );
	if ( mannequin == nullptr || crouch == 0 || sneakPack < 0 )
	{
		return;
	}

	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		if ( tick >= 120 && tick < 300 )
		{
			in.actions = crouch;
		}
		// Sprinting all along: the crouch slows it by itself.
		in.moveForward = 127;
		in.buttons = BtnSprint;
		return in;
	};
	std::string error, warnings;
	auto graph = CompileAnimGraph( schema.animGraph, schema, error, warnings );
	AnimGraphPacks packs = CompileAnimPacks( schema, warnings );
	CHECK( graph != nullptr && packs.size() == schema.animPacks.size() );
	if ( graph == nullptr || packs.size() != schema.animPacks.size() )
	{
		return;
	}
	// Every pack's clips, fitted to the mannequin, as a client does.
	std::vector<std::shared_ptr<const anim::PackClips>> fitted;
	for ( size_t i = 0; i < packs.size(); ++i )
	{
		// A pack whose look is not in this checkout (the rifle's) is not available: its swaps do nothing.
		if ( schema.animPacks[i].graph.empty() )
		{
			fitted.push_back( nullptr );
			continue;
		}
		auto packSet = LoadAnimPackFolder( root + "/server_mods/" + schema.animPacks[i].mod + "/client", schema.animPacks[i].name, error,
										   warnings );
		CHECK( packs[i] != nullptr && packSet != nullptr );
		if ( packs[i] == nullptr || packSet == nullptr )
		{
			return;
		}
		fitted.push_back( anim::FitPack( packSet, *packs[i], *mannequin->animations, warnings ) );
	}
	anim::PoseEvaluator pose( *mannequin->animations );
	pose.SetGraph( graph, warnings );
	pose.SetPacks( packs, fitted );
	Simulation& server = h.server.Sim();
	float standing = 0.0f, sneaking = 10.0f;
	bool swapped = false, restored = false;
	// The sneak mod slows its player (SetMove) and gives the speeds back.
	float sneakSpeed = 0.0f, runSpeed = 0.0f;
	bool slowed = false, givenBack = false;
	h.RunUntil( 7.0, [&]( double ) {
		uint32_t netId = server.PlayerNetId( h.bots[0].client->Slot() );
		AnimStateCopy a = server.EntityAnimState( netId );
		if ( a == nullptr || a->graph[0].started == 0 )
		{
			return;
		}
		pose.Evaluate( *a );
		float v[4];
		ozz::math::StorePtrU( pose.Models()[size_t( anim::FindJoint( *mannequin->animations, "Head" ) )].cols[3], v );
		const Character* body = server.PlayerCharacter( h.bots[0].client->Slot() );
		float speed = body != nullptr ? b3Length( b3Vec3{ body->velocity.x, 0.0f, body->velocity.z } ) : 0.0f;
		MoveParams move = server.PlayerMove( h.bots[0].client->Slot() );
		if ( a->graph[0].source == uint8_t( sneakPack + 1 ) && a->graph[0].stateTime > 0.5f )
		{
			slowed |= move[MoveParam::WalkSpeed] == 1.5f && move[MoveParam::SprintSpeed] == 1.5f;
			sneakSpeed = std::max( sneakSpeed, speed );
			swapped = true;
			sneaking = std::min( sneaking, v[1] );
		}
		else if ( a->graph[0].source == 0 )
		{
			restored |= swapped;
			givenBack |= swapped && move == MoveParams{};
			runSpeed = swapped && a->graph[0].stateTime > 1.0f ? std::max( runSpeed, speed ) : runSpeed;
			if ( swapped == false )
			{
				standing = std::max( standing, v[1] );
			}
		}
	} );
	h.Report();
	std::printf( "    head: standing %.2f, sneaking %.2f; swapped %d, restored %d\n", standing, sneaking, int( swapped ), int( restored ) );
	CHECK( swapped && restored );
	CHECK( sneaking < standing - 0.3f );
	std::printf( "    speed: sneaking %.2f m/s, afterwards %.2f; slowed %d, given back %d\n", sneakSpeed, runSpeed, int( slowed ),
				 int( givenBack ) );
	CHECK( slowed && givenBack );
	CHECK( sneakSpeed > 1.0f && sneakSpeed < 1.6f && runSpeed > 6.0f );
	CHECK( h.bots[0].client->GetStats().desyncs == 0 );
}

void TestHeadshot()
{
	Harness h( 47801 );
	const ModSchema& schema = h.server.Schema();
	ActionBits fire = kKeyUse;
	ActionBits pistol = kKeySlot1;
	int hitEvent = schema.FindEvent( "item.hit" );

	// From one spawn point's eye to the next one's head: 0.15 m down over 1.5 m, about 5.7 degrees.
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraYaw = 16384;
		in.cameraPitch = -1040;
		in.actions = pistol;
		if ( tick > 200 && tick < 300 && ( tick % 20 ) < 3 ) // once the target has landed, until it is down
		{
			in.actions |= fire;
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; }; // stands still

	std::map<uint32_t, int32_t> damageByTick;
	// Only the player hits: the pistol also reports a wall or the ground as a hit, with 0 damage.
	const uint32_t shooterId = h.server.Sim().PlayerNetId( h.bots[0].client->Slot() );
	// The inventory at a death: what the life started with is taken back (nothing is left lying),
	// and the next life gets its own.
	uint32_t diedAt = 0; // the server tick the death was first seen (0: alive)
	int carriedWhileDead = 0;
	int mostLying = 0;
	bool wasDead = false;
	bool newLifeHasItems = false;
	int firstLife = 0; // what the first life carried (the items of every mod the checkout has)
	h.RunUntil( 9.0, [&]( double ) {
		Simulation& sim = h.server.Sim();
		uint32_t victim = sim.PlayerNetId( h.bots[1].client->Slot() );
		const Character* body = sim.PlayerCharacter( h.bots[1].client->Slot() );
		int carried = 0;
		int lying = 0;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
			carried += item != nullptr && item->holder == victim ? 1 : 0;
			lying += item != nullptr && item->holder == 0 ? 1 : 0;
		}
		mostLying = std::max( mostLying, lying );
		if ( body != nullptr && body->dead != 0 )
		{
			// The mod sees the death a tick later and its commands run the tick after: by ticks, not
			// by how often this is called.
			diedAt = diedAt == 0 ? sim.Tick() : diedAt;
			wasDead = true;
			carriedWhileDead = sim.Tick() > diedAt + 5 ? std::max( carriedWhileDead, carried ) : carriedWhileDead;
		}
		else
		{
			diedAt = 0;
			firstLife = wasDead ? firstLife : std::max( firstLife, carried );
			newLifeHasItems |= wasDead && firstLife > 0 && carried == firstLife;
		}
		const SimGlobals& g = h.server.Sim().Globals();
		for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
		{
			const ModEventRecord& e = g.modEvents[i];
			if ( int( e.type ) == hitEvent && e.netIdB != shooterId && h.server.Sim().EntityAnimState( e.netIdB ) != nullptr )
			{
				damageByTick[e.tick] = e.value;
			}
		}
	} );
	h.Report();

	int heads = 0;
	int others = 0;
	for ( const auto& [tick, damage] : damageByTick )
	{
		( damage == 50 ? heads : others ) += 1;
	}
	std::printf( "    %d head hits, %d other hits\n", heads, others );
	CHECK( heads >= 2 );
	CHECK( others == 0 );
	Simulation& server = h.server.Sim();
	uint32_t targetId = server.PlayerNetId( h.bots[1].client->Slot() );
	CHECK( server.BoardValue( targetId, schema.FindField( "combat.deaths" )->slot ) >= 1 );
	std::printf( "    died %d: carried while dead %d, most left lying %d, the next life has its items %d\n", int( wasDead ), carriedWhileDead,
				 mostLying, int( newLifeHasItems ) );
	CHECK( wasDead && carriedWhileDead == 0 && mostLying == 0 && newLifeHasItems );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// The combat mod on its own terms: a weapon says "combat.damage", and health, death and the next
// life follow from the server's options. Slot 0 shoots slot 1 in the head (50) with 60 health and a
// one-second respawn: the first hit takes 50, the second what is left, the kill is credited, and
// the victim is back a second later with full health.
void TestCombat()
{
	Harness h( 47812, {}, {}, { { "combat.max_health", "60" }, { "combat.respawn_seconds", "1" } } );
	const ModSchema& schema = h.server.Schema();
	ActionBits fire = kKeyUse;
	ActionBits pistol = kKeySlot1;
	int hurtEvent = schema.FindEvent( "combat.hurt" );
	int killedEvent = schema.FindEvent( "combat.killed" );
	int respawnedEvent = schema.FindEvent( "combat.respawned" );
	CHECK( hurtEvent >= 0 && killedEvent >= 0 && respawnedEvent >= 0 && schema.FindEvent( "combat.heal" ) >= 0 );

	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraYaw = 16384;
		in.cameraPitch = -1040; // from one spawn point's eye to the next one's head
		in.actions = pistol;
		if ( tick > 200 && tick < 260 && ( tick % 20 ) < 3 )
		{
			in.actions |= fire;
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; }; // stands still

	h.RunUntil( 2.0 ); // both are in: their ids are known
	Simulation& server = h.server.Sim();
	const uint32_t shooterId = server.PlayerNetId( h.bots[0].client->Slot() );
	const uint32_t victimId = server.PlayerNetId( h.bots[1].client->Slot() );
	CHECK( shooterId != 0 && victimId != 0 && shooterId != victimId );
	int healthSlot = schema.FindField( "combat.health" )->slot;
	std::map<uint32_t, int32_t> hurts; // tick -> health lost
	uint32_t killedAt = 0;
	uint32_t backAt = 0;
	int32_t healthWhenDead = -1;
	h.RunUntil( 8.0, [&]( double ) {
		const SimGlobals& g = server.Globals();
		for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
		{
			const ModEventRecord& e = g.modEvents[i];
			if ( int( e.type ) == hurtEvent && e.netIdA == shooterId && e.netIdB == victimId )
			{
				hurts[e.tick] = e.value;
			}
			if ( int( e.type ) == killedEvent && e.netIdA == shooterId && e.netIdB == victimId && killedAt == 0 )
			{
				killedAt = e.tick;
			}
			if ( int( e.type ) == respawnedEvent && e.netIdA == victimId && killedAt != 0 && e.tick > killedAt && backAt == 0 )
			{
				backAt = e.tick;
			}
		}
		const Character* body = server.PlayerCharacter( h.bots[1].client->Slot() );
		if ( body != nullptr && body->dead != 0 )
		{
			healthWhenDead = server.BoardValue( victimId, healthSlot );
		}
	} );
	h.Report();

	std::vector<int32_t> lost;
	for ( const auto& [tick, value] : hurts )
	{
		lost.push_back( value );
	}
	std::printf( "    hurt %d times (%d, %d), killed at tick %u, back at tick %u, health %d\n", int( lost.size() ),
				 lost.size() > 0 ? lost[0] : 0, lost.size() > 1 ? lost[1] : 0, killedAt, backAt, server.BoardValue( victimId, healthSlot ) );
	CHECK( server.BoardValue( victimId, schema.FindField( "combat.max_health" )->slot ) == 60 );
	CHECK( lost.size() == 2 && lost[0] == 50 && lost[1] == 10 ); // the second hit takes what is left, and no hit lands on the dead
	CHECK( killedAt != 0 && healthWhenDead == 0 );
	uint32_t rate = server.Config().tickRate;
	CHECK( backAt >= killedAt + rate && backAt <= killedAt + rate + 4 );
	CHECK( server.BoardValue( victimId, healthSlot ) == 60 );
	CHECK( server.BoardValue( victimId, schema.FindField( "combat.dead" )->slot ) == 0 );
	CHECK( server.BoardValue( victimId, schema.FindField( "combat.deaths" )->slot ) == 1 );
	CHECK( server.BoardValue( shooterId, schema.FindField( "combat.kills" )->slot ) == 1 );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// The pistol's second action: slot 0 aims at slot 1 and presses "mark". The server answers every
// accepted press with pistol.scan, and with pistol.marked naming the player the ray found; a mark
// harms nobody and is accepted once a second.
void TestPistolMark()
{
	Harness h( 47811 );
	const ModSchema& schema = h.server.Schema();
	uint16_t mark = schema.ActionMask( "use2" );
	ActionBits pistol = kKeySlot1;
	int scanEvent = schema.FindEvent( "item.use2" );
	int markedEvent = schema.FindEvent( "mark.found" );
	CHECK( mark != 0 && scanEvent >= 0 && markedEvent >= 0 );

	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraYaw = 16384;
		in.cameraPitch = -1040; // from one spawn point's eye to the next one's head
		in.actions = pistol;
		if ( tick > 200 && ( tick % 10 ) < 3 ) // far more often than a mark is accepted
		{
			in.actions |= mark;
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; }; // stands still

	std::map<uint32_t, uint32_t> scans;	 // tick -> what the ray hit
	std::map<uint32_t, uint32_t> marks;	 // tick -> who was marked
	const uint32_t casterId = h.server.Sim().PlayerNetId( h.bots[0].client->Slot() );
	h.RunUntil( 7.0, [&]( double ) {
		const SimGlobals& g = h.server.Sim().Globals();
		for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
		{
			const ModEventRecord& e = g.modEvents[i];
			if ( int( e.type ) == scanEvent && e.netIdA == casterId )
			{
				scans[e.tick] = e.netIdB;
			}
			if ( int( e.type ) == markedEvent && e.netIdA == casterId )
			{
				marks[e.tick] = e.netIdB;
			}
		}
	} );
	h.Report();

	Simulation& server = h.server.Sim();
	uint32_t targetId = server.PlayerNetId( h.bots[1].client->Slot() );
	int onTarget = 0;
	for ( const auto& [tick, who] : marks )
	{
		onTarget += who == targetId ? 1 : 0;
		CHECK( scans.count( tick ) == 1 ); // a mark is always a scan that found someone
	}
	uint32_t closest = UINT32_MAX;
	uint32_t last = 0;
	for ( const auto& [tick, hit] : scans )
	{
		closest = last != 0 ? std::min( closest, tick - last ) : closest;
		last = tick;
	}
	std::printf( "    %d scans, %d marks (%d on the target), closest two scans %u ticks apart\n", int( scans.size() ), int( marks.size() ),
				 onTarget, closest );
	CHECK( scans.size() >= 3 && scans.size() <= 8 );
	CHECK( onTarget >= 2 && onTarget == int( marks.size() ) );
	CHECK( closest >= server.Config().tickRate ); // once a second
	CHECK( server.BoardValue( targetId, schema.FindField( "combat.health" )->slot ) == 100 );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// Automatic fire: slot 0 takes out the rifle and holds "fire"; slot 1 does the same with the pistol.
// The rifle answers the held trigger with a shot every tenth of a second, the pistol with one shot
// for the press. Held on to the end of the magazine, the rifle clicks once, reloads, and fires on.
void TestRifle()
{
	Harness h( 47823 );
	// (The second gun's look is licensed and local: a checkout without it has one gun.)
	if ( h.server.Schema().FindItemKind( "gun.rifle" ) < 0 )
	{
		std::printf( "    skipped: this checkout has no rifle (server_mods/gun/client/prefabs/rifle.tscn is local)\n" );
		return;
	}
	const ModSchema& schema = h.server.Schema();
	ActionBits fire = kKeyUse;
	ActionBits rifle = kKeySlot3;
	ActionBits pistol = kKeySlot1;
	int rifleFired = schema.FindEvent( "item.use" );
	int rifleDry = schema.FindEvent( "item.use_failed" );
	int rifleReload = schema.FindEvent( "item.reload" );
	int pistolFired = schema.FindEvent( "item.use" );
	CHECK( fire != 0 && rifle != 0 && rifleFired >= 0 && rifleDry >= 0 && rifleReload >= 0 && pistolFired >= 0 );
	CHECK( schema.FindItemKind( "gun.rifle" ) >= 0 && schema.FindField( "gun.ammo" ) != nullptr );

	// Held for two seconds (20 shots), a tap (1), then held for good (9, a reload, and on).
	auto trigger = []( uint32_t tick ) { return ( tick >= 200 && tick < 320 ) || tick == 400 || tick >= 460; };
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraPitch = 8000; // at the sky: nobody is hurt, no life ends, no magazine is refilled
		in.actions = tick < 150 ? rifle : ( trigger( tick ) ? fire : ActionBits( 0 ) );
		return in;
	};
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraPitch = 8000;
		in.actions = tick < 150 ? pistol : ( trigger( tick ) ? fire : ActionBits( 0 ) );
		return in;
	};

	std::set<uint32_t> shots, clicks, reloads, pistolShots;
	int32_t ammoAfterHold = -1;
	int32_t ammoAfterTap = -1;
	const BoardField* ammo = schema.FindField( "gun.ammo" );
	h.RunUntil( 17.0, [&]( double ) {
		Simulation& sim = h.server.Sim();
		const uint32_t rifleman = sim.PlayerNetId( h.bots[0].client->Slot() );
		const uint32_t gunner = sim.PlayerNetId( h.bots[1].client->Slot() );
		const SimGlobals& g = sim.Globals();
		for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
		{
			const ModEventRecord& e = g.modEvents[i];
			if ( e.netIdA == rifleman && int( e.type ) == rifleFired )
			{
				shots.insert( e.tick );
			}
			if ( e.netIdA == rifleman && int( e.type ) == rifleDry )
			{
				clicks.insert( e.tick );
			}
			if ( e.netIdA == rifleman && int( e.type ) == rifleReload )
			{
				reloads.insert( e.tick );
			}
			if ( e.netIdA == gunner && int( e.type ) == pistolFired )
			{
				pistolShots.insert( e.tick );
			}
		}
		if ( rifleman != 0 && sim.Tick() >= 360 && sim.Tick() < 390 )
		{
			ammoAfterHold = sim.BoardValue( rifleman, ammo->slot );
		}
		if ( rifleman != 0 && sim.Tick() >= 430 && sim.Tick() < 450 )
		{
			ammoAfterTap = sim.BoardValue( rifleman, ammo->slot );
		}
	} );
	h.Report();

	// The first hold: every shot six ticks after the last.
	std::vector<uint32_t> ticks( shots.begin(), shots.end() );
	int held = 0;
	bool evenly = true;
	for ( size_t i = 0; i < ticks.size() && ticks[i] < 390; ++i )
	{
		held += 1;
		evenly &= i == 0 || ticks[i] - ticks[i - 1] == 6;
	}
	// The second: the nine left, then nothing for the reload (two seconds), then the next magazine.
	uint32_t longest = 0;
	for ( size_t i = size_t( held ) + 2; i < ticks.size(); ++i )
	{
		longest = std::max( longest, ticks[i] - ticks[i - 1] );
	}
	std::printf( "    rifle: %d shots, %d in the two seconds held (evenly: %d), ammo %d after, %d after a tap; %d clicks, %d reloads, longest pause %u ticks\n",
				 int( shots.size() ), held, int( evenly ), ammoAfterHold, ammoAfterTap, int( clicks.size() ), int( reloads.size() ), longest );
	std::printf( "    pistol, held the same way: %d shots\n", int( pistolShots.size() ) );
	CHECK( held == 20 && evenly );
	CHECK( ammoAfterHold == 10 && ammoAfterTap == 9 );
	CHECK( shots.size() > 60 );								   // 21, the 9 left, and more than one magazine after
	CHECK( clicks.size() == reloads.size() && clicks.size() >= 2 && clicks.size() <= 3 ); // one click for each empty magazine
	CHECK( longest >= 120 && longest <= 128 );				   // the reload
	CHECK( pistolShots.size() == 3 );						   // a press each: the pistol is not automatic
	// The rounds are the pistol's own field (its scene starts a copy at 12): three shots left nine.
	{
		Simulation& sim = h.server.Sim();
		const BoardField* rounds = schema.FindField( "gun.rounds" );
		int pistolKind = schema.FindItemKind( "gun.pistol" );
		uint32_t shooter = sim.PlayerNetId( h.bots[1].client->Slot() );
		int32_t left = -1;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
			if ( rounds != nullptr && item != nullptr && item->holder == shooter && int( item->kind ) == pistolKind )
			{
				left = sim.BoardValue( r.netId, rounds->slot );
			}
		}
		std::printf( "    the pistol's own rounds after three shots: %d\n", int( left ) );
		CHECK( left == 9 );
	}
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// The two guns set their stances on the same layer. Swapped one for the other, either way, the
// body has the stance of the one that is out: the one put away does not clear it.
void TestGunSwap()
{
	Harness h( 47824 );
	// (The second gun's look is licensed and local: a checkout without it has one gun.)
	if ( h.server.Schema().FindItemKind( "gun.rifle" ) < 0 )
	{
		std::printf( "    skipped: this checkout has no rifle (server_mods/gun/client/prefabs/rifle.tscn is local)\n" );
		return;
	}
	const ModSchema& schema = h.server.Schema();
	ActionBits rifle = kKeySlot3;
	ActionBits pistol = kKeySlot1;
	int upper = schema.FindLayer( "upper" );
	int pistolStance = schema.FindStance( "gun" );
	int rifleStance = schema.FindStance( "gun" );
	// (One stance for every gun: what differs between them is their own animation pack.)
	CHECK( rifle != 0 && pistol != 0 && upper >= 0 && pistolStance >= 0 && rifleStance == pistolStance );
	// The rifle, the pistol, the rifle, the pistol: a key every second and a half.
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		uint32_t step = tick / 90;
		in.actions = tick % 90 < 10 && step >= 1 && step <= 4 ? ( step % 2 == 1 ? rifle : pistol ) : uint16_t( 0 );
		return in;
	};
	int held[5] = {};
	int total[5] = {};
	h.RunUntil( 7.6, [&]( double ) {
		Simulation& sim = h.server.Sim();
		AnimStateCopy anim = sim.EntityAnimState( sim.PlayerNetId( h.bots[0].client->Slot() ) );
		uint32_t step = sim.Tick() / 90;
		// The last half second of each: long after the swap.
		if ( anim != nullptr && step >= 1 && step <= 4 && sim.Tick() % 90 >= 60 )
		{
			total[step] += 1;
			held[step] += anim->aiming != 0 && int( anim->stances[upper] ) == ( step % 2 == 1 ? rifleStance : pistolStance ) + 1 ? 1 : 0;
		}
	} );
	h.Report();
	std::printf( "    in the stance: rifle %d/%d, then pistol %d/%d, then rifle %d/%d, then pistol %d/%d\n", held[1], total[1], held[2], total[2], held[3],
				 total[3], held[4], total[4] );
	for ( int step = 1; step <= 4; ++step )
	{
		CHECK( total[step] > 0 && held[step] == total[step] );
	}
	CHECK( h.bots[0].client->GetStats().desyncs == 0 );
}

// Layers end to end: slot 0 takes out the pistol, then the bat (a full-body stance), and swings at
// slot 1, which stands still. Swings play the swing stance, hits go out as combat.damage, the combat mod (which
// keeps health) applies them and credits the kill, and clients agree on every pose-carrying tick.
void TestMelee()
{
	Harness h( 47802 );
	const ModSchema& schema = h.server.Schema();
	ActionBits fire = kKeyUse;
	ActionBits bat = kKeySlot2;
	ActionBits pistol = kKeySlot1;
	ActionBits hands = kKeyHands;
	int full = schema.FindLayer( "full" );
	int ready = schema.FindStance( "melee" );
	int swing = schema.FindStance( "melee_swing" );
	int upper = schema.FindLayer( "upper" );
	CHECK( fire != 0 && bat != 0 && pistol != 0 && full >= 0 && ready >= 0 && swing >= 0 && upper >= 0 );

	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraYaw = 16384; // toward slot 1
		if ( tick >= 100 && tick < 110 )
		{
			in.actions = pistol;
		}
		else if ( tick >= 160 && tick < 170 )
		{
			in.actions = bat;
		}
		else if ( tick >= 245 && tick < 255 )
		{
			in.actions = hands; // the bat goes on the back, still hot from the first hit
		}
		else if ( tick >= 365 && tick < 375 )
		{
			in.actions = bat;
		}
		else if ( tick >= 220 && ( tick % 45 ) < 3 )
		{
			in.actions = fire;
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; };

	Simulation& server = h.server.Sim();
	bool sawReady = false;
	bool sawSwing = false;
	// The pistol and then the bat are items in the attacker's right hand (the swap is one tick: the
	// pistol mod takes its gun away as the melee mod puts the bat there). The bat has its own state:
	// hot after a hit.
	int gunKind = schema.FindItemKind( "gun.pistol" );
	int batKind = schema.FindItemKind( "melee.bat" );
	int hand = schema.FindSocket( "RightHand" );
	const BoardField* hot = schema.FindField( "melee.hot" );
	CHECK( gunKind >= 0 && batKind >= 0 && hand == int( kSocketRightHand ) && hot != nullptr );
	bool heldGun = false;
	bool gunAfterBat = false;
	bool heldBat = false;
	bool batWasHot = false;
	// The bat cools by itself, wherever it is: put away while hot, it is cold two seconds after the hit.
	uint32_t hotBat = 0;
	bool hotOnTheBack = false;
	bool coldOnTheBack = false;
	int killedEvent = schema.FindEvent( "combat.killed" );
	uint32_t kills = 0;
	std::map<uint32_t, bool> counted;
	h.RunUntil( 11.0, [&]( double ) {
		uint32_t attacker = server.PlayerNetId( h.bots[0].client->Slot() );
		if ( AnimStateCopy a = server.EntityAnimState( attacker ) )
		{
			sawReady |= a->stances[full] == ready + 1;
			sawSwing |= a->stances[full] == swing + 1;
			if ( uint32_t bat = server.HeldItemOf( attacker, uint32_t( hand ) ) )
			{
				flecs::entity e = server.FindEntity( bat );
				uint16_t kind = e.is_valid() ? e.get<HeldItem>().kind : uint16_t( 0xFFFF );
				heldGun |= kind == uint16_t( gunKind );
				gunAfterBat |= heldBat && kind == uint16_t( gunKind );
				heldBat |= kind == uint16_t( batKind );
				batWasHot |= server.BoardValue( bat, hot->slot ) != 0;
				if ( hotBat == 0 && kind == uint16_t( batKind ) && server.BoardValue( bat, hot->slot ) != 0 )
				{
					hotBat = bat;
				}
			}
			if ( hotBat != 0 && server.HeldItemOf( attacker, uint32_t( hand ) ) != hotBat && server.FindEntity( hotBat ).is_valid() )
			{
				bool isHot = server.BoardValue( hotBat, hot->slot ) != 0;
				hotOnTheBack |= isHot && server.Tick() < 300;
				coldOnTheBack |= isHot == false && server.Tick() >= 356 && server.Tick() < 364;
			}
		}
		const SimGlobals& g = server.Globals();
		for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
		{
			const ModEventRecord& e = g.modEvents[i];
			if ( int( e.type ) == killedEvent && e.netIdA == attacker && counted[e.tick] == false )
			{
				counted[e.tick] = true;
				++kills;
			}
		}
	} );
	h.Report();
	std::printf( "    ready stance %d, swing stance %d, kills by the bat %u\n", int( sawReady ), int( sawSwing ), kills );
	CHECK( sawReady );
	CHECK( sawSwing );
	std::printf( "    the pistol: held %d, still there after the bat %d; the bat: held %d, hot after a hit %d\n", int( heldGun ),
				 int( gunAfterBat ), int( heldBat ), int( batWasHot ) );
	CHECK( heldGun );
	CHECK( gunAfterBat == false );
	CHECK( heldBat );
	CHECK( batWasHot );
	std::printf( "    put away hot: still hot on the back %d, cold there two seconds after the hit %d\n", int( hotOnTheBack ), int( coldOnTheBack ) );
	CHECK( hotOnTheBack && coldOnTheBack );
	CHECK( kills >= 1 );
	uint32_t attackerId = server.PlayerNetId( h.bots[0].client->Slot() );
	CHECK( server.BoardValue( attackerId, schema.FindField( "combat.kills" )->slot ) >= 1 );
	CHECK( server.PlayerCharacter( h.bots[0].client->Slot() )->faceCamera == 1 );
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// The bat strikes where the player looks, up and down as well: looking over the other player's
// head misses, looking level or at the legs hits.
void TestMeleePitch()
{
	Harness h( 47893 );
	const ModSchema& schema = h.server.Schema();
	ActionBits fire = kKeyUse;
	ActionBits bat = kKeySlot2;
	int healthSlot = schema.FindField( "combat.health" )->slot;
	auto pitchOf = std::make_shared<int16_t>( int16_t( 13000 ) ); // 71 degrees up
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraYaw = 16384; // toward slot 1
		in.cameraPitch = *pitchOf;
		if ( tick >= 100 && tick < 110 )
		{
			in.actions = bat;
		}
		else if ( tick >= 160 && ( tick % 45 ) < 3 )
		{
			in.actions = fire;
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; };

	Simulation& server = h.server.Sim();
	int swingEvent = schema.FindEvent( "melee.swing" );
	h.RunUntil( 7.0 );
	uint32_t victim = server.PlayerNetId( h.bots[1].client->Slot() );
	uint32_t attacker = server.PlayerNetId( h.bots[0].client->Slot() );
	bool swung = false;
	const SimGlobals& g = server.Globals();
	for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
	{
		swung |= int( g.modEvents[i].type ) == swingEvent && g.modEvents[i].netIdA == attacker;
	}
	int afterHigh = server.BoardValue( victim, healthSlot );
	std::printf( "    swinging while looking over its head: swung %d, health %d\n", int( swung ), afterHigh );
	CHECK( swung );
	CHECK( afterHigh == 100 );
	// The body follows the look: the attacker's pose is led by the same pitch.
	CHECK( server.EntityAnimState( attacker )->look == 255 );

	*pitchOf = int16_t( -4000 ); // 22 degrees down: at its legs
	h.RunUntil( 9.0 );
	int afterLow = server.BoardValue( victim, healthSlot );
	std::printf( "    looking at its legs: health %d\n", afterLow );
	CHECK( afterLow < 100 || server.PlayerCharacter( h.bots[1].client->Slot() )->dead != 0 );
	h.Report();
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// A shot goes at what is under the crosshair of the camera the player looks through. Turned 17
// degrees away from the other player, the camera behind the shooter looks past them and so does
// the shot. Over the right shoulder the same turn puts the crosshair on them (the line of sight
// starts 0.45 m to the side), and the shot, from the head, goes there. First person looks straight.
void TestAimViews()
{
	Harness h( 47894 );
	const ModSchema& schema = h.server.Schema();
	ActionBits fire = kKeyUse;
	ActionBits pistol = kKeySlot1;
	int healthSlot = schema.FindField( "combat.health" )->slot;
	// Spawn points are 1.5 m apart: the line from the shoulder turns by asin( 0.45 / 1.5 ) to meet it.
	const uint16_t straight = 16384;
	const uint16_t turned = uint16_t( straight + 3178 );
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraPitch = kAimAtChest;
		in.actions = pistol;
		in.cameraYaw = turned;
		in.view = uint8_t( ViewMode::ThirdPerson );
		if ( tick >= 330 && tick < 390 )
		{
			in.view = uint8_t( ViewMode::ShoulderRight );
		}
		else if ( tick >= 390 )
		{
			in.cameraYaw = straight;
			in.view = uint8_t( ViewMode::FirstPerson );
		}
		bool window = ( tick > 200 && tick < 300 ) || ( tick >= 340 && tick < 345 ) || ( tick >= 400 && tick < 405 );
		if ( window && ( tick % 20 ) < 3 )
		{
			in.actions |= fire;
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; };

	Simulation& server = h.server.Sim();
	auto health = [&]() { return server.BoardValue( server.PlayerNetId( h.bots[1].client->Slot() ), healthSlot ); };
	h.RunUntil( 5.3 );
	int beside = health();
	h.RunUntil( 6.3 );
	int shoulder = health();
	h.RunUntil( 7.5 );
	int firstPerson = health();
	std::printf( "    health: 5 shots turned away from behind %d, one with the same turn over the shoulder %d, one straight in first person %d\n", beside, shoulder,
				 firstPerson );
	CHECK( beside == 100 );
	CHECK( shoulder < beside );
	CHECK( firstPerson < shoulder );
	h.Report();
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

void TestDeathmatch()
{
	Harness h( 47799, {}, {}, { { "deathmatch.kills", "2" }, { "deathmatch.pause_seconds", "2" } } );
	const ModSchema& schema = h.server.Schema();
	CHECK( schema.FindField( "deathmatch.score" ) != nullptr );
	ActionBits fire = kKeyUse;
	ActionBits pistol = kKeySlot1;

	// The same duel as mods_session: slot 0 shoots slot 1, which stands still.
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraYaw = 16384;
		in.cameraPitch = kAimAtChest;
		in.actions = pistol;
		if ( tick > 90 && ( tick % 20 ) < 3 )
		{
			in.actions |= fire;
		}
		return in;
	};
	h.RunUntil( 1.0 );
	h.AddBot().script = []( uint32_t ) { return PlayerInput{}; };

	Simulation& server = h.server.Sim();
	const BoardField* phase = schema.FindField( "deathmatch.phase" );
	const BoardField* round = schema.FindField( "deathmatch.round" );
	const BoardField* winner = schema.FindField( "deathmatch.winner" );
	// How the round ended is a number; the words for it are the look's.
	const BoardField* ending = schema.FindField( "deathmatch.ending" );
	CHECK( ending != nullptr );
	int32_t endingSeen = 0;
	bool sawIntermission = false;
	bool frozenInIntermission = true;
	uint32_t winnerSeen = 0;
	h.RunUntil( 16.0, [&]( double ) {
		if ( server.GlobalBoardValue( phase->slot ) == 1 )
		{
			sawIntermission = true;
			winnerSeen = uint32_t( server.GlobalBoardValue( winner->slot ) );
			endingSeen = server.GlobalBoardValue( ending->slot );
			for ( int s = 0; s < 2; ++s )
			{
				const Character* c = server.PlayerCharacter( PlayerSlot( s ) );
				frozenInIntermission &= c == nullptr || c->frozen == 1;
			}
		}
	} );
	h.Report();
	std::printf( "    round %d, phase %d, winner seen %u (shooter %u)\n", server.GlobalBoardValue( round->slot ),
				 server.GlobalBoardValue( phase->slot ), winnerSeen, server.PlayerNetId( 0 ) );
	CHECK( sawIntermission );
	CHECK( frozenInIntermission );
	CHECK( winnerSeen == server.PlayerNetId( 0 ) );
	CHECK( endingSeen == 2 ); // flawless: the other one never scored
	CHECK( server.GlobalBoardValue( round->slot ) >= 2 );
	for ( Bot& b : h.bots )
	{
		int compared = 0;
		CHECK( h.CompareWithServer( b, compared ) == 0 );
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

void TestProtocol()
{
	using namespace net;

	// The mod schema survives the trip, and a client refuses one that points outside the board or
	// the action bits, so presentation can index with what it read.
	{
		ModSchema schema;
		schema.mods = { "pistol" };
		schema.fields.push_back( { "gun.ammo", BoardType::Int, BoardScope::Entity, 3 } );
		schema.fields.push_back( { "round.time", BoardType::Float, BoardScope::Global, 0 } );
		schema.events = { "item.use", "combat.killed" };
		schema.actions.push_back( { "fire", 0, "MouseLeft" } );
		schema.items.push_back( { "pistol", std::string( 64, 'a' ) } );
		schema.animGraph = std::string( 70000, 'g' ); // longer than any name: its own length prefix
		std::vector<uint8_t> bytes;
		EncodeSchema( schema, bytes );
		ModSchema back;
		CHECK( DecodeSchema( bytes.data(), bytes.size(), back ) );
		CHECK( back == schema );
		CHECK( back.ActionMask( "fire" ) == 1 );
		CHECK( back.FindEvent( "combat.killed" ) == 1 );

		ModSchema bad = schema;
		bad.fields[0].slot = uint16_t( kFieldLimit );
		EncodeSchema( bad, bytes );
		CHECK( DecodeSchema( bytes.data(), bytes.size(), back ) == false );
		bytes.resize( bytes.size() - 1 );
		CHECK( DecodeSchema( bytes.data(), bytes.size(), back ) == false );

		// An item hash has to be a real SHA-256.
		bad = schema;
		bad.items[0].sha256 = "not-a-hash";
		EncodeSchema( bad, bytes );
		CHECK( DecodeSchema( bytes.data(), bytes.size(), back ) == false );

		// An empty schema (a server without mods) is valid.
		CHECK( DecodeSchema( nullptr, 0, back ) && back.fields.empty() );

		// The config carries how players move; one outside a parameter's range is refused.
		{
			MsgWelcome welcome;
			welcome.config.move.values[int( MoveParam::WalkSpeed )] = 4.5f;
			welcome.config.move.values[int( MoveParam::MaxFall )] = 30.0f;
			std::vector<uint8_t> packet;
			Encode( welcome, packet );
			MsgWelcome got;
			ByteReader r( packet.data(), packet.size() );
			CHECK( ReadType( r ) == MsgType::Welcome );
			CHECK( Decode( r, got ) && got.config == welcome.config );
			welcome.config.move.values[int( MoveParam::Gravity )] = -1.0f;
			Encode( welcome, packet );
			ByteReader r2( packet.data(), packet.size() );
			CHECK( ReadType( r2 ) == MsgType::Welcome );
			CHECK( Decode( r2, got ) == false );
		}

		// Non-finite commands never leave the server.
		SimCommand c;
		c.type = CommandType::Impulse;
		CHECK( IsSendableCommand( c ) );
		c.b.y = std::numeric_limits<float>::infinity();
		CHECK( IsSendableCommand( c ) == false );
	}

	// Frame delta codec round trip, with every input field and every kind of command.
	FrameCodec enc, dec;
	uint64_t rng = 5;
	for ( uint32_t t = 0; t < 500; ++t )
	{
		InputFrame f;
		f.tick = t;
		if ( ( NextRandom( rng ) % 20 ) == 0 )
		{
			f.events.push_back( { PlayerEventType::Join, PlayerSlot( NextRandom( rng ) % kMaxPlayers ) } );
		}
		for ( int i = 0; i < 8; ++i )
		{
			f.inputs[i] = enc.Previous()[i];
			if ( ( NextRandom( rng ) % 4 ) == 0 )
			{
				f.inputs[i].moveForward = int8_t( NextRandom( rng ) % 255 - 127 );
				f.inputs[i].cameraYaw = uint16_t( NextRandom( rng ) );
				f.inputs[i].cameraPitch = int16_t( int( NextRandom( rng ) % 32000 ) - 16000 );
				f.inputs[i].actions = uint16_t( NextRandom( rng ) );
			}
		}
		uint64_t kinds = NextRandom( rng ) % 4;
		for ( uint64_t k = 0; k < kinds; ++k )
		{
			SimCommand c;
			c.type = CommandType( 1 + NextRandom( rng ) % kLastCommandType );
			c.mode = uint8_t( NextRandom( rng ) % 3 );
			c.index = uint16_t( NextRandom( rng ) % 5 );
			c.target = ( NextRandom( rng ) & 1 ) ? SlotTarget( 3 ) : uint32_t( NextRandom( rng ) % 1000 );
			c.value = int32_t( NextRandom( rng ) % 7 ) - 3;
			c.a = { RandomRange( rng, -5.0f, 5.0f ), 0.0f, -0.0f };
			if ( NextRandom( rng ) & 1 )
			{
				c.b = { 1.0f, 2.0f, 3.0f };
			}
			f.commands.push_back( c );
		}
		std::vector<uint8_t> bytes;
		enc.Encode( f, bytes );
		ByteReader r( bytes.data(), bytes.size() );
		CHECK( ReadType( r ) == MsgType::Frame );
		InputFrame out;
		CHECK( dec.Decode( r, out ) );
		CHECK( out == f );
		CHECK( r.AtEnd() );
	}

	// A client's inputs: each as few bytes as it needs, whichever of its 64 actions are held.
	{
		MsgInput sent;
		sent.newestTick = 90;
		sent.ackTick = 80;
		for ( int i = 0; i < 6; ++i )
		{
			PlayerInput in;
			in.moveForward = int8_t( 20 * i );
			in.cameraYaw = uint16_t( 1000 * i );
			in.cameraPitch = int16_t( -300 * i );
			in.buttons = uint8_t( i & 3 );
			in.intent = uint8_t( i % 3 );
			in.intentSeq = uint8_t( i );
			in.actions = i == 0 ? 0 : i == 1 ? 1 : i == 2 ? ActionBits( 1 ) << 9 : i == 3 ? ActionBits( 1 ) << 40 : i == 4 ? ~ActionBits( 0 ) : 3;
			sent.inputs.push_back( in );
		}
		std::vector<uint8_t> bytes;
		Encode( sent, bytes );
		ByteReader r( bytes.data(), bytes.size() );
		CHECK( ReadType( r ) == MsgType::Input );
		MsgInput got;
		CHECK( Decode( r, got ) && r.AtEnd() );
		CHECK( got.inputs == sent.inputs && got.newestTick == 90 && got.ackTick == 80 );
		std::printf( "    six inputs, their actions from none to all 64: %zu bytes\n", bytes.size() );
		// 9 of header, 13 an input, and 0 + 1 + 2 + 6 + 8 + 1 bytes of actions; 2 of slot keys in the
		// first (the later ones say only what changed, and the slot keys did not).
		CHECK( bytes.size() == 1 + 9 + 6 * 13 + 18 + 2 );
	}

	// Frame batches: a chain from any starting tick decodes back to the same frames.
	{
		std::vector<InputFrame> history( 40 );
		InputArray running{};
		for ( uint32_t t = 0; t < history.size(); ++t )
		{
			history[t].tick = t;
			running[t % 8].moveForward = int8_t( t );
			// Actions anywhere in the word: the first bit, the forty-first, the last.
			running[t % 5].actions = t % 3 == 0 ? ActionBits( 1 ) : t % 3 == 1 ? ( ActionBits( 1 ) << 40 ) | 2 : ActionBits( 1 ) << 63;
			running[( t * 3 ) % 64].cameraYaw = uint16_t( t * 977 );
			history[t].inputs = running;
			if ( t % 7 == 0 )
			{
				history[t].events.push_back( { PlayerEventType::Leave, PlayerSlot( t % 64 ) } );
			}
		}
		for ( uint32_t first : { 0u, 1u, 17u, 39u } )
		{
			std::vector<const InputFrame*> ptrs;
			for ( uint32_t t = first; t < history.size(); ++t )
			{
				ptrs.push_back( &history[t] );
			}
			InputArray base = first > 0 ? history[first - 1].inputs : InputArray{};
			std::vector<uint8_t> bytes;
			EncodeFrameBatch( base, ptrs.data(), ptrs.size(), bytes );
			ByteReader r( bytes.data(), bytes.size() );
			CHECK( ReadType( r ) == MsgType::FrameBatch );
			uint32_t firstTick, count;
			CHECK( ReadFrameBatchHeader( r, firstTick, count ) );
			CHECK( firstTick == first && count == ptrs.size() );
			std::vector<InputFrame> decoded;
			CHECK( ReadFrameBatchBody( r, base, firstTick, count, decoded ) );
			CHECK( r.AtEnd() );
			for ( uint32_t i = 0; i < count; ++i )
			{
				CHECK( decoded[i] == history[first + i] );
			}
			// A wrong base must not silently produce the right frames.
			if ( first > 0 )
			{
				InputArray wrong{};
				wrong[5].moveRight = 99;
				ByteReader r2( bytes.data(), bytes.size() );
				ReadType( r2 );
				ReadFrameBatchHeader( r2, firstTick, count );
				std::vector<InputFrame> bad;
				ReadFrameBatchBody( r2, wrong, firstTick, count, bad );
				CHECK( bad.empty() || !( bad[0] == history[first] ) || history[first].inputs[5] == wrong[5] );
			}
		}
	}

	// Garbage must never crash or be accepted as a welcome with an absurd blob.
	std::vector<uint8_t> junk;
	for ( int i = 0; i < 20000; ++i )
	{
		junk.resize( NextRandom( rng ) % 64 );
		for ( auto& b : junk )
		{
			b = uint8_t( NextRandom( rng ) );
		}
		ByteReader r( junk.data(), junk.size() );
		auto type = ReadType( r );
		if ( !type )
		{
			continue;
		}
		MsgWelcome w;
		MsgInput in;
		MsgHello hello;
		FrameCodec fc;
		InputFrame f;
		switch ( *type )
		{
			case MsgType::Welcome:
				Decode( r, w );
				break;
			case MsgType::Input:
				Decode( r, in );
				break;
			case MsgType::Hello:
				Decode( r, hello );
				break;
			case MsgType::Frame:
				fc.Decode( r, f );
				break;
			case MsgType::FrameBatch:
			{
				uint32_t firstTick, count;
				std::vector<InputFrame> frames;
				if ( ReadFrameBatchHeader( r, firstTick, count ) )
				{
					ReadFrameBatchBody( r, InputArray{}, firstTick, count, frames );
				}
				break;
			}
			default:
				break;
		}
	}
}

// Swinging the bat must not depend on what the legs do: standing, walking or sprinting, a press of
// fire starts a swing, the swing's animation plays on the upper body, and its strike marker comes.
void TestSprintSwing()
{
	// The mannequin, so the strike comes from the swing animation's marker, and the bat's carry pack.
	const std::string root = CB_SOURCE_DIR;
	std::shared_ptr<const CharacterAsset> mannequin;
	Harness h( 47840, {}, {}, {}, [&]( ServerOptions& options ) {
		std::string error, warnings;
		mannequin = LoadCharacterFolder( root + "/godot/characters/mannequin", "mannequin", error, warnings );
		options.character = mannequin;
		options.loadAnimPack = [root]( const std::string& mod, const std::string& pack, std::string& error, std::string& warnings ) {
			return LoadAnimPackFolder( root + "/server_mods/" + mod + "/client", pack, error, warnings );
		};
	} );
	CHECK( mannequin != nullptr );
	const ModSchema& schema = h.server.Schema();
	ActionBits fire = kKeyUse;
	ActionBits bat = kKeySlot2;
	int swingEvent = schema.FindEvent( "melee.swing" );
	int strikeEvent = schema.FindEvent( "melee.strike" );
	CHECK( fire != 0 && bat != 0 && swingEvent >= 0 && strikeEvent >= 0 );

	// Bat out, then swing every 1.25 s (a swing lasts 1.1 s, to its clip's melee.done marker):
	// standing (ticks 200..500), walking (500..800), sprinting (800..1100), in a circle so the walls
	// are never reached.
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraYaw = uint16_t( tick * 40 );
		if ( tick >= 100 && tick < 110 )
		{
			in.actions = bat;
		}
		if ( tick >= 500 )
		{
			in.moveForward = 127;
		}
		if ( tick >= 800 )
		{
			in.buttons = BtnSprint;
		}
		if ( tick >= 200 && ( tick % 75 ) < 3 )
		{
			in.actions = fire;
		}
		return in;
	};

	Simulation& server = h.server.Sim();
	int swings[3] = {};
	int strikes[3] = {};
	uint32_t seen = 0;
	bool sprinted = false;
	h.RunUntil( 19.5, [&]( double ) {
		const SimGlobals& g = server.Globals();
		if ( const Character* c = server.PlayerCharacter( h.bots[0].client->Slot() ) )
		{
			sprinted |= c->sprinting != 0 && b3Length( c->velocity ) > 5.0f;
		}
		for ( ; seen < g.modEventCount; ++seen )
		{
			if ( g.modEventCount - seen > kModEventHistory )
			{
				continue;
			}
			const ModEventRecord& e = g.modEvents[seen % kModEventHistory];
			int phase = e.tick < 500 ? 0 : e.tick < 800 ? 1 : 2;
			swings[phase] += int( e.type ) == swingEvent ? 1 : 0;
			strikes[phase] += int( e.type ) == strikeEvent ? 1 : 0;
		}
	} );
	h.Report();
	std::printf( "    swings / strikes: standing %d / %d, walking %d / %d, sprinting %d / %d\n", swings[0], strikes[0], swings[1], strikes[1],
				 swings[2], strikes[2] );
	CHECK( sprinted );
	for ( int phase = 0; phase < 3; ++phase )
	{
		CHECK( swings[phase] >= 4 );
		CHECK( strikes[phase] >= swings[phase] - 1 ); // the last swing's strike may be still to come
	}
	CHECK( h.bots[0].client->GetStats().desyncs == 0 );
}

// A stat a view source published, or `fallback` when it has none of that name and type.
template <typename T>
T StatOf( const present::ViewFrame& frame, const char* name, T fallback )
{
	for ( const present::ViewStat& stat : frame.stats )
	{
		if ( stat.name == name )
		{
			if ( const T* value = std::get_if<T>( &stat.value ) )
			{
				return *value;
			}
		}
	}
	return fallback;
}

// Takes frames from a source until `done( frame )` or `seconds` have passed. True when done.
bool TakeUntil( present::ViewSource& source, present::ViewFrame& frame, double seconds,
				const std::function<bool( const present::ViewFrame& )>& done, const std::function<void()>& meanwhile = {} )
{
	auto start = Clock::now();
	while ( std::chrono::duration<double>( Clock::now() - start ).count() < seconds )
	{
		if ( meanwhile )
		{
			meanwhile();
		}
		if ( source.Take( frame ) && done( frame ) )
		{
			return true;
		}
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
	return false;
}

// The viewer protocol (present/view.h): a viewer is handed the same frames by a live connection
// and by the recording of that session, and tells neither apart.
void TestViewSources()
{
	std::filesystem::path replayPath = std::filesystem::temp_directory_path() / "cinderbox_view_test.cbr";
	uint16_t spawn = 0;
	{
		Harness h( 17830, replayPath.string() );
		spawn = h.server.Schema().ActionMask( "spawn_prop" );
		CHECK( spawn != 0 );

		// The first to join, so the recording's slot 0: it throws a prop now and then.
		ClientOptions options;
		options.port = h.clientPort;
		options.verbose = false;
		options.logName = "view";
		options.playerName = "Viewer";
		LiveSource live( options );
		CHECK( live.TakesInput() );

		present::ViewFrame frame;
		auto serve = [&h]() { h.RunUntil( h.Now() + 0.002 ); };
		CHECK( TakeUntil( live, frame, 10.0, []( const present::ViewFrame& f ) { return f.state == "playing" && f.hasWorld && f.frame.localNetId != 0; }, serve ) );
		CHECK( frame.rate == 1.0f );
		CHECK( frame.mapHash != 0 );
		CHECK( frame.schemaGeneration != 0 );
		CHECK( frame.schema.ActionMask( "spawn_prop" ) == spawn );
		CHECK( frame.names[0] == "Viewer" );
		CHECK( frame.localPressed == 0 ); // a source the viewer feeds reports no presses
		h.AddBot();

		// It plays on: ticks advance, serials only grow, and its input reaches the world (a prop appears).
		uint32_t firstTick = frame.frame.tick;
		uint64_t lastSerial = frame.serial;
		size_t entitiesBefore = frame.frame.entities.size();
		bool serialsGrow = true;
		bool propSeen = false;
		auto start = Clock::now();
		while ( std::chrono::duration<double>( Clock::now() - start ).count() < 3.0 )
		{
			serve();
			double t = std::chrono::duration<double>( Clock::now() - start ).count();
			PlayerInput in;
			in.moveForward = 100;
			in.actions = std::fmod( t, 1.0 ) < 0.2 ? spawn : uint16_t( 0 );
			live.SetInput( in );
			if ( live.Take( frame ) )
			{
				serialsGrow &= frame.serial > lastSerial;
				lastSerial = frame.serial;
				propSeen |= frame.frame.entities.size() > entitiesBefore + 1; // the bot, and a prop
			}
		}
		CHECK( serialsGrow );
		CHECK( propSeen );
		CHECK( frame.frame.tick > firstTick + 100 );
		CHECK( frame.frame.hasInputs );
		CHECK( StatOf<int64_t>( frame, "desyncs", -1 ) == 0 );
		CHECK( StatOf<int64_t>( frame, "checksums_verified", 0 ) > 0 );
		CHECK( StatOf<bool>( frame, "fp_environment_ok", false ) );
		std::printf( "    live: tick %u, %zu entities, rtt %lld ms\n", frame.frame.tick, frame.frame.entities.size(),
					 (long long)StatOf<int64_t>( frame, "rtt_ms", -1 ) );
	}

	// A file that is not there: rejected, with a reason.
	{
		ReplaySource missing( ( std::filesystem::temp_directory_path() / "cinderbox_no_such_file.cbr" ).string() );
		present::ViewFrame frame;
		CHECK( TakeUntil( missing, frame, 5.0, []( const present::ViewFrame& f ) { return f.state == "rejected"; } ) );
		CHECK( StatOf<std::string>( frame, "reject_reason", "" ).empty() == false );
		CHECK( frame.hasWorld == false );
	}

	// The recording of that session, through the same interface.
	ReplaySource replay( replayPath.string() );
	CHECK( replay.TakesInput() == false );
	present::ViewFrame frame;
	CHECK( TakeUntil( replay, frame, 10.0, []( const present::ViewFrame& f ) { return f.hasWorld && f.frame.localNetId != 0; } ) );
	CHECK( frame.state == "playing" );
	CHECK( frame.mapHash != 0 );
	CHECK( frame.schema.ActionMask( "spawn_prop" ) == spawn );
	CHECK( StatOf<bool>( frame, "build_matches", false ) );
	CHECK( StatOf<int64_t>( frame, "replay_follow", -1 ) == 0 ); // the first player there is
	double length = StatOf<double>( frame, "replay_length_seconds", 0.0 );
	CHECK( length > 2.0 );

	// To the end at 16x: every recorded checksum holds, and the followed player's presses come along.
	replay.Control( "speed", 16.0 );
	uint16_t pressed = 0;
	CHECK( TakeUntil( replay, frame, 20.0, [&pressed]( const present::ViewFrame& f ) {
		pressed |= f.localPressed;
		return StatOf<bool>( f, "replay_ended", false );
	} ) );
	CHECK( frame.rate == 0.0f );
	CHECK( ( pressed & spawn ) != 0 );
	CHECK( StatOf<int64_t>( frame, "desyncs", -1 ) == 0 );
	CHECK( StatOf<int64_t>( frame, "checksums_verified", 0 ) > 0 );
	uint32_t endTick = frame.frame.tick;
	uint64_t endGeneration = frame.frame.resetGeneration;

	// Back to one second in, paused: the world jumps (a new generation) and stays where it is.
	replay.Control( "pause", 1.0 );
	replay.Control( "seek", 1.0 );
	CHECK( TakeUntil( replay, frame, 10.0, [endGeneration]( const present::ViewFrame& f ) { return f.frame.resetGeneration != endGeneration; } ) );
	uint32_t soughtTick = frame.frame.tick;
	CHECK( soughtTick > 0 && soughtTick < endTick );
	CHECK( StatOf<bool>( frame, "replay_paused", false ) );
	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	replay.Take( frame );
	CHECK( frame.frame.tick == soughtTick );

	// One tick on, still paused; then nobody followed: no local player.
	replay.Control( "step", 1.0 );
	CHECK( TakeUntil( replay, frame, 5.0, [soughtTick]( const present::ViewFrame& f ) { return f.frame.tick == soughtTick + 1; } ) );
	replay.Control( "follow", -1.0 );
	CHECK( TakeUntil( replay, frame, 5.0, []( const present::ViewFrame& f ) { return f.frame.localNetId == 0; } ) );
	CHECK( StatOf<int64_t>( frame, "replay_follow", 0 ) == -1 );
	std::printf( "    replay: %.1f s, end tick %u, %lld checksums verified\n", length, endTick,
				 (long long)StatOf<int64_t>( frame, "checksums_verified", 0 ) );
	std::error_code ignored;
	std::filesystem::remove( replayPath, ignored );
}

// Private fields: the "secret" mod gives each player a number that only that player is sent. Every
// client has its own and exactly what the server keeps for it; the simulation has none of it (it
// is not a board value, and nobody desyncs over it); and a slot that is given up starts empty.
void TestPrivateFields()
{
	Harness h( 47813, {}, {}, { { "secret.numbers", "1" } } );
	const ModSchema& schema = h.server.Schema();
	const BoardField* field = schema.FindField( "secret.number" );
	CHECK( field != nullptr && field->scope == BoardScope::Private );
	if ( field == nullptr )
	{
		return;
	}
	for ( int i = 0; i < 4; ++i )
	{
		h.AddBot();
	}
	h.RunUntil( 3.0 );
	h.Report();

	bool allDiffer = true;
	for ( size_t i = 0; i < h.bots.size(); ++i )
	{
		GameClient& client = *h.bots[i].client;
		CHECK( client.State() == ClientState::Playing );
		int32_t mine = client.Privates().values[field->slot];
		int32_t kept = h.server.Privates( client.Slot() ).values[field->slot];
		std::printf( "    slot %u was told %d (the server keeps %d)\n", unsigned( client.Slot() ), mine, kept );
		CHECK( mine >= 1 && mine <= 99 );
		CHECK( mine == kept );
		// Nothing else arrived with it: every other private slot is empty (but the shop's coins,
		// which are the player's own too).
		const BoardField* coins = schema.FindField( "shop.coins" );
		for ( size_t s = 0; s < client.Privates().values.size(); ++s )
		{
			CHECK( s == field->slot || ( coins != nullptr && s == coins->slot ) || client.Privates().values[s] == 0 );
		}
		for ( size_t j = 0; j < h.bots.size(); ++j )
		{
			allDiffer &= j == i || h.bots[j].client->Privates().values[field->slot] != mine;
		}
		// It is not in the simulation, so it cannot desync anyone and no other client holds it.
		CHECK( client.GetStats().desyncs == 0 && client.GetStats().checksumsVerified > 0 );
	}
	std::printf( "    four numbers, all different: %d (they are random, 1 to 99)\n", int( allDiffer ) );
}

// Asking a server what it is before joining (ServerQuery, a menu's): its map, how many play, the
// workshop items it needs, and whether this game could join; the asker never becomes a player.
// Nobody at the address: told so once the wait is over.
void TestServerInfo()
{
	const std::string sha( 64, 'a' );
	Harness h( 47898, {}, {}, {}, [&]( ServerOptions& options ) { options.items = { { "chat", sha }, { "gun", sha } }; } );
	h.AddBot();
	h.RunUntil( 2.0 );
	ServerQuery query;
	CHECK( query.Start( "127.0.0.1", 47898, h.Now() ) );
	h.RunUntil( h.Now() + 3.0, [&]( double now ) { query.Update( now ); } );
	CHECK( query.Done() && query.Ok() );
	const net::MsgInfo& info = query.Info();
	std::printf( "    answered: map \"%s\", %u of %u players, %zu items, compatible %d\n", info.map.c_str(), unsigned( info.players ),
				 unsigned( info.maxPlayers ), info.items.size(), int( query.Compatible() ) );
	CHECK( info.map.empty() == false && info.players == 1 && info.maxPlayers == kMaxPlayers );
	CHECK( info.items.size() == 2 && info.items[0].mod == "chat" && info.items[0].sha256 == sha );
	CHECK( query.Compatible() );
	h.RunUntil( h.Now() + 1.0 );
	CHECK( h.server.GetStats().infoRequests == 1 && h.server.GetStats().joins == 1 );
	CHECK( h.bots[0].client->Names()[1].empty() ); // nobody else is in the game

	ServerQuery nobody;
	nobody.Start( "127.0.0.1", 47899, h.Now() );
	double asked = h.Now();
	while ( nobody.Update( h.Now() ) == false && h.Now() - asked < 10.0 )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
	}
	std::printf( "    nobody there: \"%s\" after %.1f s\n", nobody.Error().c_str(), h.Now() - asked );
	CHECK( nobody.Done() && nobody.Ok() == false && nobody.Error().rfind( "No answer", 0 ) == 0 );
}

// Typed text (the chat mod): a line one player types reaches everyone, with who said it; a channel
// no mod declared goes nowhere; a long line is cut; past the burst, only the sender is told to slow
// down. None of it touches the simulation.
void TestChat()
{
	using namespace net;
	CHECK( SanitizeText( "  hi\tthere \n", kMaxText ) == "hithere" );
	CHECK( SanitizeText( std::string( 300, 'x' ), kMaxText ).size() == kMaxText );
	Harness h( 47897, {}, {}, { { "chat.max_length", "20" }, { "chat.burst", "2" } } );
	h.AddBot();
	h.AddBot();
	h.RunUntil( 2.0 );
	GameClient& a = *h.bots[0].client;
	GameClient& b = *h.bots[1].client;
	CHECK( a.State() == ClientState::Playing && b.State() == ClientState::Playing );
	a.SendText( "chat", "  hello everyone  " );
	a.SendText( "nowhere", "lost" );
	h.RunUntil( h.Now() + 1.0 );
	for ( GameClient* c : { &a, &b } )
	{
		CHECK( c->TextCount() == 1 );
		if ( c->Texts().empty() == false )
		{
			const MsgText& t = c->Texts().back();
			std::printf( "    slot %u heard [%s] %u: \"%s\"\n", unsigned( c->Slot() ), t.channel.c_str(), unsigned( t.from ), t.text.c_str() );
			CHECK( t.channel == "chat" && t.from == a.Slot() && t.text == "hello everyone" );
		}
	}
	// A long line, then one past the burst (2 lines): the first is cut to chat.max_length, the
	// second is not said, and only its sender hears why.
	a.SendText( "chat", std::string( 50, 'x' ) );
	h.RunUntil( h.Now() + 0.5 );
	a.SendText( "chat", "one too many" );
	h.RunUntil( h.Now() + 1.0 );
	CHECK( b.TextCount() == 2 && b.Texts().back().text == std::string( 20, 'x' ) );
	CHECK( a.TextCount() == 3 && a.Texts().back().from == kNoSlot && a.Texts().back().text.rfind( "Slow down", 0 ) == 0 );
	std::printf( "    the sender was told: \"%s\"\n", a.Texts().empty() ? "" : a.Texts().back().text.c_str() );
	for ( GameClient* c : { &a, &b } )
	{
		CHECK( c->GetStats().desyncs == 0 && c->GetStats().checksumsVerified > 0 );
	}
}

// Movement parameters reach every client: the server's options, with its character's own values
// laid over them, travel in the welcome's config, so a client predicts the same walk and jump.
void TestMoveParams()
{
	// The robot's baked files, with how it moves added to its anim.cfg (what CbCharacter.movement bakes).
	std::filesystem::path source = std::filesystem::path( CB_SOURCE_DIR ) / "characters/robot/client/characters/robot";
	std::filesystem::path dir = std::filesystem::temp_directory_path() / "cinderbox_move_test";
	std::filesystem::remove_all( dir );
	std::filesystem::create_directories( dir );
	for ( const auto& entry : std::filesystem::directory_iterator( source ) )
	{
		if ( entry.is_regular_file() )
		{
			std::filesystem::copy_file( entry.path(), dir / entry.path().filename() );
		}
	}
	{
		std::ofstream cfg( dir / "anim.cfg", std::ios::app );
		cfg << "\nmove.jump_speed = 9\nmove.max_fall = 12\n";
	}
	std::string error, warnings;
	auto robot = LoadCharacterFolder( dir.string(), "robot", error, warnings );
	std::filesystem::remove_all( dir );
	if ( robot == nullptr )
	{
		std::printf( "    the robot did not load: %s\n", error.c_str() );
	}
	CHECK( robot != nullptr );
	if ( robot == nullptr )
	{
		return;
	}

	Harness h( 47841, {}, {}, {}, [&]( ServerOptions& options ) {
		// As `cb_server --move walk_speed=4.5 --move jump_speed=5` would.
		options.config.move.values[int( MoveParam::WalkSpeed )] = 4.5f;
		options.config.move.values[int( MoveParam::JumpSpeed )] = 5.0f;
		options.character = robot;
	} );
	MoveParams expected;
	expected.values[int( MoveParam::WalkSpeed )] = 4.5f; // the server's option
	expected.values[int( MoveParam::JumpSpeed )] = 9.0f; // the character's wins over the server's
	expected.values[int( MoveParam::MaxFall )] = 12.0f;	 // the character's
	Simulation& server = h.server.Sim();
	CHECK( server.Config().move == expected );

	h.AddBot().script = []( uint32_t tick ) {
		PlayerInput in;
		in.moveForward = 127;
		in.buttons = tick > 200 && tick < 204 ? uint8_t( BtnJump ) : uint8_t( 0 );
		return in;
	};
	float walk = 0.0f, rise = 0.0f;
	h.RunUntil( 5.0, [&]( double ) {
		const Character* c = server.PlayerCharacter( h.bots[0].client->Slot() );
		if ( c != nullptr )
		{
			walk = std::max( walk, b3Length( b3Vec3{ c->velocity.x, 0.0f, c->velocity.z } ) );
			rise = std::max( rise, c->velocity.y );
		}
	} );
	h.Report();
	std::printf( "    walked at %.2f m/s, jumped at %.2f m/s\n", walk, rise );
	CHECK( walk > 4.4f && walk < 4.51f );
	CHECK( rise > 8.0f && rise <= 9.0f );
	GameClient& client = *h.bots[0].client;
	CHECK( client.Session() != nullptr && client.Session()->Sim().Config().move == expected );
	CHECK( client.GetStats().desyncs == 0 && client.GetStats().checksumsVerified > 0 );
}

// Motions are predicted: behind 50 ms each way, a player's own dash and double jump happen in its
// own simulation on the tick of the press, exactly as the server then has them, so nothing is
// corrected. The dash mod's rules (the charges) run on the server; the dash itself is its motion
// set, baked from server_mods/dash/client/motion_sets.
void TestDash()
{
	std::filesystem::path replayPath = std::filesystem::temp_directory_path() / "cinderbox_dash_test.cbr";
	const uint32_t dashAt = 300, secondDashAt = 330, jumpAt = 480, doubleJumpAt = 495;
	{
		Harness h( 47851, replayPath.string(), {}, { { "dash.double_jump", "1" } } );
		const ModSchema& schema = h.server.Schema();
		// The boots' own key (V): used where they are.
		ActionBits dash = UseKey( schema, "dash.boots" );
		const BoardField* charges = schema.FindField( "dash.charges" );
		int started = schema.FindEvent( "dash.started" );
		int doubleJump = schema.FindEvent( "dash.double_jump" );
		CHECK( dash != 0 && charges != nullptr && started >= 0 && doubleJump >= 0 );
		bool haveSet = false;
		for ( const MotionSetInfo& set : schema.motionSets )
		{
			haveSet |= set.name == "dash.moves" && set.text.empty() == false;
		}
		CHECK( haveSet );
		if ( dash == 0 || charges == nullptr || haveSet == false )
		{
			return;
		}
		net::NetSimConfig link;
		link.latencyMs = 50;
		h.AddNetSim( 47852, link );
		Bot& bot = h.AddBot();

		// What the client predicted for each tick the first time it had simulated it, and what the
		// server then had for that tick.
		struct Seen
		{
			b3Vec3 position;
			b3Vec3 velocity;
			int32_t charges;
			uint32_t events;
		};
		std::map<uint32_t, Seen> predicted, truth;
		Simulation& server = h.server.Sim();
		auto look = [&]( const Simulation& sim, PlayerSlot slot, std::map<uint32_t, Seen>& into ) {
			uint32_t netId = sim.PlayerNetId( slot );
			const Character* c = sim.PlayerCharacter( slot );
			if ( netId == 0 || c == nullptr || into.count( sim.Tick() ) != 0 )
			{
				return;
			}
			into[sim.Tick()] = { sim.EntityTransform( netId )->position, c->velocity, sim.BoardValue( netId, charges->slot ),
								 sim.Globals().modEventCount };
		};
		// Every tick is looked at, of both: the server after each one it runs, the client as it asks
		// for the input of the next (an update of either can run several, on a busy machine).
		bot.script = [&]( uint32_t tick ) {
			GameClient& client = *bot.client;
			if ( client.Session() != nullptr && client.State() == ClientState::Playing )
			{
				look( client.Session()->Sim(), client.Slot(), predicted );
			}
			PlayerInput in;
			in.moveForward = 127;
			// Each key is held for a few ticks: one press.
			if ( ( tick >= dashAt && tick < dashAt + 4 ) || ( tick >= secondDashAt && tick < secondDashAt + 4 ) )
			{
				in.actions |= dash;
			}
			if ( ( tick >= jumpAt && tick < jumpAt + 3 ) || ( tick >= doubleJumpAt && tick < doubleJumpAt + 3 ) )
			{
				in.buttons = BtnJump;
			}
			return in;
		};
		h.server.SetTickObserver( [&] {
			GameClient& client = *bot.client;
			if ( client.State() == ClientState::Playing )
			{
				look( server, client.Slot(), truth );
			}
		} );
		h.RunUntil( 11.0 );
		h.server.SetTickObserver( {} );
		h.Report();
		GameClient& client = *h.bots[0].client;
		CHECK( client.GetStats().rttMs >= 90 );
		CHECK( client.GetStats().desyncs == 0 && client.GetStats().checksumsVerified > 0 );

		// The server had the dash on the tick of the press (the state after tick `dashAt`).
		CHECK( truth.count( dashAt ) && truth.count( dashAt + 1 ) && predicted.count( dashAt + 1 ) );
		if ( truth.count( dashAt + 1 ) == 0 || truth.count( dashAt ) == 0 || predicted.count( dashAt + 1 ) == 0 )
		{
			return;
		}
		float before = b3Length( truth[dashAt].velocity ), after = b3Length( truth[dashAt + 1].velocity );
		std::printf( "    server: %.2f m/s before the press, %.2f after; charges %d -> %d\n", before, after, truth[dashAt].charges,
					 truth[dashAt + 1].charges );
		CHECK( before < 3.1f && after > 10.5f );
		CHECK( truth[dashAt].charges == 2 && truth[dashAt + 1].charges == 1 );
		// The second dash took the other charge, and the mod gave both back in time.
		CHECK( truth.count( secondDashAt + 1 ) && truth[secondDashAt + 1].charges == 0 );
		CHECK( truth.rbegin()->second.charges == 2 );
		// The double jump: a second rise in the air.
		CHECK( truth.count( doubleJumpAt + 1 ) && truth[doubleJumpAt + 1].velocity.y > 6.0f );
		// (Other mods' motions may add their own: the flight mod's thrust is on Space in the air too.)
		CHECK( truth.rbegin()->second.events >= 3 );

		// The client had every one of those ticks the same way, the first time it simulated them:
		// its own dash was never corrected. (From a little before the first press to the end.)
		int compared = 0, wrong = 0;
		float worst = 0.0f;
		for ( const auto& [tick, seen] : predicted )
		{
			auto it = truth.find( tick );
			if ( tick < dashAt - 30 || it == truth.end() )
			{
				continue;
			}
			compared += 1;
			float off = b3Distance( seen.position, it->second.position );
			worst = std::max( worst, off );
			if ( off != 0.0f || seen.charges != it->second.charges || seen.events != it->second.events )
			{
				// The charge coming back is the mod's command: the one thing here that is the server's
				// to say, a round trip later. It does not move the player.
				wrong += off != 0.0f || seen.events != it->second.events ? 1 : 0;
			}
		}
		std::printf( "    %d predicted ticks compared with the server's: %d differ, worst %.4f m\n", compared, wrong, worst );
		CHECK( compared > 200 );
		CHECK( wrong == 0 && worst == 0.0f );
		// And it had the dash on the tick of the press: its prediction of that tick, made ahead of the
		// server, already had the speed, the charge taken and the events (the slot key's
		// "dash.boots.used", which starts the motion, and the motion's "dash.started").
		CHECK( b3Length( predicted[dashAt + 1].velocity ) > 10.5f && predicted[dashAt + 1].charges == 1 &&
			   predicted[dashAt + 1].events == truth[dashAt + 1].events && truth[dashAt + 1].events == truth[dashAt].events + 2 );
	}

	// The recording replays it: the motions travel in its header's schema.
	net::ReplayReader replay;
	std::string error;
	CHECK( replay.Open( replayPath.string(), error ) );
	CHECK( replay.MotionSets() != nullptr && replay.MotionSets()->list.size() >= 2 );
	Simulation sim( replay.Config(), replay.Map() );
	sim.SetAnimGraph( replay.Graph() );
	sim.SetAnimPacks( replay.Packs() );
	sim.SetMotions( replay.MotionSets() );
	sim.SetItemShapes( replay.Schema().itemShapes );
	size_t next = 0, verified = 0;
	for ( const InputFrame& frame : replay.Frames() )
	{
		while ( next < replay.Checksums().size() && replay.Checksums()[next].tick == sim.Tick() )
		{
			CHECK( sim.ComputeHash() == replay.Checksums()[next].hash );
			++verified;
			++next;
		}
		sim.Step( frame );
	}
	std::printf( "    replay: %zu ticks, %zu checksums verified, %u events\n", replay.Frames().size(), verified, sim.Globals().modEventCount );
	CHECK( verified > 5 && sim.Globals().modEventCount >= 3 );
	std::filesystem::remove( replayPath );
}

// Thrown from the hand across the wire, behind 100 ms: the impact grenade (the grenade mod: off by
// default, and tests run every mod) is an item the player carries (slot 7); its key plays the throw on the left arm, and the clip's marker lets go of the one
// carried (a CbLaunch of the one carried). The client's own simulation throws it on the tick the
// server does, where and as the server has it, and it flies the same way in both: nothing about it
// is corrected. What it does when it lands is the mod's C++ on the server: it goes off
// (grenade.blast) and is gone; and the mod gives a new one a little after.
void TestGrenade()
{
	const uint32_t throwAt = 300;
	// Then a crate is put down in front of the player (the props mod's key) and a second grenade
	// thrown at the floor beside it.
	const uint32_t crateAt = 480, secondAt = 600;
	// Then at its own feet: that one kills it, and the body is thrown.
	const uint32_t thirdAt = 720, fourthAt = 840;
	// (A grenade that kills at its centre and a metre or two around: the last one is thrown at the
	// thrower's own feet.)
	// (The throw is the character's to play: a character, and the grenade's pack.)
	const std::string root = CB_SOURCE_DIR;
	Harness h( 47881, {}, {}, { { "grenade.damage", "300" }, { "test.mods", "grenade" } }, [&]( ServerOptions& options ) {
		std::string error, warnings;
		options.character = LoadCharacterFolder( root + "/godot/characters/mannequin", "mannequin", error, warnings );
		options.loadAnimPack = [root]( const std::string& mod, const std::string& pack, std::string& error, std::string& warnings ) {
			return LoadAnimPackFolder( root + "/server_mods/" + mod + "/client", pack, error, warnings );
		};
	} );
	const ModSchema& schema = h.server.Schema();
	const ActionBits throwKey = UseKey( schema, "grenade.shell" ); // the impact grenade's own key (J)
	int shell = schema.FindItemKind( "grenade.shell" );
	int blast = schema.FindEvent( "grenade.blast" );
	ActionBits spawnKey = schema.ActionMask( "spawn_prop" );
	CHECK( schema.FindEvent( "grenade.release" ) >= 0 && shell >= 0 && blast >= 0 && spawnKey != 0 );
	if ( shell < 0 || blast < 0 )
	{
		return;
	}
	net::NetSimConfig link;
	link.latencyMs = 50;
	h.AddNetSim( 47882, link );
	Bot& bot = h.AddBot();

	// Where the grenade is at each tick: as the client first had it, and as the server had it.
	struct Seen
	{
		int count = 0;
		uint32_t netId = 0;
		b3Vec3 position = {};
	};
	std::map<uint32_t, Seen> predicted, truth;
	Simulation& server = h.server.Sim();
	auto look = [&]( const Simulation& sim, std::map<uint32_t, Seen>& into ) {
		if ( into.count( sim.Tick() ) != 0 )
		{
			return;
		}
		Seen seen;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const HeldItem* item = sim.FindEntity( r.netId ).try_get<HeldItem>();
			if ( item != nullptr && item->kind == uint16_t( shell ) && item->holder == 0 )
			{
				seen.count += 1;
				seen.netId = r.netId;
				seen.position = sim.EntityTransform( r.netId )->position;
			}
		}
		into[sim.Tick()] = seen;
	};
	bot.script = [&]( uint32_t tick ) {
		GameClient& client = *bot.client;
		if ( client.Session() != nullptr && client.State() == ClientState::Playing )
		{
			look( client.Session()->Sim(), predicted );
		}
		PlayerInput in;
		in.cameraPitch = 1500; // a little upward: a throw that lands some metres off
		if ( tick >= throwAt && tick < throwAt + 4 )
		{
			in.actions |= throwKey;
		}
		if ( tick >= crateAt && tick < crateAt + 4 )
		{
			in.actions |= spawnKey;
		}
		if ( tick >= secondAt - 30 )
		{
			in.cameraPitch = -7000; // down at the floor a few metres ahead
			in.actions |= tick >= secondAt && tick < secondAt + 4 ? throwKey : ActionBits( 0 );
		}
		if ( tick >= thirdAt - 30 )
		{
			in.cameraPitch = -15000; // straight down
			in.actions |= ( tick >= thirdAt && tick < thirdAt + 4 ) || ( tick >= fourthAt && tick < fourthAt + 4 ) ? throwKey : ActionBits( 0 );
		}
		return in;
	};
	uint32_t blasts = 0, seenEvents = 0, secondBlastTick = 0;
	b3Vec3 blastAt = {};
	// The crate: how fast it moves just before the second grenade, and the most after it went off.
	float crateBefore = -1.0f, crateAfter = 0.0f, crateFrom = 0.0f, crateSpin = 0.0f;
	// And the thrower, who stands in the blast: hurt, not thrown.
	float throwerMoved = 0.0f;
	// And its body, once its own grenade has killed it.
	float bodyFlew = 0.0f, bodySpeed = 0.0f;
	uint32_t lateBlasts = 0;
	bool died = false;
	b3Vec3 bodyFrom = {};
	bool bodySeen = false;
	h.server.SetTickObserver( [&] {
		if ( bot.client->State() != ClientState::Playing )
		{
			return;
		}
		look( server, truth );
		const SimGlobals& g = server.Globals();
		for ( ; seenEvents < g.modEventCount; ++seenEvents )
		{
			const ModEventRecord& e = g.modEvents[seenEvents % kModEventHistory];
			if ( int( e.type ) == blast )
			{
				if ( e.tick < secondAt )
				{
					blasts += 1;
					blastAt = e.point;
				}
				else
				{
					secondBlastTick = secondBlastTick == 0 ? e.tick : secondBlastTick;
					lateBlasts += 1;
				}
			}
		}
		for ( const Simulation::EntityRef& r : server.Entities() )
		{
			flecs::entity e = server.FindEntity( r.netId );
			const Prop* prop = e.try_get<Prop>();
			if ( prop == nullptr || prop->owner == 0 || e.has<HeldItem>() || e.has<Velocity>() == false )
			{
				continue;
			}
			float speed = b3Length( e.get<Velocity>().linear );
			if ( server.Tick() == secondAt )
			{
				crateBefore = speed;
				crateFrom = b3Distance( e.get<Transform>().position, server.EntityTransform( server.PlayerNetId( bot.client->Slot() ) )->position );
			}
			crateAfter = secondBlastTick != 0 ? std::max( crateAfter, speed ) : crateAfter;
			crateSpin = secondBlastTick != 0 ? std::max( crateSpin, b3Length( e.get<Velocity>().angular ) ) : crateSpin;
		}
		if ( const Character* c = server.PlayerCharacter( bot.client->Slot() );
			 c != nullptr && secondBlastTick != 0 && c->dead == 0 && server.Tick() < thirdAt )
		{
			throwerMoved = std::max( throwerMoved, b3Length( c->velocity ) );
		}
		if ( const Character* c = server.PlayerCharacter( bot.client->Slot() ) )
		{
			died |= c->dead != 0;
		}
		for ( const Simulation::EntityRef& r : server.Entities() )
		{
			flecs::entity e = server.FindEntity( r.netId );
			if ( e.has<Ragdoll>() == false )
			{
				continue;
			}
			b3Vec3 at = e.get<Transform>().position;
			bodyFrom = bodySeen ? bodyFrom : at;
			bodySeen = true;
			bodyFlew = std::max( bodyFlew, b3Distance( at, bodyFrom ) );
			bodySpeed = std::max( bodySpeed, b3Length( e.get<RagdollPose>().linear[0] ) );
		}
	} );
	h.RunUntil( 16.5 );
	h.server.SetTickObserver( {} );
	h.Report();
	GameClient& client = *bot.client;
	CHECK( client.GetStats().desyncs == 0 && client.GetStats().checksumsVerified > 0 );

	// The press plays the throw; the clip's marker lets go of the grenade it carried a quarter of a
	// second in (0.27 s: 16 ticks, and its motion hears it the tick after).
	uint32_t leftAt = 0;
	for ( const auto& [tick, seen] : truth )
	{
		leftAt = leftAt == 0 && tick >= throwAt && seen.count == 1 ? tick : leftAt;
	}
	std::printf( "    pressed at tick %u: it left the hand %u ticks later\n", throwAt, leftAt - throwAt );
	CHECK( leftAt > throwAt + 10 && leftAt < throwAt + 25 );
	if ( leftAt == 0 || truth.count( leftAt - 1 ) == 0 )
	{
		return;
	}
	CHECK( truth[leftAt - 1].count == 0 );
	// It flew, went off once, and was gone.
	uint32_t goneAt = 0;
	float furthest = 0.0f;
	b3Vec3 start = truth[leftAt].position;
	for ( const auto& [tick, seen] : truth )
	{
		if ( tick >= leftAt && seen.count == 1 )
		{
			furthest = std::max( furthest, b3Distance( seen.position, start ) );
		}
		goneAt = goneAt == 0 && tick > leftAt && seen.count == 0 ? tick : goneAt;
	}
	std::printf( "    flew %.1f m, went off %.2f s after it left (%u blast) at (%.1f, %.1f, %.1f)\n", furthest,
				 goneAt > leftAt ? float( goneAt - leftAt ) / 60.0f : -1.0f, blasts, blastAt.x, blastAt.y, blastAt.z );
	CHECK( blasts == 1 && goneAt > leftAt + 10 && furthest > 4.0f );

	// The client had the grenade on the same ticks, at the same places, the first time it simulated
	// them: its own throw was never corrected while it flew. (Its going off is the server's to say.)
	int compared = 0, wrong = 0;
	for ( const auto& [tick, seen] : predicted )
	{
		auto it = truth.find( tick );
		if ( tick < throwAt - 30 || it == truth.end() || ( goneAt != 0 && tick >= goneAt ) )
		{
			continue;
		}
		compared += 1;
		wrong += seen.count != it->second.count || seen.netId != it->second.netId || b3Distance( seen.position, it->second.position ) != 0.0f ? 1 : 0;
	}
	std::printf( "    %d predicted ticks compared with the server's: %d differ\n", compared, wrong );
	CHECK( compared > 40 && wrong == 0 );
	CHECK( predicted.count( leftAt ) && predicted[leftAt].count == 1 && predicted[leftAt].netId == truth[leftAt].netId );

	// A blast throws what is loose near it: the crate that lay still in front of the player moves.
	std::printf( "    a crate %.1f m away, at %.2f m/s before the second grenade: up to %.1f m/s after it went off\n", crateFrom, crateBefore,
				 crateAfter );
	CHECK( secondBlastTick > secondAt && crateBefore >= 0.0f && crateBefore < 0.5f && crateAfter > 3.0f );
	// It tumbles (the push is off its centre), and the thrower, alive in the same blast, stands.
	std::printf( "    the crate turns at up to %.1f rad/s; the thrower, alive, moves at %.2f m/s\n", crateSpin, throwerMoved );
	CHECK( crateSpin > 1.0f && throwerMoved < 0.5f );
	// Killed by its own (the fourth, at its feet), the body is thrown: it does not drop where it stood.
	std::printf( "    %u grenades went off after the first, and the thrower %s; its body goes %.1f m (up to %.1f m/s)\n", lateBlasts,
				 died ? "died" : "lived", bodyFlew, bodySpeed );
	CHECK( lateBlasts >= 2 && died );
	CHECK( bodySeen && bodyFlew > 2.0f );
}

// A chest across the wire, behind 100 ms: the chest mod's (a container template it declares, placed
// on the ground near the spawn point, filled with what a player may pick up). The bot walks to it,
// looks at it, opens it (the Open intent, in its own input) and drags the chest's first item into a
// slot of its bag: the item is the bot's, on the server and on its own screen, and nothing is
// corrected.
void TestChest()
{
	Harness h( 47885, {}, {}, { { "chest.count", "1" }, { "chest.radius", "4" }, { "chest.items", "3" } } );
	net::NetSimConfig link;
	link.latencyMs = 50;
	h.AddNetSim( 47886, link );
	Bot& bot = h.AddBot();
	Simulation& server = h.server.Sim();
	uint32_t chest = 0;
	uint8_t seq = 0;
	uint32_t openedAt = 0, movedAt = 0;
	const uint8_t bagSlot = 20;
	bot.script = [&]( uint32_t tick ) {
		PlayerInput in;
		in.intentSeq = seq;
		uint32_t me = server.PlayerNetId( bot.client->Slot() );
		if ( chest == 0 )
		{
			for ( const Simulation::EntityRef& r : server.Entities() )
			{
				chest = server.FindEntity( r.netId ).has<Container>() ? r.netId : chest;
			}
		}
		const Transform* at = me != 0 ? server.EntityTransform( me ) : nullptr;
		const Transform* to = chest != 0 ? server.EntityTransform( chest ) : nullptr;
		if ( at == nullptr || to == nullptr || tick < 120 )
		{
			return in;
		}
		// Toward it, looking at it; near enough, it opens it, then takes the first item.
		float dx = to->position.x - at->position.x;
		float dz = to->position.z - at->position.z;
		float flat = std::sqrt( dx * dx + dz * dz );
		float dy = to->position.y - ( at->position.y + kViewPivotHeight );
		in.cameraYaw = detmath::RadiansToYaw( std::atan2( dx, dz ) );
		in.cameraPitch = int16_t( std::atan2( dy, flat ) * 65536.0f / 6.2831853f );
		in.moveForward = flat > 1.6f && openedAt == 0 ? 127 : 0;
		if ( flat <= 1.6f && openedAt == 0 )
		{
			in.intent = uint8_t( SlotIntent::Open );
			in.intentSeq = ++seq;
			openedAt = tick;
		}
		else if ( openedAt != 0 && movedAt == 0 && tick > openedAt + 20 )
		{
			in.intent = uint8_t( SlotIntent::Move );
			in.intentA = kContainerSlotBit | 0;
			in.intentB = bagSlot;
			in.intentSeq = ++seq;
			movedAt = tick;
		}
		return in;
	};
	uint32_t firstItem = 0;
	h.server.SetTickObserver( [&] {
		if ( chest != 0 && firstItem == 0 )
		{
			firstItem = server.SlotItem( chest, 0 );
		}
	} );
	h.RunUntil( 8.0 );
	h.server.SetTickObserver( {} );
	h.Report();
	uint32_t me = server.PlayerNetId( bot.client->Slot() );
	int inChest = 0;
	for ( uint8_t s = 0; chest != 0 && s < 9; ++s )
	{
		inChest += server.SlotItem( chest, s ) != 0 ? 1 : 0;
	}
	std::printf( "    a chest %s; opened at tick %u, the first item moved at %u: in the bag's slot %d now %d, the chest holds %d\n",
				 chest != 0 ? "placed" : "missing", openedAt, movedAt, int( bagSlot ) + 1, int( server.SlotItem( me, bagSlot ) == firstItem && firstItem != 0 ),
				 inChest );
	CHECK( chest != 0 && firstItem != 0 && openedAt != 0 && movedAt != 0 );
	CHECK( server.SlotItem( me, bagSlot ) == firstItem && server.SlotItem( chest, 0 ) == 0 && inChest >= 1 );
	CHECK( server.FindEntity( me ).get<Slots>().open == chest );
	CHECK( bot.client->GetStats().desyncs == 0 );
	int compared = 0;
	CHECK( h.CompareWithServer( bot, compared ) == 0 );
}

// An intent a mod declares: the shop mod's "shop.buy", in a bot's input as a screen's button sends
// it (kFirstModIntent + its place, the offer in intentA). The mod reads it on the tick it arrives:
// a bat (1 coin of the 3 a player starts with) is bought and given, three times; asked a fourth
// time, refused (no coins left); the coins are the player's own private field, on its client too.
void TestShop()
{
	Harness h( 47887 );
	const ModSchema& schema = h.server.Schema();
	const int buy = schema.FindIntent( "shop.buy" );
	const int bat = schema.FindItemKind( "melee.bat" );
	const int refused = schema.FindEvent( "shop.refused" );
	const BoardField* coins = schema.FindField( "shop.coins" );
	CHECK( buy >= 0 && bat >= 0 && refused >= 0 && coins != nullptr );
	if ( buy < 0 || bat < 0 || coins == nullptr )
	{
		return;
	}
	Bot& bot = h.AddBot();
	const uint32_t firstAt = 200, every = 20, lastAt = firstAt + 3 * every;
	bot.script = [&]( uint32_t tick ) {
		PlayerInput in;
		in.intent = uint8_t( kFirstModIntent + buy );
		in.intentA = 1; // the bat
		in.intentSeq = tick < firstAt ? 0 : uint8_t( 1 + std::min( tick - firstAt, lastAt - firstAt ) / every );
		return in;
	};
	Simulation& server = h.server.Sim();
	auto bats = [&]() {
		uint32_t me = server.PlayerNetId( bot.client->Slot() );
		int n = 0;
		for ( const Simulation::EntityRef& r : server.Entities() )
		{
			const HeldItem* item = server.FindEntity( r.netId ).try_get<HeldItem>();
			n += item != nullptr && me != 0 && item->holder == me && item->kind == uint16_t( bat ) ? 1 : 0;
		}
		return n;
	};
	int32_t coinsBefore = -1, coinsEnd = -1;
	int batsBefore = -1, batsEnd = -1;
	uint32_t refusals = 0, seenEvents = 0;
	h.server.SetTickObserver( [&] {
		uint32_t tick = server.Tick();
		int32_t now = h.server.Privates( bot.client->Slot() ).values[coins->slot];
		if ( tick == firstAt - 10 )
		{
			coinsBefore = now;
			batsBefore = bats();
		}
		if ( tick == lastAt + 40 )
		{
			coinsEnd = now;
			batsEnd = bats();
		}
		const SimGlobals& g = server.Globals();
		for ( ; seenEvents < g.modEventCount; ++seenEvents )
		{
			refusals += int( g.modEvents[seenEvents % kModEventHistory].type ) == refused ? 1 : 0;
		}
	} );
	h.RunUntil( 5.5 );
	h.server.SetTickObserver( {} );
	h.Report();
	std::printf( "    coins %d, bats %d; after asking for four: %d, %d (%u refused); its own client sees %d coins\n", coinsBefore, batsBefore,
				 coinsEnd, batsEnd, refusals, bot.client->Privates().values[coins->slot] );
	CHECK( coinsBefore == 3 && batsBefore == 1 );
	CHECK( coinsEnd == 0 && batsEnd == 4 && refusals == 1 );
	CHECK( bot.client->Privates().values[coins->slot] == 0 && bot.client->GetStats().desyncs == 0 );
}

// The brawler's kit (the brawl mod, step 5's measure): bot A walks up to bot B and goes through it,
// the skills by their slots' keys (2 to 5: the fist stays in the hand all the while):
// three jabs (the third a finisher), a dash strike pressed twice (the second within its 3 s does
// nothing), an uppercut, a jump and a ground slam that lands beside B, and a jab while B guards
// (a quarter of the damage). What each did is counted by its events on the server; B's damage by
// combat.damage. Nothing is corrected on either client.
void TestBrawl()
{
	Harness h( 47889, {}, {}, { { "test.mods", "brawl" }, { "combat.max_health", "1000" } } );
	const ModSchema& schema = h.server.Schema();
	// The skills' slots' keys use them (CbItem.slot_key_uses): 2 to 5, as a player's input has them.
	const uint16_t dashKey = 1u << 1, upperKey = 1u << 2, slamKey = 1u << 3, guardKey = 1u << 4;
	const int jab = schema.FindEvent( "brawl.jab.hit" ), finisher = schema.FindEvent( "brawl.finisher.hit" );
	const int dashHit = schema.FindEvent( "brawl.dash_strike.hit" ), dashStarted = schema.FindEvent( "brawl.dash_strike.started" );
	const int upperHit = schema.FindEvent( "brawl.uppercut.hit" ), landed = schema.FindEvent( "brawl.slam.landed" );
	const int damage = schema.FindEvent( "combat.damage" );
	CHECK( jab >= 0 && finisher >= 0 && dashHit >= 0 && dashStarted >= 0 && upperHit >= 0 && landed >= 0 && damage >= 0 );
	if ( jab < 0 || damage < 0 )
	{
		return;
	}
	Simulation& server = h.server.Sim();
	h.AddBot();
	h.AddBot();
	Bot& a = h.bots[0]; // (both first: a second AddBot may move the first)
	Bot& b = h.bots[1];
	bool fistKept = true;
	// A: to within 1.6 m of B, looking at it; then the step of the plan that this tick is in.
	a.script = [&]( uint32_t tick ) {
		PlayerInput in;
		uint32_t me = server.PlayerNetId( a.client->Slot() ), it = server.PlayerNetId( b.client->Slot() );
		const Transform* at = me != 0 ? server.EntityTransform( me ) : nullptr;
		const Transform* to = it != 0 ? server.EntityTransform( it ) : nullptr;
		if ( at != nullptr && to != nullptr )
		{
			float dx = to->position.x - at->position.x, dz = to->position.z - at->position.z;
			in.cameraYaw = detmath::RadiansToYaw( std::atan2( dx, dz ) );
			in.moveForward = dx * dx + dz * dz > 1.6f * 1.6f ? 127 : 0;
		}
		in.actions = tick >= 280 && tick < 290 ? kKeySlot1 : 0; // the fist out
		auto at_ = [&]( uint32_t when ) { return tick >= when && tick < when + 3; };
		if ( at_( 320 ) || at_( 345 ) || at_( 370 ) || at_( 1010 ) )
		{
			in.actions |= kKeyUse; // jab, jab, finisher; and the jab on a guard
		}
		if ( at_( 520 ) || at_( 560 ) )
		{
			in.slotKeys |= dashKey; // the second within the 3 s: nothing
		}
		if ( at_( 700 ) )
		{
			in.slotKeys |= upperKey;
		}
		if ( at_( 860 ) )
		{
			in.buttons |= BtnJump;
		}
		if ( at_( 880 ) )
		{
			in.slotKeys |= slamKey;
		}
		// The skills' keys never take the fist out of the hand.
		fistKept = fistKept && ( tick < 300 || server.SelectedSlot( me ) == 0 );
		return in;
	};
	// B stands; it guards around the last jab.
	b.script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.slotKeys = tick >= 990 && tick < 1040 ? guardKey : 0;
		return in;
	};
	std::map<int, int> count;
	std::vector<int32_t> damages;
	uint32_t seen = 0;
	h.server.SetTickObserver( [&] {
		const SimGlobals& g = server.Globals();
		uint32_t victim = server.PlayerNetId( b.client->Slot() );
		for ( ; seen < g.modEventCount; ++seen )
		{
			const ModEventRecord& e = g.modEvents[seen % kModEventHistory];
			count[int( e.type )] += 1;
			if ( int( e.type ) == damage && e.netIdB == victim )
			{
				damages.push_back( e.value );
			}
		}
	} );
	h.RunUntil( 18.5 );
	h.server.SetTickObserver( {} );
	h.Report();
	std::string list;
	for ( int32_t d : damages )
	{
		list += " " + std::to_string( d );
	}
	std::printf( "    jabs %d, finishers %d, dash strikes started %d (pressed twice), hit %d, uppercuts %d, slams landed %d; B's damage:%s\n",
				 count[jab], count[finisher], count[dashStarted], count[dashHit], count[upperHit], count[landed], list.c_str() );
	CHECK( count[jab] == 3 && count[finisher] == 1 );
	CHECK( count[dashStarted] == 1 && count[dashHit] == 1 && count[upperHit] == 1 && count[landed] == 1 );
	// 8, 8, 20, 15, 18, 25, and the guarded jab: 2.
	CHECK( damages == std::vector<int32_t>( { 8, 8, 20, 15, 18, 25, 2 } ) );
	CHECK( fistKept );
	CHECK( a.client->GetStats().desyncs == 0 && b.client->GetStats().desyncs == 0 );
}

// A fused grenade across the wire, behind 100 ms: the frag mod's grenade is thrown from the hand like
// the impact one, bounces off the floor without going off, and when its time is up (2.5 s after it
// left the hand) the simulation says frag.grenade.expired where it is, with who threw it, and
// removes it, on the client on the same tick as on the server; the mod's C++ sets it off there.
void TestFrag()
{
	const uint32_t throwAt = 300;
	const std::string root = CB_SOURCE_DIR;
	Harness h( 47883, {}, {}, {}, [&]( ServerOptions& options ) {
		std::string error, warnings;
		options.character = LoadCharacterFolder( root + "/godot/characters/mannequin", "mannequin", error, warnings );
		options.loadAnimPack = [root]( const std::string& mod, const std::string& pack, std::string& error, std::string& warnings ) {
			return LoadAnimPackFolder( root + "/server_mods/" + mod + "/client", pack, error, warnings );
		};
	} );
	const ModSchema& schema = h.server.Schema();
	const ActionBits throwKey = UseKey( schema, "frag.grenade" ); // the frag's own key (H)
	int frag = schema.FindItemKind( "frag.grenade" );
	int expired = schema.FindEvent( "frag.grenade.expired" );
	int blast = schema.FindEvent( "frag.blast" );
	CHECK( frag >= 0 && expired >= 0 && blast >= 0 && schema.itemShapes[size_t( frag )].expiredEvent == uint16_t( expired ) );
	if ( frag < 0 || expired < 0 || blast < 0 )
	{
		return;
	}
	net::NetSimConfig link;
	link.latencyMs = 50;
	h.AddNetSim( 47884, link );
	Bot& bot = h.AddBot();

	// The grenade in the world at each tick (as the client first had it, and as the server had it),
	// and the expired events each saw.
	struct Seen
	{
		int count = 0;
		uint32_t netId = 0;
		b3Vec3 position = {};
		b3Vec3 velocity = {};
	};
	std::map<uint32_t, Seen> predicted, truth;
	std::map<uint32_t, ModEventRecord> predictedExpiry, trueExpiry;
	auto look = [&]( const Simulation& sim, std::map<uint32_t, Seen>& into, std::map<uint32_t, ModEventRecord>& expiries ) {
		if ( into.count( sim.Tick() ) != 0 )
		{
			return;
		}
		Seen seen;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const HeldItem* item = sim.FindEntity( r.netId ).try_get<HeldItem>();
			if ( item != nullptr && item->kind == uint16_t( frag ) && item->holder == 0 )
			{
				seen.count += 1;
				seen.netId = r.netId;
				seen.position = sim.EntityTransform( r.netId )->position;
				seen.velocity = sim.FindEntity( r.netId ).get<Velocity>().linear;
			}
		}
		into[sim.Tick()] = seen;
		const SimGlobals& g = sim.Globals();
		for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
		{
			const ModEventRecord& e = g.modEvents[i];
			if ( int( e.type ) == expired )
			{
				expiries[e.tick] = e;
			}
		}
	};
	bot.script = [&]( uint32_t tick ) {
		GameClient& client = *bot.client;
		if ( client.Session() != nullptr && client.State() == ClientState::Playing )
		{
			look( client.Session()->Sim(), predicted, predictedExpiry );
		}
		PlayerInput in;
		in.cameraPitch = -5000; // down at the floor a few metres ahead: it bounces
		in.actions = tick >= throwAt && tick < throwAt + 4 ? throwKey : ActionBits( 0 );
		return in;
	};
	Simulation& server = h.server.Sim();
	uint32_t seenEvents = 0, blastTick = 0, blastBy = 0;
	b3Vec3 blastAt = {};
	// How many the player carries (one stack of up to three), by tick.
	std::map<uint32_t, int> stack;
	h.server.SetTickObserver( [&] {
		if ( bot.client->State() != ClientState::Playing )
		{
			return;
		}
		look( server, truth, trueExpiry );
		uint32_t me = server.PlayerNetId( bot.client->Slot() );
		int count = 0;
		for ( const Simulation::EntityRef& r : server.Entities() )
		{
			const HeldItem* item = server.FindEntity( r.netId ).try_get<HeldItem>();
			count += item != nullptr && item->holder == me && item->kind == uint16_t( frag ) ? int( StackOf( *item ) ) : 0;
		}
		stack[server.Tick()] = count;
		const SimGlobals& g = server.Globals();
		for ( ; seenEvents < g.modEventCount; ++seenEvents )
		{
			const ModEventRecord& e = g.modEvents[seenEvents % kModEventHistory];
			if ( int( e.type ) == blast && blastTick == 0 )
			{
				blastTick = e.tick;
				blastAt = e.point;
				blastBy = e.netIdA;
			}
		}
	} );
	h.RunUntil( 9.0 );
	h.server.SetTickObserver( {} );
	h.Report();
	CHECK( bot.client->GetStats().desyncs == 0 );

	// Thrown from the hand at its clip's marker; then it lies in the world until its time is up.
	uint32_t leftAt = 0, goneAt = 0;
	bool bounced = false;
	for ( const auto& [tick, seen] : truth )
	{
		leftAt = leftAt == 0 && tick >= throwAt && seen.count == 1 ? tick : leftAt;
		goneAt = goneAt == 0 && leftAt != 0 && tick > leftAt && seen.count == 0 ? tick : goneAt;
		bounced |= leftAt != 0 && goneAt == 0 && seen.count == 1 && seen.velocity.y > 1.0f && tick > leftAt + 5;
	}
	uint32_t player = server.PlayerNetId( bot.client->Slot() );
	const ModEventRecord* fuse = trueExpiry.empty() ? nullptr : &trueExpiry.begin()->second;
	std::printf( "    left the hand %u ticks after the press; bounced %d; gone %.2f s after it left; its time up at tick %u (the thrower's %d); "
				 "the blast at tick %u, %.2f m from where it was\n",
				 leftAt - throwAt, int( bounced ), float( goneAt - leftAt ) / 60.0f, fuse != nullptr ? fuse->tick : 0u,
				 int( fuse != nullptr && fuse->netIdB == player ), blastTick, fuse != nullptr ? b3Distance( blastAt, fuse->point ) : -1.0f );
	CHECK( leftAt > throwAt + 10 && leftAt < throwAt + 25 && bounced );
	CHECK( goneAt >= leftAt + 149 && goneAt <= leftAt + 152 );
	CHECK( fuse != nullptr && trueExpiry.size() == 1 && fuse->netIdB == player && fuse->netIdA == truth[leftAt].netId );
	CHECK( blastTick != 0 && fuse != nullptr && blastTick > fuse->tick && blastTick <= fuse->tick + 2 && b3Distance( blastAt, fuse->point ) < 0.001f );
	// The client had all of it on the same ticks, at the same places, the first time it simulated
	// them: the flight, the bounces, and its time being up.
	int compared = 0, wrong = 0;
	for ( const auto& [tick, seen] : predicted )
	{
		auto it = truth.find( tick );
		if ( tick < throwAt || it == truth.end() )
		{
			continue;
		}
		compared += 1;
		wrong += seen.count != it->second.count || seen.netId != it->second.netId || b3Distance( seen.position, it->second.position ) != 0.0f ? 1 : 0;
	}
	bool sameExpiry = fuse != nullptr && predictedExpiry.count( fuse->tick ) == 1 && b3Distance( predictedExpiry[fuse->tick].point, fuse->point ) == 0.0f;
	std::printf( "    %d predicted ticks compared with the server's: %d differ; its time up predicted on the same tick %d\n", compared, wrong,
				 int( sameExpiry ) );
	CHECK( compared > 150 && wrong == 0 && sameExpiry );
	// Three to a stack: full before the throw, one fewer after, full again a refill later.
	std::printf( "    carried: %d before the throw, %d after it, %d a refill later\n", stack[throwAt - 1], stack[leftAt + 5], stack[leftAt + 80] );
	CHECK( stack[throwAt - 1] == 3 && stack[leftAt + 5] == 2 && stack[leftAt + 80] == 3 );
}

// The frag grenade breaks a wall (a breakable: a map template with pieces): thrown ahead at a wall
// of 8 x 4 bricks standing in its way. With a break radius bigger than the wall, its blast takes it
// all; with the default (half its blast's), it blows a hole: the bricks near it fly, the rest stand
// (static, held at the wall's foot), and, tick for tick, the client that predicted it has the same
// bricks standing where the server has them.
void TestFragBreaks()
{
	const std::string root = CB_SOURCE_DIR;
	LevelLayout map = GetLevelLayout();
	map.name = "wall";
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 2.0f, 1.0f, 0.2f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 0.24f, 0.24f, 0.19f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );
	EntityTemplate wall;
	wall.name = "wall";
	AuthoredComponent body;
	body.id = Fnv32( "Body" );
	AuthoredField fixed;
	fixed.id = Fnv32( "type" );
	fixed.raw[0] = 0;
	body.fields.push_back( fixed );
	wall.components.push_back( body );
	wall.shapes = uint32_t( map.shapeSets.size() - 2 );
	const uint32_t first = uint32_t( map.templates.size() );
	for ( int row = 0; row < 4; ++row )
	{
		for ( int column = 0; column < 8; ++column )
		{
			TemplatePiece piece = { first + 1, { -1.75f + 0.5f * float( column ), -0.75f + 0.5f * float( row ), 0.0f } };
			piece.held = row == 0;
			wall.pieces.push_back( piece );
			const uint16_t i = uint16_t( row * 8 + column );
			if ( column < 7 )
			{
				wall.links.push_back( { i, uint16_t( i + 1 ) } );
			}
			if ( row < 3 )
			{
				wall.links.push_back( { i, uint16_t( i + 8 ) } );
			}
		}
	}
	EntityTemplate brick;
	brick.name = "brick";
	brick.shapes = uint32_t( map.shapeSets.size() - 1 );
	AuthoredComponent rubble;
	rubble.id = Fnv32( "Debris" );
	brick.components.push_back( rubble );
	map.templates.push_back( wall );
	map.templates.push_back( brick );
	// Just in front of where the first player stands: its grenade hits it and goes off at its foot.
	const b3Vec3 spawn = map.spawnCenter;
	map.instances.push_back( { first, { spawn.x - 5.25f, 1.0f, spawn.z + 1.5f }, 0.0f, 0.0f } );
	QuantizeLayout( map );
	std::vector<uint8_t> bytes;
	SerializeMap( map, bytes );
	std::filesystem::path mapPath = std::filesystem::temp_directory_path() / "cb_net_test_frag_wall.cbmap";
	std::string error;
	CHECK( WriteMapFile( mapPath.string(), bytes, error ) );

	// Bricks loose, and those standing (by NetId, where they are).
	struct Bricks
	{
		int walls = 0;
		int loose = 0;
		std::vector<std::pair<uint32_t, b3Vec3>> standing;
	};
	auto run = [&]( uint16_t port, const char* breakRadius, Bricks& server ) {
		const uint32_t throwAt = 240;
		std::map<std::string, std::string> options;
		if ( breakRadius != nullptr )
		{
			options["frag.break_radius"] = breakRadius;
		}
		Harness h( port, {}, mapPath.string(), options, [&]( ServerOptions& options ) {
			std::string error, warnings;
			options.character = LoadCharacterFolder( root + "/godot/characters/mannequin", "mannequin", error, warnings );
			options.loadAnimPack = [root]( const std::string& mod, const std::string& pack, std::string& error, std::string& warnings ) {
				return LoadAnimPackFolder( root + "/server_mods/" + mod + "/client", pack, error, warnings );
			};
		} );
		const ActionBits throwKey = UseKey( h.server.Schema(), "frag.grenade" );
		net::NetSimConfig link;
		link.latencyMs = 50;
		h.AddNetSim( uint16_t( port + 1 ), link );
		Bot& bot = h.AddBot();
		bot.script = [&]( uint32_t tick ) {
			PlayerInput in;
			in.cameraPitch = -3000;
			in.actions = tick >= throwAt && tick < throwAt + 4 ? throwKey : ActionBits( 0 );
			return in;
		};
		auto bricks = [&]( const Simulation& sim ) {
			Bricks out;
			for ( const Simulation::EntityRef& r : sim.Entities() )
			{
				flecs::entity e = sim.FindEntity( r.netId );
				const TemplateRef* ref = e.try_get<TemplateRef>();
				out.walls += ref != nullptr && ref->index == first ? 1 : 0;
				if ( ref != nullptr && ref->index == first + 1 )
				{
					if ( e.has<WallPiece>() )
					{
						out.standing.push_back( { r.netId, e.get<Transform>().position } );
					}
					else
					{
						++out.loose;
					}
				}
			}
			return out;
		};
		uint32_t brokenAt = 0;
		b3Vec3 blastAt = {};
		const int blastEvent = h.server.Schema().FindEvent( "frag.blast" );
		h.server.SetTickObserver( [&] {
			const Simulation& sim = h.server.Sim();
			brokenAt = brokenAt == 0 && bricks( sim ).walls == 0 ? sim.Tick() : brokenAt;
			const SimGlobals& g = sim.Globals();
			for ( uint32_t k = 0; k < std::min<uint32_t>( g.modEventCount, kModEventHistory ); ++k )
			{
				const ModEventRecord& record = g.modEvents[( g.modEventCount - 1 - k ) % kModEventHistory];
				if ( int( record.type ) == blastEvent )
				{
					blastAt = record.point;
				}
			}
		} );
		h.RunUntil( 8.0 );
		h.server.SetTickObserver( {} );
		h.Report();
		server = bricks( h.server.Sim() );
		GameClient& client = *bot.client;
		const Bricks mine = client.Session() != nullptr ? bricks( client.Session()->Sim() ) : Bricks{ -1 };
		bool same = server.standing.size() == mine.standing.size();
		for ( size_t i = 0; same && i < server.standing.size(); ++i )
		{
			same = server.standing[i].first == mine.standing[i].first && b3Distance( server.standing[i].second, mine.standing[i].second ) < 1e-5f;
		}
		std::printf( "    the blast at (%.2f %.2f %.2f); the wall at (%.2f %.2f %.2f)\n", blastAt.x, blastAt.y, blastAt.z, spawn.x - 5.25f, 1.0f, spawn.z + 1.5f );
		std::printf( "    break radius %s: the wall broke at tick %u (%.2f s after the throw); the server has %d wall, %d bricks loose, %d "
					 "standing; the client %d wall, %d loose, %d standing (%s); desyncs %u\n",
					 breakRadius != nullptr ? breakRadius : "default", brokenAt, float( brokenAt - throwAt ) / 60.0f, server.walls, server.loose,
					 int( server.standing.size() ), mine.walls, mine.loose, int( mine.standing.size() ), same ? "the same" : "not the same",
					 client.GetStats().desyncs );
		CHECK( brokenAt > throwAt && server.walls == 0 && mine.walls == 0 && mine.loose == server.loose && same && client.GetStats().desyncs == 0 );
	};
	Bricks all, hole;
	run( 47900, "12", all );
	CHECK( all.loose == 32 && all.standing.empty() );
	run( 47902, nullptr, hole );
	CHECK( hole.loose > 0 && hole.standing.size() > 8 && hole.loose + int( hole.standing.size() ) == 32 );
	std::error_code ignored;
	std::filesystem::remove( mapPath, ignored );
}

// Motions that hold are predicted too: behind 50 ms each way, flying into a wall, landing, a
// jetpack run until its tank is empty and a glide down are, tick for tick, what the server then
// has: the position, the fuel the gauge shows, the events. The flight mod's C++ only gives the fuel
// a player joins with; the rest is its motion set.
void TestFlight()
{
	// Ticks: flight on, up and forward into the wall, let go, off and down; then the jetpack until
	// it is empty, and a glide down.
	const uint32_t flyOn = 300, level = 345, letGo = 700, flyOff = 760, thrustFrom = 960, thrustTo = 1230, glideTo = 1410;
	// The jetpack is off unless the server says so.
	Harness h( 47861, {}, {}, { { "flight.jetpack", "1" }, { "test.mods", "flight" } } );
	const ModSchema& schema = h.server.Schema();
	uint16_t fly = schema.ActionMask( "fly" );
	const BoardField* on = schema.FindField( "flight.on" );
	const BoardField* fuel = schema.FindField( "flight.fuel" );
	bool haveSet = false;
	for ( const MotionSetInfo& set : schema.motionSets )
	{
		haveSet |= set.name == "flight.moves" && set.text.empty() == false;
	}
	CHECK( fly != 0 && on != nullptr && fuel != nullptr && haveSet );
	if ( fly == 0 || on == nullptr || fuel == nullptr || haveSet == false )
	{
		return;
	}
	net::NetSimConfig link;
	link.latencyMs = 50;
	h.AddNetSim( 47862, link );
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		if ( ( tick >= flyOn && tick < flyOn + 4 ) || ( tick >= flyOff && tick < flyOff + 4 ) )
		{
			in.actions = fly;
		}
		if ( tick >= flyOn && tick < letGo )
		{
			// Toward the sandbox's wall, climbing.
			in.moveForward = 127;
			in.cameraPitch = tick < level ? int16_t( 2500 ) : int16_t( 0 ); // a little way up, below the walls' tops
		}
		if ( ( tick >= thrustFrom && tick < thrustTo ) )
		{
			in.buttons = BtnJump;
		}
		if ( tick >= thrustTo && tick < glideTo )
		{
			in.buttons = BtnSprint;
			in.moveForward = -127;
		}
		return in;
	};

	struct Seen
	{
		b3Vec3 position;
		int32_t on;
		int32_t fuel; // the float's bits: compared exactly
		uint32_t events;
		uint8_t grounded;
	};
	std::map<uint32_t, Seen> predicted, truth;
	Simulation& server = h.server.Sim();
	auto look = [&]( const Simulation& sim, PlayerSlot slot, std::map<uint32_t, Seen>& into ) {
		uint32_t netId = sim.PlayerNetId( slot );
		const Character* c = sim.PlayerCharacter( slot );
		if ( netId == 0 || c == nullptr || into.count( sim.Tick() ) != 0 )
		{
			return;
		}
		into[sim.Tick()] = { sim.EntityTransform( netId )->position, sim.BoardValue( netId, on->slot ), sim.BoardValue( netId, fuel->slot ),
							 sim.Globals().modEventCount, c->grounded };
	};
	// The server's state at every tick it runs (an update can run several); the client's as it shows it.
	h.server.SetTickObserver( [&] {
		GameClient& client = *h.bots[0].client;
		if ( client.State() == ClientState::Playing )
		{
			look( server, client.Slot(), truth );
		}
	} );
	h.RunUntil( 33.0, [&]( double ) {
		GameClient& client = *h.bots[0].client;
		if ( client.Session() != nullptr && client.State() == ClientState::Playing )
		{
			look( client.Session()->Sim(), client.Slot(), predicted );
		}
	} );
	h.server.SetTickObserver( {} );
	h.Report();
	GameClient& client = *h.bots[0].client;
	CHECK( client.GetStats().rttMs >= 90 );
	CHECK( client.GetStats().desyncs == 0 && client.GetStats().checksumsVerified > 0 );

	// What the server had: flight on by the press, a climb, a stop at the wall, down again; a tank
	// run empty in the air and filled again on the ground; a slow fall while gliding.
	auto at = [&]( uint32_t tick ) { return truth.count( tick ) ? truth[tick] : Seen{}; };
	float start = at( flyOn ).position.y;
	CHECK( at( flyOn ).on == 0 && at( flyOn + 1 ).on == 1 && at( flyOff + 1 ).on == 0 );
	float top = at( letGo ).position.y - start;
	float along = b3Distance( b3Vec3{ at( letGo ).position.x, 0.0f, at( letGo ).position.z },
							  b3Vec3{ at( flyOn ).position.x, 0.0f, at( flyOn ).position.z } );
	// It flew until the wall stopped it: the last half second before the keys were let go went nowhere.
	float lastStretch = b3Distance( at( letGo ).position, at( letGo - 30 ).position );
	std::printf( "    flew %.2f m up and %.2f m along; the last half second %.3f m (the wall)\n", top, along, lastStretch );
	// (How high depends a little on which inputs arrived late: a busy machine repeats a held key for a
	// tick or two. The wall, not the height, is what stops it.)
	CHECK( top > 0.3f && top < 3.5f && along > 3.0f && along < 38.0f && lastStretch < 0.05f );
	CHECK( at( thrustFrom - 1 ).grounded == 1 && BoardToFloat( at( thrustFrom - 1 ).fuel ) >= 100.0f );
	float lowest = 100.0f, peak = 0.0f, fastestFall = 0.0f;
	for ( const auto& [tick, seen] : truth )
	{
		if ( tick > thrustFrom && tick <= glideTo )
		{
			lowest = std::min( lowest, BoardToFloat( seen.fuel ) );
			peak = std::max( peak, seen.position.y - start );
		}
		if ( tick > thrustTo + 60 && tick <= glideTo && seen.grounded == 0 && truth.count( tick - 1 ) )
		{
			fastestFall = std::min( fastestFall, ( seen.position.y - truth[tick - 1].position.y ) * 60.0f );
		}
	}
	std::printf( "    jetpack: fuel down to %.2f, %.2f m up; gliding down at %.2f m/s\n", lowest, peak, fastestFall );
	CHECK( lowest <= 0.0f && peak > 8.0f );
	CHECK( fastestFall < -1.0f && fastestFall > -2.6f );
	CHECK( truth.rbegin()->second.grounded == 1 && BoardToFloat( truth.rbegin()->second.fuel ) >= 100.0f );

	// The client had every one of those ticks the same way the first time it simulated them.
	int compared = 0, wrong = 0;
	float worst = 0.0f;
	for ( const auto& [tick, seen] : predicted )
	{
		auto it = truth.find( tick );
		if ( tick < flyOn - 30 || it == truth.end() )
		{
			continue;
		}
		compared += 1;
		float off = b3Distance( seen.position, it->second.position );
		worst = std::max( worst, off );
		wrong += off != 0.0f || seen.on != it->second.on || seen.fuel != it->second.fuel || seen.events != it->second.events ? 1 : 0;
	}
	std::printf( "    %d predicted ticks compared with the server's: %d differ, worst %.4f m\n", compared, wrong, worst );
	CHECK( compared > 900 );
	CHECK( wrong == 0 && worst == 0.0f );
	// Its own press switched flight on in its own prediction of that tick.
	CHECK( predicted.count( flyOn + 1 ) && predicted[flyOn + 1].on == 1 );
}

// A grappling hook is predicted: behind 50 ms each way, the throw (a press), the pull along the rope
// and the letting go (the next press) are, tick for tick, what the server then has. And two players with their hooks in one
// ball agree with the server about where everything went. The grapple mod's C++ declares names;
// the hook is its motion.
void TestGrapple()
{
	const uint32_t throwAt = 300, letGo = 420, again = 480, letGoAgain = 600;
	{
		Harness h( 47871 );
		const ModSchema& schema = h.server.Schema();
		ActionBits grapple = UseKey( schema, "grapple.hook" ); // the hook's own key (Q)
		int fired = schema.FindEvent( "grapple.fired" );
		CHECK( grapple != 0 && fired >= 0 );
		if ( grapple == 0 )
		{
			return;
		}
		net::NetSimConfig link;
		link.latencyMs = 50;
		h.AddNetSim( 47872, link );
		h.AddBot().script = [=]( uint32_t tick ) {
			PlayerInput in;
			in.cameraPitch = 500; // at the wall ahead, below its top
			// The first one ends at that wall: the second is thrown at the wall to the side.
			in.cameraYaw = tick >= letGo + 20 ? uint16_t( 16384 ) : uint16_t( 0 );
			// One press throws it, the next lets it go: each key press is a few ticks long.
			for ( uint32_t press : { throwAt, letGo, again, letGoAgain } )
			{
				if ( tick >= press && tick < press + 4 )
				{
					in.actions = grapple;
				}
			}
			// Swinging a little on the second one.
			in.moveRight = tick >= again && tick < letGoAgain ? int8_t( 127 ) : int8_t( 0 );
			return in;
		};
		struct Seen
		{
			b3Vec3 position;
			b3Vec3 end;
			bool tethered;
			bool holds;
			uint32_t events;
		};
		std::map<uint32_t, Seen> predicted, truth;
		Simulation& server = h.server.Sim();
		auto look = [&]( const Simulation& sim, PlayerSlot slot, std::map<uint32_t, Seen>& into ) {
			uint32_t netId = sim.PlayerNetId( slot );
			if ( netId == 0 || sim.EntityTransform( netId ) == nullptr || into.count( sim.Tick() ) != 0 )
			{
				return;
			}
			Seen seen{ sim.EntityTransform( netId )->position, {}, false, false, sim.Globals().modEventCount };
			uint16_t motion = 0;
			seen.tethered = sim.EntityHold( netId, seen.end, seen.holds, motion );
			into[sim.Tick()] = seen;
		};
		// The server's state at every tick it runs (an update can run several); the client's as it shows it.
		h.server.SetTickObserver( [&] {
			GameClient& client = *h.bots[0].client;
			if ( client.State() == ClientState::Playing )
			{
				look( server, client.Slot(), truth );
			}
		} );
		h.RunUntil( 13.0, [&]( double ) {
			GameClient& client = *h.bots[0].client;
			if ( client.Session() != nullptr && client.State() == ClientState::Playing )
			{
				look( client.Session()->Sim(), client.Slot(), predicted );
			}
		} );
		h.server.SetTickObserver( {} );
		h.Report();
		GameClient& client = *h.bots[0].client;
		CHECK( client.GetStats().rttMs >= 90 && client.GetStats().desyncs == 0 && client.GetStats().checksumsVerified > 0 );

		// The server: thrown on the press, holding after the flight, pulled a long way toward the
		// wall, let go with the key.
		auto at = [&]( uint32_t tick ) { return truth.count( tick ) ? truth[tick] : Seen{}; };
		CHECK( at( throwAt ).tethered == false && at( throwAt + 1 ).tethered && at( throwAt + 1 ).holds == false );
		CHECK( at( throwAt + 60 ).holds && at( letGo + 2 ).tethered == false );
		float pulled = at( letGo ).position.z - at( throwAt ).position.z;
		std::printf( "    the hook pulled the player %.2f m toward the wall in %.1f s\n", pulled, float( letGo - throwAt ) / 60.0f );
		CHECK( pulled > 6.0f );
		CHECK( at( again + 60 ).holds && at( letGoAgain + 2 ).tethered == false );

		// The client had every tick the same way the first time it simulated it.
		int compared = 0, wrong = 0;
		float worst = 0.0f;
		for ( const auto& [tick, seen] : predicted )
		{
			auto it = truth.find( tick );
			if ( tick < throwAt - 30 || it == truth.end() )
			{
				continue;
			}
			compared += 1;
			float off = b3Distance( seen.position, it->second.position ) + b3Distance( seen.end, it->second.end );
			worst = std::max( worst, off );
			wrong += off != 0.0f || seen.tethered != it->second.tethered || seen.holds != it->second.holds || seen.events != it->second.events ? 1 : 0;
		}
		std::printf( "    %d predicted ticks compared with the server's: %d differ, worst %.4f m\n", compared, wrong, worst );
		CHECK( compared > 300 && wrong == 0 && worst == 0.0f );
		// (The client's simulation is looked at when the test's loop runs: on a slow machine it may
		// have run a few ticks between two looks. The first tick seen after the press is tethered.)
		auto firstAfter = predicted.lower_bound( throwAt + 1 );
		CHECK( firstAfter != predicted.end() && firstAfter->first <= throwAt + 4 && firstAfter->second.tethered );
	}

	// Two players, one ball: both hooks hold it, but a hook pulls only its own player (its rope and
	// its pull do not react): the players fly to the ball, which does not move until they reach it;
	// and everyone agrees.
	{
		Harness h( 47873 );
		ActionBits grapple = UseKey( h.server.Schema(), "grapple.hook" );
		struct Aim
		{
			std::atomic<int> yaw[2] = { 0, 0 };
			std::atomic<int> pitch[2] = { 0, 0 };
		};
		auto aim = std::make_shared<Aim>();
		for ( int i = 0; i < 2; ++i )
		{
			h.AddBot().script = [=]( uint32_t tick ) {
				PlayerInput in;
				in.cameraYaw = uint16_t( aim->yaw[i].load() );
				in.cameraPitch = int16_t( aim->pitch[i].load() );
				in.actions = tick >= 300 && tick < 304 ? grapple : ActionBits( 0 ); // one press: it stays out
				return in;
			};
		}
		Simulation& server = h.server.Sim();
		// The sandbox's first ball.
		uint32_t ball = 0;
		b3Vec3 ballAt = {};
		int bothOn = 0;
		b3Vec3 ballHeld = {};
		float drift = 0.0f;	  // how far the ball went while both held it and neither had reached it
		float nearest = 1e9f; // the nearest a player came to it
		float startFar = 0.0f;
		h.RunUntil( 9.0, [&]( double ) {
			if ( ball == 0 )
			{
				RayHit hit;
				if ( server.CastRay( { 4.0f, 6.0f, -3.0f }, { 0.0f, -8.0f, 0.0f }, 0, hit ) && hit.netId != 0 )
				{
					ball = hit.netId;
					ballAt = server.EntityTransform( ball )->position;
				}
			}
			if ( ball != 0 && server.Tick() < 290 )
			{
				for ( int i = 0; i < 2; ++i )
				{
					uint32_t netId = server.PlayerNetId( h.bots[size_t( i )].client->Slot() );
					const Transform* t = netId != 0 ? server.EntityTransform( netId ) : nullptr;
					if ( t == nullptr )
					{
						continue;
					}
					b3Vec3 to = b3Sub( server.EntityTransform( ball )->position, b3Add( t->position, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) );
					float flat = std::sqrt( to.x * to.x + to.z * to.z );
					aim->yaw[i].store( int( uint16_t( int( std::atan2( to.x, to.z ) * 65536.0f / 6.2831853f ) ) ) );
					aim->pitch[i].store( int( std::atan2( to.y, flat ) * 65536.0f / 6.2831853f ) );
				}
			}
			// (From the throw on: a hook lets go by itself when its player and the ball have met.)
			if ( ball != 0 && server.Tick() > 300 && server.Tick() < 420 )
			{
				int on = 0;
				float apart = 1e9f;
				for ( int i = 0; i < 2; ++i )
				{
					flecs::entity e = server.FindEntity( server.PlayerNetId( h.bots[size_t( i )].client->Slot() ) );
					const MotionHold* tether = e.is_valid() ? e.try_get<MotionHold>() : nullptr;
					on += tether != nullptr && tether->on != 0 && tether->flying == 0 && tether->missed == 0 && tether->anchor == ball ? 1 : 0;
					if ( e.is_valid() )
					{
						apart = std::min( apart, b3Distance( e.get<Transform>().position, server.EntityTransform( ball )->position ) );
					}
				}
				if ( on == 2 && bothOn < 2 )
				{
					ballHeld = server.EntityTransform( ball )->position;
					startFar = apart;
				}
				bothOn = std::max( bothOn, on );
				nearest = std::min( nearest, apart );
				// (Until a player reaches it: then it is bumped, as anything a player runs into is.)
				if ( on == 2 && apart > 1.6f )
				{
					drift = std::max( drift, b3Distance( server.EntityTransform( ball )->position, ballHeld ) );
				}
			}
		} );
		h.Report();
		std::printf( "    two hooks in one ball: the players came from %.2f m to %.2f m; the ball moved %.3f m before they reached it\n", startFar,
					 nearest, drift );
		CHECK( ball != 0 && bothOn == 2 && drift < 0.01f && nearest < startFar - 2.0f );
		for ( Bot& b : h.bots )
		{
			int compared = 0;
			CHECK( b.client->GetStats().desyncs == 0 );
			CHECK( h.CompareWithServer( b, compared ) == 0 && compared > 100 );
		}
	}
}

} // namespace

int main( int argc, char** argv )
{
	// Every index answer checked against a scan of every entity (simulation.h).
	Simulation::s_checkIndexes = true;
#if defined( _WIN32 )
	timeBeginPeriod( 1 );
#endif

	struct Test
	{
		const char* name;
		std::function<void()> fn;
	};
	const Test tests[] = {
		{ "protocol", TestProtocol },
		{ "map_sync", TestMapSync },
		{ "loopback_session", TestLoopbackSession },
		{ "late_join", TestLateJoin },
		{ "reconnect", TestReconnect },
		{ "grace_expiry", TestGraceExpiry },
		{ "lossy_session", TestLossySession },
		{ "mods_session", TestModsSession },
		{ "stall_recovery", TestStallRecovery },
		{ "names", TestNames },
		{ "items_replaced", TestItemsReplaced },
		{ "deathmatch", TestDeathmatch },
		{ "character_item", TestCharacterItem },
		{ "item_shapes", TestItemShapes },
		{ "item_parts", TestItemParts },
		{ "level_query", TestLevelQuery },
		{ "sneak", TestSneak },
		{ "move_params", TestMoveParams },
		{ "dash", TestDash },
		{ "grenade", TestGrenade },
		{ "frag", TestFrag },
		{ "frag_breaks", TestFragBreaks },
		{ "chest", TestChest },
		{ "shop", TestShop },
		{ "brawl", TestBrawl },
		{ "flight", TestFlight },
		{ "grapple", TestGrapple },
		{ "chat", TestChat },
		{ "server_info", TestServerInfo },
		{ "headshot", TestHeadshot },
		{ "pistol_mark", TestPistolMark },
		{ "rifle", TestRifle },
		{ "gun_swap", TestGunSwap },
		{ "combat", TestCombat },
		{ "private_fields", TestPrivateFields },
		{ "melee", TestMelee },
		{ "melee_pitch", TestMeleePitch },
		{ "aim_views", TestAimViews },
		{ "pickup", TestPickup },
		{ "inventory", TestInventory },
		{ "item_layers", TestItemLayers },
		{ "item_poses", TestItemPoses },
		{ "sprint_swing", TestSprintSwing },
		{ "view_sources", TestViewSources },
	};

	const char* filter = argc > 1 ? argv[1] : nullptr;
	int run = 0;
	for ( const Test& t : tests )
	{
		if ( filter != nullptr && std::strcmp( filter, t.name ) != 0 )
		{
			continue;
		}
		++run;
		int before = g_failures;
		auto start = Clock::now();
		std::printf( "[ RUN  ] %s\n", t.name );
		std::fflush( stdout );
		t.fn();
		double ms = std::chrono::duration<double, std::milli>( Clock::now() - start ).count();
		std::printf( "[ %s ] %s (%.0f ms)\n", g_failures == before ? " OK " : "FAIL", t.name, ms );
		std::fflush( stdout );
	}
	if ( run == 0 )
	{
		std::printf( "no test named %s\n", filter );
		return 1;
	}
	return g_failures == 0 ? 0 : 1;
}
