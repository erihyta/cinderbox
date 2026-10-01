#include "view_codec.h"

#include "bytes.h"

#include <algorithm>
#include <cstring>

namespace cb::present
{

namespace
{

constexpr uint32_t kMagic = 0x31564243; // "CBV1"

// What a decoder accepts at most; real frames are far below.
constexpr uint32_t kMaxEntities = 1u << 16;
constexpr uint32_t kMaxRagdolls = 1u << 12;
constexpr uint32_t kMaxStats = 256;
constexpr uint32_t kMaxTemplates = 1u << 12;

enum PacketFlag : uint8_t
{
	FlagWorld = 1 << 0,
	FlagMap = 1 << 1,
	FlagSchema = 1 << 2,
	FlagNames = 1 << 3,
	FlagStats = 1 << 4,
};

enum WorldFlag : uint8_t
{
	WorldRolledBack = 1 << 0,
	WorldHasInputs = 1 << 1,
	WorldSameEntities = 1 << 2,
	WorldSameRagdolls = 1 << 3,
};

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

template <typename Item, typename Record>
void WriteList( ByteWriter& w, const std::vector<Item>& items, const std::vector<Item>* base, bool sameIds )
{
	w.Write( uint32_t( items.size() ) );
	if ( sameIds == false )
	{
		for ( const Item& item : items )
		{
			w.Write( item.netId );
		}
	}
	// Which changed, then their deltas.
	std::vector<uint8_t> changed( ( items.size() + 7 ) / 8, 0 );
	std::vector<uint8_t> deltas;
	ByteWriter dw( deltas );
	Record zero;
	std::memset( &zero, 0, sizeof( zero ) );
	for ( size_t i = 0; i < items.size(); ++i )
	{
		Record now = ToRecord( items[i] );
		const Item* before = FindBase( base, sameIds, i, items[i].netId );
		Record was = before != nullptr ? ToRecord( *before ) : zero;
		if ( std::memcmp( &now, &was, sizeof( Record ) ) != 0 )
		{
			changed[i / 8] |= uint8_t( 1u << ( i % 8 ) );
			WriteDelta( dw, &now, &was, sizeof( Record ) );
		}
	}
	w.WriteBytes( changed.data(), changed.size() );
	w.WriteBytes( deltas.data(), deltas.size() );
}

template <typename Item, typename Record>
void ReadList( ByteReader& r, std::vector<Item>& items, const std::vector<Item>* base, bool sameIds, uint32_t most )
{
	uint32_t count = r.Read<uint32_t>();
	if ( count > most || ( sameIds && ( base == nullptr || base->size() != count ) ) )
	{
		r.Fail();
		items.clear();
		return;
	}
	std::vector<uint32_t> ids( count );
	for ( uint32_t i = 0; i < count; ++i )
	{
		ids[i] = sameIds ? ( *base )[i].netId : r.Read<uint32_t>();
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
			ReadDelta( r, &record, sizeof( Record ) );
		}
		record.netId = ids[i];
		items[i] = FromRecord( record );
	}
}

} // namespace

void EncodeView( const ViewFrame& frame, const ViewFrame* base, double ageSeconds, std::vector<uint8_t>& out )
{
	out.clear();
	ByteWriter w( out );

	bool map = base == nullptr || base->mapHash != frame.mapHash || base->mapName != frame.mapName ||
			   base->templateNames != frame.templateNames || base->templateVisuals != frame.templateVisuals;
	bool schema = base == nullptr || base->schemaGeneration != frame.schemaGeneration;
	bool names = base == nullptr || base->namesGeneration != frame.namesGeneration;
	bool stats = base == nullptr || base->stats != frame.stats;
	uint8_t flags = uint8_t( ( frame.hasWorld ? FlagWorld : 0 ) | ( map ? FlagMap : 0 ) | ( schema ? FlagSchema : 0 ) |
							 ( names ? FlagNames : 0 ) | ( stats ? FlagStats : 0 ) );

	w.Write( kMagic );
	w.Write( frame.serial );
	w.Write( uint64_t( base != nullptr ? base->serial : 0 ) );
	w.Write( float( ageSeconds ) );
	w.Write( frame.alphaAtPublish );
	w.Write( frame.rate );
	w.Write( frame.localPressed );
	w.Write( flags );
	WriteString( w, frame.state );

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
	w.Write( uint8_t( ( f.rolledBack ? WorldRolledBack : 0 ) | ( f.hasInputs ? WorldHasInputs : 0 ) |
					  ( sameEntities ? WorldSameEntities : 0 ) | ( sameRagdolls ? WorldSameRagdolls : 0 ) ) );

	WriteBlock( w, f.board, was.board, sizeof( f.board ) );
	WriteBlock( w, f.inputs.data(), was.inputs.data(), sizeof( PlayerInput ) * kMaxPlayers );

	// The event rings, as the fixed blocks they are in the simulation (a frame without them: zeros).
	ImpactRecord impacts[kImpactHistory] = {};
	ImpactRecord impactsWas[kImpactHistory] = {};
	std::copy_n( f.impacts.begin(), std::min<size_t>( f.impacts.size(), kImpactHistory ), impacts );
	std::copy_n( was.impacts.begin(), std::min<size_t>( was.impacts.size(), kImpactHistory ), impactsWas );
	WriteBlock( w, impacts, impactsWas, sizeof( impacts ) );
	ModEventRecord events[kModEventHistory] = {};
	ModEventRecord eventsWas[kModEventHistory] = {};
	std::copy_n( f.modEvents.begin(), std::min<size_t>( f.modEvents.size(), kModEventHistory ), events );
	std::copy_n( was.modEvents.begin(), std::min<size_t>( was.modEvents.size(), kModEventHistory ), eventsWas );
	WriteBlock( w, events, eventsWas, sizeof( events ) );

	WriteList<FrameEntity, EntityRecord>( w, f.entities, b != nullptr ? &b->entities : nullptr, sameEntities );
	WriteList<FrameRagdoll, RagdollRecord>( w, f.ragdolls, b != nullptr ? &b->ragdolls : nullptr, sameRagdolls );
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
	uint8_t flags = r.Read<uint8_t>();
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
	ReadBlock( r, f.board, sizeof( f.board ) );
	ReadBlock( r, f.inputs.data(), sizeof( PlayerInput ) * kMaxPlayers );

	ImpactRecord impacts[kImpactHistory] = {};
	ModEventRecord events[kModEventHistory] = {};
	if ( b != nullptr )
	{
		std::copy_n( b->impacts.begin(), std::min<size_t>( b->impacts.size(), kImpactHistory ), impacts );
		std::copy_n( b->modEvents.begin(), std::min<size_t>( b->modEvents.size(), kModEventHistory ), events );
	}
	ReadBlock( r, impacts, sizeof( impacts ) );
	ReadBlock( r, events, sizeof( events ) );
	f.impacts.assign( impacts, impacts + kImpactHistory );
	f.modEvents.assign( events, events + kModEventHistory );

	ReadList<FrameEntity, EntityRecord>( r, f.entities, b != nullptr ? &b->entities : nullptr, sameEntities, kMaxEntities );
	ReadList<FrameRagdoll, RagdollRecord>( r, f.ragdolls, b != nullptr ? &b->ragdolls : nullptr, sameRagdolls, kMaxRagdolls );
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
