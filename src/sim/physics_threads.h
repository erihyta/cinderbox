#pragma once

// The physics step on several threads.
//
// Box3D splits a step into tasks and hands them to a task system. Every world is made with the same
// number of workers (kPhysicsWorkers: what a step's work is divided by, the same on every machine),
// and how many threads actually run them is a machine's own business: none (each task runs where
// it is handed over, as before), or a pool shared by every simulation of the process.
//
// A task runs inside its world's arena (PhysicsArena::Scope), on whatever thread takes it. Tasks
// allocate, so the arena locks; in what order they do is not the same from run to run, and so
// neither is where things lie in the arena. Nothing reads that: a snapshot copies the arena as it
// is, the hash is of the ECS, and the image a joining client gets holds no addresses.

#include <cstdint>

namespace cb
{

class PhysicsArena;

// How many workers every world is made with.
constexpr uint32_t kPhysicsWorkers = 4;

// One per simulation, at an address that does not change (the world keeps a pointer to it, and a
// snapshot keeps the world).
struct PhysicsTasks
{
	PhysicsArena* arena = nullptr;
	bool threaded = false; // false: tasks run where they are handed over
};

class PhysicsThreads
{
public:
	// Threads that take tasks, besides the one that steps (so 3 makes a step run on four). 0 stops
	// them. Safe to call again; not while a world is stepping.
	static void Start( int threads );
	static int Count();
	// What a game or a server starts when it is not told: a quarter of the machine's threads, at
	// most one less than the workers (0 on a small machine: the threads look for work all through a
	// step, and must not take the processor from the thread that steps).
	static int Suggested();
	// A step is about to begin: sleeping threads get up now, so that they are there for its tasks.
	static void Wake();

	// What a world is made with (b3WorldDef::enqueueTask, finishTask; userTaskContext: a PhysicsTasks).
	static void* Enqueue( void ( *task )( void* ), void* taskContext, void* userContext, const char* name );
	static void Finish( void* userTask, void* userContext );
};

} // namespace cb
