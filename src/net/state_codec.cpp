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

// A state's parts, pointing into its bytes (sim/stand_ins.cpp has the layout).
struct Span
{
	uint32_t netId = 0;
	const uint8_t* record = nullptr;
	uint32_t recordSize = 0;
	const uint8_t* bodies = nullptr;
	uint32_t bodiesSize = 0;
	uint64_t hash = 0;
};
struct Parsed
{
	uint32_t magic = 0;
	const uint8_t* head = nullptr;
	uint32_t headSize = 0;
	std::vector<Span> entities;
};

bool Parse( const std::vector<uint8_t>& state, Parsed& out )
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
	for ( Span& s : out.entities )
	{
		s.netId = r.Read<uint32_t>();
		s.recordSize = r.Read<uint32_t>();
		s.record = r.Take( s.recordSize );
		s.bodiesSize = r.Read<uint32_t>();
		if ( s.bodiesSize > 0 )
		{
			s.hash = r.Read<uint64_t>();
			s.bodies = r.Take( s.bodiesSize );
		}
	}
	return r.Ok() && r.AtEnd();
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

} // namespace

void EncodeState( const StateFrame& frame, const StateFrame* base, std::vector<uint8_t>& out )
{
	Parsed now;
	Parsed was;
	const bool parsed = Parse( frame.state, now );
	const bool against = base != nullptr && parsed && Parse( base->state, was ) && was.headSize == now.headSize;

	std::vector<uint8_t> payload;
	payload.reserve( frame.state.size() / 2 );
	if ( parsed == false )
	{
		// (Not a state's layout: sent as it is, never against a base.)
		payload.push_back( 0 );
		PutBytes( payload, frame.state.data(), frame.state.size() );
	}
	else
	{
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
			PutXor( payload, now.head, was.head, now.headSize );
		}
		else
		{
			PutBytes( payload, now.head, now.headSize );
		}
		PutVarint( payload, uint32_t( now.entities.size() ) );
		Known known;
		if ( against )
		{
			for ( const Span& e : was.entities )
			{
				known.Add( &e );
			}
			known.Sort();
		}
		std::unordered_map<uint64_t, uint32_t> met; // hash -> its size, of this state so far
		size_t b = 0;
		uint32_t previous = 0;
		for ( const Span& e : now.entities )
		{
			const Span* old = nullptr;
			if ( against )
			{
				while ( b < was.entities.size() && was.entities[b].netId < e.netId )
				{
					++b;
				}
				old = b < was.entities.size() && was.entities[b].netId == e.netId ? &was.entities[b] : nullptr;
			}
			uint8_t flags = RecordWhole;
			if ( old != nullptr && old->recordSize == e.recordSize )
			{
				flags = std::memcmp( old->record, e.record, e.recordSize ) == 0 ? RecordSame : RecordXor;
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
	}

	mz_ulong packed = mz_compressBound( mz_ulong( payload.size() ) );
	out.clear();
	out.reserve( 13 + packed );
	out.push_back( uint8_t( MsgType::State ) );
	Put( out, frame.tick );
	Put( out, against ? base->tick : kNoStateBase );
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

bool ReadStateHeader( ByteReader& r, uint32_t& tick, uint32_t& baseTick )
{
	tick = r.Read<uint32_t>();
	baseTick = r.Read<uint32_t>();
	return r.Ok();
}

bool DecodeState( ByteReader& r, uint32_t tick, const StateFrame* base, StateFrame& out )
{
	const uint32_t size = r.Read<uint32_t>();
	if ( r.Ok() == false || size == 0 || size > kMaxStateBytes )
	{
		return false;
	}
	std::vector<uint8_t> payload( size );
	mz_ulong got = size;
	if ( mz_uncompress( payload.data(), &got, r.Here(), mz_ulong( r.Remaining() ) ) != MZ_OK || got != size )
	{
		return false;
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
