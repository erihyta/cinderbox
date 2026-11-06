#include "state_codec.h"

#include "protocol.h"

#if defined( __GNUC__ )
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "miniz.h"
#if defined( __GNUC__ )
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace cb::net
{

namespace
{

using Span = StateSpan;
using Parsed = StateLayout;

bool Parse( const std::vector<uint8_t>& state, Parsed& out )
{
	return ReadLayout( state, out );
}

void PutVarint( std::vector<uint8_t>& out, uint32_t v )
{
	while ( v >= 0x80 )
	{
		out.push_back( uint8_t( v | 0x80 ) );
		v >>= 7;
	}
	out.push_back( uint8_t( v ) );
}

uint32_t ReadVarint( ByteReader& r )
{
	uint32_t v = 0;
	for ( int shift = 0; shift < 35; shift += 7 )
	{
		const uint8_t b = r.Read<uint8_t>();
		v |= uint32_t( b & 0x7F ) << shift;
		if ( ( b & 0x80 ) == 0 || r.Ok() == false )
		{
			return v;
		}
	}
	r.Fail();
	return 0;
}

template <typename T>
void Put( std::vector<uint8_t>& out, const T& value )
{
	const auto* p = reinterpret_cast<const uint8_t*>( &value );
	out.insert( out.end(), p, p + sizeof( T ) );
}

void PutBytes( std::vector<uint8_t>& out, const uint8_t* p, size_t n )
{
	out.insert( out.end(), p, p + n );
}

// `now` XOR `base` (the same size).
void PutXor( std::vector<uint8_t>& out, const uint8_t* now, const uint8_t* base, size_t n )
{
	const size_t at = out.size();
	out.resize( at + n );
	for ( size_t i = 0; i < n; ++i )
	{
		out[at + i] = uint8_t( now[i] ^ base[i] );
	}
}

// `now` against `base` (the same size; null: none), sparse: runs of (unchanged bytes skipped, bytes
// changed and their XOR), as varints and bytes, until all `n` are said. Unchanged runs shorter
// than a few bytes stay in the changed ones.
void PutSparse( std::vector<uint8_t>& out, const uint8_t* now, const uint8_t* base, size_t n )
{
	size_t i = 0;
	while ( i < n )
	{
		size_t skip = 0;
		while ( i + skip < n && base != nullptr && now[i + skip] == base[i + skip] )
		{
			++skip;
		}
		size_t end = i + skip;
		// The changed run: until a run of 4 unchanged bytes (or the end).
		size_t run = end;
		while ( run < n )
		{
			size_t same = 0;
			while ( run + same < n && base != nullptr && now[run + same] == base[run + same] && same < 4 )
			{
				++same;
			}
			if ( same >= 4 || run + same >= n )
			{
				break;
			}
			run += same + 1;
		}
		run = std::min( run, n );
		PutVarint( out, uint32_t( skip ) );
		PutVarint( out, uint32_t( run - end ) );
		for ( size_t k = end; k < run; ++k )
		{
			out.push_back( uint8_t( now[k] ^ ( base != nullptr ? base[k] : 0 ) ) );
		}
		i = run;
	}
}

// An entity's flags: how its record goes, and its bodies.
enum : uint8_t
{
	RecordSame = 0,
	RecordXor = 1,
	RecordWhole = 2,
	RecordMask = 3,
	HasBodies = 4,
	BodiesSame = 8,	 // the base's entity's
	BodiesKnown = 16, // another entity's, of the base or met before in this state: by its hash
	HasPuppet = 32,	 // its puppet follows (M253)
	PuppetBase = 64, // as a change against the base's puppet of it (else against none)
	// M257: against its record (and bodies) as the client has it in its newest state, not a base's:
	// one that waited longer than a base is kept, sent nothing since the client acknowledged one.
	// Its puppet whole.
	RecordCarried = 128,
};

// An event's fields (M257), those not zero (or, for A, not the character it is at).
enum : uint8_t
{
	EventA = 1,
	EventB = 2,
	EventValue = 4,
	EventPoint = 8,
	EventVector = 16,
	EventLate = 32,
};
// An event's point (M257), to 1/64 m: where something happened is all a client shows it by. As a
// change from the point before it in the message (a character's events are near one another).
constexpr float kEventPointScale = 64.0f;

uint32_t ZigZag( int32_t v )
{
	return ( uint32_t( v ) << 1 ) ^ uint32_t( v >> 31 );
}
int32_t UnZigZag( uint32_t v )
{
	return int32_t( v >> 1 ) ^ -int32_t( v & 1 );
}

// What changed in a puppet, a bit each (EncodePuppet).
enum : uint32_t
{
	PuppetPlace = 1,
	PuppetFacing = 2,
	PuppetAim = 4,
	PuppetFlagsChanged = 8,
	PuppetSlot = 16,
	PuppetJump = 32,
	PuppetAir = 64,
	PuppetStep = 128,
	PuppetMotions = 256,
	PuppetStances = 512,
	PuppetLayersChanged = 1024,
	PuppetAll = 2047,
};

// A tick before `tick` as how far back it is (0: none, the tick 0; one not before it: 1 back).
uint32_t Back( uint32_t tick, uint32_t at )
{
	return at == 0 ? 0 : tick > at ? tick - at : 1;
}
uint32_t FromBack( uint32_t tick, uint32_t back )
{
	return back == 0 || back > tick ? 0 : tick - back;
}

// The bodies' descriptions either side knows, by hash: the base's, then this state's so far.
struct Known
{
	std::vector<std::pair<uint64_t, const Span*>> byHash;
	void Add( const Span* span )
	{
		if ( span->bodiesSize > 0 )
		{
			byHash.push_back( { span->hash, span } );
		}
	}
	void Sort()
	{
		std::sort( byHash.begin(), byHash.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
	}
	const Span* Find( uint64_t hash ) const
	{
		auto it = std::lower_bound( byHash.begin(), byHash.end(), hash, []( const auto& a, uint64_t h ) { return a.first < h; } );
		return it != byHash.end() && it->first == hash ? it->second : nullptr;
	}
};

const uint8_t* Bytes( const std::array<PlayerInput, kMaxPlayers>& inputs )
{
	return reinterpret_cast<const uint8_t*>( inputs.data() );
}
constexpr size_t kInputBytes = sizeof( PlayerInput ) * kMaxPlayers;

// The message around a payload: its type, tick, base, size, and the payload deflated.
// A size's top bit: the payload as it is, not deflated (updates: sparse already).
constexpr uint32_t kStored = 0x80000000u;

void Pack( uint32_t tick, uint32_t baseTick, const std::vector<uint8_t>& payload, std::vector<uint8_t>& out, bool deflate = true )
{
	if ( deflate == false )
	{
		out.clear();
		out.reserve( 13 + payload.size() );
		out.push_back( uint8_t( MsgType::State ) );
		Put( out, tick );
		Put( out, baseTick );
		Put( out, uint32_t( payload.size() ) | kStored );
		PutBytes( out, payload.data(), payload.size() );
		return;
	}
	mz_ulong packed = mz_compressBound( mz_ulong( payload.size() ) );
	out.clear();
	out.reserve( 13 + packed );
	out.push_back( uint8_t( MsgType::State ) );
	Put( out, tick );
	Put( out, baseTick );
	Put( out, uint32_t( payload.size() ) );
	const size_t at = out.size();
	out.resize( at + packed );
	if ( mz_compress2( out.data() + at, &packed, payload.data(), mz_ulong( payload.size() ), 1 ) != MZ_OK )
	{
		out.clear();
		return;
	}
	out.resize( at + packed );
}

// The message: `frame`'s inputs and `now`'s head with `entities`, against `base` (its inputs, the
// head of `was` and `baseEntities`) when there is one.
void EncodeParts( const StateFrame& frame, const Parsed& now, const std::vector<Span>& entities, const StateFrame* base, const Parsed* was,
				  const std::vector<Span>* baseEntities, std::vector<uint8_t>& out )
{
	const bool against = base != nullptr && was != nullptr && baseEntities != nullptr && was->headSize == now.headSize;
	std::vector<uint8_t> payload;
	payload.reserve( 4096 );
	payload.push_back( 1 );
	if ( against )
	{
		PutXor( payload, Bytes( frame.inputs ), Bytes( base->inputs ), kInputBytes );
	}
	else
	{
		PutBytes( payload, Bytes( frame.inputs ), kInputBytes );
	}
	Put( payload, now.magic );
	Put( payload, now.headSize );
	if ( against )
	{
		PutXor( payload, now.head, was->head, now.headSize );
	}
	else
	{
		PutBytes( payload, now.head, now.headSize );
	}
	PutVarint( payload, uint32_t( entities.size() ) );
	Known known;
	if ( against )
	{
		for ( const Span& e : *baseEntities )
		{
			known.Add( &e );
		}
		known.Sort();
	}
	std::unordered_map<uint64_t, uint32_t> met; // hash -> its size, of this state so far
	size_t b = 0;
	uint32_t previous = 0;
	for ( const Span& e : entities )
	{
		const Span* old = nullptr;
		if ( against )
		{
			while ( b < baseEntities->size() && ( *baseEntities )[b].netId < e.netId )
			{
				++b;
			}
			old = b < baseEntities->size() && ( *baseEntities )[b].netId == e.netId ? &( *baseEntities )[b] : nullptr;
		}
		uint8_t flags = RecordWhole;
		if ( old != nullptr && old->recordSize == e.recordSize )
		{
			flags = old->record == e.record || std::memcmp( old->record, e.record, e.recordSize ) == 0 ? RecordSame : RecordXor;
		}
		if ( e.bodiesSize > 0 )
		{
			flags |= HasBodies;
			if ( old != nullptr && old->bodiesSize == e.bodiesSize && old->hash == e.hash )
			{
				flags |= BodiesSame;
			}
			else if ( const Span* k = known.Find( e.hash ); k != nullptr && k->bodiesSize == e.bodiesSize )
			{
				flags |= BodiesKnown;
			}
			else if ( auto it = met.find( e.hash ); it != met.end() && it->second == e.bodiesSize )
			{
				flags |= BodiesKnown;
			}
			met.emplace( e.hash, e.bodiesSize );
		}
		PutVarint( payload, e.netId - previous );
		previous = e.netId;
		payload.push_back( flags );
		switch ( flags & RecordMask )
		{
			case RecordXor:
				PutXor( payload, e.record, old->record, e.recordSize );
				break;
			case RecordWhole:
				PutVarint( payload, e.recordSize );
				PutBytes( payload, e.record, e.recordSize );
				break;
			default:
				break;
		}
		if ( flags & BodiesKnown )
		{
			Put( payload, e.hash );
		}
		else if ( ( flags & HasBodies ) && ( flags & BodiesSame ) == 0 )
		{
			PutVarint( payload, e.bodiesSize );
			Put( payload, e.hash );
			PutBytes( payload, e.bodies, e.bodiesSize );
		}
	}
	Pack( frame.tick, against ? base->tick : kNoStateBase, payload, out );
}

} // namespace

void EncodePuppet( const Puppet& now, const Puppet* base, uint32_t tick, std::vector<uint8_t>& out )
{
	const Puppet none;
	const Puppet& was = base != nullptr ? *base : none;
	const size_t layers = std::min<size_t>( now.layerCount, kPuppetLayers );
	uint32_t changed = 0;
	changed |= now.x != was.x || now.y != was.y || now.z != was.z ? PuppetPlace : 0;
	changed |= now.facing != was.facing ? PuppetFacing : 0;
	changed |= now.aimYaw != was.aimYaw || now.aimPitch != was.aimPitch ? PuppetAim : 0;
	changed |= now.flags != was.flags ? PuppetFlagsChanged : 0;
	changed |= now.slot != was.slot ? PuppetSlot : 0;
	changed |= now.lastJumpTick != was.lastJumpTick ? PuppetJump : 0;
	changed |= now.airSince != was.airSince ? PuppetAir : 0;
	changed |= now.stepCount != was.stepCount ? PuppetStep : 0;
	changed |= now.motionsOn != was.motionsOn ? PuppetMotions : 0;
	changed |= now.layerCount != was.layerCount || std::memcmp( now.stances, was.stances, layers ) != 0 ? PuppetStances : 0;
	uint32_t layerMask = 0;
	for ( size_t l = 0; l < layers; ++l )
	{
		layerMask |= now.layers[l] == was.layers[l] ? 0 : uint32_t( 1 ) << l;
	}
	changed |= layerMask != 0 || now.layerCount != was.layerCount ? PuppetLayersChanged : 0;
	PutVarint( out, changed );
	if ( changed & PuppetPlace )
	{
		// The axes that moved, then each one's change.
		const int32_t d[3] = { int32_t( uint32_t( now.x ) - uint32_t( was.x ) ), int32_t( uint32_t( now.y ) - uint32_t( was.y ) ),
							   int32_t( uint32_t( now.z ) - uint32_t( was.z ) ) };
		out.push_back( uint8_t( ( d[0] != 0 ? 1 : 0 ) | ( d[1] != 0 ? 2 : 0 ) | ( d[2] != 0 ? 4 : 0 ) ) );
		for ( int32_t v : d )
		{
			if ( v != 0 )
			{
				PutVarint( out, ZigZag( v ) );
			}
		}
	}
	if ( changed & PuppetFacing )
	{
		PutVarint( out, ZigZag( int16_t( uint16_t( now.facing - was.facing ) ) ) );
	}
	if ( changed & PuppetAim )
	{
		out.push_back( uint8_t( now.aimYaw ) );
		out.push_back( uint8_t( now.aimPitch ) );
	}
	if ( changed & PuppetFlagsChanged )
	{
		out.push_back( now.flags );
	}
	if ( changed & PuppetSlot )
	{
		PutVarint( out, now.slot );
	}
	if ( changed & PuppetJump )
	{
		PutVarint( out, Back( tick, now.lastJumpTick ) );
	}
	if ( changed & PuppetAir )
	{
		PutVarint( out, Back( tick, now.airSince ) );
	}
	if ( changed & PuppetStep )
	{
		PutVarint( out, ZigZag( int32_t( now.stepCount - was.stepCount ) ) );
	}
	if ( changed & PuppetMotions )
	{
		PutVarint( out, uint32_t( now.motionsOn ) );
		PutVarint( out, uint32_t( now.motionsOn >> 32 ) );
	}
	if ( changed & PuppetStances )
	{
		out.push_back( uint8_t( layers ) );
		PutBytes( out, now.stances, layers );
	}
	if ( changed & PuppetLayersChanged )
	{
		out.push_back( uint8_t( layers ) );
		PutVarint( out, layerMask );
		for ( size_t l = 0; l < layers; ++l )
		{
			if ( layerMask & ( uint32_t( 1 ) << l ) )
			{
				const PuppetLayer& L = now.layers[l];
				out.push_back( L.state );
				out.push_back( L.previous );
				out.push_back( L.via );
				out.push_back( L.source );
				PutVarint( out, Back( tick, L.start ) );
			}
		}
	}
}

bool DecodePuppet( ByteReader& r, const Puppet* base, uint32_t tick, Puppet& out )
{
	out = base != nullptr ? *base : Puppet{};
	const uint32_t changed = ReadVarint( r );
	if ( r.Ok() == false || ( changed & ~PuppetAll ) != 0 )
	{
		return false;
	}
	if ( changed & PuppetPlace )
	{
		const uint8_t axes = r.Read<uint8_t>();
		int32_t* at[3] = { &out.x, &out.y, &out.z };
		for ( int i = 0; i < 3; ++i )
		{
			if ( axes & ( 1 << i ) )
			{
				*at[i] = int32_t( uint32_t( *at[i] ) + uint32_t( UnZigZag( ReadVarint( r ) ) ) );
			}
		}
	}
	if ( changed & PuppetFacing )
	{
		out.facing = uint16_t( out.facing + uint16_t( int16_t( UnZigZag( ReadVarint( r ) ) ) ) );
	}
	if ( changed & PuppetAim )
	{
		out.aimYaw = int8_t( r.Read<uint8_t>() );
		out.aimPitch = int8_t( r.Read<uint8_t>() );
	}
	if ( changed & PuppetFlagsChanged )
	{
		out.flags = r.Read<uint8_t>();
	}
	if ( changed & PuppetSlot )
	{
		out.slot = PlayerSlot( ReadVarint( r ) );
	}
	if ( changed & PuppetJump )
	{
		out.lastJumpTick = FromBack( tick, ReadVarint( r ) );
	}
	if ( changed & PuppetAir )
	{
		out.airSince = FromBack( tick, ReadVarint( r ) );
	}
	if ( changed & PuppetStep )
	{
		out.stepCount = uint32_t( int64_t( out.stepCount ) + UnZigZag( ReadVarint( r ) ) );
	}
	if ( changed & PuppetMotions )
	{
		const uint64_t lo = ReadVarint( r );
		const uint64_t hi = ReadVarint( r );
		out.motionsOn = lo | ( hi << 32 );
	}
	if ( changed & PuppetStances )
	{
		const uint8_t n = r.Read<uint8_t>();
		const uint8_t* bytes = n <= kPuppetLayers ? r.Take( n ) : nullptr;
		if ( bytes == nullptr )
		{
			return false;
		}
		std::memcpy( out.stances, bytes, n );
		std::memset( out.stances + n, 0, kPuppetLayers - n );
		out.layerCount = n;
	}
	if ( changed & PuppetLayersChanged )
	{
		const uint8_t n = r.Read<uint8_t>();
		const uint32_t mask = ReadVarint( r );
		if ( r.Ok() == false || n > kPuppetLayers || ( n < 32 && ( mask >> n ) != 0 ) )
		{
			return false;
		}
		out.layerCount = n;
		for ( size_t l = 0; l < n; ++l )
		{
			if ( mask & ( uint32_t( 1 ) << l ) )
			{
				PuppetLayer& L = out.layers[l];
				L.state = r.Read<uint8_t>();
				L.previous = r.Read<uint8_t>();
				L.via = r.Read<uint8_t>();
				L.source = r.Read<uint8_t>();
				L.start = FromBack( tick, ReadVarint( r ) );
			}
		}
		// (Past its count a puppet says nothing: as a new one has them.)
		for ( size_t l = n; l < kPuppetLayers; ++l )
		{
			out.layers[l] = PuppetLayer{};
		}
	}
	return r.Ok();
}

bool ReadLayout( const std::vector<uint8_t>& state, StateLayout& out )
{
	ByteReader r( state.data(), state.size() );
	out.magic = r.Read<uint32_t>();
	out.headSize = r.Read<uint32_t>();
	out.head = r.Take( out.headSize );
	const uint32_t count = r.Read<uint32_t>();
	if ( r.Ok() == false || count > state.size() / 12 )
	{
		return false;
	}
	out.entities.resize( count );
	for ( StateSpan& s : out.entities )
	{
		s.netId = r.Read<uint32_t>();
		s.recordSize = r.Read<uint32_t>();
		s.record = r.Take( s.recordSize );
		s.bodiesSize = r.Read<uint32_t>();
		s.bodies = nullptr;
		s.hash = 0;
		if ( s.bodiesSize > 0 )
		{
			s.hash = r.Read<uint64_t>();
			s.bodies = r.Take( s.bodiesSize );
		}
	}
	return r.Ok() && r.AtEnd();
}

const StateSpan* StateLayout::Find( uint32_t netId ) const
{
	auto it = std::lower_bound( entities.begin(), entities.end(), netId, []( const StateSpan& s, uint32_t id ) { return s.netId < id; } );
	return it != entities.end() && it->netId == netId ? &*it : nullptr;
}

void EncodeState( const StateFrame& frame, const StateFrame* base, std::vector<uint8_t>& out )
{
	Parsed now;
	Parsed was;
	if ( Parse( frame.state, now ) == false )
	{
		// (Not a state's layout: sent as it is, never against a base.)
		std::vector<uint8_t> payload;
		payload.push_back( 0 );
		PutBytes( payload, frame.state.data(), frame.state.size() );
		Pack( frame.tick, kNoStateBase, payload, out );
		return;
	}
	const bool against = base != nullptr && Parse( base->state, was );
	EncodeParts( frame, now, now.entities, against ? base : nullptr, against ? &was : nullptr, against ? &was.entities : nullptr, out );
}

bool EncodeUpdates( const StateFrame& frame, const StateFrame* base, const std::vector<EntityUpdate>& updates, const std::vector<uint32_t>& removed,
					const std::vector<PlayerSlot>& inputSlots, LayoutOf layoutOf, const void* context, std::vector<uint8_t>& out,
					UpdateCache* cache, std::vector<uint32_t>* sizes, const std::vector<CharacterEvent>* events, size_t* unpacked )
{
	if ( sizes != nullptr )
	{
		sizes->clear();
	}
	const Parsed* now = layoutOf( context, frame.tick );
	const Parsed* was = base != nullptr ? layoutOf( context, base->tick ) : nullptr;
	if ( now == nullptr || ( base != nullptr && ( was == nullptr || was->headSize != now->headSize ) ) )
	{
		return false;
	}
	if ( cache != nullptr && cache->tick != frame.tick )
	{
		cache->parts.clear();
		cache->tick = frame.tick;
	}
	std::vector<uint8_t> payload;
	payload.reserve( 4096 );
	payload.push_back( 2 );
	PutVarint( payload, uint32_t( inputSlots.size() ) );
	for ( PlayerSlot slot : inputSlots )
	{
		const auto* in = reinterpret_cast<const uint8_t*>( &frame.inputs[slot] );
		PutVarint( payload, slot ); // (two bytes past 127: M252)
		PutSparse( payload, in, base != nullptr ? reinterpret_cast<const uint8_t*>( &base->inputs[slot] ) : nullptr, sizeof( PlayerInput ) );
	}
	Put( payload, now->magic );
	Put( payload, now->headSize );
	PutSparse( payload, now->head, base != nullptr ? was->head : nullptr, now->headSize );
	PutVarint( payload, uint32_t( removed.size() ) );
	uint32_t previous = 0;
	for ( uint32_t netId : removed )
	{
		PutVarint( payload, netId - previous );
		previous = netId;
	}
	PutVarint( payload, uint32_t( updates.size() ) );
	previous = 0;
	std::vector<uint8_t> part;
	for ( const EntityUpdate& u : updates )
	{
		PutVarint( payload, u.netId - previous );
		previous = u.netId;
		// Its part: flags, how far back its base is, its record and bodies (and its puppet).
		const uint64_t key = ( uint64_t( u.netId ) << 32 ) | ( u.carried != nullptr ? u.version : u.baseTick ) | ( u.puppet ? uint64_t( 1 ) << 63 : 0 ) |
							 ( u.carried != nullptr ? uint64_t( 1 ) << 62 : 0 );
		if ( cache != nullptr )
		{
			if ( auto it = cache->parts.find( key ); it != cache->parts.end() )
			{
				PutBytes( payload, it->second.data(), it->second.size() );
				if ( sizes != nullptr )
				{
					sizes->push_back( uint32_t( it->second.size() ) );
				}
				continue;
			}
		}
		const Span* e = now->Find( u.netId );
		const Parsed* from = u.baseTick != 0 ? layoutOf( context, u.baseTick ) : nullptr;
		// (Carried: against the record the client has in its newest state, M257.)
		const bool carried = u.carried != nullptr;
		const Span* old = carried ? u.carried : from != nullptr ? from->Find( u.netId ) : nullptr;
		if ( e == nullptr || ( u.baseTick != 0 && old == nullptr ) || ( carried && u.baseTick != 0 ) )
		{
			return false;
		}
		// A puppet's client is sent the view of its record (M253), against the view it has.
		const uint8_t* record = u.puppet ? e->view : e->record;
		const uint32_t recordSize = u.puppet ? e->viewSize : e->recordSize;
		const uint8_t* oldRecord = old == nullptr ? nullptr : u.puppet ? old->view : old->record;
		const uint32_t oldSize = old == nullptr ? 0 : u.puppet ? old->viewSize : old->recordSize;
		if ( record == nullptr || ( old != nullptr && oldRecord == nullptr ) )
		{
			return false;
		}
		part.clear();
		uint8_t flags = RecordWhole;
		if ( old != nullptr && oldSize == recordSize )
		{
			flags = std::memcmp( oldRecord, record, recordSize ) == 0 ? RecordSame : RecordXor;
		}
		const bool bodiesSame = old != nullptr && e->bodiesSize > 0 && old->bodiesSize == e->bodiesSize && old->hash == e->hash;
		flags |= e->bodiesSize > 0 ? HasBodies : 0;
		flags |= bodiesSame ? BodiesSame : 0;
		const bool puppet = u.puppet && e->puppet != nullptr;
		const bool puppetBase = puppet && old != nullptr && old->puppet != nullptr && carried == false;
		flags |= puppet ? HasPuppet : 0;
		flags |= puppetBase ? PuppetBase : 0;
		flags |= carried ? RecordCarried : 0;
		part.push_back( flags );
		PutVarint( part, old != nullptr && carried == false ? frame.tick - u.baseTick : 0 );
		switch ( flags & RecordMask )
		{
			case RecordXor:
				PutSparse( part, record, oldRecord, recordSize );
				break;
			case RecordWhole:
				PutVarint( part, recordSize );
				PutBytes( part, record, recordSize );
				break;
			default:
				break;
		}
		if ( e->bodiesSize > 0 && bodiesSame == false )
		{
			PutVarint( part, e->bodiesSize );
			Put( part, e->hash );
			PutBytes( part, e->bodies, e->bodiesSize );
		}
		if ( puppet )
		{
			EncodePuppet( *e->puppet, puppetBase ? old->puppet : nullptr, frame.tick, part );
		}
		PutBytes( payload, part.data(), part.size() );
		if ( sizes != nullptr )
		{
			sizes->push_back( uint32_t( part.size() ) );
		}
		if ( cache != nullptr )
		{
			cache->parts.emplace( key, part );
		}
	}
	// The events at characters (M257): how many characters; each its NetId and count, and each event
	// its fields, type, how many ticks before the state, and what of it is not zero.
	const std::vector<CharacterEvent> none;
	const std::vector<CharacterEvent>& said = events != nullptr ? *events : none;
	uint32_t characters = 0;
	for ( size_t i = 0; i < said.size(); ++i )
	{
		characters += i == 0 || said[i].character != said[i - 1].character ? 1 : 0;
	}
	PutVarint( payload, characters );
	previous = 0;
	uint32_t serial = 0;
	int32_t pointWas[3] = {};
	auto zero = []( const b3Vec3& v ) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; };
	for ( size_t i = 0; i < said.size(); )
	{
		const uint32_t character = said[i].character;
		size_t end = i;
		while ( end < said.size() && said[end].character == character )
		{
			++end;
		}
		if ( character <= previous && i > 0 )
		{
			return false; // (not in NetId order)
		}
		PutVarint( payload, character - previous );
		previous = character;
		PutVarint( payload, uint32_t( end - i ) );
		for ( ; i < end; ++i )
		{
			const ModEventRecord& e = said[i].record;
			uint8_t fields = 0;
			fields |= e.netIdA != character ? EventA : 0;
			fields |= e.netIdB != 0 ? EventB : 0;
			fields |= e.value != 0 ? EventValue : 0;
			fields |= zero( e.point ) ? 0 : EventPoint;
			fields |= zero( e.vector ) ? 0 : EventVector;
			fields |= e.late != 0 ? EventLate : 0;
			payload.push_back( fields );
			PutVarint( payload, ZigZag( int32_t( said[i].serial - serial ) ) );
			serial = said[i].serial;
			PutVarint( payload, e.type );
			PutVarint( payload, frame.tick > e.tick ? frame.tick - e.tick : 0 );
			if ( fields & EventA )
			{
				PutVarint( payload, e.netIdA );
			}
			if ( fields & EventB )
			{
				PutVarint( payload, e.netIdB );
			}
			if ( fields & EventValue )
			{
				PutVarint( payload, ZigZag( e.value ) );
			}
			if ( fields & EventPoint )
			{
				const float at[3] = { e.point.x, e.point.y, e.point.z };
				for ( int a = 0; a < 3; ++a )
				{
					const float scaled = std::clamp( at[a] * kEventPointScale, -1e9f, 1e9f );
					const int32_t q = int32_t( std::lround( std::isfinite( scaled ) ? scaled : 0.0f ) );
					PutVarint( payload, ZigZag( q - pointWas[a] ) );
					pointWas[a] = q;
				}
			}
			if ( fields & EventVector )
			{
				Put( payload, e.vector );
			}
		}
	}
	if ( unpacked != nullptr )
	{
		*unpacked = payload.size();
	}
	Pack( frame.tick, base != nullptr ? base->tick : kNoStateBase, payload, out );
	return true;
}

bool ReadStateHeader( ByteReader& r, uint32_t& tick, uint32_t& baseTick )
{
	tick = r.Read<uint32_t>();
	baseTick = r.Read<uint32_t>();
	return r.Ok();
}

bool DecodeState( ByteReader& r, uint32_t tick, const StateFrame* base, StateFrame& out )
{
	return DecodeState( r, tick, base, nullptr, nullptr, nullptr, out );
}

namespace
{

// Updates (payload kind 2) on `previous`.
bool DecodeUpdates( ByteReader& p, uint32_t tick, const StateFrame* base, const StateFrame* previous, StateOf stateOf, const void* context,
					StateFrame& out )
{
	// PutSparse's runs: `n` bytes into `to`, against `old` (null: zeros).
	auto takeXor = [&]( const uint8_t* old, size_t n, uint8_t* to ) {
		size_t i = 0;
		while ( i < n )
		{
			const uint32_t skip = ReadVarint( p );
			const uint32_t run = ReadVarint( p );
			if ( p.Ok() == false || skip > n - i || run > n - i - skip || ( skip == 0 && run == 0 ) )
			{
				return false;
			}
			for ( uint32_t k = 0; k < skip; ++k, ++i )
			{
				to[i] = old != nullptr ? old[i] : 0;
			}
			const uint8_t* d = p.Take( run );
			if ( d == nullptr )
			{
				return false;
			}
			for ( uint32_t k = 0; k < run; ++k, ++i )
			{
				to[i] = uint8_t( d[k] ^ ( old != nullptr ? old[i] : 0 ) );
			}
		}
		return true;
	};
	Parsed was;
	if ( base != nullptr && Parse( base->state, was ) == false )
	{
		return false;
	}
	// The inputs: those sent, the rest as the newest state had them.
	out.inputs = previous != nullptr ? previous->inputs : std::array<PlayerInput, kMaxPlayers>{};
	const uint32_t inputCount = ReadVarint( p );
	if ( p.Ok() == false || inputCount > uint32_t( kMaxPlayers ) )
	{
		return false;
	}
	for ( uint32_t i = 0; i < inputCount; ++i )
	{
		const uint32_t slot = ReadVarint( p );
		if ( p.Ok() == false || slot >= kMaxPlayers ||
			 takeXor( base != nullptr ? reinterpret_cast<const uint8_t*>( &base->inputs[slot] ) : nullptr, sizeof( PlayerInput ),
					  reinterpret_cast<uint8_t*>( &out.inputs[slot] ) ) == false )
		{
			return false;
		}
	}
	const uint32_t magic = p.Read<uint32_t>();
	const uint32_t headSize = p.Read<uint32_t>();
	if ( p.Ok() == false || headSize > kMaxStateBytes || ( base != nullptr && headSize != was.headSize ) )
	{
		return false;
	}
	std::vector<uint8_t> head( headSize );
	if ( takeXor( base != nullptr ? was.head : nullptr, headSize, head.data() ) == false )
	{
		return false;
	}
	// What it had: the newest state's entities, but those named gone.
	Parsed had;
	if ( previous != nullptr && Parse( previous->state, had ) == false )
	{
		return false;
	}
	const uint32_t removedCount = ReadVarint( p );
	if ( p.Ok() == false || removedCount > kMaxStateBytes / 4 )
	{
		return false;
	}
	std::vector<uint32_t> removed( removedCount );
	uint32_t netId = 0;
	for ( uint32_t& id : removed )
	{
		netId += ReadVarint( p );
		id = netId;
	}
	const uint32_t count = ReadVarint( p );
	if ( p.Ok() == false || count > kMaxStateBytes / 2 )
	{
		return false;
	}
	// The entities sent: each decoded against its base, the client's state of that tick.
	struct Decoded
	{
		uint32_t netId = 0;
		std::vector<uint8_t> record;
		const uint8_t* bodies = nullptr; // (into the message's payload or a base's state)
		uint32_t bodiesSize = 0;
		uint64_t hash = 0;
		bool hasPuppet = false;
		Puppet puppet;
	};
	// A state's puppet of an entity (null: none).
	auto puppetIn = []( const StateFrame* f, uint32_t id ) -> const PuppetEntry* {
		if ( f == nullptr )
		{
			return nullptr;
		}
		auto it = std::lower_bound( f->puppets.begin(), f->puppets.end(), id, []( const PuppetEntry& e, uint32_t n ) { return e.netId < n; } );
		return it != f->puppets.end() && it->netId == id ? &*it : nullptr;
	};
	std::vector<Decoded> sent( count );
	struct Base
	{
		uint32_t tick = 0;
		const StateFrame* frame = nullptr;
		Parsed layout;
	};
	std::vector<Base> bases; // (read once each)
	netId = 0;
	for ( Decoded& d : sent )
	{
		netId += ReadVarint( p );
		d.netId = netId;
		const uint8_t flags = p.Read<uint8_t>();
		const uint32_t back = ReadVarint( p );
		if ( p.Ok() == false )
		{
			return false;
		}
		const Span* old = nullptr;
		const StateFrame* oldFrame = nullptr;
		if ( flags & RecordCarried )
		{
			// (What it has in its newest state.)
			old = had.Find( netId );
			if ( back != 0 || old == nullptr || ( flags & PuppetBase ) != 0 )
			{
				return false;
			}
		}
		else if ( back != 0 )
		{
			if ( back > tick || stateOf == nullptr )
			{
				return false;
			}
			const uint32_t at = tick - back;
			const Base* found = nullptr;
			for ( const Base& b : bases )
			{
				found = b.tick == at ? &b : found;
			}
			if ( found == nullptr )
			{
				const StateFrame* f = stateOf( context, at );
				if ( f == nullptr )
				{
					return false;
				}
				bases.push_back( { at, f, {} } );
				if ( Parse( f->state, bases.back().layout ) == false )
				{
					return false;
				}
				found = &bases.back();
			}
			old = found->layout.Find( netId );
			oldFrame = found->frame;
			if ( old == nullptr )
			{
				return false;
			}
		}
		switch ( flags & RecordMask )
		{
			case RecordSame:
			case RecordXor:
				if ( old == nullptr )
				{
					return false;
				}
				d.record.resize( old->recordSize );
				if ( ( flags & RecordMask ) == RecordSame )
				{
					std::memcpy( d.record.data(), old->record, old->recordSize );
				}
				else if ( takeXor( old->record, old->recordSize, d.record.data() ) == false )
				{
					return false;
				}
				break;
			case RecordWhole:
			{
				const uint32_t size = ReadVarint( p );
				const uint8_t* record = p.Ok() && size <= kMaxStateBytes ? p.Take( size ) : nullptr;
				if ( record == nullptr )
				{
					return false;
				}
				d.record.assign( record, record + size );
				break;
			}
			default:
				return false;
		}
		if ( flags & HasBodies )
		{
			if ( flags & BodiesSame )
			{
				if ( old == nullptr || old->bodiesSize == 0 )
				{
					return false;
				}
				d.bodies = old->bodies;
				d.bodiesSize = old->bodiesSize;
				d.hash = old->hash;
			}
			else
			{
				d.bodiesSize = ReadVarint( p );
				d.hash = p.Read<uint64_t>();
				d.bodies = p.Ok() && d.bodiesSize > 0 && d.bodiesSize <= kMaxStateBytes ? p.Take( d.bodiesSize ) : nullptr;
				if ( d.bodies == nullptr )
				{
					return false;
				}
			}
		}
		if ( flags & HasPuppet )
		{
			// (Against the base's puppet of it: the one it was sent there.)
			const PuppetEntry* was = ( flags & PuppetBase ) ? puppetIn( oldFrame, netId ) : nullptr;
			if ( ( ( flags & PuppetBase ) && was == nullptr ) || DecodePuppet( p, was != nullptr ? &was->puppet : nullptr, tick, d.puppet ) == false )
			{
				return false;
			}
			d.hasPuppet = true;
		}
		else if ( flags & PuppetBase )
		{
			return false;
		}
	}
	// The events at characters (M257).
	out.events.clear();
	const uint32_t characters = ReadVarint( p );
	if ( p.Ok() == false || characters > kMaxStateBytes / 4 )
	{
		return false;
	}
	uint32_t character = 0, serial = 0;
	int32_t pointWas[3] = {};
	for ( uint32_t c = 0; c < characters; ++c )
	{
		const uint32_t step = ReadVarint( p );
		const uint32_t n = ReadVarint( p );
		if ( p.Ok() == false || ( c > 0 && step == 0 ) || n == 0 || n > kMaxStateBytes / 4 )
		{
			return false;
		}
		character += step;
		for ( uint32_t k = 0; k < n; ++k )
		{
			CharacterEvent said;
			said.character = character;
			ModEventRecord& e = said.record;
			const uint8_t fields = p.Read<uint8_t>();
			serial += uint32_t( UnZigZag( ReadVarint( p ) ) );
			said.serial = serial;
			const uint32_t type = ReadVarint( p );
			const uint32_t back = ReadVarint( p );
			if ( p.Ok() == false || type > 0xFFFF || back > tick )
			{
				return false;
			}
			e.type = uint16_t( type );
			e.tick = tick - back;
			e.late = ( fields & EventLate ) != 0 ? 1 : 0;
			e.netIdA = ( fields & EventA ) != 0 ? ReadVarint( p ) : character;
			e.netIdB = ( fields & EventB ) != 0 ? ReadVarint( p ) : 0;
			e.value = ( fields & EventValue ) != 0 ? UnZigZag( ReadVarint( p ) ) : 0;
			if ( fields & EventPoint )
			{
				float at[3];
				for ( int a = 0; a < 3; ++a )
				{
					pointWas[a] += UnZigZag( ReadVarint( p ) );
					at[a] = float( pointWas[a] ) / kEventPointScale;
				}
				e.point = { at[0], at[1], at[2] };
			}
			if ( fields & EventVector )
			{
				e.vector = p.Read<b3Vec3>();
			}
			if ( p.Ok() == false )
			{
				return false;
			}
			out.events.push_back( said );
		}
	}
	if ( p.Ok() == false || p.AtEnd() == false )
	{
		return false;
	}
	for ( size_t i = 1; i < sent.size(); ++i )
	{
		if ( sent[i].netId <= sent[i - 1].netId )
		{
			return false;
		}
	}

	// The state: the head, then by NetId what it had (but the gone) and what came, what came first.
	std::vector<uint8_t>& s = out.state;
	s.clear();
	Put( s, magic );
	Put( s, headSize );
	PutBytes( s, head.data(), head.size() );
	const size_t countAt = s.size();
	Put( s, uint32_t( 0 ) );
	uint32_t total = 0;
	auto putEntity = [&]( uint32_t id, const uint8_t* record, uint32_t recordSize, const uint8_t* bodies, uint32_t bodiesSize, uint64_t hash ) {
		Put( s, id );
		Put( s, recordSize );
		PutBytes( s, record, recordSize );
		Put( s, bodiesSize );
		if ( bodiesSize > 0 )
		{
			Put( s, hash );
			PutBytes( s, bodies, bodiesSize );
		}
		total += 1;
		return s.size() <= kMaxStateBytes;
	};
	// And the puppets: those sent, and those it had of what it keeps (each from the state it came in).
	out.puppets.clear();
	size_t h = 0, k = 0, gone = 0;
	while ( h < had.entities.size() || k < sent.size() )
	{
		const bool takeSent = k < sent.size() && ( h >= had.entities.size() || sent[k].netId <= had.entities[h].netId );
		if ( takeSent )
		{
			const Decoded& d = sent[k];
			if ( h < had.entities.size() && had.entities[h].netId == d.netId )
			{
				++h;
			}
			++k;
			if ( putEntity( d.netId, d.record.data(), uint32_t( d.record.size() ), d.bodies, d.bodiesSize, d.hash ) == false )
			{
				return false;
			}
			if ( d.hasPuppet )
			{
				out.puppets.push_back( { d.netId, tick, d.puppet } );
			}
			continue;
		}
		const Span& e = had.entities[h++];
		while ( gone < removed.size() && removed[gone] < e.netId )
		{
			++gone;
		}
		if ( gone < removed.size() && removed[gone] == e.netId )
		{
			continue;
		}
		if ( putEntity( e.netId, e.record, e.recordSize, e.bodies, e.bodiesSize, e.hash ) == false )
		{
			return false;
		}
		if ( const PuppetEntry* kept = puppetIn( previous, e.netId ) )
		{
			out.puppets.push_back( *kept );
		}
	}
	std::memcpy( s.data() + countAt, &total, sizeof( total ) );
	out.tick = tick;
	return true;
}

} // namespace

bool DecodeState( ByteReader& r, uint32_t tick, const StateFrame* base, const StateFrame* previous, StateOf stateOf, const void* context,
				  StateFrame& out )
{
	const uint32_t sized = r.Read<uint32_t>();
	const bool stored = ( sized & kStored ) != 0;
	const uint32_t size = sized & ~kStored;
	if ( r.Ok() == false || size == 0 || size > kMaxStateBytes || ( stored && size != r.Remaining() ) )
	{
		return false;
	}
	std::vector<uint8_t> payload( size );
	if ( stored )
	{
		std::memcpy( payload.data(), r.Here(), size );
	}
	else
	{
		mz_ulong got = size;
		if ( mz_uncompress( payload.data(), &got, r.Here(), mz_ulong( r.Remaining() ) ) != MZ_OK || got != size )
		{
			return false;
		}
	}

	ByteReader p( payload.data(), payload.size() );
	out.tick = tick;
	out.state.clear();
	out.puppets.clear(); // (a whole state has none: every record whole)
	const uint8_t kind = p.Read<uint8_t>();
	if ( kind == 0 )
	{
		if ( base != nullptr )
		{
			return false;
		}
		out.inputs = {};
		out.state.assign( p.Here(), p.Here() + p.Remaining() );
		return true;
	}
	if ( kind == 2 )
	{
		return DecodeUpdates( p, tick, base, previous, stateOf, context, out );
	}
	if ( kind != 1 )
	{
		return false;
	}

	Parsed was;
	if ( base != nullptr && Parse( base->state, was ) == false )
	{
		return false;
	}
	auto takeXor = [&]( const uint8_t* old, size_t n, uint8_t* to ) {
		const uint8_t* d = p.Take( n );
		if ( d == nullptr )
		{
			return false;
		}
		for ( size_t i = 0; i < n; ++i )
		{
			to[i] = old != nullptr ? uint8_t( d[i] ^ old[i] ) : d[i];
		}
		return true;
	};

	if ( takeXor( base != nullptr ? Bytes( base->inputs ) : nullptr, kInputBytes, reinterpret_cast<uint8_t*>( out.inputs.data() ) ) == false )
	{
		return false;
	}
	std::vector<uint8_t>& s = out.state;
	s.reserve( base != nullptr ? base->state.size() + 1024 : size );
	Put( s, p.Read<uint32_t>() ); // magic
	const uint32_t headSize = p.Read<uint32_t>();
	if ( p.Ok() == false || headSize > size || ( base != nullptr && headSize != was.headSize ) )
	{
		return false;
	}
	Put( s, headSize );
	s.resize( s.size() + headSize );
	if ( takeXor( base != nullptr ? was.head : nullptr, headSize, s.data() + s.size() - headSize ) == false )
	{
		return false;
	}
	const uint32_t count = ReadVarint( p );
	if ( p.Ok() == false || count > size )
	{
		return false;
	}
	Put( s, count );
	Known known;
	for ( const Span& e : was.entities )
	{
		known.Add( &e );
	}
	known.Sort();
	// This state's descriptions so far: where in `s` each is (it grows, so offsets, not pointers).
	std::unordered_map<uint64_t, std::pair<size_t, uint32_t>> met;
	size_t b = 0;
	uint32_t netId = 0;
	for ( uint32_t n = 0; n < count; ++n )
	{
		netId += ReadVarint( p );
		const uint8_t flags = p.Read<uint8_t>();
		if ( p.Ok() == false )
		{
			return false;
		}
		const Span* old = nullptr;
		if ( base != nullptr )
		{
			while ( b < was.entities.size() && was.entities[b].netId < netId )
			{
				++b;
			}
			old = b < was.entities.size() && was.entities[b].netId == netId ? &was.entities[b] : nullptr;
		}
		Put( s, netId );
		switch ( flags & RecordMask )
		{
			case RecordSame:
			case RecordXor:
			{
				if ( old == nullptr )
				{
					return false;
				}
				Put( s, old->recordSize );
				if ( ( flags & RecordMask ) == RecordSame )
				{
					PutBytes( s, old->record, old->recordSize );
				}
				else
				{
					s.resize( s.size() + old->recordSize );
					if ( takeXor( old->record, old->recordSize, s.data() + s.size() - old->recordSize ) == false )
					{
						return false;
					}
				}
				break;
			}
			case RecordWhole:
			{
				const uint32_t recordSize = ReadVarint( p );
				const uint8_t* record = p.Take( recordSize );
				if ( record == nullptr )
				{
					return false;
				}
				Put( s, recordSize );
				PutBytes( s, record, recordSize );
				break;
			}
			default:
				return false;
		}
		if ( ( flags & HasBodies ) == 0 )
		{
			Put( s, uint32_t( 0 ) );
			if ( s.size() > kMaxStateBytes )
			{
				return false;
			}
			continue;
		}
		// A description: its size and hash, then its bytes, from wherever they come.
		uint64_t hash = 0;
		uint32_t bodiesSize = 0;
		const uint8_t* from = nullptr; // (null: earlier in this state, at `metAt`)
		size_t metAt = 0;
		if ( flags & BodiesKnown )
		{
			hash = p.Read<uint64_t>();
			if ( const Span* k = p.Ok() ? known.Find( hash ) : nullptr )
			{
				bodiesSize = k->bodiesSize;
				from = k->bodies;
			}
			else if ( auto it = met.find( hash ); it != met.end() )
			{
				metAt = it->second.first;
				bodiesSize = it->second.second;
			}
			else
			{
				return false;
			}
		}
		else if ( flags & BodiesSame )
		{
			if ( old == nullptr || old->bodiesSize == 0 )
			{
				return false;
			}
			hash = old->hash;
			bodiesSize = old->bodiesSize;
			from = old->bodies;
		}
		else
		{
			bodiesSize = ReadVarint( p );
			hash = p.Read<uint64_t>();
			from = p.Take( bodiesSize );
			if ( from == nullptr || bodiesSize == 0 )
			{
				return false;
			}
		}
		if ( s.size() + bodiesSize > kMaxStateBytes )
		{
			return false;
		}
		Put( s, bodiesSize );
		Put( s, hash );
		const size_t at = s.size();
		s.resize( at + bodiesSize );
		std::memcpy( s.data() + at, from != nullptr ? from : s.data() + metAt, bodiesSize );
		met.emplace( hash, std::make_pair( at, bodiesSize ) );
	}
	return p.Ok() && p.AtEnd();
}

} // namespace cb::net
