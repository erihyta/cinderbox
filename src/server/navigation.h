#pragma once

// Finding the way (ROADMAP.md, step 9): a navigation mesh of what does not move, built on the
// server when the game starts (Recast), and paths along it (Detour), for a mod's brains. Server
// only: nothing here is part of the simulation, so it need not be deterministic; what a mod does
// with a path (the input it drives a body with) is what every simulation runs.

#include "box3d/math_functions.h"

#include <memory>
#include <string>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;

namespace cb
{

class Simulation;

// The body the mesh is for: where it fits (its radius and height), the step it climbs, the slope
// it walks up. The server's own character's capsule.
struct NavAgent
{
	float radius = 0.3f;
	float height = 2.18f; // from its feet to the top of its capsule
	float climb = 0.45f;
	float maxSlope = 50.0f; // degrees
};

class NavMesh
{
public:
	NavMesh();
	~NavMesh();
	NavMesh( const NavMesh& ) = delete;
	NavMesh& operator=( const NavMesh& ) = delete;

	// From every static body of the simulation's physics world (the level, its hulls, meshes and
	// terrain, static templates' bodies: a wall's standing pieces): their triangles. False, with
	// `error`, when there is nothing to walk on or Recast refuses.
	bool Build( Simulation& sim, const NavAgent& agent, std::string& error );
	bool Ready() const
	{
		return m_query != nullptr;
	}

	// The corners of the way from `from` to `to` (both near the mesh, feet heights): the first is
	// `from` on the mesh, the last `to` or the nearest point to it that can be reached. False when
	// neither end is near the mesh.
	bool FindPath( b3Vec3 from, b3Vec3 to, std::vector<b3Vec3>& corners ) const;
	// The nearest point on the mesh to `point` (within a few metres), false for none.
	bool Nearest( b3Vec3 point, b3Vec3& out ) const;

	size_t Triangles() const
	{
		return m_triangles;
	}
	size_t Polygons() const
	{
		return m_polygons;
	}

private:
	dtNavMesh* m_mesh = nullptr;
	dtNavMeshQuery* m_query = nullptr;
	size_t m_triangles = 0;
	size_t m_polygons = 0;
};

} // namespace cb
