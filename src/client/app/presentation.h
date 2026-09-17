#pragma once

// raylib rendering of the client presentation (see src/present/mirror.h for the logic).

#include "mirror.h"
#include "simulation.h"

#include "raylib.h"

#include <memory>

namespace cb::present
{

// What to show: the client's predicted simulation, or a replay.
struct SimView
{
	Simulation* sim = nullptr;
	uint64_t resetGeneration = 0; // changes when the world was replaced (snap instead of smooth)
	float tickAlpha = 0.0f;
	bool rolledBack = false;
	bool hasLocalPlayer = false;
	PlayerSlot localSlot = 0;
};

// Draws an evaluated pose as one box per bone. `feet` is where the skeleton origin goes.
void DrawSkeleton( Vector3 feet, Quaternion rotation, float scale, const anim::PoseEvaluator* eval, Color color );

class Presentation
{
public:
	explicit Presentation( std::shared_ptr<const anim::AnimSet> animSet );

	// Capture the simulation, update the mirror and run the presentation scripts.
	void Update( const SimView& view, float frameSeconds );

	void Render();

	// Interpolated position of the local player's capsule center, if it exists.
	bool LocalPlayerPosition( Vector3& out ) const;

	Mirror& GetMirror()
	{
		return m_mirror;
	}

private:
	Mirror m_mirror;
	PresentationFrame m_frame;
};

} // namespace cb::present
