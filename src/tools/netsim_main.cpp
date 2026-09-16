// UDP relay with artificial latency, jitter, loss and duplication.
//
//   cb_netsim --listen PORT --target HOST:PORT [--latency MS] [--jitter MS] [--loss PERCENT]
//             [--duplicate PERCENT] [--no-reorder] [--seed N]
//
// Latency is added in each direction, so the round trip grows by twice the value.
// Example: cb_server --port 7777, cb_netsim --listen 7778 --target 127.0.0.1:7777 --latency 40,
// then cb_client --port 7778 plays with ~80 ms extra RTT.

#include "netsim.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined( _WIN32 )
#include <windows.h>
#include <timeapi.h>
#endif

int main( int argc, char** argv )
{
	cb::net::NetSimConfig config;
	uint16_t listenPort = 7778;
	std::string targetHost = "127.0.0.1";
	uint16_t targetPort = 7777;

	for ( int i = 1; i < argc; ++i )
	{
		std::string arg = argv[i];
		if ( arg == "--no-reorder" )
		{
			config.allowReorder = false;
			continue;
		}
		if ( i + 1 >= argc )
		{
			std::printf( "missing value for %s\n", arg.c_str() );
			return 1;
		}
		std::string v = argv[++i];
		if ( arg == "--listen" )
			listenPort = uint16_t( std::atoi( v.c_str() ) );
		else if ( arg == "--target" )
		{
			size_t colon = v.rfind( ':' );
			if ( colon == std::string::npos )
			{
				std::printf( "--target must be HOST:PORT\n" );
				return 1;
			}
			targetHost = v.substr( 0, colon );
			targetPort = uint16_t( std::atoi( v.c_str() + colon + 1 ) );
		}
		else if ( arg == "--latency" )
			config.latencyMs = uint32_t( std::atoi( v.c_str() ) );
		else if ( arg == "--jitter" )
			config.jitterMs = uint32_t( std::atoi( v.c_str() ) );
		else if ( arg == "--loss" )
			config.lossPercent = float( std::atof( v.c_str() ) );
		else if ( arg == "--duplicate" )
			config.duplicatePercent = float( std::atof( v.c_str() ) );
		else if ( arg == "--seed" )
			config.seed = std::strtoull( v.c_str(), nullptr, 10 );
		else
		{
			std::printf( "unknown option %s\n", arg.c_str() );
			std::printf( "usage: cb_netsim --listen PORT --target HOST:PORT [--latency MS] [--jitter MS] [--loss %%]\n"
						 "                 [--duplicate %%] [--no-reorder] [--seed N]\n" );
			return 1;
		}
	}

#if defined( _WIN32 )
	timeBeginPeriod( 1 );
#endif

	cb::net::NetSimProxy proxy;
	if ( proxy.Start( listenPort, targetHost, targetPort, config ) == false )
	{
		std::printf( "cannot listen on %u or resolve %s\n", listenPort, targetHost.c_str() );
		return 1;
	}
	std::printf( "netsim: %u -> %s:%u, latency %u ms (+%u jitter) each way, loss %.1f%%, duplicate %.1f%%%s\n", listenPort,
				 targetHost.c_str(), targetPort, config.latencyMs, config.jitterMs, config.lossPercent, config.duplicatePercent,
				 config.allowReorder ? "" : ", no reordering" );
	std::fflush( stdout );

	using Clock = std::chrono::steady_clock;
	auto start = Clock::now();
	double nextReport = 10.0;
	for ( ;; )
	{
		double now = std::chrono::duration<double>( Clock::now() - start ).count();
		proxy.Update( now );
		if ( now >= nextReport )
		{
			auto s = proxy.GetStats();
			std::printf( "netsim: %u links, %llu forwarded, %llu dropped, %llu duplicated\n", s.links,
						 (unsigned long long)s.forwarded, (unsigned long long)s.dropped, (unsigned long long)s.duplicated );
			std::fflush( stdout );
			nextReport = now + 10.0;
		}
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
}
