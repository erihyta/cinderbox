// Headless dedicated server.
//
//   cb_server [--port N] [--tick-rate HZ] [--seed N] [--substeps N]
//             [--prop-lifetime SEC] [--props-per-player N] [--props-global N]

#include "game_server.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined( _WIN32 )
#include <windows.h>
#include <timeapi.h>
#endif

namespace
{

void Usage()
{
	std::printf( "usage: cb_server [--port N] [--tick-rate HZ] [--seed N] [--substeps N]\n"
				 "                 [--prop-lifetime SEC] [--props-per-player N] [--props-global N]\n" );
}

bool ParseArgs( int argc, char** argv, cb::ServerOptions& o )
{
	for ( int i = 1; i < argc; ++i )
	{
		std::string arg = argv[i];
		if ( arg == "--help" || arg == "-h" )
		{
			return false;
		}
		if ( i + 1 >= argc )
		{
			std::printf( "missing value for %s\n", arg.c_str() );
			return false;
		}
		unsigned long long v = std::strtoull( argv[++i], nullptr, 10 );
		if ( arg == "--port" )
			o.port = uint16_t( v );
		else if ( arg == "--tick-rate" )
			o.config.tickRate = uint32_t( v );
		else if ( arg == "--seed" )
			o.config.seed = v;
		else if ( arg == "--substeps" )
			o.config.subSteps = uint32_t( v );
		else if ( arg == "--prop-lifetime" )
			o.config.propLifetimeSeconds = uint32_t( v );
		else if ( arg == "--props-per-player" )
			o.config.propsPerPlayer = uint32_t( v );
		else if ( arg == "--props-global" )
			o.config.propsGlobal = uint32_t( v );
		else
		{
			std::printf( "unknown option %s\n", arg.c_str() );
			return false;
		}
	}
	if ( o.config.tickRate < 10 || o.config.tickRate > 240 )
	{
		std::printf( "--tick-rate must be in [10, 240]\n" );
		return false;
	}
	if ( o.config.subSteps < 1 || o.config.subSteps > 16 )
	{
		std::printf( "--substeps must be in [1, 16]\n" );
		return false;
	}
	return true;
}

} // namespace

int main( int argc, char** argv )
{
	cb::ServerOptions options;
	if ( ParseArgs( argc, argv, options ) == false )
	{
		Usage();
		return 1;
	}

#if defined( _WIN32 )
	timeBeginPeriod( 1 ); // 1 ms sleep resolution
#endif

	cb::GameServer server;
	if ( server.Start( options ) == false )
	{
		return 1;
	}

	using Clock = std::chrono::steady_clock;
	const auto start = Clock::now();
	auto seconds = [&] { return std::chrono::duration<double>( Clock::now() - start ).count(); };

	double nextStatus = 5.0;
	for ( ;; )
	{
		double now = seconds();
		server.Update( now );

		if ( now >= nextStatus )
		{
			const auto& s = server.GetStats();
			std::printf( "[server %6u] %d connected, %zu entities, %zu KB physics, late inputs %llu, snapshots sent %llu\n",
						 server.Tick(), server.ConnectedClients(), server.Sim().Entities().size(),
						 server.Sim().PhysicsBytesInUse() / 1024, (unsigned long long)s.lateInputs,
						 (unsigned long long)s.snapshotsSent );
			std::fflush( stdout );
			nextStatus = now + 5.0;
		}

		double wait = server.TimeUntilNextTick( seconds() );
		if ( wait > 0.002 )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
		else if ( wait > 0.0 )
		{
			std::this_thread::yield();
		}
	}
}
