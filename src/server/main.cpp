// Headless dedicated server.
//
//   cb_server [--port N] [--tick-rate HZ] [--seed N] [--substeps N]
//             [--prop-lifetime SEC] [--props-per-player N] [--props-global N]
//             [--map FILE.cbmap] [--record FILE] [--mods A,B | --mods none] [--list-mods]
//             [--items DIR] [--mod-option NAME=VALUE]... [--quiet]
//
// Every gameplay mod compiled in (server_mods/) runs unless --mods names a subset. Mods with a look
// need their workshop item: its SHA-256 is read from <items dir>/<mod>.item (default: items/ next
// to this executable) and announced to clients, who must have that exact item to join.

#include "game_server.h"
#include "registry.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#include <timeapi.h>
#endif

namespace
{

void Usage()
{
	std::printf( "usage: cb_server [--port N] [--tick-rate HZ] [--seed N] [--substeps N]\n"
				 "                 [--prop-lifetime SEC] [--props-per-player N] [--props-global N]\n"
				 "                 [--map FILE.cbmap] [--record FILE] [--mods A,B | --mods none] [--list-mods]\n"
				 "                 [--items DIR] [--mod-option NAME=VALUE]... [--quiet]\n" );
}

// <dir>/<mod>.item: "sha256=<64 hex digits>" (written by tools/publish_mod.ps1).
bool ReadItem( const std::string& dir, const std::string& mod, cb::ModItem& out )
{
	std::ifstream in( std::filesystem::path( dir ) / ( mod + ".item" ) );
	std::string line;
	while ( std::getline( in, line ) )
	{
		while ( line.empty() == false && ( line.back() == '\r' || line.back() == ' ' ) )
		{
			line.pop_back();
		}
		if ( line.rfind( "sha256=", 0 ) == 0 && cb::IsSha256( line.substr( 7 ) ) )
		{
			out.mod = mod;
			out.sha256 = line.substr( 7 );
			return true;
		}
	}
	return false;
}

std::vector<std::string> SplitList( const std::string& list )
{
	std::vector<std::string> out;
	size_t start = 0;
	while ( start <= list.size() )
	{
		size_t comma = list.find( ',', start );
		std::string item = list.substr( start, comma == std::string::npos ? std::string::npos : comma - start );
		if ( item.empty() == false )
		{
			out.push_back( item );
		}
		if ( comma == std::string::npos )
		{
			break;
		}
		start = comma + 1;
	}
	return out;
}

bool ParseArgs( int argc, char** argv, cb::ServerOptions& o, std::vector<std::string>& mods, bool& listMods,
				std::string& itemsDir )
{
	for ( int i = 1; i < argc; ++i )
	{
		std::string arg = argv[i];
		if ( arg == "--help" || arg == "-h" )
		{
			return false;
		}
		if ( arg == "--quiet" )
		{
			o.verbose = false;
			continue;
		}
		if ( arg == "--record" && i + 1 < argc )
		{
			o.recordPath = argv[++i];
			continue;
		}
		if ( arg == "--map" && i + 1 < argc )
		{
			o.mapPath = argv[++i];
			continue;
		}
		if ( arg == "--mods" && i + 1 < argc )
		{
			std::string list = argv[++i];
			mods = list == "none" ? std::vector<std::string>{} : SplitList( list );
			continue;
		}
		if ( arg == "--list-mods" )
		{
			listMods = true;
			continue;
		}
		if ( arg == "--items" && i + 1 < argc )
		{
			itemsDir = argv[++i];
			continue;
		}
		if ( arg == "--mod-option" && i + 1 < argc )
		{
			std::string kv = argv[++i];
			size_t eq = kv.find( '=' );
			if ( eq == std::string::npos )
			{
				std::printf( "--mod-option wants name=value, got %s\n", kv.c_str() );
				return false;
			}
			o.modOptions[kv.substr( 0, eq )] = kv.substr( eq + 1 );
			continue;
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
	std::vector<std::string> modNames;
	for ( const cb::mods::ModInfo& info : cb::mods::CompiledMods() )
	{
		modNames.push_back( info.name );
	}
	bool listMods = false;
	std::string itemsDir = ( std::filesystem::absolute( argv[0] ).parent_path() / "items" ).string();
	if ( ParseArgs( argc, argv, options, modNames, listMods, itemsDir ) == false )
	{
		Usage();
		return 1;
	}
	if ( listMods )
	{
		for ( const cb::mods::ModInfo& info : cb::mods::CompiledMods() )
		{
			std::printf( "%s\n", info.name );
		}
		return 0;
	}

#if defined( _WIN32 )
	timeBeginPeriod( 1 ); // 1 ms sleep resolution
#endif

	cb::GameServer server;
	for ( const std::string& name : modNames )
	{
		std::unique_ptr<cb::mods::ServerMod> mod = cb::mods::CreateMod( name );
		if ( mod == nullptr )
		{
			std::printf( "no mod named %s (see --list-mods)\n", name.c_str() );
			return 1;
		}
		server.AddMod( std::move( mod ) );

		// The item players need for this mod's look, if it has one.
		bool clientContent = false;
		for ( const cb::mods::ModInfo& info : cb::mods::CompiledMods() )
		{
			clientContent |= name == info.name && info.clientContent;
		}
		if ( clientContent == false )
		{
			continue;
		}
		cb::ModItem item;
		if ( ReadItem( itemsDir, name, item ) )
		{
			options.items.push_back( item );
		}
		else
		{
			std::printf( "warning: mod %s has client content but no item manifest in %s; clients will not load its look\n",
						 name.c_str(), itemsDir.c_str() );
		}
	}
	if ( server.Start( options ) == false )
	{
		return 1;
	}

	using Clock = std::chrono::steady_clock;
	const auto start = Clock::now();
	auto seconds = [&] { return std::chrono::duration<double>( Clock::now() - start ).count(); };

	double nextStatus = 5.0;
	double lastStatus = 0.0;
	uint64_t lastBytesSent = 0;
	uint64_t lastLate = 0;
	uint64_t lastInputTicks = 0;
	for ( ;; )
	{
		double now = seconds();
		server.Update( now );

		if ( now >= nextStatus )
		{
			const auto& s = server.GetStats();
			double avg = s.ticks ? s.tickMsTotal / double( s.ticks ) : 0.0;
			double upKbps = double( s.bytesSent - lastBytesSent ) * 8.0 / 1000.0 / ( now - lastStatus );
			uint64_t expected = s.inputTicks - lastInputTicks;
			double latePercent = expected ? 100.0 * double( s.lateInputs - lastLate ) / double( expected ) : 0.0;
			std::printf( "[server %6u] %d connected, %zu entities, %zu KB physics | tick %.2f ms avg %.2f max | "
						 "late inputs %.2f%% | snapshots %llu | out %.0f kbit/s\n",
						 server.Tick(), server.ConnectedClients(), server.Sim().Entities().size(),
						 server.Sim().PhysicsBytesInUse() / 1024, avg, s.tickMsMax, latePercent,
						 (unsigned long long)s.snapshotsSent, upKbps );
			std::fflush( stdout );
			server.ResetTickTiming();
			lastBytesSent = s.bytesSent;
			lastLate = s.lateInputs;
			lastInputTicks = s.inputTicks;
			lastStatus = now;
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
