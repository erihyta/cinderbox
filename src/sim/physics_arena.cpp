#include "physics_arena.h"

#include "box3d/base.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined( _WIN32 )
#include <malloc.h>
#endif

namespace cb
{

namespace
{

constexpr size_t kBlockAlign = 64;
constexpr int kClassCount = 48;
constexpr uint32_t kMagic = 0xA7E4A7E4u;

// Lives just before every payload.
struct BlockHeader
{
	uint32_t sizeClass;
	uint32_t payloadOffset;
	uint32_t magic;
	uint32_t reserved;
};

thread_local PhysicsArena* t_current = nullptr;

[[noreturn]] void Fatal( const char* message )
{
	std::fprintf( stderr, "PhysicsArena: %s\n", message );
	std::fflush( stderr );
	std::abort();
}

void* AlignedAlloc( size_t size )
{
#if defined( _WIN32 )
	return _aligned_malloc( size, kBlockAlign );
#else
	return std::aligned_alloc( kBlockAlign, size );
#endif
}

void AlignedFree( void* p )
{
#if defined( _WIN32 )
	_aligned_free( p );
#else
	std::free( p );
#endif
}

int SizeClassFor( size_t bytes )
{
	int c = 6; // 64 bytes minimum
	while ( ( size_t( 1 ) << c ) < bytes )
	{
		++c;
	}
	return c;
}

void* BoxAlloc( int32_t size, int32_t alignment )
{
	if ( t_current == nullptr )
	{
		Fatal( "Box3D allocation outside of a PhysicsArena::Scope" );
	}
	return t_current->Allocate( size_t( size ), size_t( alignment ) );
}

void BoxFree( void* mem )
{
	if ( t_current == nullptr )
	{
		Fatal( "Box3D free outside of a PhysicsArena::Scope" );
	}
	t_current->Free( mem );
}

} // namespace

// Stored at the start of the arena, so snapshots include the allocator state.
struct PhysicsArena::Header
{
	uint64_t top; // offset of the first never-used byte
	uint64_t freeHeads[kClassCount]; // offset of the first free block per class, 0 = empty
	uint64_t liveBytes;
};

PhysicsArena::PhysicsArena( size_t capacityBytes )
{
	InstallAllocator();

	m_capacity = ( capacityBytes + kBlockAlign - 1 ) & ~( kBlockAlign - 1 );
	m_base = static_cast<uint8_t*>( AlignedAlloc( m_capacity ) );
	if ( m_base == nullptr )
	{
		Fatal( "out of memory reserving the arena" );
	}
	std::memset( m_base, 0, m_capacity );

	Header* h = GetHeader();
	h->top = ( sizeof( Header ) + kBlockAlign - 1 ) & ~( kBlockAlign - 1 );
}

PhysicsArena::~PhysicsArena()
{
	if ( t_current == this )
	{
		t_current = nullptr;
	}
	AlignedFree( m_base );
}

PhysicsArena::Header* PhysicsArena::GetHeader() const
{
	return reinterpret_cast<Header*>( m_base );
}

void* PhysicsArena::Allocate( size_t size, size_t alignment )
{
	if ( alignment > kBlockAlign )
	{
		Fatal( "alignment above 64 is not supported" );
	}

	Header* h = GetHeader();
	size_t payloadOffset = alignment <= sizeof( BlockHeader ) ? sizeof( BlockHeader ) : kBlockAlign;
	int sizeClass = SizeClassFor( size + payloadOffset );
	if ( sizeClass >= kClassCount )
	{
		Fatal( "allocation too large" );
	}
	size_t blockSize = size_t( 1 ) << sizeClass;

	uint64_t blockOffset = h->freeHeads[sizeClass];
	if ( blockOffset != 0 )
	{
		uint64_t next;
		std::memcpy( &next, m_base + blockOffset, sizeof( next ) );
		h->freeHeads[sizeClass] = next;
	}
	else
	{
		blockOffset = h->top;
		if ( blockOffset + blockSize > m_capacity )
		{
			Fatal( "arena exhausted, raise SimConfig::physicsArenaMB" );
		}
		h->top += blockSize;
	}

	uint8_t* payload = m_base + blockOffset + payloadOffset;
	BlockHeader bh = { uint32_t( sizeClass ), uint32_t( payloadOffset ), kMagic, 0 };
	std::memcpy( payload - sizeof( BlockHeader ), &bh, sizeof( bh ) );
	h->liveBytes += blockSize;
	return payload;
}

void PhysicsArena::Free( void* ptr )
{
	if ( ptr == nullptr )
	{
		return;
	}

	uint8_t* payload = static_cast<uint8_t*>( ptr );
	if ( payload < m_base || payload >= m_base + m_capacity )
	{
		Fatal( "freeing memory that belongs to another arena" );
	}

	BlockHeader bh;
	std::memcpy( &bh, payload - sizeof( BlockHeader ), sizeof( bh ) );
	if ( bh.magic != kMagic )
	{
		Fatal( "corrupt block header (double free?)" );
	}

	Header* h = GetHeader();
	uint64_t blockOffset = uint64_t( payload - m_base ) - bh.payloadOffset;

	// Clear the magic so a double free is caught, then link the block.
	bh.magic = 0;
	std::memcpy( payload - sizeof( BlockHeader ), &bh, sizeof( bh ) );
	uint64_t next = h->freeHeads[bh.sizeClass];
	std::memcpy( m_base + blockOffset, &next, sizeof( next ) );
	h->freeHeads[bh.sizeClass] = blockOffset;
	h->liveBytes -= size_t( 1 ) << bh.sizeClass;
}

size_t PhysicsArena::UsedBytes() const
{
	return size_t( GetHeader()->top );
}

void PhysicsArena::Save( std::vector<uint8_t>& out ) const
{
	size_t used = UsedBytes();
	out.resize( used );
	std::memcpy( out.data(), m_base, used );
}

void PhysicsArena::Restore( const uint8_t* data, size_t size )
{
	if ( size > m_capacity || size < sizeof( Header ) )
	{
		Fatal( "restore size mismatch" );
	}
	std::memcpy( m_base, data, size );
}

PhysicsArena::Scope::Scope( PhysicsArena& arena )
	: m_previous( t_current )
{
	t_current = &arena;
}

PhysicsArena::Scope::~Scope()
{
	t_current = m_previous;
}

void PhysicsArena::InstallAllocator()
{
	static bool installed = false;
	if ( installed == false )
	{
		b3SetAllocator( BoxAlloc, BoxFree );
		installed = true;
	}
}

} // namespace cb
