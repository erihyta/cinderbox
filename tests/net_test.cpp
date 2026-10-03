// End-to-end tests over real UDP loopback: a GameServer and headless GameClients in one process.
//
//   cb_net_tests [name]

#include "detmath.h"
#include "bot_brain.h"
#include "anim_graph.h"
#include "character_item.h"
#include "joint_math.h"
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
// Debug build the server and four simulating clients cannot keep up on one thread.
#if defined( NDEBUG )
constexpr bool kTimingChecks = true;
#else
constexpr bool kTimingChecks = false;
#endif

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

struct Bot
{
	std::unique_ptr<GameClient> client;
	BotBrain brain;
	// Replaces the brain when set.
	std::function<PlayerInput( uint32_t )> script;

	PlayerInput Sample( uint32_t tick )
	{
		if ( script )
		{
			return script( tick );
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
		for ( const mods::ModInfo& info : mods::CompiledMods() )
		{
			server.AddMod( info.create() );
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
		options.loadItemShape = []( const std::string& mod, const std::string& kind, ItemShape& shape, std::string& error,
									std::map<std::string, float>& properties ) {
			return LoadItemShapeFolder( std::string( CB_SOURCE_DIR ) + "/server_mods/" + mod + "/client", kind, shape, error, &properties );
		};
		if ( configure )
		{
			configure( options );
		}
		if ( server.Start( options ) == false )
		{
			std::printf( "    server failed to start on port %u\n", port );
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
			for ( Bot& b : bots )
			{
				b.client->Update( now, [&b]( uint32_t tick ) { return b.Sample( tick ); } );
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

	Harness h( 17806, {}, mapPath.string() );
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
		CHECK( b.client->MapHash() == MapHash( bytes.data(), bytes.size() ) );
		// Templates travel with the map, so clients can create the same entities the server does.
		CHECK( b.client->Map().templates.size() == 1 );
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
		std::printf( "\n" );
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
	CHECK( schema.FindEvent( "pistol.fired" ) >= 0 );
	CHECK( schema.ActionMask( "fire" ) != 0 );
	uint16_t fire = schema.ActionMask( "fire" );
	uint16_t pistol = schema.ActionMask( "slot_2" );
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
	uint16_t bat = schema.ActionMask( "slot_3" );
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
	CHECK( mostLying >= int( schema.itemKinds.size() ) );
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

// The inventory: a life starts with a pistol (slot 2) and a bat (slot 3), carried but put away.
// Switching slots moves items between the hand and stowed and never drops or makes one. Picking up
// a bat that lies in the world while carrying one swaps them (the old one drops), leaves the pistol
// alone, and brings the new bat into the hand; switching away from it and back keeps it. A thrown
// bat leaves its slot empty: no second one this life. The number of items never changes.
void TestInventory()
{
	Harness h( 47805, {}, {}, { { "pickup.spawn_each", "1" } } );
	const ModSchema& schema = h.server.Schema();
	uint16_t gunSlot = schema.ActionMask( "slot_2" );
	uint16_t batSlot = schema.ActionMask( "slot_3" );
	uint16_t pickup = schema.ActionMask( "pickup" );
	uint16_t drop = schema.ActionMask( "drop" );
	int gunKind = schema.FindItemKind( "pistol.gun" );
	int batKind = schema.FindItemKind( "melee.bat" );
	const BoardField* slotField = schema.FindField( "inventory.slot" );
	const BoardField* slot2 = schema.FindField( "inventory.item_2" );
	const BoardField* slot3 = schema.FindField( "inventory.item_3" );
	CHECK( gunSlot != 0 && batSlot != 0 && pickup != 0 && drop != 0 && gunKind >= 0 && batKind >= 0 && slotField != nullptr );
	CHECK( slot2 != nullptr && slot3 != nullptr );
	if ( gunKind < 0 || batKind < 0 || slotField == nullptr || slot2 == nullptr || slot3 == nullptr )
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
		if ( ( tick >= 100 && tick < 110 ) || ( tick >= 560 && tick < 570 ) || ( tick >= 660 && tick < 670 ) )
		{
			in.actions = batSlot;
		}
		else if ( ( tick >= 150 && tick < 160 ) || ( tick >= 520 && tick < 530 ) || ( tick >= 630 && tick < 640 ) )
		{
			in.actions = gunSlot;
		}
		else if ( tick >= 600 && tick < 605 )
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
	bool swapped = false;
	bool gunKept = false;
	bool awayAndKept = false;
	bool backInHand = false;
	bool thrown = false;
	bool noSecondBat = false;
	bool gunAfterThrow = false;
	bool boardSaysStart = false;
	bool boardSaysSwap = false;
	bool boardSaysEmpty = false;
	uint32_t ownBat = 0;
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
		startsStowed |= tick > 80 && tick < 100 && inHand == 0 && c.carried == 3 && c.stowed == 3 && c.lying == 3; // a pistol, a bat and a rifle, and one of each lying about
		if ( tick > 130 && tick < 150 && kindOf( inHand ) == batKind )
		{
			batOut = c.carried == 3 && c.stowed == 2;
			ownBat = inHand;
		}
		// The board names what is in each slot, for the HUD: the life's own, then the picked-up bat, then nothing.
		uint32_t in2 = uint32_t( server.BoardValue( me, slot2->slot ) );
		uint32_t in3 = uint32_t( server.BoardValue( me, slot3->slot ) );
		boardSaysStart |= tick > 130 && tick < 150 && ownBat != 0 && in3 == ownBat && kindOf( in2 ) == gunKind;
		boardSaysSwap |= reached && tick < 600 && in3 == lyingBat && kindOf( in2 ) == gunKind;
		boardSaysEmpty |= tick > 700 && in3 == 0 && kindOf( in2 ) == gunKind;
		gunOut |= tick > 180 && tick < 200 && kindOf( inHand ) == gunKind && c.carried == 3 && c.stowed == 2;
		droppedBySwitching |= tick > 100 && tick < 200 && c.lying != 3;
		// The lying bat is in the hand: the swap happened.
		if ( inHand == lyingBat && lyingBat != 0 && tick < 520 )
		{
			reached = true;
			const HeldItem* old = ownBat != 0 ? server.FindEntity( ownBat ).try_get<HeldItem>() : nullptr;
			swapped |= old != nullptr && old->holder == 0 && c.lying == 3;
			gunKept |= c.carried == 3 && c.stowed == 2;
		}
		// Slot 2 and back to 3: the picked-up bat is put away and taken out, never dropped.
		const HeldItem* mine = lyingBat != 0 ? server.FindEntity( lyingBat ).try_get<HeldItem>() : nullptr;
		awayAndKept |= tick > 545 && tick < 560 && kindOf( inHand ) == gunKind && mine != nullptr && mine->holder == me && mine->stowed != 0 &&
					   c.lying == 3;
		backInHand |= tick > 585 && tick < 600 && inHand == lyingBat && reached;
		thrown |= tick > 610 && tick < 630 && inHand == 0 && c.carried == 2 && c.lying == 4;
		gunAfterThrow |= tick > 645 && tick < 660 && kindOf( inHand ) == gunKind;
		noSecondBat |= tick > 700 && inHand == 0 && c.carried == 2 && server.BoardValue( me, slotField->slot ) == 3;
	} );
	h.Report();
	std::printf( "    starts stowed %d, bat out %d, pistol out %d, dropped by switching %d\n", int( startsStowed ), int( batOut ), int( gunOut ),
				 int( droppedBySwitching ) );
	std::printf( "    picked up the lying bat %d: old bat dropped %d, pistol kept %d; put away and kept %d, out again %d\n", int( reached ),
				 int( swapped ), int( gunKept ), int( awayAndKept ), int( backInHand ) );
	std::printf( "    thrown %d, pistol after %d, no second bat %d; items %d at the start, %d to %d throughout\n", int( thrown ),
				 int( gunAfterThrow ), int( noSecondBat ), startTotal, leastTotal, mostTotal );
	CHECK( startsStowed && batOut && gunOut );
	CHECK( droppedBySwitching == false );
	CHECK( reached && swapped && gunKept );
	CHECK( awayAndKept && backInHand );
	CHECK( thrown && gunAfterThrow && noSecondBat );
	std::printf( "    the board's slots: at the start %d, after the swap %d, empty after the throw %d\n", int( boardSaysStart ),
				 int( boardSaysSwap ), int( boardSaysEmpty ) );
	CHECK( boardSaysStart && boardSaysSwap && boardSaysEmpty );
	CHECK( startTotal == 9 && leastTotal == 9 && mostTotal == 9 ); // three lying, three per player; none made, none lost
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
	CHECK( ParseItemShape( "shape capsule\nhalf 0.1 0.1 0.1\n", shape, error ) == false && error.empty() == false );
	CHECK( ParseItemShape( "shape box\n", shape, error ) == false );				   // no size
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1\n", shape, error ) == false );	   // two numbers
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 9\n", shape, error ) == false );   // 18 m long
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nmass 0\n", shape, error ) == false );
	CHECK( ParseItemShape( "shape box\nhalf nan 0.1 0.1\n", shape, error ) == false );
	// The ones the mods ship.
	const std::string root = CB_SOURCE_DIR;
	CHECK( LoadItemShapeFolder( root + "/server_mods/melee/client", "melee.bat", shape, error ) && std::max( shape.half.x, shape.half.z ) > 0.3f ); // long, along how it is carried
	CHECK( LoadItemShapeFolder( root + "/server_mods/pistol/client", "pistol.gun", shape, error ) && shape.mass < 1.0f );
	CHECK( LoadItemShapeFolder( root + "/server_mods/rifle/client", "rifle.gun", shape, error ) && shape.mass > 2.0f && shape.grip == 3 );
	CHECK( LoadItemShapeFolder( root + "/server_mods/melee/client", "no.such", shape, error ) == false );
	// Properties authored on the body ride along: the bat's hold time is in its scene, not in its mod.
	ItemProperties properties;
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nproperty pickup.hold_seconds 0.75\nproperty a.b 2\n", shape, error, &properties ) );
	CHECK( properties.size() == 2 && properties["pickup.hold_seconds"] == 0.75f && properties["a.b"] == 2.0f );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nproperty lonely\n", shape, error, &properties ) == false );
	CHECK( ParseItemShape( "shape box\nhalf 0.1 0.1 0.1\nproperty x nan\n", shape, error, &properties ) == false );
	// A grip: where the other hand holds the item, baked from the scene's CbGrip.
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
	CHECK( LoadItemShapeFolder( root + "/server_mods/pistol/client", "pistol.gun", shape, error ) && shape.grip == 3 ); // as its animations have the hands
	CHECK( LoadItemShapeFolder( root + "/server_mods/melee/client", "melee.bat", shape, error ) && shape.grip != 0 );
	properties.clear();
	CHECK( LoadItemShapeFolder( root + "/server_mods/melee/client", "melee.bat", shape, error, &properties ) );
	CHECK( properties.count( "pickup.hold_seconds" ) == 1 && properties["pickup.hold_seconds"] == 0.5f );
}

// A held item brings its layers: with the bat out, the player's "Base" layer plays from the melee
// mod's carry pack; crouching (the sneak mod's own swap) wins while it lasts; standing up gives the
// carry back; throwing the bat away gives the player's own layer back. Clients agree throughout.
void TestItemLayers()
{
	const std::string root = CB_SOURCE_DIR;
	std::shared_ptr<const CharacterAsset> mannequin;
	Harness h( 47806, {}, {}, {}, [&]( ServerOptions& options ) {
		std::string error, warnings;
		mannequin = LoadCharacterFolder( root + "/godot/characters/mannequin", "mannequin", error, warnings );
		options.character = mannequin;
		options.loadAnimPack = [root]( const std::string& mod, const std::string& pack, std::string& error, std::string& warnings ) {
			return LoadAnimPackFolder( root + "/server_mods/" + mod + "/client", pack, error, warnings );
		};
	} );
	const ModSchema& schema = h.server.Schema();
	uint16_t bat = schema.ActionMask( "slot_3" );
	uint16_t crouch = schema.ActionMask( "crouch" );
	uint16_t drop = schema.ActionMask( "drop" );
	int carry = -1;
	int sneak = -1;
	for ( size_t i = 0; i < schema.animPacks.size(); ++i )
	{
		carry = schema.animPacks[i].name == "melee.carry" && schema.animPacks[i].graph.empty() == false ? int( i ) : carry;
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
		const AnimState* a = server.EntityAnimState( server.PlayerNetId( h.bots[0].client->Slot() ) );
		if ( a == nullptr )
		{
			return;
		}
		uint8_t base = a->graph[0].source; // the mannequin's first layer is "Base"
		sawCarry |= tick > 130 && tick < 195 && base == uint8_t( carry + 1 );
		sawCrouch |= tick > 215 && tick < 255 && base == uint8_t( sneak + 1 );
		carryAgain |= tick > 275 && tick < 315 && base == uint8_t( carry + 1 );
		ownAgain |= tick > 345 && base == 0;
		// Never the carry once the bat is gone, never the player's own while it is held and standing.
		wrong |= ( tick > 345 && base != 0 ) || ( tick > 275 && tick < 315 && base != uint8_t( carry + 1 ) );
	} );
	h.Report();
	std::printf( "    carry with the bat %d, crouch over it %d, carry again %d, its own after the throw %d\n", int( sawCarry ),
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
	Harness h( 47803, {}, {}, {}, [&]( ServerOptions& options ) {
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
	h.RunUntil( 7.0, [&]( double ) {
		uint32_t netId = server.PlayerNetId( h.bots[0].client->Slot() );
		const AnimState* a = server.EntityAnimState( netId );
		if ( a == nullptr || a->graph[0].started == 0 )
		{
			return;
		}
		pose.Evaluate( *a );
		float v[4];
		ozz::math::StorePtrU( pose.Models()[size_t( anim::FindJoint( *mannequin->animations, "Head" ) )].cols[3], v );
		if ( a->graph[0].source == uint8_t( sneakPack + 1 ) && a->graph[0].stateTime > 0.5f )
		{
			swapped = true;
			sneaking = std::min( sneaking, v[1] );
		}
		else if ( a->graph[0].source == 0 )
		{
			restored |= swapped;
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
	CHECK( h.bots[0].client->GetStats().desyncs == 0 );
}

void TestHeadshot()
{
	Harness h( 47801 );
	const ModSchema& schema = h.server.Schema();
	uint16_t fire = schema.ActionMask( "fire" );
	uint16_t pistol = schema.ActionMask( "slot_2" );
	int hitEvent = schema.FindEvent( "pistol.hit" );

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
			newLifeHasItems |= wasDead && carried == 3;
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
	uint16_t fire = schema.ActionMask( "fire" );
	uint16_t pistol = schema.ActionMask( "slot_2" );
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
	uint16_t mark = schema.ActionMask( "mark" );
	uint16_t pistol = schema.ActionMask( "slot_2" );
	int scanEvent = schema.FindEvent( "pistol.scan" );
	int markedEvent = schema.FindEvent( "pistol.marked" );
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
	const ModSchema& schema = h.server.Schema();
	uint16_t fire = schema.ActionMask( "fire" );
	uint16_t rifle = schema.ActionMask( "slot_4" );
	uint16_t pistol = schema.ActionMask( "slot_2" );
	int rifleFired = schema.FindEvent( "rifle.fired" );
	int rifleDry = schema.FindEvent( "rifle.dry" );
	int rifleReload = schema.FindEvent( "rifle.reload" );
	int pistolFired = schema.FindEvent( "pistol.fired" );
	CHECK( fire != 0 && rifle != 0 && rifleFired >= 0 && rifleDry >= 0 && rifleReload >= 0 && pistolFired >= 0 );
	CHECK( schema.FindItemKind( "rifle.gun" ) >= 0 && schema.FindField( "rifle.ammo" ) != nullptr );

	// Held for two seconds (20 shots), a tap (1), then held for good (9, a reload, and on).
	auto trigger = []( uint32_t tick ) { return ( tick >= 200 && tick < 320 ) || tick == 400 || tick >= 460; };
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraPitch = 8000; // at the sky: nobody is hurt, no life ends, no magazine is refilled
		in.actions = tick < 150 ? rifle : uint16_t( trigger( tick ) ? fire : 0 );
		return in;
	};
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraPitch = 8000;
		in.actions = tick < 150 ? pistol : uint16_t( trigger( tick ) ? fire : 0 );
		return in;
	};

	std::set<uint32_t> shots, clicks, reloads, pistolShots;
	int32_t ammoAfterHold = -1;
	int32_t ammoAfterTap = -1;
	const BoardField* ammo = schema.FindField( "rifle.ammo" );
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
	for ( Bot& b : h.bots )
	{
		CHECK( b.client->GetStats().desyncs == 0 );
	}
}

// Layers end to end: slot 0 takes out the pistol, then the bat (a full-body stance), and swings at
// slot 1, which stands still. Swings play the swing stance, hits go out as combat.damage, the combat mod (which
// keeps health) applies them and credits the kill, and clients agree on every pose-carrying tick.
void TestMelee()
{
	Harness h( 47802 );
	const ModSchema& schema = h.server.Schema();
	uint16_t fire = schema.ActionMask( "fire" );
	uint16_t bat = schema.ActionMask( "slot_3" );
	uint16_t pistol = schema.ActionMask( "slot_2" );
	uint16_t hands = schema.ActionMask( "slot_1" );
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
	int gunKind = schema.FindItemKind( "pistol.gun" );
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
		if ( const AnimState* a = server.EntityAnimState( attacker ) )
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
	Harness h( 47803 );
	const ModSchema& schema = h.server.Schema();
	uint16_t fire = schema.ActionMask( "fire" );
	uint16_t bat = schema.ActionMask( "slot_3" );
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
	Harness h( 47804 );
	const ModSchema& schema = h.server.Schema();
	uint16_t fire = schema.ActionMask( "fire" );
	uint16_t pistol = schema.ActionMask( "slot_2" );
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
	uint16_t fire = schema.ActionMask( "fire" );
	uint16_t pistol = schema.ActionMask( "slot_2" );

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
	bool sawIntermission = false;
	bool frozenInIntermission = true;
	uint32_t winnerSeen = 0;
	h.RunUntil( 16.0, [&]( double ) {
		if ( server.GlobalBoardValue( phase->slot ) == 1 )
		{
			sawIntermission = true;
			winnerSeen = uint32_t( server.GlobalBoardValue( winner->slot ) );
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
		schema.fields.push_back( { "pistol.ammo", BoardType::Int, BoardScope::Entity, 3 } );
		schema.fields.push_back( { "round.time", BoardType::Float, BoardScope::Global, 0 } );
		schema.events = { "pistol.fired", "combat.killed" };
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
		bad.fields[0].slot = kBoardSlots;
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

	// Frame batches: a chain from any starting tick decodes back to the same frames.
	{
		std::vector<InputFrame> history( 40 );
		InputArray running{};
		for ( uint32_t t = 0; t < history.size(); ++t )
		{
			history[t].tick = t;
			running[t % 8].moveForward = int8_t( t );
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
	uint16_t fire = schema.ActionMask( "fire" );
	uint16_t bat = schema.ActionMask( "slot_3" );
	int swingEvent = schema.FindEvent( "melee.swing" );
	int strikeEvent = schema.FindEvent( "melee.strike" );
	CHECK( fire != 0 && bat != 0 && swingEvent >= 0 && strikeEvent >= 0 );

	// Bat out, then swing once a second: standing (ticks 200..500), walking (500..800), sprinting
	// (800..1100), in a circle so the walls are never reached.
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
		if ( tick >= 200 && ( tick % 60 ) < 3 )
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
		// Nothing else arrived with it: every other private slot is empty.
		for ( int s = 0; s < kBoardSlots; ++s )
		{
			CHECK( s == field->slot || client.Privates().values[s] == 0 );
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

} // namespace

int main( int argc, char** argv )
{
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
		{ "deathmatch", TestDeathmatch },
		{ "character_item", TestCharacterItem },
		{ "item_shapes", TestItemShapes },
		{ "sneak", TestSneak },
		{ "headshot", TestHeadshot },
		{ "pistol_mark", TestPistolMark },
		{ "rifle", TestRifle },
		{ "combat", TestCombat },
		{ "private_fields", TestPrivateFields },
		{ "melee", TestMelee },
		{ "melee_pitch", TestMeleePitch },
		{ "aim_views", TestAimViews },
		{ "pickup", TestPickup },
		{ "inventory", TestInventory },
		{ "item_layers", TestItemLayers },
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
