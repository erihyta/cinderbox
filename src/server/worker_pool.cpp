#include "worker_pool.h"

namespace cb
{

WorkerPool::~WorkerPool()
{
	Start( 0 );
}

void WorkerPool::Start( int threads )
{
	threads = threads < 0 ? 0 : threads;
	if ( threads == Threads() )
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		m_stop = true;
	}
	m_wake.notify_all();
	for ( std::thread& t : m_threads )
	{
		t.join();
	}
	m_threads.clear();
	m_stop = false;
	for ( int i = 0; i < threads; ++i )
	{
		m_threads.emplace_back( [this, i] { Work( i ); } );
	}
}

void WorkerPool::Take( int worker )
{
	for ( ;; )
	{
		const size_t item = m_next.fetch_add( 1, std::memory_order_relaxed );
		if ( item >= m_count )
		{
			return;
		}
		( *m_job )( item, worker );
	}
}

void WorkerPool::Run( size_t count, const std::function<void( size_t, int )>& fn )
{
	if ( m_threads.empty() || count < 2 )
	{
		for ( size_t i = 0; i < count; ++i )
		{
			fn( i, Threads() );
		}
		return;
	}
	{
		std::lock_guard<std::mutex> lock( m_mutex );
		m_job = &fn;
		m_count = count;
		m_next.store( 0, std::memory_order_relaxed );
		m_busy = Threads();
		m_generation += 1;
	}
	m_wake.notify_all();
	Take( Threads() );
	std::unique_lock<std::mutex> lock( m_mutex );
	m_done.wait( lock, [this] { return m_busy == 0; } );
	m_job = nullptr;
}

void WorkerPool::Work( int worker )
{
	uint64_t seen = 0;
	for ( ;; )
	{
		{
			std::unique_lock<std::mutex> lock( m_mutex );
			m_wake.wait( lock, [&] { return m_stop || m_generation != seen; } );
			if ( m_stop )
			{
				return;
			}
			seen = m_generation;
		}
		Take( worker );
		{
			std::lock_guard<std::mutex> lock( m_mutex );
			m_busy -= 1;
		}
		m_done.notify_one();
	}
}

} // namespace cb
