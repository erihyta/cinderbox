#include "physics_arena.h"

#include "box3d/base.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <mutex>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <sys/mman.h>
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

constexpr size_t kCommitChunk = 16u * 1024u * 1024u;

// Page-aligned address space from the OS. Memory is committed in chunks as the arena grows
// (CommitPages), so many simulations can each reserve a large arena cheaply. Fresh pages are zero.
void* ReservePages( size_t size )
{
#if defined( _WIN32 )
	return VirtualAlloc( nullptr, size, MEM_RESERVE, PAGE_NOACCESS );
#else
	void* p = mmap( nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0 );
	return p == MAP_FAILED ? nullptr : p;
#endif
}

bool CommitPages( void* p, size_t size )
{
#if defined( _WIN32 )
	return VirtualAlloc( p, size, MEM_COMMIT, PAGE_READWRITE ) != nullptr;
#else
	(void)p;
	(void)size;
	return true; // mmap pages are committed lazily by the kernel
#endif
}

void ReleasePages( void* p, size_t size )
{
#if defined( _WIN32 )
	(void)size;
	VirtualFree( p, 0, MEM_RELEASE );
#else
	munmap( p, size );
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
	// Page size (4 KiB+) is a multiple of the 64-byte block alignment.
	m_base = static_cast<uint8_t*>( ReservePages( m_capacity ) );
	if ( m_base == nullptr )
	{
		Fatal( "out of address space reserving the arena" );
	}
	EnsureCommitted( kCommitChunk );

	Header* h = GetHeader();
	h->top = ( sizeof( Header ) + kBlockAlign - 1 ) & ~( kBlockAlign - 1 );
}

PhysicsArena::~PhysicsArena()
{
	if ( t_current == this )
	{
		t_current = nullptr;
	}
	ReleasePages( m_base, m_capacity );
}

void PhysicsArena::EnsureCommitted( size_t bytes )
{
	if ( bytes <= m_committed )
	{
		return;
	}
	size_t target = std::min( m_capacity, ( bytes + kCommitChunk - 1 ) / kCommitChunk * kCommitChunk );
	if ( CommitPages( m_base + m_committed, target - m_committed ) == false )
	{
		Fatal( "out of memory committing arena pages" );
	}
	m_committed = target;
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
		EnsureCommitted( size_t( blockOffset + blockSize ) );
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
	EnsureCommitted( size );
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
	static std::once_flag once;
	std::call_once( once, [] { b3SetAllocator( BoxAlloc, BoxFree ); } );
}

} // namespace cb
