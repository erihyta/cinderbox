// End-to-end tests over real UDP loopback: a GameServer and headless GameClients in one process.
//
//   cb_net_tests [name]

#include "bot_brain.h"
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

	PlayerInput Sample( uint32_t )
	{
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

	explicit Harness( uint16_t p, const std::string& recordPath = {}, const std::string& mapPath = {} )
		: port( p )
		, clientPort( p )
	{
		ServerOptions options;
		options.port = port;
		options.mapPath = mapPath;
		options.recordHashes = true;
		options.verbose = false;
		options.config.physicsArenaMB = 64;
		options.reconnectGraceSeconds = 10.0;
		options.recordPath = recordPath;
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

	Bot& AddBot( uint32_t rollbackWindow = 8 )
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

void TestProtocol()
{
	using namespace net;

	// Frame delta codec round trip.
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
			}
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
