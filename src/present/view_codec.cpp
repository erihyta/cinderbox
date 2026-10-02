#include "view_codec.h"

#include "bytes.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace cb::present
{

namespace
{

constexpr uint32_t kMagic = 0x33564243; // "CBV3"

// What a decoder accepts at most; real frames are far below.
constexpr uint32_t kMaxEntities = 1u << 16;
constexpr uint32_t kMaxRagdolls = 1u << 12;
constexpr uint32_t kMaxStats = 256;
constexpr uint32_t kMaxTemplates = 1u << 12;

const Blackboard kNoPrivates{};

enum PacketFlag : uint8_t
{
	FlagWorld = 1 << 0,
	FlagMap = 1 << 1,
	FlagSchema = 1 << 2,
	FlagNames = 1 << 3,
	FlagStats = 1 << 4,
	FlagCompact = 1 << 5,
	FlagPrivates = 1 << 6, // the local player's private fields follow (else: the base's)
};

enum WorldFlag : uint8_t
{
	WorldRolledBack = 1 << 0,
	WorldHasInputs = 1 << 1,
	WorldSameEntities = 1 << 2,
	WorldSameRagdolls = 1 << 3,
};

// Where the packet being encoded counts its bytes (null: nobody asked).
thread_local ViewCost* t_cost = nullptr;

void Count( size_t ViewCost::*part, size_t bytes )
{
	if ( t_cost != nullptr )
	{
		t_cost->*part += bytes;
	}
}

// --- Records: an entity and a ragdoll as blocks of 32-bit words, with no padding ----------------

struct EntityRecord
{
	uint32_t netId;
	uint8_t kind;
	uint8_t shape;
	uint8_t slot;
	uint8_t flags; // 1 hasAnim, 2 dead, 4 hasBoard, 8 stowed
	uint32_t templateIndex;
	uint32_t stepCount;
	b3Vec3 halfExtents;
	Transform transform;
	b3Vec3 velocity;
	AnimState anim;
	Blackboard board;
	uint32_t ragdoll;
	uint32_t holder;
	uint16_t itemKind;
	uint8_t socket;
	uint8_t reserved;
};
static_assert( sizeof( EntityRecord ) == 80 + sizeof( AnimState ) + sizeof( Blackboard ), "EntityRecord has padding" );
static_assert( sizeof( EntityRecord ) % 4 == 0 );

struct RagdollRecord
{
	uint32_t netId;
	uint32_t owner;
	uint8_t slot;
	uint8_t reserved[3];
	float yaw;
	Transform parts[kRagdollParts];
};
static_assert( sizeof( RagdollRecord ) == 16 + sizeof( Transform ) * kRagdollParts, "RagdollRecord has padding" );
static_assert( sizeof( RagdollRecord ) % 4 == 0 );
static_assert( sizeof( ImpactRecord ) % 4 == 0 && sizeof( ModEventRecord ) % 4 == 0 );
static_assert( ( sizeof( PlayerInput ) * kMaxPlayers ) % 4 == 0 );

EntityRecord ToRecord( const FrameEntity& f )
{
	EntityRecord r;
	std::memset( &r, 0, sizeof( r ) );
	r.netId = f.netId;
	r.kind = uint8_t( f.kind );
	r.shape = uint8_t( f.shape );
	r.slot = f.slot;
	r.flags = uint8_t( ( f.hasAnim ? 1 : 0 ) | ( f.dead ? 2 : 0 ) | ( f.hasBoard ? 4 : 0 ) | ( f.stowed ? 8 : 0 ) );
	r.templateIndex = f.templateIndex;
	r.stepCount = f.stepCount;
	r.halfExtents = f.halfExtents;
	r.transform = f.transform;
	r.velocity = f.velocity;
	r.anim = f.anim;
	r.board = f.board;
	r.ragdoll = f.ragdoll;
	r.holder = f.holder;
	r.itemKind = f.itemKind;
	r.socket = f.socket;
	return r;
}

FrameEntity FromRecord( const EntityRecord& r )
{
	FrameEntity f;
	f.netId = r.netId;
	f.kind = VisualKind( r.kind );
	f.shape = ShapeKind( r.shape );
	f.slot = PlayerSlot( std::min<int>( r.slot, kMaxPlayers - 1 ) );
	f.hasAnim = ( r.flags & 1 ) != 0;
	f.dead = ( r.flags & 2 ) != 0;
	f.hasBoard = ( r.flags & 4 ) != 0;
	f.stowed = ( r.flags & 8 ) != 0;
	f.templateIndex = r.templateIndex;
	f.stepCount = r.stepCount;
	f.halfExtents = r.halfExtents;
	f.transform = r.transform;
	f.velocity = r.velocity;
	f.anim = r.anim;
	f.board = r.board;
	f.ragdoll = r.ragdoll;
	f.holder = r.holder;
	f.itemKind = r.itemKind;
	f.socket = r.socket;
	return f;
}

RagdollRecord ToRecord( const FrameRagdoll& f )
{
	RagdollRecord r;
	std::memset( &r, 0, sizeof( r ) );
	r.netId = f.netId;
	r.owner = f.owner;
	r.slot = f.slot;
	r.yaw = f.yaw;
	std::copy( f.parts, f.parts + kRagdollParts, r.parts );
	return r;
}

FrameRagdoll FromRecord( const RagdollRecord& r )
{
	FrameRagdoll f;
	f.netId = r.netId;
	f.owner = r.owner;
	f.slot = PlayerSlot( std::min<int>( r.slot, kMaxPlayers - 1 ) );
	f.yaw = r.yaw;
	std::copy( r.parts, r.parts + kRagdollParts, f.parts );
	return f;
}

// --- Word deltas ---------------------------------------------------------------------------------

// A bit per 32-bit word that differs from the base, then those words.
void WriteDelta( ByteWriter& w, const void* now, const void* base, size_t bytes )
{
	const auto* a = static_cast<const uint8_t*>( now );
	const auto* b = static_cast<const uint8_t*>( base );
	size_t words = bytes / 4;
	uint8_t mask[64] = {};
	for ( size_t begin = 0; begin < words; begin += sizeof( mask ) * 8 )
	{
		size_t count = std::min( words - begin, sizeof( mask ) * 8 );
		std::memset( mask, 0, sizeof( mask ) );
		for ( size_t i = 0; i < count; ++i )
		{
			if ( std::memcmp( a + ( begin + i ) * 4, b + ( begin + i ) * 4, 4 ) != 0 )
			{
				mask[i / 8] |= uint8_t( 1u << ( i % 8 ) );
			}
		}
		w.WriteBytes( mask, ( count + 7 ) / 8 );
		for ( size_t i = 0; i < count; ++i )
		{
			if ( mask[i / 8] & ( 1u << ( i % 8 ) ) )
			{
				w.WriteBytes( a + ( begin + i ) * 4, 4 );
			}
		}
	}
}

// `inOut` holds the base and becomes the new value.
void ReadDelta( ByteReader& r, void* inOut, size_t bytes )
{
	auto* out = static_cast<uint8_t*>( inOut );
	size_t words = bytes / 4;
	for ( size_t begin = 0; begin < words; begin += 64 * 8 )
	{
		size_t count = std::min<size_t>( words - begin, 64 * 8 );
		const uint8_t* mask = r.Take( ( count + 7 ) / 8 );
		if ( mask == nullptr )
		{
			return;
		}
		for ( size_t i = 0; i < count; ++i )
		{
			if ( mask[i / 8] & ( 1u << ( i % 8 ) ) )
			{
				const uint8_t* word = r.Take( 4 );
				if ( word == nullptr )
				{
					return;
				}
				std::memcpy( out + ( begin + i ) * 4, word, 4 );
			}
		}
	}
}

// A block that usually does not change at all: one byte says so.
void WriteBlock( ByteWriter& w, const void* now, const void* base, size_t bytes )
{
	bool changed = std::memcmp( now, base, bytes ) != 0;
	w.Write( uint8_t( changed ? 1 : 0 ) );
	if ( changed )
	{
		WriteDelta( w, now, base, bytes );
	}
}

void ReadBlock( ByteReader& r, void* inOut, size_t bytes )
{
	if ( r.Read<uint8_t>() != 0 )
	{
		ReadDelta( r, inOut, bytes );
	}
}

void WriteString( ByteWriter& w, const std::string& s )
{
	uint16_t size = uint16_t( std::min<size_t>( s.size(), 0xFFFF ) );
	w.Write( size );
	w.WriteBytes( s.data(), size );
}

std::string ReadString( ByteReader& r )
{
	uint16_t size = r.Read<uint16_t>();
	const uint8_t* p = r.Take( size );
	return p != nullptr ? std::string( reinterpret_cast<const char*>( p ), size ) : std::string();
}


// --- Compact: what a small view file holds ---------------------------------------------------------
//
// Positions on a 1/512 m grid and animation values on a 1/1024 grid, sent as how far they moved
// (one or two bytes for a tick of walking); rotations as 32 bits; no velocity; no transform for an
// item that is in someone's hand. Whatever lands on the same grid point as before is not sent, so a
// body that settles stops costing anything.
//
// Both sides must put the base on the same grid point. A value that came through a compact packet
// is already on it (an integer below 2^24 over a power of two is exact in a float), which is why a
// compact delta needs a base that was itself decoded from compact packets.

constexpr float kPositionGrid = 512.0f;
constexpr float kAnimGrid = 1024.0f;
constexpr int32_t kGridMost = ( 1 << 24 ) - 1;

int32_t ToGrid( float value, float grid )
{
	if ( std::isfinite( value ) == false )
	{
		return 0;
	}
	float scaled = value * grid;
	if ( scaled >= float( kGridMost ) )
	{
		return kGridMost;
	}
	if ( scaled <= -float( kGridMost ) )
	{
		return -kGridMost;
	}
	return int32_t( std::lround( scaled ) );
}

void WriteVarint( ByteWriter& w, int32_t value )
{
	uint32_t zigzag = ( uint32_t( value ) << 1 ) ^ uint32_t( value >> 31 );
	while ( zigzag >= 0x80 )
	{
		w.Write( uint8_t( zigzag | 0x80 ) );
		zigzag >>= 7;
	}
	w.Write( uint8_t( zigzag ) );
}

int32_t ReadVarint( ByteReader& r )
{
	uint32_t zigzag = 0;
	for ( int shift = 0; shift < 35; shift += 7 )
	{
		uint8_t byte = r.Read<uint8_t>();
		zigzag |= uint32_t( byte & 0x7F ) << shift;
		if ( ( byte & 0x80 ) == 0 )
		{
			return int32_t( ( zigzag >> 1 ) ^ ( ~( zigzag & 1 ) + 1 ) );
		}
	}
	r.Fail();
	return 0;
}

// A rotation in 32 bits: which of the four components is largest, and the other three in 10 bits.
uint32_t RotationCode( b3Quat q )
{
	float c[4] = { q.v.x, q.v.y, q.v.z, q.s };
	int largest = 0;
	for ( int i = 1; i < 4; ++i )
	{
		largest = std::fabs( c[i] ) > std::fabs( c[largest] ) ? i : largest;
	}
	float length = std::sqrt( c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3] );
	if ( std::isfinite( length ) == false || length < 1e-6f )
	{
		return 3u << 30 | 511u << 20 | 511u << 10 | 511u; // identity, near enough
	}
	float sign = c[largest] < 0.0f ? -1.0f / length : 1.0f / length;
	uint32_t code = uint32_t( largest ) << 30;
	int shift = 20;
	for ( int i = 0; i < 4; ++i )
	{
		if ( i == largest )
		{
			continue;
		}
		// The others lie within +/- 1/sqrt(2).
		float unit = c[i] * sign * 0.70710678f + 0.5f;
		uint32_t bits = uint32_t( std::clamp( int( std::lround( unit * 1023.0f ) ), 0, 1023 ) );
		code |= bits << shift;
		shift -= 10;
	}
	return code;
}

b3Quat RotationFromCode( uint32_t code )
{
	int largest = int( code >> 30 );
	float c[4] = {};
	float sum = 0.0f;
	int shift = 20;
	for ( int i = 0; i < 4; ++i )
	{
		if ( i == largest )
		{
			continue;
		}
		c[i] = ( float( ( code >> shift ) & 1023u ) / 1023.0f - 0.5f ) * 1.41421356f;
		sum += c[i] * c[i];
		shift -= 10;
	}
	c[largest] = std::sqrt( std::max( 0.0f, 1.0f - sum ) );
	return { { c[0], c[1], c[2] }, c[3] };
}

// Which 32-bit words of an AnimState are floats (the rest are small integers, sent as they are).
constexpr size_t kAnimWords = sizeof( AnimState ) / 4;
static_assert( offsetof( AnimState, modeTime ) == 4 && offsetof( AnimState, stances ) == 24 && offsetof( AnimState, moveForward ) == 28 &&
				   offsetof( AnimState, graph ) == 36 && sizeof( AnimGraphLayerState ) == 40 && offsetof( AnimGraphLayerState, time ) == 4,
			   "AnimState layout changed: say which of its words are floats" );

bool AnimWordIsFloat( size_t word )
{
	size_t byte = word * 4;
	if ( byte < offsetof( AnimState, graph ) )
	{
		return ( byte >= 4 && byte < 24 ) || byte >= 28;
	}
	return ( byte - offsetof( AnimState, graph ) ) % sizeof( AnimGraphLayerState ) != 0;
}

void AnimToGrid( const AnimState& anim, int32_t* words )
{
	std::memcpy( words, &anim, sizeof( AnimState ) );
	for ( size_t i = 0; i < kAnimWords; ++i )
	{
		if ( AnimWordIsFloat( i ) )
		{
			float value;
			std::memcpy( &value, &words[i], 4 );
			words[i] = ToGrid( value, kAnimGrid );
		}
	}
}

void AnimFromGrid( const int32_t* words, AnimState& anim )
{
	int32_t raw[kAnimWords];
	for ( size_t i = 0; i < kAnimWords; ++i )
	{
		raw[i] = words[i];
		if ( AnimWordIsFloat( i ) )
		{
			float value = float( words[i] ) / kAnimGrid;
			std::memcpy( &raw[i], &value, 4 );
		}
	}
	std::memcpy( &anim, raw, sizeof( AnimState ) );
}

// A position that moved on the grid: three small numbers. True when it did.
bool WritePositionDelta( ByteWriter& w, b3Vec3 now, b3Vec3 was )
{
	int32_t delta[3] = { ToGrid( now.x, kPositionGrid ) - ToGrid( was.x, kPositionGrid ), ToGrid( now.y, kPositionGrid ) - ToGrid( was.y, kPositionGrid ),
						 ToGrid( now.z, kPositionGrid ) - ToGrid( was.z, kPositionGrid ) };
	if ( delta[0] == 0 && delta[1] == 0 && delta[2] == 0 )
	{
		return false;
	}
	WriteVarint( w, delta[0] );
	WriteVarint( w, delta[1] );
	WriteVarint( w, delta[2] );
	return true;
}

void ReadPositionDelta( ByteReader& r, b3Vec3& position )
{
	// (Sums are done unsigned: bytes from anywhere must not overflow a signed int.)
	auto moved = [&r]( float was ) {
		uint32_t at = uint32_t( ToGrid( was, kPositionGrid ) ) + uint32_t( ReadVarint( r ) );
		return float( std::clamp( int32_t( at ), -kGridMost, kGridMost ) ) / kPositionGrid;
	};
	position.x = moved( position.x );
	position.y = moved( position.y );
	position.z = moved( position.z );
}

enum EntityGroup : uint8_t
{
	GroupRest = 1 << 0, // everything that is not below: identity, shape, flags, who holds it
	GroupPosition = 1 << 1,
	GroupRotation = 1 << 2,
	GroupAnim = 1 << 3,
	GroupBoard = 1 << 4,
};

// An entity as a compact packet holds it: no velocity, and no place of its own while it is held.
EntityRecord Seen( const EntityRecord& record )
{
	EntityRecord seen = record;
	seen.velocity = {};
	if ( seen.holder != 0 )
	{
		seen.transform = Transform{};
	}
	return seen;
}

// The record without the parts that have groups of their own: ten words.
struct RestRecord
{
	uint32_t netId;
	uint8_t kind;
	uint8_t shape;
	uint8_t slot;
	uint8_t flags;
	uint32_t templateIndex;
	uint32_t stepCount;
	b3Vec3 halfExtents;
	uint32_t ragdoll;
	uint32_t holder;
	uint16_t itemKind;
	uint8_t socket;
	uint8_t reserved;
};
static_assert( sizeof( RestRecord ) == 40, "RestRecord has padding" );

RestRecord Rest( const EntityRecord& record )
{
	RestRecord rest;
	std::memset( &rest, 0, sizeof( rest ) );
	rest.netId = record.netId;
	rest.kind = record.kind;
	rest.shape = record.shape;
	rest.slot = record.slot;
	rest.flags = record.flags;
	rest.templateIndex = record.templateIndex;
	rest.stepCount = record.stepCount;
	rest.halfExtents = record.halfExtents;
	rest.ragdoll = record.ragdoll;
	rest.holder = record.holder;
	rest.itemKind = record.itemKind;
	rest.socket = record.socket;
	return rest;
}

void SetRest( EntityRecord& record, const RestRecord& rest )
{
	record.netId = rest.netId;
	record.kind = rest.kind;
	record.shape = rest.shape;
	record.slot = rest.slot;
	record.flags = rest.flags;
	record.templateIndex = rest.templateIndex;
	record.stepCount = rest.stepCount;
	record.halfExtents = rest.halfExtents;
	record.ragdoll = rest.ragdoll;
	record.holder = rest.holder;
	record.itemKind = rest.itemKind;
	record.socket = rest.socket;
}

struct CompactEntity
{
	// `was` null: the entity is new to the receiver.
	static bool Write( ByteWriter& w, const EntityRecord& nowExact, const EntityRecord* wasExact )
	{
		EntityRecord zero;
		std::memset( &zero, 0, sizeof( zero ) );
		EntityRecord now = Seen( nowExact );
		EntityRecord was = wasExact != nullptr ? Seen( *wasExact ) : zero;

		std::vector<uint8_t> body;
		ByteWriter bw( body );
		uint8_t groups = 0;
		RestRecord restNow = Rest( now );
		RestRecord restWas = Rest( was );
		size_t mark = 0;
		if ( std::memcmp( &restNow, &restWas, sizeof( RestRecord ) ) != 0 )
		{
			groups |= GroupRest;
			WriteDelta( bw, &restNow, &restWas, sizeof( RestRecord ) );
		}
		Count( &ViewCost::rest, body.size() - mark );
		mark = body.size();
		if ( WritePositionDelta( bw, now.transform.position, was.transform.position ) )
		{
			groups |= GroupPosition;
		}
		Count( &ViewCost::positions, body.size() - mark );
		mark = body.size();
		uint32_t rotation = RotationCode( now.transform.rotation );
		if ( wasExact == nullptr || rotation != RotationCode( was.transform.rotation ) )
		{
			groups |= GroupRotation;
			bw.Write( rotation );
		}
		Count( &ViewCost::rotations, body.size() - mark );
		mark = body.size();
		int32_t animNow[kAnimWords];
		int32_t animWas[kAnimWords];
		AnimToGrid( now.anim, animNow );
		AnimToGrid( was.anim, animWas );
		if ( std::memcmp( animNow, animWas, sizeof( animNow ) ) != 0 )
		{
			groups |= GroupAnim;
			uint8_t mask[( kAnimWords + 7 ) / 8] = {};
			for ( size_t i = 0; i < kAnimWords; ++i )
			{
				mask[i / 8] |= animNow[i] != animWas[i] ? uint8_t( 1u << ( i % 8 ) ) : uint8_t( 0 );
			}
			bw.WriteBytes( mask, sizeof( mask ) );
			for ( size_t i = 0; i < kAnimWords; ++i )
			{
				if ( animNow[i] == animWas[i] )
				{
					continue;
				}
				if ( AnimWordIsFloat( i ) )
				{
					WriteVarint( bw, int32_t( uint32_t( animNow[i] ) - uint32_t( animWas[i] ) ) );
				}
				else
				{
					bw.Write( animNow[i] );
				}
			}
		}
		Count( &ViewCost::animation, body.size() - mark );
		mark = body.size();
		if ( std::memcmp( &now.board, &was.board, sizeof( Blackboard ) ) != 0 )
		{
			groups |= GroupBoard;
			WriteDelta( bw, &now.board, &was.board, sizeof( Blackboard ) );
		}
		Count( &ViewCost::boards, body.size() - mark );
		if ( groups == 0 )
		{
			return false;
		}
		Count( &ViewCost::lists, 1 );
		w.Write( groups );
		w.WriteBytes( body.data(), body.size() );
		return true;
	}

	// `record` holds the base (zeros for a new entity) and becomes the entity.
	static void Read( ByteReader& r, EntityRecord& record )
	{
		uint8_t groups = r.Read<uint8_t>();
		if ( groups & GroupRest )
		{
			RestRecord rest = Rest( record );
			ReadDelta( r, &rest, sizeof( RestRecord ) );
			SetRest( record, rest );
		}
		if ( groups & GroupPosition )
		{
			ReadPositionDelta( r, record.transform.position );
		}
		if ( groups & GroupRotation )
		{
			record.transform.rotation = RotationFromCode( r.Read<uint32_t>() );
		}
		if ( groups & GroupAnim )
		{
			int32_t anim[kAnimWords];
			AnimToGrid( record.anim, anim );
			const uint8_t* mask = r.Take( ( kAnimWords + 7 ) / 8 );
			for ( size_t i = 0; mask != nullptr && i < kAnimWords; ++i )
			{
				if ( ( mask[i / 8] & ( 1u << ( i % 8 ) ) ) == 0 )
				{
					continue;
				}
				if ( AnimWordIsFloat( i ) )
				{
					uint32_t moved = uint32_t( anim[i] ) + uint32_t( ReadVarint( r ) );
					anim[i] = std::clamp( int32_t( moved ), -kGridMost, kGridMost );
				}
				else
				{
					anim[i] = r.Read<int32_t>();
				}
			}
			AnimFromGrid( anim, record.anim );
		}
		if ( groups & GroupBoard )
		{
			ReadDelta( r, &record.board, sizeof( Blackboard ) );
		}
	}

	// What an entity that was not in the packet at all is, given the base: the base as the
	// packets left it (it is already on the grid).
	static void Settle( EntityRecord& record )
	{
		record.velocity = {};
		if ( record.holder != 0 )
		{
			record.transform = Transform{};
		}
	}
};

struct CompactRagdoll
{
	static bool Write( ByteWriter& w, const RagdollRecord& now, const RagdollRecord* wasExact )
	{
		RagdollRecord zero;
		std::memset( &zero, 0, sizeof( zero ) );
		const RagdollRecord& was = wasExact != nullptr ? *wasExact : zero;

		std::vector<uint8_t> body;
		ByteWriter bw( body );
		// Two bits a part: it moved, it turned.
		uint8_t mask[( kRagdollParts * 2 + 7 ) / 8] = {};
		for ( int i = 0; i < kRagdollParts; ++i )
		{
			if ( WritePositionDelta( bw, now.parts[i].position, was.parts[i].position ) )
			{
				mask[( i * 2 ) / 8] |= uint8_t( 1u << ( ( i * 2 ) % 8 ) );
			}
			uint32_t rotation = RotationCode( now.parts[i].rotation );
			if ( wasExact == nullptr || rotation != RotationCode( was.parts[i].rotation ) )
			{
				mask[( i * 2 + 1 ) / 8] |= uint8_t( 1u << ( ( i * 2 + 1 ) % 8 ) );
				bw.Write( rotation );
			}
		}
		bool head = std::memcmp( &now, &was, offsetof( RagdollRecord, parts ) ) != 0;
		bool parts = false;
		for ( uint8_t byte : mask )
		{
			parts |= byte != 0;
		}
		if ( head == false && parts == false )
		{
			return false;
		}
		w.Write( uint8_t( head ? 1 : 0 ) );
		if ( head )
		{
			WriteDelta( w, &now, &was, offsetof( RagdollRecord, parts ) );
		}
		w.WriteBytes( mask, sizeof( mask ) );
		w.WriteBytes( body.data(), body.size() );
		Count( &ViewCost::ragdolls, 1 + sizeof( mask ) + body.size() + ( head ? 6 : 0 ) );
		return true;
	}

	static void Read( ByteReader& r, RagdollRecord& record )
	{
		if ( r.Read<uint8_t>() != 0 )
		{
			ReadDelta( r, &record, offsetof( RagdollRecord, parts ) );
		}
		const uint8_t* taken = r.Take( ( kRagdollParts * 2 + 7 ) / 8 );
		if ( taken == nullptr )
		{
			return;
		}
		uint8_t mask[( kRagdollParts * 2 + 7 ) / 8];
		std::memcpy( mask, taken, sizeof( mask ) );
		for ( int i = 0; i < kRagdollParts; ++i )
		{
			if ( mask[( i * 2 ) / 8] & ( 1u << ( ( i * 2 ) % 8 ) ) )
			{
				ReadPositionDelta( r, record.parts[i].position );
			}
			if ( mask[( i * 2 + 1 ) / 8] & ( 1u << ( ( i * 2 + 1 ) % 8 ) ) )
			{
				record.parts[i].rotation = RotationFromCode( r.Read<uint32_t>() );
			}
		}
	}

	static void Settle( RagdollRecord& )
	{
	}
};

// Exact: every word that differs, as it is.
template <typename Record>
struct Exact
{
	static bool Write( ByteWriter& w, const Record& now, const Record* was )
	{
		Record zero;
		std::memset( &zero, 0, sizeof( zero ) );
		const Record& before = was != nullptr ? *was : zero;
		if ( std::memcmp( &now, &before, sizeof( Record ) ) == 0 )
		{
			return false;
		}
		WriteDelta( w, &now, &before, sizeof( Record ) );
		return true;
	}
	static void Read( ByteReader& r, Record& record )
	{
		ReadDelta( r, &record, sizeof( Record ) );
	}
	static void Settle( Record& )
	{
	}
};

// A ring of records in which a few are new: a bit per record, and the new ones whole.
template <typename Record, size_t Size>
void WriteRing( ByteWriter& w, const Record ( &now )[Size], const Record ( &was )[Size] )
{
	uint8_t mask[( Size + 7 ) / 8] = {};
	for ( size_t i = 0; i < Size; ++i )
	{
		mask[i / 8] |= std::memcmp( &now[i], &was[i], sizeof( Record ) ) != 0 ? uint8_t( 1u << ( i % 8 ) ) : uint8_t( 0 );
	}
	w.WriteBytes( mask, sizeof( mask ) );
	for ( size_t i = 0; i < Size; ++i )
	{
		if ( mask[i / 8] & ( 1u << ( i % 8 ) ) )
		{
			w.Write( now[i] );
		}
	}
}

template <typename Record, size_t Size>
void ReadRing( ByteReader& r, Record ( &inOut )[Size] )
{
	const uint8_t* taken = r.Take( ( Size + 7 ) / 8 );
	if ( taken == nullptr )
	{
		return;
	}
	uint8_t mask[( Size + 7 ) / 8];
	std::memcpy( mask, taken, sizeof( mask ) );
	for ( size_t i = 0; i < Size; ++i )
	{
		if ( mask[i / 8] & ( 1u << ( i % 8 ) ) )
		{
			inOut[i] = r.Read<Record>();
		}
	}
}

// --- Lists of records keyed by NetId ---------------------------------------------------------------

template <typename Item>
bool SameIds( const std::vector<Item>& a, const std::vector<Item>& b )
{
	if ( a.size() != b.size() )
	{
		return false;
	}
	for ( size_t i = 0; i < a.size(); ++i )
	{
		if ( a[i].netId != b[i].netId )
		{
			return false;
		}
	}
	return true;
}

// The base's item for a NetId (lists are in NetId order), or null. Encoder and decoder ask the
// same question of the same base, so they agree whatever the answer.
template <typename Item>
const Item* FindBase( const std::vector<Item>* base, bool sameIds, size_t index, uint32_t netId )
{
	if ( base == nullptr )
	{
		return nullptr;
	}
	if ( sameIds )
	{
		return &( *base )[index];
	}
	auto it = std::lower_bound( base->begin(), base->end(), netId, []( const Item& item, uint32_t id ) { return item.netId < id; } );
	return it != base->end() && it->netId == netId ? &*it : nullptr;
}

void WriteCount( ByteWriter& w, uint32_t value )
{
	while ( value >= 0x80 )
	{
		w.Write( uint8_t( value | 0x80 ) );
		value >>= 7;
	}
	w.Write( uint8_t( value ) );
}

uint32_t ReadCount( ByteReader& r )
{
	uint32_t value = 0;
	for ( int shift = 0; shift < 35; shift += 7 )
	{
		uint8_t byte = r.Read<uint8_t>();
		value |= uint32_t( byte & 0x7F ) << shift;
		if ( ( byte & 0x80 ) == 0 )
		{
			return value;
		}
	}
	r.Fail();
	return 0;
}

template <typename Item>
bool Ascending( const std::vector<Item>& items )
{
	for ( size_t i = 1; i < items.size(); ++i )
	{
		if ( items[i - 1].netId >= items[i].netId )
		{
			return false;
		}
	}
	return true;
}

// Which NetIds a list has, when it is not the base's list: usually the base's with a few gone and
// a few new (both are in NetId order), so those are named; otherwise all of them.
template <typename Item>
void WriteIds( ByteWriter& w, const std::vector<Item>& items, const std::vector<Item>* base )
{
	if ( base == nullptr || Ascending( items ) == false || Ascending( *base ) == false )
	{
		w.Write( uint8_t( 0 ) );
		for ( const Item& item : items )
		{
			w.Write( item.netId );
		}
		return;
	}
	std::vector<uint32_t> gone;	 // indices into the base
	std::vector<uint32_t> fresh; // NetIds
	size_t a = 0, b = 0;
	while ( a < base->size() || b < items.size() )
	{
		if ( b == items.size() || ( a < base->size() && ( *base )[a].netId < items[b].netId ) )
		{
			gone.push_back( uint32_t( a++ ) );
		}
		else if ( a == base->size() || items[b].netId < ( *base )[a].netId )
		{
			fresh.push_back( items[b++].netId );
		}
		else
		{
			++a;
			++b;
		}
	}
	w.Write( uint8_t( 1 ) );
	WriteCount( w, uint32_t( gone.size() ) );
	uint32_t last = 0;
	for ( uint32_t index : gone )
	{
		WriteCount( w, index - last );
		last = index;
	}
	WriteCount( w, uint32_t( fresh.size() ) );
	last = 0;
	for ( uint32_t id : fresh )
	{
		WriteCount( w, id - last );
		last = id;
	}
}

// False when the bytes do not describe a list of `count` ids.
template <typename Item>
bool ReadIds( ByteReader& r, std::vector<uint32_t>& ids, uint32_t count, const std::vector<Item>* base )
{
	ids.clear();
	uint8_t mode = r.Read<uint8_t>();
	if ( mode == 0 )
	{
		for ( uint32_t i = 0; i < count && r.Ok(); ++i )
		{
			ids.push_back( r.Read<uint32_t>() );
		}
		return r.Ok();
	}
	if ( mode != 1 || base == nullptr )
	{
		return false;
	}
	std::vector<bool> isGone( base->size(), false );
	uint32_t gone = ReadCount( r );
	uint32_t at = 0;
	for ( uint32_t i = 0; i < gone && r.Ok(); ++i )
	{
		at += ReadCount( r );
		if ( at >= base->size() )
		{
			return false;
		}
		isGone[at] = true;
	}
	uint32_t fresh = ReadCount( r );
	if ( r.Ok() == false || gone > base->size() || fresh > count )
	{
		return false;
	}
	std::vector<uint32_t> added;
	uint32_t id = 0;
	for ( uint32_t i = 0; i < fresh && r.Ok(); ++i )
	{
		id += ReadCount( r );
		added.push_back( id );
	}
	// The base's that stayed and the new ones, merged in NetId order.
	size_t a = 0, b = 0;
	while ( a < base->size() || b < added.size() )
	{
		if ( a < base->size() && isGone[a] )
		{
			++a;
		}
		else if ( b == added.size() || ( a < base->size() && ( *base )[a].netId < added[b] ) )
		{
			ids.push_back( ( *base )[a++].netId );
		}
		else
		{
			ids.push_back( added[b++] );
		}
	}
	return r.Ok() && ids.size() == count;
}

template <typename Item, typename Record, typename Codec>
void WriteList( ByteWriter& w, const std::vector<Item>& items, const std::vector<Item>* base, bool sameIds )
{
	w.Write( uint32_t( items.size() ) );
	if ( sameIds == false )
	{
		WriteIds( w, items, base );
	}
	// Which changed, then their deltas.
	std::vector<uint8_t> changed( ( items.size() + 7 ) / 8, 0 );
	std::vector<uint8_t> deltas;
	ByteWriter dw( deltas );
	for ( size_t i = 0; i < items.size(); ++i )
	{
		Record now = ToRecord( items[i] );
		const Item* before = FindBase( base, sameIds, i, items[i].netId );
		Record was;
		if ( before != nullptr )
		{
			was = ToRecord( *before );
		}
		if ( Codec::Write( dw, now, before != nullptr ? &was : nullptr ) )
		{
			changed[i / 8] |= uint8_t( 1u << ( i % 8 ) );
		}
	}
	w.WriteBytes( changed.data(), changed.size() );
	w.WriteBytes( deltas.data(), deltas.size() );
}

template <typename Item, typename Record, typename Codec>
void ReadList( ByteReader& r, std::vector<Item>& items, const std::vector<Item>* base, bool sameIds, uint32_t most )
{
	uint32_t count = r.Read<uint32_t>();
	if ( count > most || ( sameIds && ( base == nullptr || base->size() != count ) ) )
	{
		r.Fail();
		items.clear();
		return;
	}
	std::vector<uint32_t> ids;
	if ( sameIds )
	{
		for ( uint32_t i = 0; i < count; ++i )
		{
			ids.push_back( ( *base )[i].netId );
		}
	}
	else if ( ReadIds( r, ids, count, base ) == false )
	{
		r.Fail();
		items.clear();
		return;
	}
	const uint8_t* changedBits = r.Take( ( size_t( count ) + 7 ) / 8 );
	if ( changedBits == nullptr )
	{
		items.clear();
		return;
	}
	std::vector<uint8_t> changed( changedBits, changedBits + ( size_t( count ) + 7 ) / 8 );
	items.resize( count );
	Record zero;
	std::memset( &zero, 0, sizeof( zero ) );
	for ( uint32_t i = 0; i < count; ++i )
	{
		const Item* before = FindBase( base, sameIds, i, ids[i] );
		Record record = before != nullptr ? ToRecord( *before ) : zero;
		if ( changed[i / 8] & ( 1u << ( i % 8 ) ) )
		{
			Codec::Read( r, record );
		}
		Codec::Settle( record );
		record.netId = ids[i];
		items[i] = FromRecord( record );
	}
}

} // namespace

void EncodeView( const ViewFrame& frame, const ViewFrame* base, double ageSeconds, std::vector<uint8_t>& out, ViewPrecision precision,
				 ViewCost* cost )
{
	bool compact = precision == ViewPrecision::Compact;
	// Sections count themselves as they are written; the codecs of the lists count their own parts.
	struct Counting
	{
		explicit Counting( ViewCost* cost )
		{
			t_cost = cost;
		}
		~Counting()
		{
			t_cost = nullptr;
		}
	} counting( cost );
	size_t mark = 0;
	auto section = [&]( size_t ViewCost::*part ) {
		Count( part, out.size() - mark );
		mark = out.size();
	};
	out.clear();
	ByteWriter w( out );

	bool map = base == nullptr || base->mapHash != frame.mapHash || base->mapName != frame.mapName ||
			   base->templateNames != frame.templateNames || base->templateVisuals != frame.templateVisuals;
	bool schema = base == nullptr || base->schemaGeneration != frame.schemaGeneration;
	bool names = base == nullptr || base->namesGeneration != frame.namesGeneration;
	bool stats = base == nullptr || base->stats != frame.stats;
	bool privates = base == nullptr ? std::memcmp( &frame.privates, &kNoPrivates, sizeof( Blackboard ) ) != 0
									: std::memcmp( &frame.privates, &base->privates, sizeof( Blackboard ) ) != 0;
	uint8_t flags = uint8_t( ( frame.hasWorld ? FlagWorld : 0 ) | ( map ? FlagMap : 0 ) | ( schema ? FlagSchema : 0 ) |
							 ( names ? FlagNames : 0 ) | ( stats ? FlagStats : 0 ) | ( compact ? FlagCompact : 0 ) |
							 ( privates ? FlagPrivates : 0 ) );

	w.Write( kMagic );
	w.Write( frame.serial );
	w.Write( uint64_t( base != nullptr ? base->serial : 0 ) );
	w.Write( float( ageSeconds ) );
	w.Write( frame.alphaAtPublish );
	w.Write( frame.rate );
	w.Write( frame.localPressed );
	w.Write( uint16_t( std::clamp<uint32_t>( frame.stride, 1, 0xFFFF ) ) );
	w.Write( flags );
	WriteString( w, frame.state );
	section( &ViewCost::header );

	if ( stats )
	{
		w.Write( uint16_t( std::min<size_t>( frame.stats.size(), kMaxStats ) ) );
		for ( size_t i = 0; i < frame.stats.size() && i < kMaxStats; ++i )
		{
			const ViewStat& stat = frame.stats[i];
			WriteString( w, stat.name );
			w.Write( uint8_t( stat.value.index() ) );
			if ( const int64_t* integer = std::get_if<int64_t>( &stat.value ) )
			{
				w.Write( *integer );
			}
			else if ( const double* number = std::get_if<double>( &stat.value ) )
			{
				w.Write( *number );
			}
			else if ( const bool* flag = std::get_if<bool>( &stat.value ) )
			{
				w.Write( uint8_t( *flag ? 1 : 0 ) );
			}
			else
			{
				WriteString( w, std::get<std::string>( stat.value ) );
			}
		}
	}
	section( &ViewCost::stats );
	if ( map )
	{
		w.Write( frame.mapHash );
		WriteString( w, frame.mapName );
		uint32_t templates = uint32_t( std::min<size_t>( frame.templateNames.size(), kMaxTemplates ) );
		w.Write( templates );
		for ( uint32_t i = 0; i < templates; ++i )
		{
			WriteString( w, frame.templateNames[i] );
			WriteString( w, i < frame.templateVisuals.size() ? frame.templateVisuals[i] : std::string() );
		}
	}
	if ( schema )
	{
		std::vector<uint8_t> bytes;
		EncodeSchema( frame.schema, bytes );
		w.Write( frame.schemaGeneration );
		w.WriteBlob( bytes );
	}
	if ( names )
	{
		w.Write( frame.namesGeneration );
		for ( const std::string& name : frame.names )
		{
			WriteString( w, name );
		}
	}
	if ( privates )
	{
		// The ones that are not 0: a slot and its value each.
		uint8_t count = 0;
		for ( int32_t value : frame.privates.values )
		{
			count += value != 0 ? 1 : 0;
		}
		w.Write( count );
		for ( int slot = 0; slot < kBoardSlots; ++slot )
		{
			if ( frame.privates.values[slot] != 0 )
			{
				w.Write( uint8_t( slot ) );
				w.Write( frame.privates.values[slot] );
			}
		}
	}
	section( &ViewCost::session );
	if ( frame.hasWorld == false )
	{
		return;
	}

	// The world, against the base's (or against nothing).
	const PresentationFrame& f = frame.frame;
	const PresentationFrame* b = base != nullptr && base->hasWorld ? &base->frame : nullptr;
	static const PresentationFrame kEmpty;
	const PresentationFrame& was = b != nullptr ? *b : kEmpty;
	bool sameEntities = b != nullptr && SameIds( f.entities, b->entities );
	bool sameRagdolls = b != nullptr && SameIds( f.ragdolls, b->ragdolls );

	w.Write( f.tick );
	w.Write( f.resetGeneration );
	w.Write( f.localNetId );
	w.Write( f.tickSeconds );
	w.Write( f.impactCount );
	w.Write( f.modEventCount );
	// (Nobody draws inputs: a compact packet leaves them out.)
	bool inputs = f.hasInputs && compact == false;
	w.Write( uint8_t( ( f.rolledBack ? WorldRolledBack : 0 ) | ( inputs ? WorldHasInputs : 0 ) | ( sameEntities ? WorldSameEntities : 0 ) |
					  ( sameRagdolls ? WorldSameRagdolls : 0 ) ) );
	section( &ViewCost::header );

	if ( compact == false )
	{
		WriteBlock( w, f.inputs.data(), was.inputs.data(), sizeof( PlayerInput ) * kMaxPlayers );
	}
	section( &ViewCost::inputs );
	WriteBlock( w, f.board, was.board, sizeof( f.board ) );

	// The event rings, as the fixed blocks they are in the simulation (a frame without them: zeros).
	ImpactRecord impacts[kImpactHistory] = {};
	ImpactRecord impactsWas[kImpactHistory] = {};
	std::copy_n( f.impacts.begin(), std::min<size_t>( f.impacts.size(), kImpactHistory ), impacts );
	std::copy_n( was.impacts.begin(), std::min<size_t>( was.impacts.size(), kImpactHistory ), impactsWas );
	WriteRing( w, impacts, impactsWas );
	ModEventRecord events[kModEventHistory] = {};
	ModEventRecord eventsWas[kModEventHistory] = {};
	std::copy_n( f.modEvents.begin(), std::min<size_t>( f.modEvents.size(), kModEventHistory ), events );
	std::copy_n( was.modEvents.begin(), std::min<size_t>( was.modEvents.size(), kModEventHistory ), eventsWas );
	WriteRing( w, events, eventsWas );
	section( &ViewCost::events );

	if ( compact )
	{
		// (The codecs count what they write; what is left over is the lists' own: ids and bits.)
		size_t before = cost != nullptr ? cost->rest + cost->positions + cost->rotations + cost->animation + cost->boards + cost->ragdolls + cost->lists : 0;
		WriteList<FrameEntity, EntityRecord, CompactEntity>( w, f.entities, b != nullptr ? &b->entities : nullptr, sameEntities );
		WriteList<FrameRagdoll, RagdollRecord, CompactRagdoll>( w, f.ragdolls, b != nullptr ? &b->ragdolls : nullptr, sameRagdolls );
		if ( cost != nullptr )
		{
			size_t counted = cost->rest + cost->positions + cost->rotations + cost->animation + cost->boards + cost->ragdolls + cost->lists - before;
			cost->lists += out.size() - mark - counted;
		}
	}
	else
	{
		WriteList<FrameEntity, EntityRecord, Exact<EntityRecord>>( w, f.entities, b != nullptr ? &b->entities : nullptr, sameEntities );
		WriteList<FrameRagdoll, RagdollRecord, Exact<RagdollRecord>>( w, f.ragdolls, b != nullptr ? &b->ragdolls : nullptr, sameRagdolls );
		section( &ViewCost::exact );
	}
}

bool ViewPacketCompact( const uint8_t* data, size_t size )
{
	ByteReader r( data, size );
	if ( r.Read<uint32_t>() != kMagic )
	{
		return false;
	}
	r.Take( 8 + 8 + 4 + 4 + 4 + 2 + 2 );
	uint8_t flags = r.Read<uint8_t>();
	return r.Ok() && ( flags & FlagCompact ) != 0;
}

uint64_t ViewPacketBase( const uint8_t* data, size_t size )
{
	ByteReader r( data, size );
	if ( r.Read<uint32_t>() != kMagic )
	{
		return 0;
	}
	r.Read<uint64_t>();
	uint64_t base = r.Read<uint64_t>();
	return r.Ok() ? base : 0;
}

bool DecodeView( const uint8_t* data, size_t size, const ViewFrame* base, ViewFrame& out )
{
	ByteReader r( data, size );
	if ( r.Read<uint32_t>() != kMagic )
	{
		return false;
	}
	out.serial = r.Read<uint64_t>();
	uint64_t baseSerial = r.Read<uint64_t>();
	if ( baseSerial != 0 && ( base == nullptr || base->serial != baseSerial ) )
	{
		return false;
	}
	if ( baseSerial == 0 )
	{
		base = nullptr;
	}
	out.publishedAt = ViewClock() - double( r.Read<float>() );
	out.alphaAtPublish = r.Read<float>();
	out.rate = r.Read<float>();
	out.localPressed = r.Read<uint16_t>();
	out.stride = std::max<uint32_t>( r.Read<uint16_t>(), 1 );
	uint8_t flags = r.Read<uint8_t>();
	bool compact = ( flags & FlagCompact ) != 0;
	out.state = ReadString( r );
	// What a packet leaves out is the base's; a packet without a base leaves nothing out.
	if ( base == nullptr && ( flags & ( FlagMap | FlagSchema | FlagNames | FlagStats ) ) != ( FlagMap | FlagSchema | FlagNames | FlagStats ) )
	{
		return false;
	}

	if ( flags & FlagStats )
	{
		uint16_t count = r.Read<uint16_t>();
		if ( count > kMaxStats )
		{
			return false;
		}
		out.stats.clear();
		for ( uint16_t i = 0; i < count && r.Ok(); ++i )
		{
			ViewStat stat;
			stat.name = ReadString( r );
			switch ( r.Read<uint8_t>() )
			{
				case 0:
					stat.value = r.Read<int64_t>();
					break;
				case 1:
					stat.value = r.Read<double>();
					break;
				case 2:
					stat.value = r.Read<uint8_t>() != 0;
					break;
				case 3:
					stat.value = ReadString( r );
					break;
				default:
					return false;
			}
			out.stats.push_back( std::move( stat ) );
		}
	}
	else
	{
		out.stats = base->stats;
	}
	if ( flags & FlagMap )
	{
		out.mapHash = r.Read<uint64_t>();
		out.mapName = ReadString( r );
		uint32_t templates = r.Read<uint32_t>();
		if ( templates > kMaxTemplates )
		{
			return false;
		}
		out.templateNames.clear();
		out.templateVisuals.clear();
		for ( uint32_t i = 0; i < templates && r.Ok(); ++i )
		{
			out.templateNames.push_back( ReadString( r ) );
			out.templateVisuals.push_back( ReadString( r ) );
		}
	}
	else
	{
		out.mapHash = base->mapHash;
		out.mapName = base->mapName;
		out.templateNames = base->templateNames;
		out.templateVisuals = base->templateVisuals;
	}
	if ( flags & FlagSchema )
	{
		out.schemaGeneration = r.Read<uint64_t>();
		std::vector<uint8_t> bytes;
		r.ReadBlob( bytes );
		out.schema = ModSchema();
		if ( r.Ok() == false || DecodeSchema( bytes.data(), bytes.size(), out.schema ) == false )
		{
			return false;
		}
	}
	else if ( out.schemaGeneration != base->schemaGeneration )
	{
		// (Skipped when `out` already holds this schema: it is the largest thing to copy.)
		out.schemaGeneration = base->schemaGeneration;
		out.schema = base->schema;
	}
	if ( flags & FlagNames )
	{
		out.namesGeneration = r.Read<uint64_t>();
		for ( std::string& name : out.names )
		{
			name = ReadString( r );
		}
	}
	else
	{
		out.namesGeneration = base->namesGeneration;
		out.names = base->names;
	}
	if ( flags & FlagPrivates )
	{
		out.privates = Blackboard{};
		uint8_t count = r.Read<uint8_t>();
		for ( uint8_t i = 0; i < count && r.Ok(); ++i )
		{
			uint8_t slot = r.Read<uint8_t>();
			int32_t value = r.Read<int32_t>();
			if ( slot >= kBoardSlots )
			{
				return false;
			}
			out.privates.values[slot] = value;
		}
	}
	else
	{
		out.privates = base != nullptr ? base->privates : Blackboard{};
	}

	out.hasWorld = ( flags & FlagWorld ) != 0;
	if ( out.hasWorld == false )
	{
		out.frame = PresentationFrame();
		return r.Ok() && r.AtEnd();
	}

	PresentationFrame& f = out.frame;
	const PresentationFrame* b = base != nullptr && base->hasWorld ? &base->frame : nullptr;
	f.tick = r.Read<uint32_t>();
	f.resetGeneration = r.Read<uint64_t>();
	f.localNetId = r.Read<uint32_t>();
	f.tickSeconds = r.Read<float>();
	f.impactCount = r.Read<uint32_t>();
	f.modEventCount = r.Read<uint32_t>();
	uint8_t world = r.Read<uint8_t>();
	f.rolledBack = ( world & WorldRolledBack ) != 0;
	f.hasInputs = ( world & WorldHasInputs ) != 0;
	bool sameEntities = ( world & WorldSameEntities ) != 0;
	bool sameRagdolls = ( world & WorldSameRagdolls ) != 0;
	if ( ( sameEntities || sameRagdolls ) && b == nullptr )
	{
		return false;
	}

	if ( b != nullptr )
	{
		std::copy( b->board, b->board + kBoardSlots, f.board );
		f.inputs = b->inputs;
	}
	else
	{
		std::fill( f.board, f.board + kBoardSlots, 0 );
		f.inputs = {};
	}
	if ( compact )
	{
		f.inputs = {};
	}
	else
	{
		ReadBlock( r, f.inputs.data(), sizeof( PlayerInput ) * kMaxPlayers );
	}
	ReadBlock( r, f.board, sizeof( f.board ) );

	ImpactRecord impacts[kImpactHistory] = {};
	ModEventRecord events[kModEventHistory] = {};
	if ( b != nullptr )
	{
		std::copy_n( b->impacts.begin(), std::min<size_t>( b->impacts.size(), kImpactHistory ), impacts );
		std::copy_n( b->modEvents.begin(), std::min<size_t>( b->modEvents.size(), kModEventHistory ), events );
	}
	ReadRing( r, impacts );
	ReadRing( r, events );
	f.impacts.assign( impacts, impacts + kImpactHistory );
	f.modEvents.assign( events, events + kModEventHistory );

	if ( compact )
	{
		ReadList<FrameEntity, EntityRecord, CompactEntity>( r, f.entities, b != nullptr ? &b->entities : nullptr, sameEntities, kMaxEntities );
		ReadList<FrameRagdoll, RagdollRecord, CompactRagdoll>( r, f.ragdolls, b != nullptr ? &b->ragdolls : nullptr, sameRagdolls, kMaxRagdolls );
	}
	else
	{
		ReadList<FrameEntity, EntityRecord, Exact<EntityRecord>>( r, f.entities, b != nullptr ? &b->entities : nullptr, sameEntities, kMaxEntities );
		ReadList<FrameRagdoll, RagdollRecord, Exact<RagdollRecord>>( r, f.ragdolls, b != nullptr ? &b->ragdolls : nullptr, sameRagdolls, kMaxRagdolls );
	}
	// An entity names its ragdoll by index: never past the list.
	for ( FrameEntity& e : f.entities )
	{
		if ( e.ragdoll != UINT32_MAX && e.ragdoll >= f.ragdolls.size() )
		{
			e.ragdoll = UINT32_MAX;
		}
	}
	if ( f.tickSeconds <= 0.0f || f.tickSeconds > 1.0f )
	{
		return false;
	}
	return r.Ok() && r.AtEnd();
}

} // namespace cb::present
