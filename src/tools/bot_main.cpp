// Headless bot clients for load and soak testing.
//
//   cb_bot [--host H] [--port P] [--count N] [--full M] [--threads T (for lite bots)] [--duration SEC]
//          [--stagger MS] [--spawn-one-in N] [--rollback TICKS] [--report SEC]
//
// --count bots join; the first --full of them run the complete client (prediction, rollback,
// checksum verification) and report its cost. The rest are "lite": they keep pace and send input
// without simulating, which loads the server like real players without needing a CPU core each.
// Exit code 0 if no full bot saw a desync.

#include "bot_brain.h"
#include "game_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#include <timeapi.h>
#endif

using namespace cb;
using Clock = std::chrono::steady_clock;

namespace
{

struct Options
{
	ClientOptions client;
	int count = 16;
	int full = 2;
	int threads = 0; // 0 = hardware concurrency
	double duration = 0.0;
	int staggerMs = 50;
	uint32_t spawnOneIn = 40;
	double reportSeconds = 5.0;
};

struct Bot
{
	std::unique_ptr<GameClient> client;
	BotBrain brain;
	double startAt = 0.0;
	bool started = false;
	bool full = false;

	// Per-report accumulators (owned by the bot's thread).
	double simMsSum = 0.0;
	uint64_t frames = 0;
};

// Summary a worker publishes for the reporter.
struct Summary
{
	int playing = 0;
	int connecting = 0;
	int reconnecting = 0;
	int rejected = 0;
	int fullBots = 0;
	uint64_t desyncs = 0;
	uint64_t checksums = 0;
	uint64_t rollbacks = 0;
	uint64_t resimulated = 0;
	double stalledSeconds = 0.0;
	double playingSeconds = 0.0;
	double simMsSum = 0.0;
	uint64_t frames = 0;
	double simMsMax = 0.0;
	uint32_t rttSum = 0;
	uint32_t rttMax = 0;
	int rttCount = 0;
	uint64_t bytesReceived = 0;
	uint64_t bytesSent = 0;

	void Add( const Summary& o )
	{
		playing += o.playing;
		connecting += o.connecting;
		reconnecting += o.reconnecting;
		rejected += o.rejected;
		fullBots += o.fullBots;
		desyncs += o.desyncs;
		checksums += o.checksums;
		rollbacks += o.rollbacks;
		resimulated += o.resimulated;
		stalledSeconds += o.stalledSeconds;
		playingSeconds += o.playingSeconds;
		simMsSum += o.simMsSum;
		frames += o.frames;
		simMsMax = std::max( simMsMax, o.simMsMax );
		rttSum += o.rttSum;
		rttMax = std::max( rttMax, o.rttMax );
		rttCount += o.rttCount;
		bytesReceived += o.bytesReceived;
		bytesSent += o.bytesSent;
	}
};

struct Worker
{
	std::vector<Bot> bots;
	std::mutex mutex;
	Summary published;
	bool resetMax = false;
};

double Seconds( Clock::time_point start )
{
	return std::chrono::duration<double>( Clock::now() - start ).count();
}

void RunWorker( Worker& w, Clock::time_point start, const std::atomic<bool>& stop )
{
	double nextPublish = 0.0;
	while ( stop.load() == false )
	{
		double now = Seconds( start );
		for ( Bot& b : w.bots )
		{
			if ( b.started == false )
			{
				if ( now < b.startAt )
				{
					continue;
				}
				b.started = true;
			}
			b.client->Update( now, [&b]( uint32_t ) { return b.brain.Next(); } );
			if ( b.full && b.client->GetStats().ticksLastFrame > 0 )
			{
				b.simMsSum += b.client->GetStats().simMsLastFrame;
				b.frames += 1;
			}
		}

		if ( now >= nextPublish )
		{
			Summary s;
			bool resetMax;
			{
				std::lock_guard<std::mutex> lock( w.mutex );
				resetMax = w.resetMax;
				w.resetMax = false;
			}
			for ( Bot& b : w.bots )
			{
				GameClient& c = *b.client;
				const auto& cs = c.GetStats();
				switch ( c.State() )
				{
					case ClientState::Playing:
						s.playing += 1;
						break;
					case ClientState::Reconnecting:
						s.reconnecting += 1;
						break;
					case ClientState::Rejected:
						s.rejected += 1;
						break;
					default:
						s.connecting += 1;
						break;
				}
				if ( c.State() == ClientState::Playing )
				{
					s.rttSum += cs.rttMs;
					s.rttMax = std::max( s.rttMax, cs.rttMs );
					s.rttCount += 1;
				}
				s.bytesReceived += cs.bytesReceived;
				s.bytesSent += cs.bytesSent;
				if ( b.full )
				{
					s.fullBots += 1;
					s.desyncs += cs.desyncs;
					s.checksums += cs.checksumsVerified;
					s.simMsSum += b.simMsSum;
					s.frames += b.frames;
					s.simMsMax = std::max( s.simMsMax, cs.simMsMax );
					s.stalledSeconds += cs.stalledSeconds;
					s.playingSeconds += cs.playingSeconds;
					if ( RollbackSession* session = c.Session() )
					{
						s.rollbacks += session->GetStats().rollbacks;
						s.resimulated += session->GetStats().resimulatedTicks;
					}
					b.simMsSum = 0.0;
					b.frames = 0;
					if ( resetMax )
					{
						c.ResetMaxStats();
					}
				}
			}
			std::lock_guard<std::mutex> lock( w.mutex );
			w.published = s;
			nextPublish = now + 0.5;
		}

		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
}

bool Parse( int argc, char** argv, Options& o )
{
	for ( int i = 1; i < argc; ++i )
	{
		std::string arg = argv[i];
		if ( i + 1 >= argc )
		{
			return false;
		}
		std::string v = argv[++i];
		if ( arg == "--host" )
			o.client.host = v;
		else if ( arg == "--port" )
			o.client.port = uint16_t( std::atoi( v.c_str() ) );
		else if ( arg == "--count" )
			o.count = std::atoi( v.c_str() );
		else if ( arg == "--full" )
			o.full = std::atoi( v.c_str() );
		else if ( arg == "--threads" )
			o.threads = std::atoi( v.c_str() );
		else if ( arg == "--duration" )
			o.duration = std::atof( v.c_str() );
		else if ( arg == "--stagger" )
			o.staggerMs = std::atoi( v.c_str() );
		else if ( arg == "--spawn-one-in" )
			o.spawnOneIn = uint32_t( std::atoi( v.c_str() ) );
		else if ( arg == "--rollback" )
			o.client.maxRollbackTicks = uint32_t( std::atoi( v.c_str() ) );
		else if ( arg == "--report" )
			o.reportSeconds = std::atof( v.c_str() );
		else
			return false;
	}
	return o.count > 0 && o.full >= 0 && o.full <= o.count;
}

} // namespace

int main( int argc, char** argv )
{
	Options o;
	if ( Parse( argc, argv, o ) == false )
	{
		std::printf( "usage: cb_bot [--host H] [--port P] [--count N] [--full M] [--threads T] [--duration SEC]\n"
					 "              [--stagger MS] [--spawn-one-in N] [--rollback TICKS] [--report SEC]\n" );
		return 1;
	}

#if defined( _WIN32 )
	timeBeginPeriod( 1 );
#endif

	// Each full bot gets a thread of its own: a rollback can take several milliseconds, and bots
	// sharing its thread would send their input late meanwhile. Lite bots share the rest.
	int lite = o.count - o.full;
	int liteThreads = o.threads > 0 ? o.threads : int( std::max( 1u, std::thread::hardware_concurrency() / 4 ) );
	liteThreads = std::max( 1, std::min( liteThreads, lite ) );
	int threads = o.full + ( lite > 0 ? liteThreads : 0 );
	std::vector<std::unique_ptr<Worker>> workers;
	for ( int t = 0; t < threads; ++t )
	{
		workers.push_back( std::make_unique<Worker>() );
	}

	for ( int i = 0; i < o.count; ++i )
	{
		Bot bot;
		bot.full = i < o.full;
		bot.brain = BotBrain( uint64_t( i ) + 1 );
		bot.brain.spawnOneIn = o.spawnOneIn;
		bot.startAt = double( i ) * double( o.staggerMs ) / 1000.0;
		bot.client = std::make_unique<GameClient>();
		ClientOptions co = o.client;
		co.simulate = bot.full;
		co.verbose = false;
		co.logName = "bot" + std::to_string( i );
		bot.client->Start( co, bot.startAt );
		size_t worker = bot.full ? size_t( i ) : size_t( o.full ) + size_t( i - o.full ) % size_t( liteThreads );
		workers[worker]->bots.push_back( std::move( bot ) );
	}

	std::printf( "cb_bot: %d bots (%d full) on %d threads -> %s:%u\n", o.count, o.full, threads, o.client.host.c_str(),
				 o.client.port );
	std::fflush( stdout );

	std::atomic<bool> stop{ false };
	auto start = Clock::now();
	std::vector<std::thread> pool;
	for ( auto& w : workers )
	{
		pool.emplace_back( RunWorker, std::ref( *w ), start, std::cref( stop ) );
	}

	Summary previous;
	double previousTime = 0.0;
	Summary last;
	for ( ;; )
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( int( o.reportSeconds * 1000.0 ) ) );
		double now = Seconds( start );

		Summary s;
		for ( auto& w : workers )
		{
			std::lock_guard<std::mutex> lock( w->mutex );
			s.Add( w->published );
			w->resetMax = true;
		}

		double dt = now - previousTime;
		double rollbacksPerSec = s.fullBots ? double( s.rollbacks - previous.rollbacks ) / dt / s.fullBots : 0.0;
		double resimPerSec = s.fullBots ? double( s.resimulated - previous.resimulated ) / dt / s.fullBots : 0.0;
		double playedDelta = s.playingSeconds - previous.playingSeconds;
		double stalledPercent = playedDelta > 0.0 ? 100.0 * ( s.stalledSeconds - previous.stalledSeconds ) / playedDelta : 0.0;
		double downKbps = o.count ? double( s.bytesReceived - previous.bytesReceived ) * 8.0 / 1000.0 / dt / o.count : 0.0;
		double upKbps = o.count ? double( s.bytesSent - previous.bytesSent ) * 8.0 / 1000.0 / dt / o.count : 0.0;
		std::printf( "[%6.1fs] playing %d/%d (connecting %d, reconnecting %d, rejected %d) | rtt avg %u max %u ms | "
					 "per bot down %.0f up %.0f kbit/s\n",
					 now, s.playing, o.count, s.connecting, s.reconnecting, s.rejected, s.rttCount ? s.rttSum / s.rttCount : 0,
					 s.rttMax, downKbps, upKbps );
		if ( s.fullBots > 0 )
		{
			std::printf( "          full bots: client work %.2f ms/frame avg, %.2f max | rollbacks %.1f/s, resimulated %.1f "
						 "ticks/s per bot | stalled %.1f%% of the time | checksums ok %llu | DESYNCS %llu\n",
						 s.frames ? s.simMsSum / double( s.frames ) : 0.0, s.simMsMax, rollbacksPerSec, resimPerSec,
						 stalledPercent, (unsigned long long)s.checksums, (unsigned long long)s.desyncs );
		}
		std::fflush( stdout );
		previous = s;
		previousTime = now;
		last = s;

		if ( o.duration > 0.0 && now >= o.duration )
		{
			break;
		}
	}

	stop = true;
	for ( auto& t : pool )
	{
		t.join();
	}

	bool ok = last.desyncs == 0 && last.rejected == 0 && last.playing == o.count;
	std::printf( "cb_bot: %s (%d/%d playing, %llu desyncs, %llu checksums verified)\n", ok ? "OK" : "PROBLEMS", last.playing,
				 o.count, (unsigned long long)last.desyncs, (unsigned long long)last.checksums );
	return ok ? 0 : 2;
}
