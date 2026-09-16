#pragma once

// Box3D allocates through a global hook. We route every allocation to the arena of the world that
// is currently being operated on, so the complete physics state of a world is:
//   [arena bytes in use] + [the b3World struct in Box3D's static world table].
// Saving and restoring that is a pair of memcpys, and exact (contact caches, warm starting, ids).
//
// Rules:
// - Wrap every Box3D call in a PhysicsArena::Scope for the owning world.
// - The arena never moves, so pointers inside it stay valid across restores.
// - Physics must be single-threaded (worker count 1).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cb
{

class PhysicsArena
{
public:
	explicit PhysicsArena( size_t capacityBytes );
	~PhysicsArena();

	PhysicsArena( const PhysicsArena& ) = delete;
	PhysicsArena& operator=( const PhysicsArena& ) = delete;

	void* Allocate( size_t size, size_t alignment );
	void Free( void* ptr );

	// Bytes that a snapshot has to copy.
	size_t UsedBytes() const;
	size_t Capacity() const
	{
		return m_capacity;
	}

	void Save( std::vector<uint8_t>& out ) const;
	void Restore( const uint8_t* data, size_t size );

	// Sets the arena used by Box3D allocations on this thread for the scope's lifetime.
	class Scope
	{
	public:
		explicit Scope( PhysicsArena& arena );
		~Scope();
		Scope( const Scope& ) = delete;
		Scope& operator=( const Scope& ) = delete;

	private:
		PhysicsArena* m_previous;
	};

	// Installs the Box3D allocator hooks. Called automatically by the first arena.
	static void InstallAllocator();

private:
	struct Header;
	Header* GetHeader() const;

	uint8_t* m_base = nullptr;
	size_t m_capacity = 0;
};

} // namespace cb
