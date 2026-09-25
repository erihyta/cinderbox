// Replay tool.
//
//   cb_replay info FILE      summary of a recorded session
//   cb_replay verify FILE    re-simulate the whole session and check every recorded checksum
//
// Exit codes: 0 ok, 1 usage / unreadable file, 2 checksum mismatch, 3 recorded with another build.

#include "fingerprint.h"
#include "replay.h"
#include "simulation.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using namespace cb;

namespace
{

void PrintInfo( const net::ReplayReader& replay )
{
	const SimConfig& c = replay.Config();
	uint32_t joins = 0;
	uint32_t leaves = 0;
	for ( const InputFrame& f : replay.Frames() )
	{
		for ( const PlayerEvent& e : f.events )
		{
			( e.type == PlayerEventType::Join ? joins : leaves ) += 1;
		}
	}
	double seconds = double( replay.Frames().size() ) / double( c.tickRate );
	std::printf( "ticks        %zu (%.1f s at %u Hz)%s\n", replay.Frames().size(), seconds, c.tickRate,
				 replay.Truncated() ? ", file truncated (crash?)" : "" );
	std::printf( "checksums    %zu\n", replay.Checksums().size() );
	std::printf( "joins/leaves %u / %u\n", joins, leaves );
	std::printf( "seed         %llu, substeps %u, props %u per player / %u global, lifetime %u s\n",
				 (unsigned long long)c.seed, c.subSteps, c.propsPerPlayer, c.propsGlobal, c.propLifetimeSeconds );
	const LevelLayout& map = replay.Map();
	std::printf( "map          %s (%zu statics, %zu props, hash %016llx)\n", map.name.empty() ? "unnamed" : map.name.c_str(),
				 map.statics.size(), map.props.size(),
				 (unsigned long long)MapHash( replay.MapBytes().data(), replay.MapBytes().size() ) );
	bool same = replay.Fingerprint() == BuildFingerprint();
	std::printf( "fingerprint  %016llx (%s)\n", (unsigned long long)replay.Fingerprint(),
				 same ? "matches this build" : "DIFFERENT simulation build" );
}

int Verify( const net::ReplayReader& replay )
{
	bool sameBuild = replay.Fingerprint() == BuildFingerprint();
	if ( sameBuild == false )
	{
		std::printf( "warning: recorded with a different simulation build; checksums will likely not match\n" );
	}

	Simulation sim( replay.Config(), replay.Map() );
	sim.SetAnimGraph( replay.Graph() );
	const auto& checksums = replay.Checksums();
	size_t nextChecksum = 0;
	size_t verified = 0;
	auto check = [&]() -> bool {
		while ( nextChecksum < checksums.size() && checksums[nextChecksum].tick == sim.Tick() )
		{
			uint64_t hash = sim.ComputeHash();
			if ( hash != checksums[nextChecksum].hash )
			{
				std::printf( "MISMATCH at tick %u: replay %016llx, simulated %016llx\n", sim.Tick(),
							 (unsigned long long)checksums[nextChecksum].hash, (unsigned long long)hash );
				return false;
			}
			++verified;
			++nextChecksum;
		}
		return true;
	};

	auto start = std::chrono::steady_clock::now();
	if ( check() == false )
	{
		return sameBuild ? 2 : 3;
	}
	for ( const InputFrame& frame : replay.Frames() )
	{
		sim.Step( frame );
		if ( check() == false )
		{
			return sameBuild ? 2 : 3;
		}
	}
	double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();

	std::printf( "verified %zu checksums over %zu ticks in %.0f ms (%.3f ms/tick); final state %016llx\n", verified,
				 replay.Frames().size(), ms, replay.Frames().empty() ? 0.0 : ms / double( replay.Frames().size() ),
				 (unsigned long long)sim.ComputeHash() );
	if ( nextChecksum < checksums.size() )
	{
		std::printf( "note: %zu checksums lie beyond the last recorded frame\n", checksums.size() - nextChecksum );
	}
	return 0;
}

} // namespace

int main( int argc, char** argv )
{
	if ( argc != 3 || ( std::strcmp( argv[1], "info" ) != 0 && std::strcmp( argv[1], "verify" ) != 0 ) )
	{
		std::printf( "usage: cb_replay info FILE\n       cb_replay verify FILE\n" );
		return 1;
	}

	net::ReplayReader replay;
	std::string error;
	if ( replay.Open( argv[2], error ) == false )
	{
		std::printf( "error: %s\n", error.c_str() );
		return 1;
	}

	PrintInfo( replay );
	if ( std::strcmp( argv[1], "verify" ) == 0 )
	{
		return Verify( replay );
	}
	return 0;
}
