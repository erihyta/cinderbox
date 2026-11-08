#include "source_thread.h"

#if defined( __x86_64__ ) || defined( _M_X64 )
#include <xmmintrin.h>
#endif

#include <cstdio>
#include <utility>

namespace cb
{

namespace
{

// Default SSE control state: all exceptions masked, round to nearest, no flush-to-zero and no
// denormals-are-zero. The simulation is only bit-exact under this environment.
constexpr unsigned kDefaultMxcsr = 0x1F80;

bool ResetFloatingPointEnvironment()
{
#if defined( __x86_64__ ) || defined( _M_X64 )
	_mm_setcsr( kDefaultMxcsr );
	return ( _mm_getcsr() & 0xFFC0 ) == ( kDefaultMxcsr & 0xFFC0 );
#else
	return true;
#endif
}

} // namespace

std::string FingerprintText( uint64_t fingerprint )
{
	char text[32];
	std::snprintf( text, sizeof( text ), "%llx", static_cast<unsigned long long>( fingerprint ) );
	return text;
}

ThreadedSource::~ThreadedSource()
{
	StopThread();
}

void ThreadedSource::StartThread()
{
	StopThread();
	m_stop = false;
	m_thread = std::thread( &ThreadedSource::ThreadMain, this );
}

void ThreadedSource::StopThread()
{
	if ( m_thread.joinable() )
	{
		m_stop = true;
		m_thread.join();
	}
}

void ThreadedSource::ThreadMain()
{
	m_fpOk = ResetFloatingPointEnvironment();
	Run();
}

bool ThreadedSource::Take( present::ViewFrame& out )
{
	std::lock_guard<std::mutex> lock( m_frameMutex );
	if ( m_latestTaken || m_latest.serial == out.serial )
	{
		return false;
	}
	// (Swapped, not copied: at 1023 players a frame is megabytes, 66 ms a copy. M259. What the viewer
	// had goes back to be filled again; a source keeps nothing in it that it does not check.)
	std::swap( out, m_latest );
	m_latestTaken = true;
	return true;
}

void ThreadedSource::Publish( bool rolledBack, ActionBits localPressed )
{
	present::ViewFrame& f = m_building;
	f.publishedAt = present::ViewClock();

	std::lock_guard<std::mutex> lock( m_frameMutex );
	// Neither must get lost if the viewer skipped the previous frame.
	bool skipped = m_latestTaken == false;
	f.frame.rolledBack = rolledBack || ( skipped && m_latest.frame.rolledBack );
	f.localPressed = localPressed | ( skipped ? m_latest.localPressed : 0 );
	f.serial = ++m_serial; // (m_latest may be the viewer's old frame: Take swaps)
	std::swap( m_latest, f );
	m_latestTaken = false;
}

} // namespace cb
