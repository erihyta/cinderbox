#pragma once

// A client of the hybrid network model (M248; ROADMAP step 3), beside RollbackSession (the inputs
// model's).
//
// One simulation, two clocks. Its physics runs in the past, at PhysicsTick(): each tick stepped
// once, when the server's physics inputs for it have come (its commands, physics events and
// pushers' places: AddFrame), and checked against the server's physics checksum (AddChecksum).
// Nothing is rolled back. The other characters are the server's, placed from its characters'
// states (AddState) at the physics' tick, so one standing on debris stands on it here too. The
// own character runs ahead, at PredictedTick(), by the local inputs (Predict), against the physics
// world as it is; each new state makes it the server's again and runs the inputs since once more
// (Reconcile).

#include "simulation.h"

#include <array>
#include <map>
#include <memory>
#include <vector>

namespace cb
{

class HybridSession
{
public:
	struct Stats
	{
		uint64_t physicsSteps = 0, physicsNs = 0;
		uint64_t physicsStalls = 0; // a physics tick waited for its frame
		uint64_t states = 0;		// characters' states taken
		uint64_t rejected = 0;		// not a state of this simulation's kind
		uint64_t predicted = 0, predictNs = 0;
		uint64_t resimulatedTicks = 0;
		uint64_t predictStalls = 0; // too far ahead of the newest state
		uint64_t applyNs = 0;
		uint64_t checksums = 0, desyncs = 0;
		uint32_t lastDepth = 0; // ticks predicted again after the last state
	};

	// `maxAhead`: how far the prediction may run past the newest state.
	HybridSession( const SimConfig& config, PlayerSlot localSlot, uint32_t maxAhead, const LevelLayout& map = GetLevelLayout() );

	// The world as the server had it when this client joined (its snapshot loaded into Sim()); the
	// physics and the prediction both start at its tick.
	void Start();
	bool Started() const
	{
		return m_started;
	}

	// The server's physics inputs for frame.tick (the players' joins and leaves are not used: the
	// characters come in states). Kept until the physics reaches it; older ones are dropped.
	void AddFrame( const InputFrame& frame );
	// The server's characters' state in which its Tick() == `tick` (Simulation::SaveState), and the
	// inputs it used for the tick before (presentation aims the others with them).
	void AddState( uint32_t tick, const uint8_t* data, size_t size, const std::array<PlayerInput, kMaxPlayers>& inputs = {} );
	// The server's PhysicsHash() after it stepped `tick`.
	void AddChecksum( uint32_t tick, uint64_t hash );

	// Steps the physics up to `upTo` (exclusive), as far as the frames have come. The ticks stepped.
	uint32_t AdvancePhysics( uint32_t upTo );
	// Whether a frame for the physics' next tick is here.
	bool HasNextFrame() const;
	// The newest frame's tick + 1 (0: none yet).
	uint32_t FramesReached() const
	{
		return m_framesReached;
	}

	// Takes the newest state not yet taken for the own character, and predicts its inputs since again.
	void Reconcile();
	bool CanPredict() const;
	// The own character does PredictedTick() with `input`. False (nothing done): too far ahead.
	bool Predict( const PlayerInput& input );

	uint32_t PhysicsTick() const
	{
		return m_sim->Tick();
	}
	uint32_t PredictedTick() const
	{
		return m_predicted;
	}
	// The newest state's tick (0: none yet).
	uint32_t StateTick() const
	{
		return m_newest;
	}
	// The tick of the state the others stand as (UINT32_MAX: the joining snapshot's).
	uint32_t PlacedTick() const
	{
		return m_placed;
	}
	// The first physics tick whose checksum differed (UINT32_MAX: none).
	uint32_t DesyncTick() const
	{
		return m_desyncTick;
	}
	Simulation& Sim()
	{
		return *m_sim;
	}
	const Simulation& Sim() const
	{
		return *m_sim;
	}
	PlayerSlot LocalSlot() const
	{
		return m_localSlot;
	}
	// The inputs presentation aims with: the others' from the state they stand as, the own one's
	// last prediction (null: nothing predicted yet).
	const InputFrame* LastSimulatedFrame() const
	{
		return m_last.tick != UINT32_MAX ? &m_last : nullptr;
	}
	const Stats& GetStats() const
	{
		return m_stats;
	}

private:
	// The others as the newest state not ahead of `tick` has them (`again`: even when they already
	// stood as that one: a command may have moved them since).
	void PlaceOthers( uint32_t tick, bool again );

	SimConfig m_config;
	PlayerSlot m_localSlot;
	uint32_t m_maxAhead;
	std::unique_ptr<Simulation> m_sim;
	bool m_started = false;

	std::map<uint32_t, InputFrame> m_frames;
	uint32_t m_framesReached = 0;
	struct State
	{
		std::vector<uint8_t> bytes;
		std::array<PlayerInput, kMaxPlayers> inputs{};
	};
	std::map<uint32_t, State> m_states;
	std::array<PlayerInput, kMaxPlayers> m_othersInputs{};
	uint32_t m_newest = 0;		  // the newest state's tick
	uint32_t m_placed = UINT32_MAX; // the state the others stand as
	uint32_t m_reconciled = 0;	  // the state the own character was last made
	std::map<uint32_t, uint64_t> m_checksums;
	uint32_t m_desyncTick = UINT32_MAX;

	uint32_t m_predicted = 0;
	struct Sampled
	{
		uint32_t tick = UINT32_MAX;
		PlayerInput input{};
	};
	std::vector<Sampled> m_sampled;
	InputFrame m_last;
	std::vector<uint8_t> m_own; // (scratch: the own character around a physics step)
	Stats m_stats;
};

} // namespace cb
