// End-to-end tests over real UDP loopback: a GameServer and headless GameClients in one process.
//
//   cb_net_tests [name]

#include "bot_brain.h"
#include "registry.h"
#include "fingerprint.h"
#include "game_client.h"
#include "game_server.h"
#include "map.h"
#include "netsim.h"
#include "replay.h"
#include "simulation.h"
#include "util.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <thread>

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
					  const std::map<std::string, std::string>& modOptions = {} )
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
	// toward +X, straight at slot 1.
	h.AddBot().script = [=]( uint32_t tick ) {
		PlayerInput in;
		in.cameraYaw = 16384;
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
