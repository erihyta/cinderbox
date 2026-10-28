// Replay tool.
//
//   cb_replay info FILE      summary of a recorded session
//   cb_replay verify FILE    re-simulate the whole session and check every recorded checksum
//   cb_replay view FILE      summary of a view file (cb_server --record-view): frames and their sizes
//   cb_replay state FILE [--slot N] [--delay TICKS]
//                            the state network model played against the session: what its states
//                            cost, whether each applies exactly, and how a client of a slot does
//
// Exit codes: 0 ok, 1 usage / unreadable file, 2 checksum mismatch, 3 recorded with another build.

#include "fingerprint.h"
#include "replay.h"
#include "simulation.h"
#include "state_session.h"
#include "view_file.h"

#if defined( __GNUC__ )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <miniz.h>
#if defined( __GNUC__ )
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <deque>
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
	{
		std::vector<cb::Simulation::CharacterBody> characters;
		std::string ignored;
		cb::Simulation::CharactersOf( replay.Schema(), characters, ignored );
		sim.SetCharacters( std::move( characters ) );
	}
	sim.SetAnimPacks( replay.Packs() );
	sim.SetMotions( replay.MotionSets() );
	sim.SetItemShapes( replay.Schema().itemShapes );
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

// A simulation as the recording's server had it: its characters, packs, motions and item shapes.
void SetUpLike( const net::ReplayReader& replay, Simulation& sim )
{
	std::vector<cb::Simulation::CharacterBody> characters;
	std::string ignored;
	cb::Simulation::CharactersOf( replay.Schema(), characters, ignored );
	sim.SetCharacters( std::move( characters ) );
	sim.SetAnimPacks( replay.Packs() );
	sim.SetMotions( replay.MotionSets() );
	sim.SetItemShapes( replay.Schema().itemShapes );
}

size_t Deflated( const std::vector<uint8_t>& in )
{
	mz_ulong size = mz_compressBound( mz_ulong( in.size() ) );
	std::vector<uint8_t> out( size );
	mz_compress2( out.data(), &size, in.data(), mz_ulong( in.size() ), 1 );
	return size_t( size );
}

double MsSince( std::chrono::steady_clock::time_point start )
{
	return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
}

// The state network model played against a recording (ROADMAP step 3): the recording is the
// server; every second tick its state (Simulation::SaveState) goes to a client of `slot`, arriving
// `delay` ticks later, which predicts that slot's body with the recorded inputs and places the
// rest. Each state is also applied at once to a third simulation and checked against the server's.
int StateModel( const net::ReplayReader& replay, PlayerSlot slot, uint32_t delay )
{
	Simulation server( replay.Config(), replay.Map() );
	SetUpLike( replay, server );
	Simulation mirror( replay.Config(), replay.Map() );
	SetUpLike( replay, mirror );
	StateSession client( replay.Config(), slot, 60, replay.Map() );
	SetUpLike( replay, client.Sim() );

	struct Sent
	{
		uint32_t tick;
		uint32_t arrives;
		std::vector<uint8_t> bytes;
		std::array<PlayerInput, kMaxPlayers> inputs;
	};
	std::deque<Sent> wire;
	const auto& frames = replay.Frames();
	const uint32_t total = uint32_t( frames.size() );

	struct Seen
	{
		bool has = false;
		uint32_t netId = 0;
		b3Vec3 at = {};
	};
	// By tick (the state in which Tick() == t).
	std::vector<Seen> serverSaw( total + 1 ), clientSaw( total + 1 );
	auto ownOf = [slot]( Simulation& sim, Seen& seen ) {
		const uint32_t netId = sim.Globals().playerNetIds[slot];
		const Transform* t = netId != 0 ? sim.EntityTransform( netId ) : nullptr;
		if ( t != nullptr )
		{
			seen = { true, netId, t->position };
		}
	};

	double serverMs = 0.0, saveMs = 0.0;
	size_t states = 0, bytes = 0, most = 0, packed = 0, changed = 0, sampled = 0, wrong = 0, entities = 0;
	std::vector<uint8_t> previous;
	for ( uint32_t i = 0; i < total; ++i )
	{
		auto start = std::chrono::steady_clock::now();
		server.Step( frames[i] );
		serverMs += MsSince( start );
		if ( server.Tick() <= total )
		{
			ownOf( server, serverSaw[server.Tick()] );
		}

		if ( server.Tick() % 2 == 0 )
		{
			Sent sent;
			sent.tick = server.Tick();
			sent.arrives = i + delay;
			start = std::chrono::steady_clock::now();
			server.SaveState( sent.bytes );
			saveMs += MsSince( start );
			sent.inputs = frames[i].inputs;
			states += 1;
			bytes += sent.bytes.size();
			most = std::max( most, sent.bytes.size() );
			// (A sample: deflating is slow. The change is against the state before it.)
			if ( states % 8 == 0 && previous.empty() == false )
			{
				packed += Deflated( sent.bytes );
				std::vector<uint8_t> x( sent.bytes );
				for ( size_t k = 0; k < x.size() && k < previous.size(); ++k )
				{
					x[k] ^= previous[k];
				}
				changed += Deflated( x );
				sampled += 1;
			}
			if ( states % 8 == 7 )
			{
				previous = sent.bytes;
			}

			bool same = mirror.ApplyState( sent.bytes.data(), sent.bytes.size(), slot ) && mirror.Tick() == server.Tick() &&
						mirror.Entities().size() == server.Entities().size();
			for ( size_t k = 0; same && k < server.Entities().size(); ++k )
			{
				const uint32_t netId = server.Entities()[k].netId;
				const Transform* a = server.EntityTransform( netId );
				const Transform* b = mirror.EntityTransform( netId );
				same = mirror.Entities()[k].netId == netId && ( a == nullptr ) == ( b == nullptr ) &&
					   ( a == nullptr || std::memcmp( &a->position, &b->position, sizeof( b3Vec3 ) ) == 0 );
			}
			wrong += same ? 0 : 1;
			entities = std::max( entities, server.Entities().size() );
			wire.push_back( std::move( sent ) );
		}
		while ( wire.empty() == false && wire.front().arrives <= i )
		{
			client.AddState( wire.front().tick, wire.front().bytes.data(), wire.front().bytes.size(), wire.front().inputs );
			wire.pop_front();
		}
		client.Reconcile();
		// The client ahead of the server by about the delay (a tick a frame, two to catch up).
		for ( int step = 0; step < 2 && client.CurrentTick() < std::min( server.Tick() + delay + 2, total ); ++step )
		{
			const uint32_t t = client.CurrentTick();
			if ( client.AdvanceOne( frames[t].inputs[slot] ) == false )
			{
				break;
			}
			if ( clientSaw[client.CurrentTick()].has == false )
			{
				ownOf( client.Sim(), clientSaw[client.CurrentTick()] );
			}
		}
	}

	uint32_t compared = 0, near = 0;
	float worst = 0.0f, sum = 0.0f;
	for ( uint32_t t = 0; t <= total; ++t )
	{
		if ( serverSaw[t].has && clientSaw[t].has && serverSaw[t].netId == clientSaw[t].netId )
		{
			const float off = b3Length( b3Sub( serverSaw[t].at, clientSaw[t].at ) );
			compared += 1;
			near += off < 0.05f ? 1 : 0;
			sum += off;
			worst = std::max( worst, off );
		}
	}
	const auto& st = client.GetStats();
	const double n = double( std::max<size_t>( states, 1 ) );
	const double s = double( std::max<size_t>( sampled, 1 ) );
	std::printf( "states       %zu (every 2 ticks), %zu entities at most; %zu applied wrong\n", states, entities, wrong );
	std::printf( "a state      %.0f bytes on average, %zu at most; deflated %.0f; as a deflated change %.0f (%.0f kbit/s at 30 a second)\n",
				 double( bytes ) / n, most, double( packed ) / s, double( changed ) / s, double( changed ) / s * 30.0 * 8.0 / 1000.0 );
	std::printf( "the server   a step %.3f ms, a state saved %.3f ms\n", serverMs / std::max( 1.0, double( total ) ), saveMs / n );
	std::printf( "the client   slot %u, states %u ticks late: a step %.3f ms, a state applied %.3f ms; %" PRIu64 " states taken, %" PRIu64
				 " overtaken, %" PRIu64 " not states; %.1f ticks run again a state\n",
				 unsigned( slot ), delay, st.steps > 0 ? double( st.stepNs ) / 1e6 / double( st.steps ) : 0.0,
				 st.states > 0 ? double( st.applyNs ) / 1e6 / double( st.states ) : 0.0, st.states, st.skipped, st.rejected,
				 st.states > 0 ? double( st.resimulatedTicks ) / double( st.states ) : 0.0 );
	std::printf( "its body     predicted within 5 cm of the server's %u of %u ticks; %.3f m on average, %.3f at most\n", near, compared,
				 compared > 0 ? sum / float( compared ) : 0.0f, worst );
	return wrong == 0 && st.rejected == 0 ? 0 : 2;
}

int PrintViewInfo( const char* path )
{
	present::ViewFileInfo info;
	std::string error;
	if ( present::ReadViewFileInfo( path, info, error ) == false )
	{
		std::printf( "error: %s\n", error.c_str() );
		return 1;
	}
	size_t deltas = info.frames - info.keys;
	double perDelta = deltas > 0 ? double( info.deltaBytes ) / double( deltas ) : 0.0;
	double rate = info.seconds > 0.0 ? double( info.frames ) / info.seconds : 0.0;
	std::printf( "frames       %zu (%.1f s, one every %u ticks), %zu of them stand alone\n", info.frames, info.seconds, info.stride, info.keys );
	std::printf( "map          %s\n", info.map.c_str() );
	std::printf( "most         %zu players, %zu entities\n", info.mostPlayers, info.mostEntities );
	std::printf( "whole frame  %.0f bytes on average\n", info.keys > 0 ? double( info.keyBytes ) / double( info.keys ) : 0.0 );
	std::printf( "delta frame  %.0f bytes on average, %u at most (%.0f kbit/s at %.0f frames a second), %s\n", perDelta, info.deltaMost,
				 perDelta * rate * 8.0 / 1000.0, rate, info.compact ? "compact" : "exact" );
	if ( deltas > 0 )
	{
		// Where an average delta frame's bytes go.
		const present::ViewCost& c = info.cost;
		const struct
		{
			const char* name;
			size_t bytes;
		} parts[] = { { "header", c.header },	  { "stats", c.stats },			{ "session", c.session },	  { "inputs", c.inputs },
					  { "events", c.events },	  { "lists", c.lists },			{ "rest", c.rest },			  { "positions", c.positions },
					  { "rotations", c.rotations }, { "animation", c.animation }, { "boards", c.boards },		  { "ragdolls", c.ragdolls },
					  { "entities", c.exact } };
		std::printf( "of which    " );
		for ( const auto& part : parts )
		{
			if ( part.bytes > 0 )
			{
				std::printf( " %s %.0f", part.name, double( part.bytes ) / double( deltas ) );
			}
		}
		std::printf( "\n" );
	}
	return 0;
}

} // namespace

int main( int argc, char** argv )
{
	if ( argc == 3 && std::strcmp( argv[1], "view" ) == 0 )
	{
		return PrintViewInfo( argv[2] );
	}
	const bool state = argc >= 3 && std::strcmp( argv[1], "state" ) == 0;
	if ( ( argc != 3 && state == false ) ||
		 ( state == false && std::strcmp( argv[1], "info" ) != 0 && std::strcmp( argv[1], "verify" ) != 0 ) )
	{
		std::printf( "usage: cb_replay info FILE\n       cb_replay verify FILE\n       cb_replay view FILE\n"
					 "       cb_replay state FILE [--slot N] [--delay TICKS]\n" );
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
	if ( state )
	{
		PlayerSlot slot = 0;
		uint32_t delay = 6;
		for ( int i = 3; i + 1 < argc; i += 2 )
		{
			if ( std::strcmp( argv[i], "--slot" ) == 0 )
			{
				slot = PlayerSlot( std::atoi( argv[i + 1] ) );
			}
			else if ( std::strcmp( argv[i], "--delay" ) == 0 )
			{
				delay = uint32_t( std::max( 1, std::atoi( argv[i + 1] ) ) );
			}
		}
		return StateModel( replay, slot, delay );
	}
	return 0;
}
