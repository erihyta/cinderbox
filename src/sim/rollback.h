#pragma once

// Client-side prediction with rollback.
//
// The server streams authoritative InputFrames in tick order. The client simulates ahead of them,
// predicting remote players by repeating their last authoritative input and using its own local
// input immediately. When an authoritative frame differs from the frame that was predicted for
// that tick, the session restores a snapshot from before that tick and re-simulates.
//
// Snapshots are the costly part of a tick after the step itself, so few are taken: one before each
// tick predicted for the first time, and, in a re-simulation, one where the confirmed frames end
// (where the next misprediction will most likely be). A re-simulation starts from the nearest one.

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
		// Where the time goes (nanoseconds, and how many of each).
		uint64_t steps = 0, stepNs = 0;
		uint64_t saves = 0, saveNs = 0;
		uint64_t loads = 0, loadNs = 0;
		uint64_t hashes = 0, hashNs = 0; // a hash without a snapshot
	};

	// `maxRollbackCap` bounds SetMaxRollback() and sizes the snapshot ring.
	// `map` must be the server's map (the client receives it on join).
	RollbackSession( const SimConfig& config, PlayerSlot localSlot, uint32_t maxRollbackTicks = 8, uint32_t maxRollbackCap = 0,
					 const LevelLayout& map = GetLevelLayout() );

	// How far prediction may run ahead of confirmed frames. Clamped to [1, cap].
	void SetMaxRollback( uint32_t ticks );
	uint32_t MaxRollback() const
	{
		return m_maxRollback;
	}

	// A misprediction is fixed at once (1, the default), or only when this many ticks have passed
	// since the last fix: the same ticks are then re-simulated less often, and what other players
	// do is shown up to that many ticks later. Clamped to [1, kMaxReconcileEvery].
	static constexpr uint32_t kMaxReconcileEvery = 8;
	void SetReconcileEvery( uint32_t ticks );

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

	// Every state with tick <= FinalizedTick() is final: nothing will change it.
	uint32_t FinalizedTick() const
	{
		return m_finalized;
	}

	// Hash of the fully-confirmed state at `tick` (the state in which Tick() == tick), if known:
	// not every final state is hashed (one predicted in a re-simulation and confirmed as it was is
	// not), and old ones are forgotten.
	bool GetConfirmedHash( uint32_t tick, uint64_t& hash ) const;

	Simulation& Sim()
	{
		return *m_sim;
	}
	// The frame the most recent tick was simulated with (predicted or authoritative), if kept.
	const InputFrame* LastSimulatedFrame() const;
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
		bool hasHash = false;
		uint64_t hashBefore = 0; // hash of the state before this tick was simulated (hasHash)
		Snapshot snapshot;		 // state before this tick (hasSnapshot)
		InputFrame used;		 // frame that was simulated for this tick
	};

	TickRecord& Record( uint32_t tick );
	const InputFrame* FindAuthoritative( uint32_t tick ) const;
	InputFrame BuildFrame( uint32_t tick, const PlayerInput& localInput ) const;
	enum class Keep
	{
		Predicting,	 // a tick simulated for the first time
		Start,		 // the first tick of a re-simulation: its snapshot is the one just loaded
		Resimulated, // a later tick of a re-simulation
	};
	void SimulateTick( const InputFrame& frame, Keep keep );
	void FinalizeStates();
	void TrimHistory();

	SimConfig m_config;
	PlayerSlot m_localSlot;
	uint32_t m_maxRollback;
	uint32_t m_maxRollbackCap;
	std::unique_ptr<Simulation> m_sim;

	std::vector<TickRecord> m_records; // ring indexed by tick % size
	std::deque<InputFrame> m_authFrames;
	uint32_t m_authBase = 0; // tick of m_authFrames.front()
	uint32_t m_confirmedEnd = 0;
	uint32_t m_rollbackFrom = UINT32_MAX;
	uint32_t m_reconcileEvery = 1;
	uint32_t m_reconciledAt = 0; // the tick the last misprediction was fixed at
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
