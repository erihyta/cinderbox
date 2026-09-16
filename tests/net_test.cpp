// End-to-end tests over real UDP loopback: a GameServer and headless GameClients in one process.
//
//   cb_net_tests [name]

#include "game_client.h"
#include "game_server.h"
#include "util.h"

#include <chrono>
#include <cstdio>
#include <cstring>
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
	uint64_t rng = 0;
	PlayerInput held{};

	PlayerInput Sample( uint32_t )
	{
		uint64_t r = NextRandom( rng );
		if ( ( r & 31 ) == 0 )
		{
			held.moveForward = int8_t( int( ( r >> 8 ) % 255 ) - 127 );
			held.moveRight = int8_t( int( ( r >> 16 ) % 255 ) - 127 );
			held.cameraYaw = uint16_t( r >> 24 );
		}
		held.buttons = 0;
		if ( ( ( r >> 40 ) % 50 ) == 0 )
		{
			held.buttons |= BtnJump;
		}
		if ( ( ( r >> 48 ) % 40 ) == 0 )
		{
			held.buttons |= BtnSpawnProp;
		}
		if ( ( r >> 56 ) & 1 )
		{
			held.buttons |= BtnSprint;
		}
		return held;
	}
};

struct Harness
{
	GameServer server;
	std::vector<Bot> bots;
	Clock::time_point start = Clock::now();
	uint16_t port = 0;

	explicit Harness( uint16_t p )
		: port( p )
	{
		ServerOptions options;
		options.port = port;
		options.recordHashes = true;
		options.verbose = false;
		options.config.physicsArenaMB = 64;
		options.reconnectGraceSeconds = 10.0;
		if ( server.Start( options ) == false )
		{
			std::printf( "    server failed to start on port %u\n", port );
		}
	}

	double Now() const
	{
		return std::chrono::duration<double>( Clock::now() - start ).count();
	}

	Bot& AddBot()
	{
		Bot bot;
		bot.client = std::make_unique<GameClient>();
		bot.rng = 1000 + bots.size();
		ClientOptions options;
		options.port = port;
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
	CHECK( lateSteady * 100 < playerTicks );

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
		{ "loopback_session", TestLoopbackSession },
		{ "late_join", TestLateJoin },
		{ "reconnect", TestReconnect },
		{ "grace_expiry", TestGraceExpiry },
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
