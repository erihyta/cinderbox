#pragma once

// Client-side prediction with rollback.
//
// The server streams authoritative InputFrames in tick order. The client simulates ahead of them,
// predicting remote players by repeating their last authoritative input and using its own local
// input immediately. When an authoritative frame differs from the frame that was predicted for
// that tick, the session restores the snapshot taken before that tick and re-simulates.

#include "simulation.h"

#include <deque>

namespace cb
{

class RollbackSession
{
public:
	struct Stats
	{
		uint64_t rollbacks = 0;
		uint64_t resimulatedTicks = 0;
		uint64_t stalls = 0;
		uint32_t lastRollbackDepth = 0;
	};

	RollbackSession( const SimConfig& config, PlayerSlot localSlot, uint32_t maxRollbackTicks = 8 );

	// Start from a server-provided state (join or desync recovery). Drops all history.
	// The snapshot must come from this session's Simulation (e.g. Save() right after LoadPortable()).
	// `previousInputs` are the inputs of frame snapshot.tick - 1; they seed remote-player prediction.
	void Reset( const Snapshot& snapshot, const std::array<PlayerInput, kMaxPlayers>& previousInputs );

	// Authoritative frames must arrive in order starting at ConfirmedTick(). Others are ignored.
	void AddAuthoritativeFrame( const InputFrame& frame );

	// Fix up mispredictions. Call once per rendered frame, before AdvanceOne().
	void Reconcile();

	// True if another tick can be predicted without exceeding the rollback window.
	bool CanAdvance() const;

	// Simulate CurrentTick() with the local player's input. Returns false (and does nothing) when stalled.
	bool AdvanceOne( const PlayerInput& localInput );

	// Next tick to be simulated.
	uint32_t CurrentTick() const
	{
		return m_sim->Tick();
	}

	// Every frame with tick < ConfirmedTick() is authoritative and known.
	uint32_t ConfirmedTick() const
	{
		return m_confirmedEnd;
	}

	// Hash of the fully-confirmed state at `tick` (the state in which Tick() == tick), if still known.
	bool GetConfirmedHash( uint32_t tick, uint64_t& hash ) const;

	Simulation& Sim()
	{
		return *m_sim;
	}
	const Stats& GetStats() const
	{
		return m_stats;
	}
	PlayerSlot LocalSlot() const
	{
		return m_localSlot;
	}

private:
	struct TickRecord
	{
		uint32_t tick = UINT32_MAX;
		bool hasSnapshot = false;
		uint64_t hashBefore = 0; // hash of the state before this tick was simulated
		Snapshot snapshot;		 // state before this tick (only while it could still be rolled back)
		InputFrame used;		 // frame that was simulated for this tick
	};

	TickRecord& Record( uint32_t tick );
	const InputFrame* FindAuthoritative( uint32_t tick ) const;
	InputFrame BuildFrame( uint32_t tick, const PlayerInput& localInput ) const;
	void SimulateTick( const InputFrame& frame, bool keepExistingSnapshot );
	void FinalizeStates();
	void TrimHistory();

	SimConfig m_config;
	PlayerSlot m_localSlot;
	uint32_t m_maxRollback;
	std::unique_ptr<Simulation> m_sim;

	std::vector<TickRecord> m_records; // ring indexed by tick % size
	std::deque<InputFrame> m_authFrames;
	uint32_t m_authBase = 0; // tick of m_authFrames.front()
	uint32_t m_confirmedEnd = 0;
	uint32_t m_rollbackFrom = UINT32_MAX;
	uint32_t m_finalized = 0; // states with tick <= this are final and hashed

	struct ConfirmedHash
	{
		uint32_t tick = UINT32_MAX;
		uint64_t hash = 0;
	};
	std::vector<ConfirmedHash> m_confirmedHashes; // ring indexed by tick % size

	Stats m_stats;
};

} // namespace cb
