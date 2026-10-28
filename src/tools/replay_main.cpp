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
#include "state_codec.h"
#include "state_session.h"
#include "view_file.h"


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

// Where a state's change against a base is, by component (sim/stand_ins.cpp has the layout): the
// bytes that differ in records of the same mask, by the component they are in; records new or of
// another mask, whole; bodies described again.
struct ChangeCost
{
	std::vector<double> component;
	double whole = 0.0, bodies = 0.0, entities = 0.0, changedEntities = 0.0;
};

void CountChange( const std::vector<uint8_t>& now, const std::vector<uint8_t>& was, const std::vector<std::pair<std::string, uint32_t>>& layout,
				  ChangeCost& cost )
{
	struct Span
	{
		uint32_t netId = 0, recordSize = 0, bodiesSize = 0;
		const uint8_t* record = nullptr;
		uint64_t hash = 0;
	};
	auto parse = []( const std::vector<uint8_t>& state, std::vector<Span>& out ) {
		ByteReader r( state.data(), state.size() );
		r.Read<uint32_t>();
		r.Take( r.Read<uint32_t>() );
		const uint32_t count = r.Read<uint32_t>();
		for ( uint32_t i = 0; i < count && r.Ok(); ++i )
		{
			Span e;
			e.netId = r.Read<uint32_t>();
			e.recordSize = r.Read<uint32_t>();
			e.record = r.Take( e.recordSize );
			e.bodiesSize = r.Read<uint32_t>();
			if ( e.bodiesSize > 0 )
			{
				e.hash = r.Read<uint64_t>();
				r.Take( e.bodiesSize );
			}
			out.push_back( e );
		}
	};
	std::vector<Span> a, b;
	parse( now, a );
	parse( was, b );
	cost.component.resize( layout.size(), 0.0 );
	size_t k = 0;
	for ( const Span& e : a )
	{
		while ( k < b.size() && b[k].netId < e.netId )
		{
			++k;
		}
		const Span* old = k < b.size() && b[k].netId == e.netId ? &b[k] : nullptr;
		cost.entities += 1.0;
		if ( e.bodiesSize > 0 && ( old == nullptr || old->hash != e.hash ) )
		{
			cost.bodies += double( e.bodiesSize );
		}
		uint32_t mask = 0, oldMask = 0;
		std::memcpy( &mask, e.record, 4 );
		if ( old != nullptr )
		{
			std::memcpy( &oldMask, old->record, 4 );
		}
		if ( old == nullptr || oldMask != mask || old->recordSize != e.recordSize )
		{
			cost.whole += double( e.recordSize );
			cost.changedEntities += 1.0;
			continue;
		}
		bool changed = false;
		size_t at = 4;
		for ( size_t c = 0; c < layout.size(); ++c )
		{
			if ( ( mask & ( 1u << c ) ) == 0 || layout[c].second == 0 )
			{
				continue;
			}
			for ( uint32_t i = 0; i < layout[c].second; ++i )
			{
				if ( e.record[at + i] != old->record[at + i] )
				{
					cost.component[c] += 1.0;
					changed = true;
				}
			}
			at += layout[c].second;
		}
		cost.changedEntities += changed ? 1.0 : 0.0;
	}
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
	size_t states = 0, bytes = 0, most = 0, packed = 0, changed = 0, mostChange = 0, sampled = 0, wrong = 0, undecoded = 0, entities = 0;
	double encodeMs = 0.0;
	std::deque<net::StateFrame> kept;
	const auto layout = server.StateComponents();
	ChangeCost where;
	size_t counted = 0;
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
			// As the wire has it (net/state_codec.h): whole, and as a change against the state a
			// client would have acknowledged by now (a round trip before: twice the delay), each
			// decoded again and checked.
			{
				net::StateFrame frame;
				frame.tick = sent.tick;
				frame.inputs = sent.inputs;
				server.SaveState( frame.state, true ); // (compact, as a server sends it)
				const net::StateFrame* base = nullptr;
				for ( const net::StateFrame& b : kept )
				{
					base = b.tick + 2 * delay <= frame.tick ? &b : base;
				}
				std::vector<uint8_t> message;
				net::EncodeState( frame, nullptr, message );
				packed += message.size();
				const auto encodeStart = std::chrono::steady_clock::now();
				net::EncodeState( frame, base, message );
				encodeMs += MsSince( encodeStart );
				changed += message.size();
				most = std::max( most, sent.bytes.size() );
				mostChange = std::max( mostChange, message.size() );
				sampled += 1;
				ByteReader r( message.data(), message.size() );
				uint32_t tick = 0, baseTick = 0;
				net::StateFrame back;
				const bool decoded = net::ReadType( r ) == net::MsgType::State && net::ReadStateHeader( r, tick, baseTick ) &&
									 baseTick == ( base != nullptr ? base->tick : net::kNoStateBase ) &&
									 net::DecodeState( r, tick, base, back ) && back.state == frame.state &&
									 std::memcmp( back.inputs.data(), frame.inputs.data(), sizeof( frame.inputs ) ) == 0;
				undecoded += decoded ? 0 : 1;
				if ( base != nullptr )
				{
					CountChange( frame.state, base->state, layout, where );
					counted += 1;
				}
				kept.push_back( std::move( frame ) );
				if ( kept.size() > 32 )
				{
					kept.pop_front();
				}
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
	std::printf( "states       %zu (every 2 ticks), %zu entities at most; %zu applied wrong, %zu not decoded as they were\n", states,
				 entities, wrong, undecoded );
	std::printf( "a state      %.0f bytes on average, %zu at most; on the wire whole %.0f, as a change against one %u ticks before %.0f "
				 "(%zu at most, %.2f ms to make; %.0f kbit/s at 30 a second)\n",
				 double( bytes ) / n, most, double( packed ) / s, 2 * delay, double( changed ) / s, mostChange, encodeMs / s,
				 double( changed ) / s * 30.0 * 8.0 / 1000.0 );
	if ( counted > 0 )
	{
		// Before deflating: bytes that differ, by component, in an average change.
		const double c = double( counted );
		std::printf( "of a change  %.0f of %.0f entities differ; bytes that differ:", where.changedEntities / c, where.entities / c );
		std::vector<std::pair<double, std::string>> parts;
		for ( size_t i = 0; i < layout.size(); ++i )
		{
			parts.push_back( { where.component[i] / c, layout[i].first } );
		}
		std::sort( parts.rbegin(), parts.rend() );
		for ( size_t i = 0; i < parts.size() && i < 8 && parts[i].first >= 1.0; ++i )
		{
			std::printf( " %s %.0f", parts[i].second.c_str(), parts[i].first );
		}
		std::printf( "; records whole %.0f; bodies %.0f\n", where.whole / c, where.bodies / c );
	}
	std::printf( "the server   a step %.3f ms, a state saved %.3f ms\n", serverMs / std::max( 1.0, double( total ) ), saveMs / n );
	std::printf( "the client   slot %u, states %u ticks late: a step %.3f ms, a state applied %.3f ms; %" PRIu64 " states taken, %" PRIu64
				 " overtaken, %" PRIu64 " not states; %.1f ticks run again a state\n",
				 unsigned( slot ), delay, st.steps > 0 ? double( st.stepNs ) / 1e6 / double( st.steps ) : 0.0,
				 st.states > 0 ? double( st.applyNs ) / 1e6 / double( st.states ) : 0.0, st.states, st.skipped, st.rejected,
				 st.states > 0 ? double( st.resimulatedTicks ) / double( st.states ) : 0.0 );
	std::printf( "its body     predicted within 5 cm of the server's %u of %u ticks; %.3f m on average, %.3f at most\n", near, compared,
				 compared > 0 ? sum / float( compared ) : 0.0f, worst );
	return wrong == 0 && undecoded == 0 && st.rejected == 0 ? 0 : 2;
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
