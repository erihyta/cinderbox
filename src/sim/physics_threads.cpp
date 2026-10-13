#include "physics_threads.h"

#include "physics_arena.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined( _MSC_VER ) || defined( __x86_64__ ) || defined( __i386__ )
#include <immintrin.h>
#define CB_PAUSE() _mm_pause()
#else
#define CB_PAUSE() std::this_thread::yield()
#endif

namespace cb
{

namespace
{

struct Task
{
	void ( *run )( void* ) = nullptr;
	void* context = nullptr;
	PhysicsArena* arena = nullptr;
	std::atomic<bool> done{ false };
};

// How long a thread with nothing to do looks for work before it sleeps. A step's tasks come in
// bursts with a fraction of a millisecond between them, and waking a sleeping thread takes about as
// long as a whole burst: measured, threads that slept between bursts made the physics twice as
// slow as no threads at all. So a step wakes them when it begins (Wake) and they stay up through it.
constexpr auto kSpin = std::chrono::microseconds( 1000 );

struct Pool
{
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<Task*> queue;
	std::vector<Task*> spare;
	std::atomic<int> waiting{ 0 }; // tasks in the queue
	std::atomic<int> sleeping{ 0 };
	std::atomic<int> threadCount{ 0 };
	bool stop = false;
	bool roused = false; // Wake: one sleeper gets up though nothing waits yet (and rouses the next)
	std::vector<std::thread> threads;

	~Pool()
	{
		Stop();
		for ( Task* t : spare )
		{
			delete t;
		}
	}

	void Stop()
	{
		{
			std::lock_guard<std::mutex> lock( mutex );
			stop = true;
		}
		wake.notify_all();
		for ( std::thread& t : threads )
		{
			t.join();
		}
		threads.clear();
		threadCount.store( 0 );
		stop = false;
	}

	Task* Take()
	{
		if ( waiting.load( std::memory_order_acquire ) == 0 )
		{
			return nullptr;
		}
		std::lock_guard<std::mutex> lock( mutex );
		if ( queue.empty() )
		{
			return nullptr;
		}
		Task* t = queue.front();
		queue.pop_front();
		waiting.fetch_sub( 1, std::memory_order_release );
		return t;
	}

	static void Run( Task* t )
	{
		{
			PhysicsArena::Scope scope( *t->arena );
			t->run( t->context );
		}
		t->done.store( true, std::memory_order_release );
	}

	void Work()
	{
		for ( ;; )
		{
			if ( Task* t = Take() )
			{
				Run( t );
				continue;
			}

			auto until = std::chrono::steady_clock::now() + kSpin;
			bool found = false;
			while ( std::chrono::steady_clock::now() < until )
			{
				for ( int i = 0; i < 64 && found == false; ++i )
				{
					CB_PAUSE();
					found = waiting.load( std::memory_order_acquire ) > 0;
				}
				if ( found )
				{
					break;
				}
			}
			if ( found )
			{
				continue;
			}

			std::unique_lock<std::mutex> lock( mutex );
			sleeping.fetch_add( 1 );
			wake.wait( lock, [this] { return stop || queue.empty() == false || roused; } );
			roused = false;
			sleeping.fetch_sub( 1 );
			if ( stop )
			{
				return;
			}
		}
	}
};

Pool& ThePool()
{
	static Pool pool;
	return pool;
}

} // namespace

void PhysicsThreads::Start( int threads )
{
	Pool& pool = ThePool();
	threads = threads < 0 ? 0 : threads;
	if ( threads == pool.threadCount.load() )
	{
		return;
	}
	pool.Stop();
	for ( int i = 0; i < threads; ++i )
	{
		pool.threads.emplace_back( [&pool] { pool.Work(); } );
	}
	pool.threadCount.store( threads );
}

void PhysicsThreads::Wake()
{
	Pool& pool = ThePool();
	if ( pool.sleeping.load( std::memory_order_relaxed ) > 0 )
	{
		{
			std::lock_guard<std::mutex> lock( pool.mutex );
			pool.roused = true;
		}
		pool.wake.notify_all();
	}
}

int PhysicsThreads::Suggested()
{
	int quarter = int( std::thread::hardware_concurrency() / 4 );
	return quarter < int( kPhysicsWorkers ) - 1 ? quarter : int( kPhysicsWorkers ) - 1;
}

int PhysicsThreads::Count()
{
	return ThePool().threadCount.load();
}

void* PhysicsThreads::Enqueue( void ( *task )( void* ), void* taskContext, void* userContext, const char* )
{
	PhysicsTasks* tasks = static_cast<PhysicsTasks*>( userContext );
	Pool& pool = ThePool();
	if ( tasks == nullptr || tasks->threaded == false || pool.threadCount.load( std::memory_order_relaxed ) == 0 )
	{
		// Here and now (the caller is inside its world's arena already). Nothing to wait for.
		task( taskContext );
		return nullptr;
	}

	Task* t;
	{
		std::lock_guard<std::mutex> lock( pool.mutex );
		if ( pool.spare.empty() )
		{
			t = new Task;
		}
		else
		{
			t = pool.spare.back();
			pool.spare.pop_back();
		}
		t->run = task;
		t->context = taskContext;
		t->arena = tasks->arena;
		t->done.store( false, std::memory_order_relaxed );
		pool.queue.push_back( t );
		pool.waiting.fetch_add( 1, std::memory_order_release );
	}
	if ( pool.sleeping.load() > 0 )
	{
		pool.wake.notify_one();
	}
	return t;
}

void PhysicsThreads::Finish( void* userTask, void* )
{
	Task* t = static_cast<Task*>( userTask );
	Pool& pool = ThePool();
	// Until it is done, do what waits (this very task, if nobody took it yet).
	while ( t->done.load( std::memory_order_acquire ) == false )
	{
		if ( Task* other = pool.Take() )
		{
			Pool::Run( other );
		}
		else
		{
			CB_PAUSE();
		}
	}
	std::lock_guard<std::mutex> lock( pool.mutex );
	pool.spare.push_back( t );
}

} // namespace cb
