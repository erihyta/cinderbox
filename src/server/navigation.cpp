#include "navigation.h"

#include "simulation.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cb
{

namespace
{

// The triangles of what does not move, in world space.
struct Soup
{
	std::vector<float> vertices;
	std::vector<int> triangles;

	void Add( b3Vec3 a, b3Vec3 b, b3Vec3 c )
	{
		const int base = int( vertices.size() / 3 );
		for ( const b3Vec3& v : { a, b, c } )
		{
			vertices.push_back( v.x );
			vertices.push_back( v.y );
			vertices.push_back( v.z );
		}
		triangles.push_back( base );
		triangles.push_back( base + 1 );
		triangles.push_back( base + 2 );
	}
	// A box (an AABB: a sphere's, a capsule's), its faces outward.
	void AddBox( b3Vec3 lo, b3Vec3 hi )
	{
		auto at = [&]( int i ) { return b3Vec3{ ( i & 1 ) ? hi.x : lo.x, ( i & 2 ) ? hi.y : lo.y, ( i & 4 ) ? hi.z : lo.z }; };
		// Each face as two triangles, counter-clockwise seen from outside.
		const int faces[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };
		for ( const auto& f : faces )
		{
			Add( at( f[0] ), at( f[1] ), at( f[2] ) );
			Add( at( f[0] ), at( f[2] ), at( f[3] ) );
		}
	}
};

void AddShape( b3ShapeId shape, const b3WorldTransform& at, Soup& out )
{
	switch ( b3Shape_GetType( shape ) )
	{
		case b3_hullShape:
		{
			const b3HullData* hull = b3Shape_GetHull( shape );
			const b3Vec3* points = b3GetHullPoints( hull );
			const b3HullHalfEdge* edges = b3GetHullEdges( hull );
			const b3HullFace* faces = b3GetHullFaces( hull );
			for ( int f = 0; f < hull->faceCount; ++f )
			{
				// The face's corners in order (counter-clockwise seen from outside), as a fan.
				std::vector<b3Vec3> corners;
				int e = faces[f].edge;
				for ( int guard = 0; guard < 64; ++guard )
				{
					corners.push_back( b3TransformWorldPoint( at, points[edges[e].origin] ) );
					e = edges[e].next;
					if ( e == faces[f].edge )
					{
						break;
					}
				}
				for ( size_t i = 2; i < corners.size(); ++i )
				{
					out.Add( corners[0], corners[i - 1], corners[i] );
				}
			}
			break;
		}
		case b3_meshShape:
		{
			const b3Mesh mesh = b3Shape_GetMesh( shape );
			const b3Vec3* vertices = b3GetMeshVertices( mesh.data );
			const b3MeshTriangle* triangles = b3GetMeshTriangles( mesh.data );
			for ( int t = 0; t < mesh.data->triangleCount; ++t )
			{
				auto v = [&]( int i ) { return b3TransformWorldPoint( at, b3Mul( mesh.scale, vertices[i] ) ); };
				out.Add( v( triangles[t].index1 ), v( triangles[t].index2 ), v( triangles[t].index3 ) );
			}
			break;
		}
		case b3_heightShape:
		{
			const b3HeightFieldData* hf = b3Shape_GetHeightField( shape );
			const uint16_t* heights = b3GetHeightFieldCompressedHeights( hf );
			const uint8_t* flags = b3GetHeightFieldFlags( hf );
			auto corner = [&]( int row, int column ) {
				const float h = hf->minHeight + hf->heightScale * float( heights[row * hf->columnCount + column] );
				return b3TransformWorldPoint( at, b3Mul( hf->scale, b3Vec3{ float( column ), h, float( row ) } ) );
			};
			for ( int row = 0; row + 1 < hf->rowCount; ++row )
			{
				for ( int column = 0; column + 1 < hf->columnCount; ++column )
				{
					const int cell = row * ( hf->columnCount - 1 ) + column;
					const b3Vec3 c0 = corner( row, column ), c1 = corner( row, column + 1 ), c2 = corner( row + 1, column ),
								 c3 = corner( row + 1, column + 1 );
					// (Up-facing: Recast walks a triangle whose normal points up.)
					if ( flags[2 * cell] != B3_HEIGHT_FIELD_HOLE )
					{
						out.Add( c0, c2, c1 );
					}
					if ( flags[2 * cell + 1] != B3_HEIGHT_FIELD_HOLE )
					{
						out.Add( c1, c2, c3 );
					}
				}
			}
			break;
		}
		case b3_sphereShape:
		case b3_capsuleShape:
		default:
		{
			// (A round thing in the level: its box is near enough for where one can walk.)
			const b3AABB box = b3Shape_GetAABB( shape );
			out.AddBox( box.lowerBound, box.upperBound );
			break;
		}
	}
}

} // namespace

NavMesh::NavMesh() = default;

NavMesh::~NavMesh()
{
	dtFreeNavMeshQuery( m_query );
	dtFreeNavMesh( m_mesh );
}

bool NavMesh::Build( Simulation& sim, const NavAgent& agent, std::string& error )
{
	Soup soup;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		const flecs::entity e( sim.World(), r.entity );
		const PhysicsBody* pb = e.try_get<PhysicsBody>();
		if ( pb == nullptr || e.has<Character>() )
		{
			continue;
		}
		const b3BodyId body = sim.BodyOf( *pb );
		if ( b3Body_GetType( body ) != b3_staticBody || b3Body_IsEnabled( body ) == false )
		{
			continue;
		}
		const b3WorldTransform at = b3Body_GetTransform( body );
		b3ShapeId shapes[64];
		const int count = b3Body_GetShapes( body, shapes, 64 );
		for ( int i = 0; i < count; ++i )
		{
			AddShape( shapes[i], at, soup );
		}
	}
	m_triangles = soup.triangles.size() / 3;
	if ( m_triangles == 0 )
	{
		error = "nothing static to walk on";
		return false;
	}

	rcConfig cfg{};
	cfg.cs = 0.2f;
	cfg.ch = 0.1f;
	cfg.walkableSlopeAngle = agent.maxSlope;
	cfg.walkableHeight = int( std::ceil( agent.height / cfg.ch ) );
	cfg.walkableClimb = int( std::floor( agent.climb / cfg.ch ) );
	cfg.walkableRadius = int( std::ceil( agent.radius / cfg.cs ) );
	cfg.maxEdgeLen = int( 12.0f / cfg.cs );
	cfg.maxSimplificationError = 1.3f;
	cfg.minRegionArea = 8 * 8;
	cfg.mergeRegionArea = 20 * 20;
	cfg.maxVertsPerPoly = 6;
	cfg.detailSampleDist = 6.0f * cfg.cs;
	cfg.detailSampleMaxError = 1.0f * cfg.ch;
	const int vertexCount = int( soup.vertices.size() / 3 );
	const int triangleCount = int( m_triangles );
	rcCalcBounds( soup.vertices.data(), vertexCount, cfg.bmin, cfg.bmax );
	rcCalcGridSize( cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height );

	rcContext ctx( false );
	auto fail = [&]( const char* what ) {
		error = what;
		return false;
	};
	struct Freed
	{
		rcHeightfield* solid = nullptr;
		rcCompactHeightfield* compact = nullptr;
		rcContourSet* contours = nullptr;
		rcPolyMesh* polys = nullptr;
		rcPolyMeshDetail* detail = nullptr;
		~Freed()
		{
			rcFreeHeightField( solid );
			rcFreeCompactHeightfield( compact );
			rcFreeContourSet( contours );
			rcFreePolyMesh( polys );
			rcFreePolyMeshDetail( detail );
		}
	} work;
	work.solid = rcAllocHeightfield();
	if ( rcCreateHeightfield( &ctx, *work.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch ) == false )
	{
		return fail( "the level is too big for one navigation mesh" );
	}
	std::vector<unsigned char> areas( size_t( triangleCount ), 0 );
	rcMarkWalkableTriangles( &ctx, cfg.walkableSlopeAngle, soup.vertices.data(), vertexCount, soup.triangles.data(), triangleCount, areas.data() );
	if ( rcRasterizeTriangles( &ctx, soup.vertices.data(), vertexCount, soup.triangles.data(), areas.data(), triangleCount, *work.solid,
							   cfg.walkableClimb ) == false )
	{
		return fail( "Recast could not rasterize the level" );
	}
	rcFilterLowHangingWalkableObstacles( &ctx, cfg.walkableClimb, *work.solid );
	rcFilterLedgeSpans( &ctx, cfg.walkableHeight, cfg.walkableClimb, *work.solid );
	rcFilterWalkableLowHeightSpans( &ctx, cfg.walkableHeight, *work.solid );
	work.compact = rcAllocCompactHeightfield();
	if ( rcBuildCompactHeightfield( &ctx, cfg.walkableHeight, cfg.walkableClimb, *work.solid, *work.compact ) == false ||
		 rcErodeWalkableArea( &ctx, cfg.walkableRadius, *work.compact ) == false || rcBuildDistanceField( &ctx, *work.compact ) == false ||
		 rcBuildRegions( &ctx, *work.compact, 0, cfg.minRegionArea, cfg.mergeRegionArea ) == false )
	{
		return fail( "Recast could not find the walkable regions" );
	}
	work.contours = rcAllocContourSet();
	work.polys = rcAllocPolyMesh();
	work.detail = rcAllocPolyMeshDetail();
	if ( rcBuildContours( &ctx, *work.compact, cfg.maxSimplificationError, cfg.maxEdgeLen, *work.contours ) == false ||
		 rcBuildPolyMesh( &ctx, *work.contours, cfg.maxVertsPerPoly, *work.polys ) == false ||
		 rcBuildPolyMeshDetail( &ctx, *work.polys, *work.compact, cfg.detailSampleDist, cfg.detailSampleMaxError, *work.detail ) == false )
	{
		return fail( "Recast could not build the navigation mesh" );
	}
	m_polygons = size_t( work.polys->npolys );
	if ( m_polygons == 0 )
	{
		return fail( "nowhere to walk" );
	}
	for ( int i = 0; i < work.polys->npolys; ++i )
	{
		work.polys->flags[i] = work.polys->areas[i] == RC_WALKABLE_AREA ? 1 : 0;
	}

	dtNavMeshCreateParams params{};
	params.verts = work.polys->verts;
	params.vertCount = work.polys->nverts;
	params.polys = work.polys->polys;
	params.polyAreas = work.polys->areas;
	params.polyFlags = work.polys->flags;
	params.polyCount = work.polys->npolys;
	params.nvp = work.polys->nvp;
	params.detailMeshes = work.detail->meshes;
	params.detailVerts = work.detail->verts;
	params.detailVertsCount = work.detail->nverts;
	params.detailTris = work.detail->tris;
	params.detailTriCount = work.detail->ntris;
	params.walkableHeight = agent.height;
	params.walkableRadius = agent.radius;
	params.walkableClimb = agent.climb;
	rcVcopy( params.bmin, work.polys->bmin );
	rcVcopy( params.bmax, work.polys->bmax );
	params.cs = cfg.cs;
	params.ch = cfg.ch;
	params.buildBvTree = true;
	unsigned char* data = nullptr;
	int size = 0;
	if ( dtCreateNavMeshData( &params, &data, &size ) == false )
	{
		return fail( "Detour could not make the navigation mesh" );
	}
	m_mesh = dtAllocNavMesh();
	if ( dtStatusFailed( m_mesh->init( data, size, DT_TILE_FREE_DATA ) ) )
	{
		dtFree( data );
		return fail( "Detour could not load the navigation mesh" );
	}
	m_query = dtAllocNavMeshQuery();
	if ( dtStatusFailed( m_query->init( m_mesh, 4096 ) ) )
	{
		dtFreeNavMeshQuery( m_query );
		m_query = nullptr;
		return fail( "Detour could not query the navigation mesh" );
	}
	return true;
}

bool NavMesh::Nearest( b3Vec3 point, b3Vec3& out ) const
{
	if ( m_query == nullptr )
	{
		return false;
	}
	dtQueryFilter filter;
	const float extents[3] = { 2.0f, 4.0f, 2.0f };
	dtPolyRef ref = 0;
	float nearest[3];
	const float at[3] = { point.x, point.y, point.z };
	if ( dtStatusFailed( m_query->findNearestPoly( at, extents, &filter, &ref, nearest ) ) || ref == 0 )
	{
		return false;
	}
	out = { nearest[0], nearest[1], nearest[2] };
	return true;
}

bool NavMesh::FindPath( b3Vec3 from, b3Vec3 to, std::vector<b3Vec3>& corners ) const
{
	corners.clear();
	if ( m_query == nullptr )
	{
		return false;
	}
	dtQueryFilter filter;
	const float extents[3] = { 2.0f, 4.0f, 2.0f };
	const float a[3] = { from.x, from.y, from.z }, b[3] = { to.x, to.y, to.z };
	dtPolyRef startRef = 0, endRef = 0;
	float start[3], end[3];
	if ( dtStatusFailed( m_query->findNearestPoly( a, extents, &filter, &startRef, start ) ) || startRef == 0 ||
		 dtStatusFailed( m_query->findNearestPoly( b, extents, &filter, &endRef, end ) ) || endRef == 0 )
	{
		return false;
	}
	constexpr int kMaxPolys = 512;
	dtPolyRef path[kMaxPolys];
	int count = 0;
	m_query->findPath( startRef, endRef, start, end, &filter, path, &count, kMaxPolys );
	if ( count == 0 )
	{
		return false;
	}
	// Not all the way (another island): to the nearest point of the last polygon reached.
	float goal[3] = { end[0], end[1], end[2] };
	if ( path[count - 1] != endRef )
	{
		m_query->closestPointOnPoly( path[count - 1], end, goal, nullptr );
	}
	constexpr int kMaxCorners = 128;
	float straight[kMaxCorners * 3];
	unsigned char flags[kMaxCorners];
	dtPolyRef refs[kMaxCorners];
	int corner = 0;
	m_query->findStraightPath( start, goal, path, count, straight, flags, refs, &corner, kMaxCorners );
	for ( int i = 0; i < corner; ++i )
	{
		corners.push_back( { straight[3 * i], straight[3 * i + 1], straight[3 * i + 2] } );
	}
	return corners.empty() == false;
}

} // namespace cb
