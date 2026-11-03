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
};

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
					UpdateCache* cache )
{
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
		payload.push_back( slot );
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
		// Its part: flags, how far back its base is, its record and bodies.
		const uint64_t key = ( uint64_t( u.netId ) << 32 ) | u.baseTick;
		if ( cache != nullptr )
		{
			if ( auto it = cache->parts.find( key ); it != cache->parts.end() )
			{
				PutBytes( payload, it->second.data(), it->second.size() );
				continue;
			}
		}
		const Span* e = now->Find( u.netId );
		const Parsed* from = u.baseTick != 0 ? layoutOf( context, u.baseTick ) : nullptr;
		const Span* old = from != nullptr ? from->Find( u.netId ) : nullptr;
		if ( e == nullptr || ( u.baseTick != 0 && old == nullptr ) )
		{
			return false;
		}
		part.clear();
		uint8_t flags = RecordWhole;
		if ( old != nullptr && old->recordSize == e->recordSize )
		{
			flags = std::memcmp( old->record, e->record, e->recordSize ) == 0 ? RecordSame : RecordXor;
		}
		const bool bodiesSame = old != nullptr && e->bodiesSize > 0 && old->bodiesSize == e->bodiesSize && old->hash == e->hash;
		flags |= e->bodiesSize > 0 ? HasBodies : 0;
		flags |= bodiesSame ? BodiesSame : 0;
		part.push_back( flags );
		PutVarint( part, old != nullptr ? frame.tick - u.baseTick : 0 );
		switch ( flags & RecordMask )
		{
			case RecordXor:
				PutSparse( part, e->record, old->record, e->recordSize );
				break;
			case RecordWhole:
				PutVarint( part, e->recordSize );
				PutBytes( part, e->record, e->recordSize );
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
		PutBytes( payload, part.data(), part.size() );
		if ( cache != nullptr )
		{
			cache->parts.emplace( key, part );
		}
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
		const uint8_t slot = p.Read<uint8_t>();
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
	};
	std::vector<Decoded> sent( count );
	std::vector<std::pair<uint32_t, Parsed>> bases; // (read once each)
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
		if ( back != 0 )
		{
			if ( back > tick || stateOf == nullptr )
			{
				return false;
			}
			const uint32_t at = tick - back;
			const Parsed* layout = nullptr;
			for ( const auto& [t, l] : bases )
			{
				layout = t == at ? &l : layout;
			}
			if ( layout == nullptr )
			{
				const StateFrame* f = stateOf( context, at );
				if ( f == nullptr )
				{
					return false;
				}
				bases.push_back( { at, {} } );
				if ( Parse( f->state, bases.back().second ) == false )
				{
					return false;
				}
				layout = &bases.back().second;
			}
			old = layout->Find( netId );
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
