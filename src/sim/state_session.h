#pragma once

// Client-side prediction for the state network model (ROADMAP step 3), beside RollbackSession
// (the inputs model's).
//
// The server sends the state of its world now and then (Simulation::SaveState). The client takes
// the newest it has (Simulation::ApplyState: everything but its own body and what that carries is
// a stand-in, placed where the server has it), then runs its own inputs since that state's tick
// again, up to where it was. Nothing is compared and nothing is kept to go back to: a state is the
// truth, and the simulation is only ever ahead of it by the client's own inputs.

#include "simulation.h"

#include <array>
#include <vector>

namespace cb
{

class StateSession
{
public:
	struct Stats
	{
		uint64_t states = 0;	   // taken and applied
		uint64_t skipped = 0;	   // a newer one came before it was applied
		uint64_t rejected = 0;	   // not a state of this simulation's kind
		uint64_t resimulatedTicks = 0;
		uint64_t stalls = 0;
		uint64_t steps = 0, stepNs = 0;
		uint64_t applyNs = 0;
		uint32_t lastDepth = 0; // ticks run again after the last state
	};

	// `maxAhead`: how far the simulation may run past the newest state.
	StateSession( const SimConfig& config, PlayerSlot localSlot, uint32_t maxAhead, const LevelLayout& map = GetLevelLayout() );

	void SetMaxAhead( uint32_t ticks );
	uint32_t MaxAhead() const
	{
		return m_maxAhead;
	}

	// A state of the server's world, the one in which its Tick() == `tick`, and the inputs it used
	// for the tick before (the other players' guesses from now on; presentation aims arms with
	// them). Kept until Reconcile(); one older than the newest kept is dropped.
	void AddState( uint32_t tick, const uint8_t* data, size_t size, const std::array<PlayerInput, kMaxPlayers>& inputs );

	// Takes the newest state kept, and runs the local inputs since its tick again.
	void Reconcile();

	// Whether another tick may be predicted (at most MaxAhead() past the newest state).
	bool CanAdvance() const;
	// Simulates CurrentTick() with the local player's input. False (nothing done): too far ahead,
	// or no state yet.
	bool AdvanceOne( const PlayerInput& localInput );

	uint32_t CurrentTick() const
	{
		return m_sim->Tick();
	}
	// The tick of the newest state applied (0: none yet).
	uint32_t StateTick() const
	{
		return m_stateTick;
	}
	bool HasState() const
	{
		return m_hasState;
	}
	Simulation& Sim()
	{
		return *m_sim;
	}
	PlayerSlot LocalSlot() const
	{
		return m_localSlot;
	}
	// The frame the most recent tick was simulated with.
	const InputFrame* LastSimulatedFrame() const
	{
		return m_last.tick + 1 == m_sim->Tick() ? &m_last : nullptr;
	}
	const Stats& GetStats() const
	{
		return m_stats;
	}

private:
	void Simulate( const PlayerInput& localInput );

	SimConfig m_config;
	PlayerSlot m_localSlot;
	uint32_t m_maxAhead;
	std::unique_ptr<Simulation> m_sim;

	// The local inputs sampled, by tick % size.
	struct Sampled
	{
		uint32_t tick = UINT32_MAX;
		PlayerInput input{};
	};
	std::vector<Sampled> m_sampled;
	std::array<PlayerInput, kMaxPlayers> m_others{};

	bool m_hasPending = false;
	uint32_t m_pendingTick = 0;
	std::vector<uint8_t> m_pending;
	std::array<PlayerInput, kMaxPlayers> m_pendingInputs{};

	bool m_hasState = false;
	uint32_t m_stateTick = 0;
	InputFrame m_last;
	Stats m_stats;
};

} // namespace cb
