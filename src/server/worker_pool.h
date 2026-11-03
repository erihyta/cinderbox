#pragma once

// A few threads for the server's work that splits by client (M250: each client's characters'
// state), apart from the physics' (PhysicsThreads: a step's tasks, inside its arena). Nothing here
// is the simulation's: what is computed on them is the same without them.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cb
{

class WorkerPool
{
public:
	WorkerPool() = default;
	~WorkerPool();
	WorkerPool( const WorkerPool& ) = delete;
	WorkerPool& operator=( const WorkerPool& ) = delete;

	// Threads besides the caller's (0: Run does everything where it is called). Not during a Run.
	void Start( int threads );
	int Threads() const
	{
		return int( m_threads.size() );
	}
	// fn( item, worker ) for every item in [0, count): `worker` is in [0, Threads()] (the caller's
	// is Threads()), so a job can keep something of its own per worker. Returns when all are done.
	void Run( size_t count, const std::function<void( size_t, int )>& fn );

private:
	void Work( int worker );
	void Take( int worker );

	std::vector<std::thread> m_threads;
	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::condition_variable m_done;
	const std::function<void( size_t, int )>* m_job = nullptr;
	size_t m_count = 0;
	std::atomic<size_t> m_next{ 0 };
	int m_busy = 0;			 // workers not yet done with this job
	uint64_t m_generation = 0; // which job the workers are on
	bool m_stop = false;
};

} // namespace cb
