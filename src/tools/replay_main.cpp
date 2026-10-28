// Replay tool.
//
//   cb_replay info FILE      summary of a recorded session
//   cb_replay verify FILE    re-simulate the whole session and check every recorded checksum
//   cb_replay view FILE      summary of a view file (cb_server --record-view): frames and their sizes
//   cb_replay hybrid FILE [--slot N] [--delay TICKS]
//                            the hybrid network model played against the session: what its frames
//                            and states cost, whether its physics stays the server's, and how a
//                            client predicts a slot's character
//
// Exit codes: 0 ok, 1 usage / unreadable file, 2 checksum mismatch, 3 recorded with another build.

#include "fingerprint.h"
#include "replay.h"
#include "simulation.h"
#include "state_codec.h"
#include "hybrid_session.h"
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

// The hybrid network model played against a recording (M248; ROADMAP step 3): the recording is
// the server. Every tick its physics inputs (the frame's commands, the physics events and pushers'
// places it made) and its physics checksum go to a client of `slot`, and every second tick its
// characters' state, each arriving `delay` ticks later; the client's physics runs `delay` + 2 ticks
// behind, and it predicts that slot's character with the recorded inputs. What each costs on the
// wire is measured as the server sends it (net/protocol.h, net/state_codec.h), decoded again and
// checked.
int HybridModel( const net::ReplayReader& replay, PlayerSlot slot, uint32_t delay )
{
	auto serverSim = std::make_unique<Simulation>( replay.Config(), replay.Map() );
	Simulation& server = *serverSim;
	SetUpLike( replay, server );
	HybridSession client( replay.Config(), slot, 60, replay.Map() );
	SetUpLike( replay, client.Sim() );
	{
		std::vector<uint8_t> image;
		server.SavePortable( image );
		if ( client.Sim().LoadPortable( image ) == false )
		{
			std::printf( "error: the client could not load the server's world\n" );
			return 2;
		}
		client.Start();
	}

	struct Sent
	{
		uint32_t arrives;
		bool state;
		uint32_t tick;
		InputFrame frame;
		uint64_t checksum;
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

	double serverMs = 0.0, saveMs = 0.0, encodeMs = 0.0;
	size_t states = 0, bytes = 0, most = 0, packed = 0, changed = 0, mostChange = 0, undecoded = 0;
	size_t frameBytes = 0, frameMost = 0, events = 0, pushers = 0, framesWrong = 0;
	std::deque<net::StateFrame> kept;
	const auto layout = server.StateComponents();
	ChangeCost where;
	size_t counted = 0;
	for ( uint32_t i = 0; i < total; ++i )
	{
		if ( server.Tick() % 2 == 0 )
		{
			Sent sent{ i + delay, true, server.Tick(), {}, 0, {}, i > 0 ? frames[i - 1].inputs : net::InputArray{} };
			auto start = std::chrono::steady_clock::now();
			server.SaveState( sent.bytes, true ); // (compact, as a server sends it)
			saveMs += MsSince( start );
			states += 1;
			bytes += sent.bytes.size();
			most = std::max( most, sent.bytes.size() );
			// As the wire has it (net/state_codec.h): whole, and as a change against the state a
			// client would have acknowledged by now (a round trip before: twice the delay), each
			// decoded again and checked.
			net::StateFrame frame;
			frame.tick = sent.tick;
			frame.inputs = sent.inputs;
			frame.state = sent.bytes;
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
			mostChange = std::max( mostChange, message.size() );
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
			wire.push_back( std::move( sent ) );
		}

		auto start = std::chrono::steady_clock::now();
		server.Step( frames[i] );
		serverMs += MsSince( start );
		if ( server.Tick() <= total )
		{
			ownOf( server, serverSaw[server.Tick()] );
		}
		{
			Sent sent{ i + delay, false, i, frames[i], server.PhysicsHash(), {}, {} };
			sent.frame.physics = server.PhysicsMade().physics;
			sent.frame.pushers = server.PhysicsMade().pushers;
			events += sent.frame.physics.size();
			pushers += sent.frame.pushers.size();
			// A frame as a batch of one has it (without the inputs), decoded again.
			const InputFrame* one = &sent.frame;
			std::vector<uint8_t> message;
			net::EncodeFrameBatch( {}, &one, 1, message, false, &sent.checksum );
			frameBytes += message.size();
			frameMost = std::max( frameMost, message.size() );
			ByteReader r( message.data(), message.size() );
			uint32_t first = 0, count = 0;
			std::vector<InputFrame> back;
			std::vector<uint64_t> sums;
			const bool decoded = net::ReadType( r ) == net::MsgType::FrameBatch && net::ReadFrameBatchHeader( r, first, count ) &&
								 net::ReadFrameBatchBody( r, {}, first, count, back, &sums ) && back.size() == 1 &&
								 back[0].commands.size() == sent.frame.commands.size() && back[0].physics == sent.frame.physics &&
								 back[0].pushers == sent.frame.pushers && sums[0] == sent.checksum;
			framesWrong += decoded ? 0 : 1;
			wire.push_back( std::move( sent ) );
		}

		while ( wire.empty() == false && wire.front().arrives <= i )
		{
			const Sent& s = wire.front();
			if ( s.state )
			{
				client.AddState( s.tick, s.bytes.data(), s.bytes.size(), s.inputs );
			}
			else
			{
				client.AddChecksum( s.tick, s.checksum );
				client.AddFrame( s.frame );
			}
			wire.pop_front();
		}
		if ( i + 1 > delay + 2 )
		{
			client.AdvancePhysics( i + 1 - ( delay + 2 ) );
		}
		// The own character ahead of the server by about the delay (a tick a frame, two to catch up).
		for ( int step = 0; step < 2 && client.PredictedTick() < std::min( server.Tick() + delay + 2, total ); ++step )
		{
			if ( client.Predict( frames[client.PredictedTick()].inputs[slot] ) == false )
			{
				break;
			}
			if ( clientSaw[client.PredictedTick()].has == false )
			{
				ownOf( client.Sim(), clientSaw[client.PredictedTick()] );
			}
		}
	}

	uint32_t compared = 0, nearby = 0;
	float worst = 0.0f, sum = 0.0f;
	for ( uint32_t t = 0; t <= total; ++t )
	{
		if ( serverSaw[t].has && clientSaw[t].has && serverSaw[t].netId == clientSaw[t].netId )
		{
			const float off = b3Length( b3Sub( serverSaw[t].at, clientSaw[t].at ) );
			compared += 1;
			nearby += off < 0.05f ? 1 : 0;
			sum += off;
			worst = std::max( worst, off );
		}
	}
	const auto& st = client.GetStats();
	const double n = double( std::max<size_t>( states, 1 ) );
	const double f = double( std::max<uint32_t>( total, 1 ) );
	std::printf( "frames       %u: %zu physics events, %zu pushers' places; on the wire %.0f bytes on average, %zu at most (%.0f kbit/s "
				 "at %u a second); %zu not decoded as they were\n",
				 total, events, pushers, double( frameBytes ) / f, frameMost, double( frameBytes ) / f * replay.Config().tickRate * 8.0 / 1000.0,
				 unsigned( replay.Config().tickRate ), framesWrong );
	std::printf( "states       %zu (every 2 ticks): %.0f bytes on average, %zu at most; on the wire whole %.0f, as a change against one "
				 "%u ticks before %.0f (%zu at most, %.2f ms to make; %.0f kbit/s); %zu not decoded as they were\n",
				 states, double( bytes ) / n, most, double( packed ) / n, 2 * delay, double( changed ) / n, mostChange, encodeMs / n,
				 double( changed ) / n * replay.Config().tickRate / 2.0 * 8.0 / 1000.0, undecoded );
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
	std::printf( "the server   a step %.3f ms, a state saved %.3f ms\n", serverMs / f, saveMs / n );
	std::printf( "the client   slot %u, %u ticks late: its physics %" PRIu64 " ticks, %.3f ms a step, %" PRIu64 " checksums, %" PRIu64
				 " different (the first at %d); %" PRIu64 " states taken, a state applied %.3f ms, %.1f ticks predicted again a state\n",
				 unsigned( slot ), delay, st.physicsSteps, st.physicsSteps > 0 ? double( st.physicsNs ) / 1e6 / double( st.physicsSteps ) : 0.0,
				 st.checksums, st.desyncs, client.DesyncTick() == UINT32_MAX ? -1 : int( client.DesyncTick() ), st.states,
				 st.states > 0 ? double( st.applyNs ) / 1e6 / double( st.states ) : 0.0,
				 st.states > 0 ? double( st.resimulatedTicks ) / double( st.states ) : 0.0 );
	std::printf( "its body     predicted within 5 cm of the server's %u of %u ticks; %.3f m on average, %.3f at most\n", nearby, compared,
				 compared > 0 ? sum / float( compared ) : 0.0f, worst );
	return framesWrong == 0 && undecoded == 0 && st.rejected == 0 && st.desyncs == 0 ? 0 : 2;
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
	const bool state = argc >= 3 && std::strcmp( argv[1], "hybrid" ) == 0;
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
		return HybridModel( replay, slot, delay );
	}
	return 0;
}
