#pragma once

// What sources with work of their own share (client/live_source.h, client/replay_source.h):
// a thread, and the newest frame handed to the viewer's thread.
//
// A dedicated thread keeps a source's spikes (a rollback, a seek, a burst of packets) off the
// render thread. For the sources that simulate it also owns the floating-point environment the
// simulation runs in: whatever the viewer's engine does to its own threads' FP control register
// cannot change simulation results.

#include "view.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace cb
{

// A build fingerprint the way the "fingerprint" stat shows it (hex).
std::string FingerprintText( uint64_t fingerprint );

class ThreadedSource : public present::ViewSource
{
public:
	bool Take( present::ViewFrame& out ) override;

protected:
	// A derived destructor calls StopThread() first: the thread runs its Run().
	~ThreadedSource() override;

	void StartThread();
	void StopThread();
	bool Stopping() const
	{
		return m_stop.load();
	}

	// On the thread, with the simulation's floating-point environment set. Returns when Stopping().
	virtual void Run() = 0;

	// The frame being built (thread only). It holds what was published two frames ago, so the
	// session fields only need writing when they changed.
	present::ViewFrame& Building()
	{
		return m_building;
	}
	// Hands Building() to the viewer. A rollback and presses the viewer has not taken yet are kept.
	void Publish( bool rolledBack, ActionBits localPressed = 0 );
	// Whether the thread got the floating-point environment the simulation needs.
	bool FpEnvironmentOk() const
	{
		return m_fpOk;
	}

private:
	void ThreadMain();

	std::thread m_thread;
	std::atomic<bool> m_stop{ false };
	bool m_fpOk = true;

	std::mutex m_frameMutex;
	present::ViewFrame m_latest;
	bool m_latestTaken = true;
	present::ViewFrame m_building;
};

} // namespace cb
