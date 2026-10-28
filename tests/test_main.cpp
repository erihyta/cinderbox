// Determinism and rollback tests.
//
//   cb_tests                    run all tests
//   cb_tests <name>             run one test
//   cb_tests --dump <file>      write per-tick hashes of the reference scenario (cross-build check)
//   cb_tests --compare <file>   compare against a dump from another build/platform
//   cb_tests --anim-hash         pose hash of the procedural rig (cross-build check)
//   cb_tests --anim-hash-parts   the same in parts: the layers alone, with the leg turn, with the aim and the look
//   cb_tests --save-portable <file> / --load-portable <file>   portable snapshot across builds

#include "anim_controller.h"
#include "anim_graph.h"
#include "anim_catch_up.h"
#include "anim_lead.h"
#include "map.h"
#include "pose.h"
#include "pose_tools.h"
#include "capture.h"
#include "camera.h"
#include "event_news.h"
#include "fields.h"
#include "hitboxes.h"
#include "detmath.h"
#include "expr.h"
#include "ragdoll.h"
#include "retarget.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "rollback.h"
#include "hybrid_session.h"
#include "scenario.h"
#include "simulation.h"

#include "box3d/box3d.h"
#include "mirror.h"
#include "mover.h"
#include "pose_tools.h"
#include "view_codec.h"
#include "view_file.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace cb;

namespace
{

int g_failures = 0;

#define CHECK( cond )                                                                                                            \
	do                                                                                                                           \
	{                                                                                                                            \
		if ( !( cond ) )                                                                                                         \
		{                                                                                                                        \
			std::printf( "    CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond );                                            \
			++g_failures;                                                                                                        \
			return;                                                                                                              \
		}                                                                                                                        \
	}                                                                                                                            \
	while ( 0 )

using Clock = std::chrono::steady_clock;

double MsSince( Clock::time_point start )
{
	return std::chrono::duration<double, std::milli>( Clock::now() - start ).count();
}

SimConfig TestConfig()
{
	SimConfig config;
	config.physicsArenaMB = 64;
	// What a server's mods would have declared: the scenario writes fields.
	config.fields = test::kScenarioFields;
	config.globalFields = test::kScenarioFields;
	config.motions = test::kScenarioMotions;
	config.layers = test::kScenarioLayers;
	return config;
}

// hashes[t] = hash of the state in which Tick() == t
std::vector<uint64_t> RunReference( const std::vector<InputFrame>& frames, const SimConfig& config )
{
	Simulation sim( config );
	std::vector<uint64_t> hashes;
	hashes.reserve( frames.size() + 1 );
	hashes.push_back( sim.ComputeHash() );
	for ( const InputFrame& f : frames )
	{
		sim.Step( f );
		hashes.push_back( sim.ComputeHash() );
	}
	return hashes;
}

bool SameLayout( const LevelLayout& a, const LevelLayout& b )
{
	auto sameVec = []( const b3Vec3& u, const b3Vec3& v ) { return u.x == v.x && u.y == v.y && u.z == v.z; };
	if ( a.name != b.name || a.statics.size() != b.statics.size() || a.props.size() != b.props.size() )
	{
		return false;
	}
	if ( sameVec( a.spawnCenter, b.spawnCenter ) == false || a.spawnRadius != b.spawnRadius )
	{
		return false;
	}
	for ( size_t i = 0; i < a.statics.size(); ++i )
	{
		const LevelBox& x = a.statics[i];
		const LevelBox& y = b.statics[i];
		if ( sameVec( x.center, y.center ) == false || sameVec( x.halfExtents, y.halfExtents ) == false || x.pitch != y.pitch ||
			 x.yaw != y.yaw || x.roll != y.roll || x.kind != y.kind || x.hull != y.hull )
		{
			return false;
		}
	}
	if ( a.hulls.size() != b.hulls.size() || a.shapeSets.size() != b.shapeSets.size() )
	{
		return false;
	}
	for ( size_t i = 0; i < a.shapeSets.size(); ++i )
	{
		if ( a.shapeSets[i].size() != b.shapeSets[i].size() )
		{
			return false;
		}
		for ( size_t p = 0; p < a.shapeSets[i].size(); ++p )
		{
			const LevelBox& x = a.shapeSets[i][p];
			const LevelBox& y = b.shapeSets[i][p];
			if ( sameVec( x.center, y.center ) == false || sameVec( x.halfExtents, y.halfExtents ) == false || x.pitch != y.pitch ||
				 x.yaw != y.yaw || x.roll != y.roll || x.kind != y.kind || x.hull != y.hull )
			{
				return false;
			}
		}
	}
	for ( size_t i = 0; i < a.hulls.size(); ++i )
	{
		if ( a.hulls[i].size() != b.hulls[i].size() )
		{
			return false;
		}
		for ( size_t p = 0; p < a.hulls[i].size(); ++p )
		{
			if ( sameVec( a.hulls[i][p], b.hulls[i][p] ) == false )
			{
				return false;
			}
		}
	}
	for ( size_t i = 0; i < a.props.size(); ++i )
	{
		const LevelProp& x = a.props[i];
		const LevelProp& y = b.props[i];
		if ( x.kind != y.kind || sameVec( x.position, y.position ) == false || sameVec( x.halfExtents, y.halfExtents ) == false ||
			 x.mass != y.mass || x.density != y.density || x.friction != y.friction || x.restitution != y.restitution ||
			 x.rolling != y.rolling || x.drag != y.drag )
		{
			return false;
		}
	}
	return true;
}

// A map must survive a bake -> load round trip bit-exactly, and a simulation built from the loaded
// map must be identical to one built from the layout in memory. Otherwise a client that received
// the map would desync from the server that baked it.
void TestMapFormat()
{
	const LevelLayout& builtin = GetLevelLayout();
	std::vector<uint8_t> bytes;
	SerializeMap( builtin, bytes );

	LevelLayout loaded;
	std::string error;
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) );
	CHECK( SameLayout( builtin, loaded ) );
	std::printf( "    built-in map: %zu bytes, %zu statics, %zu props, hash %016" PRIx64 "\n", bytes.size(),
				 loaded.statics.size(), loaded.props.size(), MapHash( bytes.data(), bytes.size() ) );

	// Re-baking the loaded map must produce the same file.
	std::vector<uint8_t> again;
	SerializeMap( loaded, again );
	CHECK( again == bytes );

	// Values off the storage grid (what an editor produces) must land on it, and stay there.
	LevelLayout authored;
	authored.name = "authored";
	authored.spawnCenter = { 1.0f / 3.0f, 0.7071067f, -12.3456f };
	authored.spawnRadius = 2.5f;
	authored.statics.push_back( { { 0.1234f, -0.5f, 9.87654f }, { 3.3333f, 0.25f, 1.7f }, 0.4567f, -1.2345f } );
	authored.props.push_back( { ShapeKind::Sphere, { -4.4444f, 1.1111f, 0.0f }, { 0.5f, 0.0f, 0.0f } } );
	// (And a body that is not the usual one: a weight of its own, slippery, bouncy.)
	authored.props.push_back( { ShapeKind::Box, { 2.0f, 1.0f, 0.0f }, { 0.5f, 0.5f, 0.5f }, 12.3456f, 77.7777f, 0.1234f, 0.8765f } );
	// (And a character's body it places: map version 12.)
	authored.bodies.push_back( { "critter", { 3.3333f, 0.0f, -2.5f }, 1.2345f } );
	QuantizeLayout( authored );
	std::vector<uint8_t> authoredBytes;
	SerializeMap( authored, authoredBytes );
	LevelLayout authoredBack;
	CHECK( DeserializeMap( authoredBytes.data(), authoredBytes.size(), authoredBack, error ) );
	CHECK( SameLayout( authored, authoredBack ) );
	CHECK( authoredBack.bodies.size() == 1 && authoredBack.bodies[0].character == "critter" &&
		   authoredBack.bodies[0].feet.x == authored.bodies[0].feet.x && authoredBack.bodies[0].yaw == authored.bodies[0].yaw );
	CHECK( authoredBack.props.size() == 2 && authoredBack.props[1].mass > 12.3f && authoredBack.props[1].restitution > 0.87f &&
		   authoredBack.props[0].mass == 0.0f && authoredBack.props[0].density == 40.0f );
	{
		// A body's numbers are checked like the rest: a bounce above 1 would make energy.
		LevelLayout bad = authored;
		bad.props[1].restitution = 1.5f;
		std::vector<uint8_t> badBytes;
		SerializeMap( bad, badBytes );
		LevelLayout refused;
		CHECK( DeserializeMap( badBytes.data(), badBytes.size(), refused, error ) == false );
	}

	// What a prop's body says is what the simulation builds: of two balls dropped from 5 m, the one
	// with bounce comes back up and the other stays down.
	{
		LevelLayout drop;
		drop.name = "bounce";
		drop.spawnCenter = { 0.0f, 1.5f, 8.0f };
		drop.spawnRadius = 2.0f;
		drop.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
		drop.props.push_back( { ShapeKind::Sphere, { -3.0f, 5.0f, 0.0f }, { 0.4f, 0.0f, 0.0f } } );
		drop.props.push_back( { ShapeKind::Sphere, { 3.0f, 5.0f, 0.0f }, { 0.4f, 0.0f, 0.0f }, 0.0f, 40.0f, 0.6f, 0.8f } );
		QuantizeLayout( drop );
		Simulation fall( TestConfig(), drop );
		uint32_t plain = 0, bouncy = 0;
		for ( const Simulation::EntityRef& r : fall.Entities() )
		{
			const Transform* at = fall.EntityTransform( r.netId );
			if ( at != nullptr && at->position.y > 4.0f )
			{
				( at->position.x < 0.0f ? plain : bouncy ) = r.netId;
			}
		}
		CHECK( plain != 0 && bouncy != 0 );
		InputFrame ff;
		bool landed[2] = { false, false };
		float rose[2] = { 0.0f, 0.0f };
		for ( uint32_t tick = 0; tick < 240 && plain != 0 && bouncy != 0; ++tick )
		{
			ff.tick = tick;
			fall.Step( ff );
			const uint32_t ids[2] = { plain, bouncy };
			for ( int i = 0; i < 2; ++i )
			{
				float y = fall.EntityTransform( ids[i] )->position.y;
				landed[i] |= y < 0.45f;
				rose[i] = landed[i] ? std::max( rose[i], y ) : rose[i];
			}
		}
		std::printf( "    dropped from 5 m: a ball without bounce comes back to %.2f m, one with 0.8 to %.2f m\n", rose[0], rose[1] );
		CHECK( landed[0] && landed[1] && rose[0] < 0.6f && rose[1] > 2.0f );
	}

	// Bad input is refused, never trusted.
	LevelLayout ignored;
	CHECK( DeserializeMap( nullptr, 0, ignored, error ) == false );
	std::vector<uint8_t> truncated( bytes.begin(), bytes.begin() + 40 );
	CHECK( DeserializeMap( truncated.data(), truncated.size(), ignored, error ) == false );
	std::vector<uint8_t> wrongVersion = bytes;
	wrongVersion[4] = 99;
	CHECK( DeserializeMap( wrongVersion.data(), wrongVersion.size(), ignored, error ) == false );

	// The real requirement: same map bytes => same simulation, tick for tick.
	auto frames = test::MakeScenario( { 300, 4, 6, 99, true } );
	SimConfig config = TestConfig();
	Simulation fromMemory( config, builtin );
	Simulation fromFile( config, loaded );
	for ( const InputFrame& f : frames )
	{
		fromMemory.Step( f );
		fromFile.Step( f );
		if ( fromMemory.ComputeHash() != fromFile.ComputeHash() )
		{
			std::printf( "    diverged at tick %u\n", f.tick );
			CHECK( false );
			break;
		}
	}

	// A different map must produce a different simulation, or the hash would not protect anything.
	LevelLayout moved = loaded;
	moved.statics[0].center.y += 0.5f;
	std::vector<uint8_t> movedBytes;
	SerializeMap( moved, movedBytes );
	CHECK( MapHash( movedBytes.data(), movedBytes.size() ) != MapHash( bytes.data(), bytes.size() ) );
	Simulation other( config, moved );
	CHECK( other.ComputeHash() != fromMemory.ComputeHash() );
}

// Builds a template the way the baker does: every field of a component the author added.
AuthoredComponent MakeComponent( const char* componentName, const std::vector<std::pair<const char*, float>>& values )
{
	AuthoredComponent component;
	component.id = Fnv32( componentName );
	for ( const auto& entry : values )
	{
		AuthoredField field;
		field.id = Fnv32( entry.first );
		field.raw[0] = MapQuantize( entry.second, kMapPositionScale );
		component.fields.push_back( field );
	}
	return component;
}

// Enums and ints are stored raw, not on the fixed-point grid.
AuthoredComponent MakeIntComponent( const char* componentName, const std::vector<std::pair<const char*, int32_t>>& values )
{
	AuthoredComponent component;
	component.id = Fnv32( componentName );
	for ( const auto& entry : values )
	{
		AuthoredField field;
		field.id = Fnv32( entry.first );
		field.raw[0] = entry.second;
		component.fields.push_back( field );
	}
	return component;
}

size_t CountFromTemplate( Simulation& sim, uint32_t index )
{
	size_t count = 0;
	for ( const auto& r : sim.Entities() )
	{
		const TemplateRef* ref = flecs::entity( sim.World(), r.entity ).try_get<TemplateRef>();
		count += ( ref != nullptr && ref->index == index ) ? 1 : 0;
	}
	return count;
}

float BallHeightAfter( float restitution, uint32_t ticks )
{
	LevelLayout map;
	map.name = "bounce";
	map.spawnCenter = { 0.0f, 1.5f, 6.0f };
	map.spawnRadius = 2.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );

	EntityTemplate ball;
	ball.name = "ball";
	ball.visual = "prop_bouncy";
	ball.components.push_back( MakeIntComponent( "Shape", { { "kind", int32_t( ShapeKind::Sphere ) } } ) );
	ball.components.push_back( MakeComponent( "Shape", { { "radius", 0.4f } } ) );
	// Two entries for the same component would be odd; merge them instead.
	ball.components[0].fields.push_back( ball.components[1].fields[0] );
	ball.components.pop_back();
	ball.components.push_back( MakeIntComponent( "Body", { { "type", 2 } } ) );
	ball.components.push_back( MakeComponent( "Material", { { "restitution", restitution }, { "friction", 0.2f } } ) );
	map.templates.push_back( ball );
	map.instances.push_back( { 0, { 0.0f, 6.0f, 0.0f }, 0.0f, 0.0f } );
	QuantizeLayout( map );

	SimConfig config = TestConfig();
	Simulation sim( config, map );
	InputFrame frame;
	for ( uint32_t t = 0; t < ticks; ++t )
	{
		frame.tick = t;
		sim.Step( frame );
	}

	for ( const auto& r : sim.Entities() )
	{
		flecs::entity e( sim.World(), r.entity );
		const TemplateRef* ref = e.try_get<TemplateRef>();
		if ( ref != nullptr && ref->index == 0 )
		{
			return e.get<Transform>().position.y;
		}
	}
	return -1.0f;
}

// Characters read the physics world and never write it themselves (M246): a player walks at a
// ball pushed by characters and at a crate that is solid. Through the ball, which its pusher sends
// off; stopped by the crate, which does not move. Its pusher stands in the world only while
// something pushable is near.
void TestPushers()
{
	auto walkAt = [&]( bool pushable, b3Vec3& bodyMoved, float& playerZ, float& bodyZ, size_t& pushed ) {
		LevelLayout map;
		map.name = "pushers";
		map.spawnCenter = { 5.25f, 1.5f, 6.0f }; // (slot 0 stands at x 0)
		map.spawnRadius = 2.0f;
		map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
		EntityTemplate body;
		body.name = pushable ? "ball" : "crate";
		if ( pushable )
		{
			body.components.push_back( MakeIntComponent( "Shape", { { "kind", int32_t( ShapeKind::Sphere ) } } ) );
			body.components[0].fields.push_back( MakeComponent( "Shape", { { "radius", 0.3f } } ).fields[0] );
			body.components.push_back( MakeComponent( "Pushable", {} ) );
		}
		else
		{
			body.components.push_back( MakeIntComponent( "Shape", { { "kind", int32_t( ShapeKind::Box ) } } ) ); // (1 m)
		}
		body.components.push_back( MakeIntComponent( "Body", { { "type", 2 } } ) );
		body.components.push_back( MakeComponent( "Material", { { "mass", pushable ? 0.5f : 20.0f } } ) );
		map.templates.push_back( body );
		map.instances.push_back( { 0, { 0.0f, pushable ? 0.3f : 0.5f, 3.0f }, 0.0f, 0.0f } );
		QuantizeLayout( map );

		Simulation sim( TestConfig(), map );
		InputFrame f;
		f.events.push_back( { PlayerEventType::Join, 0 } );
		b3Vec3 start = {};
		uint32_t bodyId = 0;
		pushed = 0;
		for ( uint32_t t = 0; t < 150; ++t )
		{
			f.tick = sim.Tick();
			f.inputs[0].cameraYaw = 32768; // toward -Z
			f.inputs[0].moveForward = t > 30 ? int8_t( 127 ) : int8_t( 0 );
			sim.Step( f );
			f.events.clear();
			if ( t == 0 )
			{
				for ( const auto& r : sim.Entities() )
				{
					if ( flecs::entity( sim.World(), r.entity ).has<TemplateRef>() )
					{
						bodyId = r.netId;
						start = sim.EntityTransform( bodyId )->position;
					}
				}
			}
			pushed += sim.PhysicsMade().pushers.size();
		}
		bodyMoved = b3Sub( sim.EntityTransform( bodyId )->position, start );
		bodyZ = start.z;
		playerZ = sim.EntityTransform( sim.PlayerNetId( 0 ) )->position.z;
	};

	b3Vec3 ballMoved, crateMoved;
	float playerZ = 0.0f, ballZ = 0.0f, crateZ = 0.0f, stoppedZ = 0.0f;
	size_t ballPushers = 0, cratePushers = 0;
	walkAt( true, ballMoved, playerZ, ballZ, ballPushers );
	walkAt( false, crateMoved, stoppedZ, crateZ, cratePushers );
	std::printf( "    a ball pushed by characters: the player walked through it to z %.2f (it stood at %.2f) and it went %.2f m, "
				 "a pusher %zu ticks; a solid crate: the player stopped at z %.2f (it stands at %.2f), it moved %.3f m, a pusher %zu ticks\n",
				 playerZ, ballZ, b3Length( ballMoved ), ballPushers, stoppedZ, crateZ, b3Length( crateMoved ), cratePushers );
	CHECK( playerZ < ballZ - 1.0f );
	CHECK( b3Length( ballMoved ) > 0.5f );
	CHECK( ballPushers > 0 && ballPushers < 150 );
	CHECK( stoppedZ > crateZ + 0.6f );
	CHECK( b3Length( crateMoved ) < 0.01f );
	CHECK( cratePushers == 0 );
}

// Level pieces of every shape, turned any way (map versions 4 and 5): a box rolled upright is a
// wall, a capsule rolled over lies along X, a sphere is round, a hull (a pyramid) slopes where its
// points say and turns over with its piece; the turn is Godot's (yaw, then pitch, then roll); they
// survive the file; a version 3 file (boxes, no roll) still loads; a bad shape, and a hull whose
// points lie in a plane, are refused.
void TestMapShapes()
{
	constexpr float kQuarter = 1.5707964f;
	LevelLayout map;
	map.name = "shapes";
	map.spawnCenter = { 0.0f, 1.5f, 10.0f };
	map.spawnRadius = 2.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
	// A slab 4 m long, rolled upright: a wall from y 0 to 4.
	map.statics.push_back( { { 0.0f, 2.0f, 0.0f }, { 2.0f, 0.1f, 0.5f }, 0.0f, 0.0f, kQuarter, ShapeKind::Box } );
	map.statics.push_back( { { 5.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Sphere } );
	// A capsule 4 m tip to tip, rolled over: lying along X, its top at y 1.5.
	map.statics.push_back( { { -6.0f, 1.0f, 0.0f }, { 0.5f, 1.5f, 0.0f }, 0.0f, 0.0f, kQuarter, ShapeKind::Capsule } );
	// Yawed and rolled a quarter each: its X turns to +Y, its Z to +X (as Godot's YXZ euler has it).
	map.statics.push_back( { { 0.0f, 1.0f, -8.0f }, { 1.0f, 0.2f, 0.3f }, 0.0f, kQuarter, kQuarter, ShapeKind::Box } );
	// A pyramid 2 m across and 2 m tall, and the same one upside down (rolled half a turn): both
	// pieces share the one point list.
	map.hulls.push_back( { { -1.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 1.0f }, { 0.0f, 2.0f, 0.0f } } );
	map.statics.push_back( { { 10.0f, 0.0f, 0.0f }, { 1.0f, 2.0f, 1.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Hull, 0 } );
	map.statics.push_back( { { 14.0f, 2.0f, 0.0f }, { 1.0f, 2.0f, 1.0f }, 0.0f, 0.0f, 2.0f * kQuarter, ShapeKind::Hull, 0 } );
	QuantizeLayout( map );

	std::vector<uint8_t> bytes;
	SerializeMap( map, bytes );
	LevelLayout loaded;
	std::string error;
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) );
	CHECK( SameLayout( map, loaded ) );

	Simulation sim( TestConfig(), loaded );
	auto down = [&]( float x, float z ) {
		RayHit hit;
		return sim.CastRay( { x, 6.0f, z }, { 0.0f, -10.0f, 0.0f }, 0, hit ) ? hit.point.y : -1.0f;
	};
	RayHit wall;
	bool walled = sim.CastRay( { -3.0f, 3.5f, 0.0f }, { 6.0f, 0.0f, 0.0f }, 0, wall );
	std::printf( "    a ray at 3.5 m meets the rolled slab at x %.3f; down onto the sphere %.3f, the lying capsule %.3f\n",
				 walled ? wall.point.x : 99.0f, down( 5.0f, 0.0f ), down( -7.2f, 0.0f ) );
	CHECK( walled && std::fabs( wall.point.x + 0.1f ) < 0.01f );
	CHECK( std::fabs( down( 5.0f, 0.0f ) - 2.0f ) < 0.01f );
	CHECK( std::fabs( down( -7.2f, 0.0f ) - 1.5f ) < 0.01f ); // standing, it would be 3 m
	std::printf( "    the pyramid's slope halfway out is at %.3f m; the one upside down is flat on top at %.3f m\n", down( 10.5f, 0.0f ),
				 down( 14.0f, 0.0f ) );
	CHECK( std::fabs( down( 10.5f, 0.0f ) - 1.0f ) < 0.01f );
	CHECK( std::fabs( down( 14.0f, 0.0f ) - 2.0f ) < 0.01f ); // its apex would be at 4 m

	const Transform* turned = nullptr;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		const Transform* at = sim.EntityTransform( r.netId );
		if ( at != nullptr && at->position.z < -7.0f )
		{
			turned = at;
		}
	}
	CHECK( turned != nullptr );
	b3Vec3 x = b3RotateVector( turned->rotation, { 1.0f, 0.0f, 0.0f } );
	b3Vec3 z = b3RotateVector( turned->rotation, { 0.0f, 0.0f, 1.0f } );
	CHECK( x.y > 0.999f && z.x > 0.999f );

	// Version 3: boxes, pitch and yaw. Read as boxes without roll.
	std::vector<uint8_t> old;
	auto u32 = [&]( uint32_t v ) {
		for ( int i = 0; i < 4; ++i )
		{
			old.push_back( uint8_t( v >> ( 8 * i ) ) );
		}
	};
	auto pos = [&]( float a, float b, float c ) {
		for ( float v : { a, b, c } )
		{
			u32( uint32_t( MapQuantize( v, kMapPositionScale ) ) );
		}
	};
	old = { 'C', 'B', 'M', 'P' };
	u32( 3 );
	u32( 0 );
	u32( 3 );
	old.insert( old.end(), { 'o', 'l', 'd' } );
	pos( 0.0f, 1.5f, 0.0f );
	u32( uint32_t( MapQuantize( 2.0f, kMapPositionScale ) ) );
	u32( 1 ); // one static
	pos( 0.0f, -0.5f, 0.0f );
	pos( 10.0f, 0.5f, 10.0f );
	u32( uint32_t( MapQuantize( 0.25f, kMapAngleScale ) ) );
	u32( uint32_t( MapQuantize( -0.5f, kMapAngleScale ) ) );
	u32( 0 ); // props
	u32( 0 ); // templates
	u32( 0 ); // instances
	u32( kNoTemplate );
	LevelLayout fromOld;
	CHECK( DeserializeMap( old.data(), old.size(), fromOld, error ) );
	CHECK( fromOld.statics.size() == 1 && fromOld.statics[0].kind == ShapeKind::Box && fromOld.statics[0].roll == 0.0f &&
		   fromOld.statics[0].pitch == 0.25f && fromOld.statics[0].yaw == -0.5f && fromOld.statics[0].halfExtents.x == 10.0f );

	// A shape this build does not know, and a box of no size, are refused.
	const size_t firstKind = 4 + 4 + 4 + 4 + map.name.size() + 12 + 4 + 4 + 4 + 5 * 12 + 4; // (past the hull)
	std::vector<uint8_t> unknown = bytes;
	CHECK( unknown[firstKind] == uint8_t( ShapeKind::Box ) );
	unknown[firstKind] = 7;
	LevelLayout refused;
	CHECK( DeserializeMap( unknown.data(), unknown.size(), refused, error ) == false );
	LevelLayout flat = map;
	flat.statics[0].halfExtents.y = 0.0f;
	SerializeMap( flat, unknown );
	CHECK( DeserializeMap( unknown.data(), unknown.size(), refused, error ) == false );
	LevelLayout plane = map;
	plane.hulls[0][4] = { 0.0f, 0.0f, 0.5f }; // the apex down in the base's plane
	SerializeMap( plane, unknown );
	CHECK( DeserializeMap( unknown.data(), unknown.size(), refused, error ) == false );
	std::printf( "    a flat hull: %s\n", error.c_str() );
	LevelLayout missing = map;
	missing.statics.back().hull = 5;
	SerializeMap( missing, unknown );
	CHECK( DeserializeMap( unknown.data(), unknown.size(), refused, error ) == false );
}

// Bodies of several shapes (map version 6): a template's shape set is one body, whatever part of it
// is hit; a weight given is the body's however many shapes it has; a hull can be one of them; a
// placement turns with roll; the set, the look and the roll survive the file.
void TestMapBodies()
{
	LevelLayout map;
	map.name = "bodies";
	map.spawnCenter = { 0.0f, 1.5f, 10.0f };
	map.spawnRadius = 2.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
	map.hulls.push_back( { { -0.3f, 0.0f, -0.3f }, { 0.3f, 0.0f, -0.3f }, { 0.3f, 0.0f, 0.3f }, { -0.3f, 0.0f, 0.3f }, { 0.0f, 0.5f, 0.0f } } );
	// A dumbbell: two balls on a bar lying along X.
	map.shapeSets.push_back( { { { -1.0f, 0.0f, 0.0f }, { 0.4f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Sphere },
							   { { 1.0f, 0.0f, 0.0f }, { 0.4f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Sphere },
							   { { 0.0f, 0.0f, 0.0f }, { 0.1f, 0.8f, 0.0f }, 0.0f, 0.0f, 1.5707964f, ShapeKind::Capsule } } );
	// An L of two boards, with a little pyramid on its foot.
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.1f, 0.3f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box },
							   { { 0.9f, 0.5f, 0.0f }, { 0.1f, 0.6f, 0.3f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box },
							   { { -0.6f, 0.1f, 0.0f }, { 0.3f, 0.5f, 0.3f }, 0.0f, 0.0f, 0.0f, ShapeKind::Hull, 0 } } );
	EntityTemplate dumbbell;
	dumbbell.name = "dumbbell";
	dumbbell.components.push_back( MakeComponent( "Material", { { "mass", 10.0f } } ) );
	dumbbell.shapes = 0;
	dumbbell.look = 3;
	EntityTemplate ell;
	ell.name = "ell";
	ell.shapes = 1;
	map.templates = { dumbbell, ell };
	map.instances.push_back( { 0, { 0.0f, 2.0f, 0.0f }, 0.0f, 0.0f } );
	map.instances.push_back( { 1, { 6.0f, 2.0f, 0.0f }, 0.0f, 0.0f, 0.3f } );
	map.instances.push_back( { 0, { -6.0f, 2.0f, 0.0f }, 0.0f, 1.5707964f } ); // along Z
	QuantizeLayout( map );

	std::vector<uint8_t> bytes;
	SerializeMap( map, bytes );
	LevelLayout loaded;
	std::string error;
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) );
	CHECK( SameLayout( map, loaded ) );
	CHECK( loaded.shapeSets.size() == 2 && loaded.shapeSets[1].size() == 3 && loaded.shapeSets[1][2].kind == ShapeKind::Hull );
	CHECK( loaded.templates.size() == 2 && loaded.templates[0].shapes == 0 && loaded.templates[0].look == 3 &&
		   loaded.templates[1].shapes == 1 && loaded.templates[1].look == kNoLook );
	CHECK( TemplateLookName( loaded.templates[0] ) == "#3" && TemplateLookName( loaded.templates[1] ).empty() );
	CHECK( loaded.instances.size() == 3 && loaded.instances[1].roll == map.instances[1].roll && map.instances[1].roll != 0.0f );

	Simulation sim( TestConfig(), loaded );
	InputFrame frame;
	for ( int i = 0; i < 180; ++i )
	{
		frame.tick = sim.Tick();
		sim.Step( frame );
	}
	uint32_t ids[3] = {};
	int found = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		flecs::entity e( sim.World(), r.entity );
		if ( e.has<TemplateRef>() && found < 3 )
		{
			ids[found++] = r.netId;
		}
	}
	CHECK( found == 3 );
	flecs::entity first = sim.FindEntity( ids[0] );
	const float mass = b3Body_GetMass( sim.BodyOf( first.get<PhysicsBody>() ) );
	const Shape& bounds = first.get<Shape>();
	const Transform& lying = *sim.EntityTransform( ids[0] );
	std::printf( "    the dumbbell weighs %.3f kg, its bounds %.2f x %.2f x %.2f, its middle at %.3f m\n", mass, bounds.halfExtents.x,
				 bounds.halfExtents.y, bounds.halfExtents.z, lying.position.y );
	CHECK( std::fabs( mass - 10.0f ) < 0.01f );
	CHECK( std::fabs( bounds.halfExtents.x - 1.4f ) < 0.01f && std::fabs( bounds.halfExtents.y - 0.4f ) < 0.01f );
	CHECK( std::fabs( lying.position.y - 0.4f ) < 0.02f ); // on its balls, the bar off the ground

	// Whatever part is hit, it is the one entity.
	auto down = [&]( float x, float z, uint32_t& who ) {
		RayHit hit;
		bool any = sim.CastRay( { x, 6.0f, z }, { 0.0f, -10.0f, 0.0f }, 0, hit );
		who = any ? hit.netId : 0;
		return any ? hit.point.y : -1.0f;
	};
	const float x = lying.position.x, z = lying.position.z;
	uint32_t ball = 0, bar = 0, turned = 0;
	const float ballTop = down( x + 1.0f, z, ball );
	const float barTop = down( x, z, bar );
	down( -6.0f, 1.0f, turned );
	std::printf( "    down onto a ball %.3f m, onto the bar %.3f m\n", ballTop, barTop );
	CHECK( ball == ids[0] && bar == ids[0] && std::fabs( ballTop - 0.8f ) < 0.03f && std::fabs( barTop - 0.5f ) < 0.03f );
	CHECK( turned == ids[2] ); // the one along Z has a ball at z 1

	// The L came to rest on its boards, not through the floor.
	const Transform& ellAt = *sim.EntityTransform( ids[1] );
	std::printf( "    the L rests at %.3f m\n", ellAt.position.y );
	CHECK( ellAt.position.y > 0.05f && ellAt.position.y < 1.5f );

	// A set naming no hull of the map, and a template naming no set, are refused.
	LevelLayout bad = map;
	bad.shapeSets[1][2].hull = 4;
	SerializeMap( bad, bytes );
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) == false );
	bad = map;
	bad.templates[1].shapes = 2;
	SerializeMap( bad, bytes );
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) == false );
}

// Level surfaces (map version 7): a triangle mesh (a trough) and a height field (a bowl with a hole).
// Balls dropped on them roll to their bottoms; one over the hole falls through; rays meet them where
// they are; they survive the file; a rollback snapshot and a portable one (its bytes gone once
// loaded) continue as the original does; a surface where it may not be is refused.
void TestMapSurfaces()
{
	LevelLayout map;
	map.name = "surfaces";
	map.spawnCenter = { 0.0f, 1.5f, 30.0f };
	map.spawnRadius = 2.0f;
	// The trough: two slopes 4 m wide meeting at x 0, along z from -3 to 3; faces up.
	LevelMesh trough;
	trough.vertices = { { -4.0f, 2.0f, -3.0f }, { 0.0f, 0.0f, -3.0f }, { 4.0f, 2.0f, -3.0f },
						{ -4.0f, 2.0f, 3.0f },	{ 0.0f, 0.0f, 3.0f },  { 4.0f, 2.0f, 3.0f } };
	for ( auto [a, b, c] : { std::array<uint32_t, 3>{ 0, 1, 4 }, { 0, 4, 3 }, { 1, 2, 5 }, { 1, 5, 4 } } )
	{
		// Counter-clockwise seen from above: the face points up.
		const b3Vec3 n = b3Cross( b3Sub( trough.vertices[b], trough.vertices[a] ), b3Sub( trough.vertices[c], trough.vertices[a] ) );
		trough.indices.insert( trough.indices.end(), { a, n.y > 0.0f ? b : c, n.y > 0.0f ? c : b } );
	}
	map.meshes.push_back( trough );
	map.statics.push_back( { { 0.0f, 0.0f, 0.0f }, { 4.0f, 2.0f, 3.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Mesh, 0 } );
	// The bowl: 9 by 9 points a metre apart from its corner at (20, 0, -4); its bottom at (24, 0, 0).
	LevelHeightField bowl;
	bowl.countX = 9;
	bowl.countZ = 9;
	for ( int z = 0; z < 9; ++z )
	{
		for ( int x = 0; x < 9; ++x )
		{
			bowl.heights.push_back( 0.125f * float( ( x - 4 ) * ( x - 4 ) + ( z - 4 ) * ( z - 4 ) ) );
		}
	}
	bowl.holes.assign( 64, 0 );
	bowl.holes[7 * 8 + 7] = 1; // the far corner cell: x 7..8, z 7..8
	map.heightFields.push_back( bowl );
	map.statics.push_back( { { 20.0f, 0.0f, -4.0f }, { 8.0f, 4.0f, 8.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::HeightField, 0 } );
	map.props.push_back( { ShapeKind::Sphere, { 2.5f, 4.0f, 0.0f }, { 0.3f, 0.0f, 0.0f } } );	// onto a slope
	map.props.push_back( { ShapeKind::Sphere, { 26.0f, 5.0f, 1.0f }, { 0.3f, 0.0f, 0.0f } } );	// into the bowl
	map.props.push_back( { ShapeKind::Sphere, { 27.5f, 9.0f, 3.5f }, { 0.15f, 0.0f, 0.0f } } ); // over the hole
	QuantizeLayout( map );

	std::vector<uint8_t> bytes;
	SerializeMap( map, bytes );
	LevelLayout loaded;
	std::string error;
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) );
	CHECK( SameLayout( map, loaded ) );
	CHECK( loaded.meshes.size() == 1 && loaded.meshes[0].indices == map.meshes[0].indices && loaded.heightFields.size() == 1 &&
		   loaded.heightFields[0].heights == map.heightFields[0].heights && loaded.heightFields[0].holes == map.heightFields[0].holes );

	Simulation sim( TestConfig(), loaded );
	std::vector<uint32_t> balls;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		if ( flecs::entity( sim.World(), r.entity ).has<Prop>() )
		{
			balls.push_back( r.netId );
		}
	}
	CHECK( balls.size() == 3 );
	RayHit hit;
	CHECK( sim.CastRay( { 1.0f, 5.0f, 0.0f }, { 0.0f, -10.0f, 0.0f }, 0, hit ) && std::fabs( hit.point.y - 0.5f ) < 0.01f );
	CHECK( sim.CastRay( { 26.0f, 5.0f, 0.0f }, { 0.0f, -10.0f, 0.0f }, 0, hit ) && std::fabs( hit.point.y - 0.5f ) < 0.02f );
	const float bottom = sim.CastRay( { 24.0f, 5.0f, 0.0f }, { 0.0f, -10.0f, 0.0f }, 0, hit ) ? hit.point.y : -1.0f;
	CHECK( std::fabs( bottom ) < 0.02f );

	// A rollback snapshot at tick 30, and a portable one, taken while the balls roll.
	InputFrame frame;
	auto step = [&]( Simulation& s ) {
		frame.tick = s.Tick();
		s.Step( frame );
	};
	for ( int i = 0; i < 30; ++i )
	{
		step( sim );
	}
	Snapshot early;
	sim.Save( early );
	Simulation other( TestConfig(), loaded );
	{
		std::vector<uint8_t> portable;
		sim.SavePortable( portable );
		CHECK( other.LoadPortable( portable ) );
	} // (its bytes are gone: the loaded world keeps its own copy of the surfaces)
	std::vector<uint64_t> ahead, again, elsewhere;
	for ( int i = 0; i < 300; ++i )
	{
		step( sim );
		ahead.push_back( sim.ComputeHash() );
		step( other );
		elsewhere.push_back( other.ComputeHash() );
	}
	CHECK( sim.EntityTransform( balls[0] ) != nullptr && sim.EntityTransform( balls[1] ) != nullptr );
	const b3Vec3 inTrough = sim.EntityTransform( balls[0] )->position;
	const b3Vec3 inBowl = sim.EntityTransform( balls[1] )->position;
	const Transform* fell = sim.EntityTransform( balls[2] );
	std::printf( "    after 5.5 s: the trough's ball at x %.2f, y %.2f; the bowl's at (%.2f, %.2f, %.2f); the one over the hole %s\n",
				 inTrough.x, inTrough.y, inBowl.x, inBowl.y, inBowl.z, fell == nullptr ? "fell through (gone at the kill plane)" : "is still here" );
	CHECK( std::fabs( inTrough.x ) < 0.6f && inTrough.y < 0.8f );
	// (A ball rolls on and on in a bowl: nothing slows its rolling. It stays in it, on its surface.)
	const float surface = 0.125f * ( ( inBowl.x - 24.0f ) * ( inBowl.x - 24.0f ) + inBowl.z * inBowl.z );
	CHECK( std::fabs( inBowl.x - 24.0f ) < 4.0f && std::fabs( inBowl.z ) < 4.0f && inBowl.y > surface && inBowl.y < surface + 0.6f );
	CHECK( fell == nullptr || fell->position.y < -1.0f );
	sim.Load( early );
	for ( int i = 0; i < 300; ++i )
	{
		step( sim );
		again.push_back( sim.ComputeHash() );
	}
	CHECK( again == ahead );	 // restored from the rollback snapshot: the same run
	CHECK( elsewhere == ahead ); // and from the portable one

	// A player stands on the terrain (at its bottom, and on its slope) and walks up it, and stands in
	// the trough: as high above each as on flat ground (1.38 m), grounded.
	for ( b3Vec3 spawn : { b3Vec3{ 24.0f, 2.0f, 0.0f }, b3Vec3{ 22.0f, 2.0f, 0.0f }, b3Vec3{ 1.0f, 3.0f, 0.0f } } )
	{
		LevelLayout walk = loaded;
		walk.props.clear(); // (no ball rolling under it)
		walk.spawnCenter = { spawn.x + 5.25f, spawn.y, spawn.z }; // (the first slot stands 5.25 m to its left)
		walk.spawnRadius = 0.0f;
		Simulation player( TestConfig(), walk );
		InputFrame f;
		f.events.push_back( { PlayerEventType::Join, 0 } );
		for ( int i = 0; i < 90; ++i )
		{
			f.tick = player.Tick();
			player.Step( f );
			f.events.clear();
		}
		const Character* body = player.PlayerCharacter( 0 );
		const b3Vec3 stood = player.EntityTransform( player.PlayerNetId( 0 ) )->position;
		f.inputs[0].moveForward = 127;
		for ( int i = 0; i < 60; ++i )
		{
			f.tick = player.Tick();
			player.Step( f );
		}
		const b3Vec3 walked = player.EntityTransform( player.PlayerNetId( 0 ) )->position;
		std::printf( "    a player put at (%.0f, %.0f, %.0f) stands at y %.2f (grounded %d), and a second's walk takes it %.2f m, %.2f m up\n",
					 spawn.x, spawn.y, spawn.z, stood.y, int( body != nullptr && body->grounded ), b3Distance( stood, walked ),
					 walked.y - stood.y );
		RayHit ground;
		CHECK( player.CastRay( { spawn.x, 5.0f, spawn.z }, { 0.0f, -10.0f, 0.0f }, player.PlayerNetId( 0 ), ground, true ) );
		CHECK( body != nullptr && body->grounded && std::fabs( stood.y - ground.point.y - 1.38f ) < 0.03f );
		CHECK( b3Distance( stood, walked ) > 1.0f );
	}

	// A surface in a body's set, or in a version 6 map, is refused.
	LevelLayout bad = map;
	bad.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 4.0f, 2.0f, 3.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Mesh, 0 } } );
	SerializeMap( bad, bytes );
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) == false );
	bad = map;
	bad.meshes[0].indices[0] = 99;
	SerializeMap( bad, bytes );
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) == false );
}

// Rolling and drag (a body's, as a map's prop has them): of three balls rolled off the same ramp,
// the one without rolling resistance rolls on, the one with it comes to a stop, and the one with
// drag stops sooner; the values survive the file.
void TestRolling()
{
	LevelLayout map;
	map.name = "rolling";
	map.spawnCenter = { 0.0f, 1.5f, 40.0f };
	map.spawnRadius = 2.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 40.0f, 0.5f, 40.0f }, 0.0f, 0.0f } );
	// A ramp 6 m long at 20°.
	const float a = 0.349f;
	map.statics.push_back( { { 0.0f, 3.0f * std::sin( a ), -3.0f * std::cos( a ) }, { 6.0f, 0.1f, 3.0f }, -a, 0.0f } );
	for ( int i = 0; i < 3; ++i )
	{
		LevelProp ball{ ShapeKind::Sphere, { -3.0f + 3.0f * float( i ), 2.5f, -5.0f }, { 0.3f, 0.0f, 0.0f } };
		ball.rolling = i == 0 ? 0.0f : 0.05f;
		ball.drag = i == 2 ? 0.5f : 0.0f;
		map.props.push_back( ball );
	}
	QuantizeLayout( map );
	std::vector<uint8_t> bytes;
	SerializeMap( map, bytes );
	LevelLayout loaded;
	std::string error;
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) && SameLayout( map, loaded ) );
	CHECK( loaded.props[1].rolling == map.props[1].rolling && loaded.props[2].drag == map.props[2].drag && map.props[2].drag > 0.0f );

	Simulation sim( TestConfig(), loaded );
	std::vector<uint32_t> balls;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		if ( flecs::entity( sim.World(), r.entity ).has<Prop>() )
		{
			balls.push_back( r.netId );
		}
	}
	CHECK( balls.size() == 3 );
	InputFrame f;
	float speed[3] = {}, reach[3] = {};
	for ( int t = 0; t < 900; ++t )
	{
		f.tick = sim.Tick();
		sim.Step( f );
	}
	for ( int i = 0; i < 3; ++i )
	{
		const flecs::entity e = sim.FindEntity( balls[size_t( i )] );
		const b3Vec3 v = b3Body_GetLinearVelocity( sim.BodyOf( e.get<PhysicsBody>() ) );
		speed[i] = b3Length( v );
		reach[i] = std::fabs( sim.EntityTransform( balls[size_t( i )] )->position.z + 5.0f ); // from where it was dropped
	}
	std::printf( "    after 15 s: rolling 0 at %.2f m/s, %.1f m out; rolling 0.05 at %.2f m/s, %.1f m out; with drag 0.5 at %.2f m/s, %.1f m out\n",
				 speed[0], reach[0], speed[1], reach[1], speed[2], reach[2] );
	CHECK( speed[0] > 1.0f );						   // rolls on
	CHECK( speed[1] < 0.05f && speed[2] < 0.05f );	   // came to a stop
	CHECK( reach[2] < reach[1] && reach[1] < reach[0] ); // drag stopped it sooner
}

// A breakable wall (map version 9: a template's pieces): it stops a player like any wall; a Break
// command takes it away and puts its four pieces where they were, flying away from the blast; they
// settle; a player walking into the rubble is stopped by it and stands on it but never shoves it;
// the pieces survive the file; a piece that breaks itself is refused.
void TestBreakable()
{
	LevelLayout map;
	map.name = "breakable";
	map.spawnCenter = { 5.25f, 1.5f, 6.0f };
	map.spawnRadius = 0.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 1.5f, 1.0f, 0.2f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );	 // the wall
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 0.74f, 0.49f, 0.19f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } ); // a quarter
	EntityTemplate wall;
	wall.name = "wall";
	wall.components.push_back( MakeIntComponent( "Body", { { "type", 0 } } ) );
	wall.shapes = 0;
	for ( float x : { -0.75f, 0.75f } )
	{
		for ( float y : { -0.5f, 0.5f } )
		{
			wall.pieces.push_back( { 1, { x, y, 0.0f } } );
		}
	}
	EntityTemplate brick;
	brick.name = "brick";
	brick.shapes = 1;
	AuthoredComponent debris;
	debris.id = Fnv32( "Debris" );
	brick.components.push_back( debris );
	EntityTemplate slabTemplate; // rubble of another shape, flat
	slabTemplate.name = "slab";
	slabTemplate.shapes = 2;
	slabTemplate.components.push_back( debris );
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.15f, 1.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );
	map.templates = { wall, brick, slabTemplate };
	map.instances.push_back( { 0, { 0.0f, 1.0f, 0.0f }, 0.0f, 0.0f } );
	QuantizeLayout( map );
	std::vector<uint8_t> bytes;
	SerializeMap( map, bytes );
	LevelLayout loaded;
	std::string error;
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) );
	CHECK( loaded.templates.size() == 3 && loaded.templates[0].pieces == map.templates[0].pieces && loaded.templates[0].pieces.size() == 4 );

	Simulation sim( TestConfig(), loaded );
	InputFrame f;
	auto run = [&]( int ticks ) {
		for ( int i = 0; i < ticks; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto of = [&]( uint32_t index ) {
		std::vector<uint32_t> out;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const TemplateRef* ref = flecs::entity( sim.World(), r.entity ).try_get<TemplateRef>();
			if ( ref != nullptr && ref->index == index )
			{
				out.push_back( r.netId );
			}
		}
		return out;
	};
	auto player = [&]() { return sim.EntityTransform( sim.PlayerNetId( 0 ) )->position; };
	f.events.push_back( { PlayerEventType::Join, 0 } );
	run( 30 );
	f.inputs[0].cameraYaw = 32768; // forward is -Z: toward the wall and on through
	f.inputs[0].moveForward = 127;
	run( 180 );
	const float stopped = player().z;
	CHECK( of( 0 ).size() == 1 && stopped > 0.4f ); // the wall stops it
	f.inputs[0].moveForward = 0;
	f.inputs[0].cameraYaw = 0;
	run( 30 );

	// The blast in front of the wall: the pieces fly away from it, behind the wall.
	SimCommand blast;
	blast.type = CommandType::Break;
	blast.target = of( 0 )[0];
	blast.a = { 0.0f, 1.0f, 1.0f };
	blast.b = { 14.0f, 0.0f, 0.0f };
	f.commands.push_back( blast );
	run( 1 );
	const std::vector<uint32_t> pieces = of( 1 );
	CHECK( of( 0 ).empty() && pieces.size() == 4 );
	if ( pieces.size() != 4 )
	{
		return;
	}
	bool away = true;
	for ( uint32_t id : pieces )
	{
		const flecs::entity e = sim.FindEntity( id );
		away = away && b3Body_GetLinearVelocity( sim.BodyOf( e.get<PhysicsBody>() ) ).z < -1.0f;
	}
	CHECK( away );
	run( 240 ); // they land and settle

	// The player walks on through where the wall was, into the rubble: it does not shove it.
	std::vector<b3Vec3> before;
	for ( uint32_t id : pieces )
	{
		before.push_back( sim.EntityTransform( id )->position );
	}
	const b3Vec3 start = player();
	f.inputs[0].cameraYaw = 32768;
	f.inputs[0].moveForward = 127;
	float moved = 0.0f, highest = 0.0f;
	for ( int i = 0; i < 240; ++i )
	{
		run( 1 );
		highest = std::max( highest, player().y );
	}
	for ( size_t i = 0; i < pieces.size(); ++i )
	{
		moved = std::max( moved, b3Distance( before[i], sim.EntityTransform( pieces[i] )->position ) );
	}
	std::printf( "    the wall stopped the player at z %.2f; broken, its 4 pieces flew off behind it and settled; the player walked from z %.2f "
				 "to %.2f through them (up to %.2f m high), and the most a piece moved was %.4f m\n",
				 stopped, start.z, player().z, highest, moved );
	CHECK( player().z < -1.0f ); // it went on through where the wall was
	CHECK( moved < 0.001f );	 // and shoved no piece

	// A slab of rubble 30 cm high in its way: it walks up onto it, stands on it, and does not shove it.
	SimCommand slab;
	slab.type = CommandType::SpawnProp;
	slab.value = 2;
	slab.a = { player().x, 0.2f, player().z - 2.5f };
	f.commands.push_back( slab );
	f.inputs[0].moveForward = 0;
	run( 60 );
	const std::vector<uint32_t> slabs = of( 2 );
	CHECK( slabs.size() == 1 );
	if ( slabs.empty() )
	{
		return;
	}
	const b3Vec3 slabAt = sim.EntityTransform( slabs[0] )->position;
	f.inputs[0].moveForward = 127;
	float onTop = 0.0f;
	for ( int i = 0; i < 50; ++i )
	{
		run( 1 );
		onTop = std::max( onTop, player().y );
	}
	const float shoved = b3Distance( slabAt, sim.EntityTransform( slabs[0] )->position );
	std::printf( "    onto a slab of rubble: the player rose to %.2f m, the slab moved %.4f m\n", onTop, shoved );
	CHECK( onTop > 1.38f + 0.25f && shoved < 0.001f );

	// A piece that breaks itself, or names no template, is refused.
	LevelLayout bad = map;
	bad.templates[1].pieces.push_back( { 1, {} } );
	SerializeMap( bad, bytes );
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) == false );
	bad = map;
	bad.templates[0].pieces[0].templateIndex = 7;
	SerializeMap( bad, bytes );
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) == false );
}

// Holes (map version 10: which pieces hold a wall up, which touch): a wall of 3 x 3 bricks held at
// its foot. A blast at its middle takes the middle brick and leaves 8 standing, static and still; a
// blast at its foot's left end takes that one, and the rest still stand (joined round by the right);
// one at its right end takes that one, and then nothing joins the upper bricks to the one held brick
// left, so they fall. Standing pieces survive a portable snapshot and the file.
void TestHoles()
{
	LevelLayout map;
	map.name = "holes";
	map.spawnCenter = { 0.0f, 1.0f, 8.0f };
	map.spawnRadius = 0.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 1.5f, 1.5f, 0.2f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );	 // the wall
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 0.49f, 0.49f, 0.19f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } ); // a brick
	EntityTemplate wall;
	wall.name = "wall";
	wall.components.push_back( MakeIntComponent( "Body", { { "type", 0 } } ) );
	wall.shapes = 0;
	// Brick (row, column) is piece row * 3 + column; the bottom row is held.
	for ( int row = 0; row < 3; ++row )
	{
		for ( int column = 0; column < 3; ++column )
		{
			TemplatePiece piece = { 1, { float( column - 1 ), float( row - 1 ), 0.0f } };
			piece.held = row == 0;
			wall.pieces.push_back( piece );
			const uint16_t i = uint16_t( row * 3 + column );
			if ( column < 2 )
			{
				wall.links.push_back( { i, uint16_t( i + 1 ) } );
			}
			if ( row < 2 )
			{
				wall.links.push_back( { i, uint16_t( i + 3 ) } );
			}
		}
	}
	EntityTemplate brick;
	brick.name = "brick";
	brick.shapes = 1;
	AuthoredComponent debris;
	debris.id = Fnv32( "Debris" );
	brick.components.push_back( debris );
	map.templates = { wall, brick };
	map.instances.push_back( { 0, { 0.0f, 1.5f, 0.0f }, 0.0f, 0.0f } );
	QuantizeLayout( map );
	std::vector<uint8_t> bytes;
	SerializeMap( map, bytes );
	LevelLayout loaded;
	std::string error;
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) );
	CHECK( loaded.templates.size() == 2 && loaded.templates[0].pieces == map.templates[0].pieces && loaded.templates[0].links == map.templates[0].links &&
		   loaded.templates[0].links.size() == 12 && loaded.templates[0].pieces[1].held && loaded.templates[0].pieces[4].held == false );

	Simulation sim( TestConfig(), loaded );
	InputFrame f;
	auto run = [&]( int ticks ) {
		for ( int i = 0; i < ticks; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	// The standing pieces by brick, and the loose ones.
	auto standing = [&]() {
		std::vector<uint32_t> out( 9, 0 );
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const WallPiece* piece = flecs::entity( sim.World(), r.entity ).try_get<WallPiece>();
			if ( piece != nullptr && piece->piece < 9 )
			{
				out[piece->piece] = r.netId;
			}
		}
		return out;
	};
	auto count = []( const std::vector<uint32_t>& ids ) { return int( std::count_if( ids.begin(), ids.end(), []( uint32_t id ) { return id != 0; } ) ); };
	auto loose = [&]() {
		std::vector<uint32_t> out;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			flecs::entity e( sim.World(), r.entity );
			const TemplateRef* ref = e.try_get<TemplateRef>();
			if ( ref != nullptr && ref->index == 1 && e.has<WallPiece>() == false )
			{
				out.push_back( r.netId );
			}
		}
		return out;
	};
	auto blastAt = [&]( uint32_t target, b3Vec3 at ) {
		SimCommand c;
		c.type = CommandType::Break;
		c.target = target;
		c.a = { at.x, at.y, at.z };
		c.b = { 6.0f, 0.6f, 0.0f }; // speed, radius
		f.commands.push_back( c );
		run( 1 );
	};
	run( 10 );
	uint32_t whole = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		const TemplateRef* ref = flecs::entity( sim.World(), r.entity ).try_get<TemplateRef>();
		whole = ref != nullptr && ref->index == 0 ? r.netId : whole;
	}
	CHECK( whole != 0 );

	// At the middle: the middle brick goes; eight stand, static, where they were, and stay.
	blastAt( whole, { 0.0f, 1.5f, 0.5f } );
	std::vector<uint32_t> up = standing();
	CHECK( sim.FindEntity( whole ).is_valid() == false && count( up ) == 8 && up[4] == 0 && loose().size() == 1 );
	bool still = up[0] != 0;
	std::vector<b3Vec3> was( 9 );
	for ( int i = 0; i < 9; ++i )
	{
		if ( up[size_t( i )] != 0 )
		{
			flecs::entity e = sim.FindEntity( up[size_t( i )] );
			still = still && b3Body_GetType( sim.BodyOf( e.get<PhysicsBody>() ) ) == b3_staticBody && e.has<StaticGeometry>();
			was[size_t( i )] = e.get<Transform>().position;
			const b3Vec3 placed = { float( i % 3 - 1 ), 1.5f + float( i / 3 - 1 ), 0.0f };
			still = still && b3Distance( was[size_t( i )], placed ) < 0.001f;
		}
	}
	run( 120 );
	for ( int i = 0; i < 9; ++i )
	{
		if ( up[size_t( i )] != 0 )
		{
			still = still && b3Distance( sim.EntityTransform( up[size_t( i )] )->position, was[size_t( i )] ) < 1e-6f;
		}
	}
	CHECK( still );

	// A copy of the world as it stands, loaded elsewhere, is the same world.
	{
		std::vector<uint8_t> image;
		sim.SavePortable( image );
		Simulation other( TestConfig(), loaded );
		CHECK( other.LoadPortable( image ) && other.ComputeHash() == sim.ComputeHash() );
	}

	// At the foot's left end: that brick goes; the rest stand, joined round to the right.
	blastAt( up[1], { -1.0f, 0.5f, 0.5f } );
	up = standing();
	CHECK( count( up ) == 7 && up[0] == 0 && up[6] != 0 && loose().size() == 2 );
	// At its right end: that brick goes, and the five above, joined to nothing held, fall.
	blastAt( up[1], { 1.0f, 0.5f, 0.5f } );
	up = standing();
	const std::vector<uint32_t> fell = loose();
	CHECK( count( up ) == 1 && up[1] != 0 && fell.size() == 8 );
	run( 120 );
	// (The top row stood at 2.5 m: none is up there now; one may lie on the brick left standing.)
	int down = 0;
	float highest = 0.0f;
	for ( uint32_t id : fell )
	{
		down += sim.EntityTransform( id )->position.y < 1.0f ? 1 : 0;
		highest = std::max( highest, sim.EntityTransform( id )->position.y );
	}
	std::printf( "    a 3 x 3 wall: a hole in the middle left 8 standing; two blasts at its foot's ends left 1; of the 8 down, %d on the "
				 "ground, the highest at %.2f m\n",
				 down, highest );
	CHECK( down >= 7 && highest < 1.6f );

	// A link between pieces it does not have is refused.
	LevelLayout bad = map;
	bad.templates[0].links.push_back( { 3, 9 } );
	SerializeMap( bad, bytes );
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) == false );
	bad = map;
	bad.templates[0].links.push_back( { 4, 4 } );
	SerializeMap( bad, bytes );
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) == false );
}

// Items that break walls (ItemShape::breakSpeed): a ball thrown at a 3 x 3 wall at 30 m/s breaks it
// in the step it meets it: the middle brick goes (a hole 0.9 m across at 30 m/s), eight stand, the
// ball never turns back and goes on through, and says so (its breakEvent) there. Thrown at 8 m/s,
// below its break speed, it bounces off and the wall stands whole.
void TestItemBreaks()
{
	LevelLayout map;
	map.name = "item_breaks";
	map.spawnCenter = { 0.0f, 1.0f, 12.0f };
	map.spawnRadius = 0.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 1.5f, 1.5f, 0.2f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 0.49f, 0.49f, 0.19f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );
	EntityTemplate wall;
	wall.name = "wall";
	wall.components.push_back( MakeIntComponent( "Body", { { "type", 0 } } ) );
	wall.shapes = 0;
	for ( int i = 0; i < 9; ++i )
	{
		TemplatePiece piece = { 1, { float( i % 3 - 1 ), float( i / 3 - 1 ), 0.0f } };
		piece.held = i < 3;
		wall.pieces.push_back( piece );
		if ( i % 3 < 2 )
		{
			wall.links.push_back( { uint16_t( i ), uint16_t( i + 1 ) } );
		}
		if ( i < 6 )
		{
			wall.links.push_back( { uint16_t( i ), uint16_t( i + 3 ) } );
		}
	}
	EntityTemplate brick;
	brick.name = "brick";
	brick.shapes = 1;
	AuthoredComponent debris;
	debris.id = Fnv32( "Debris" );
	brick.components.push_back( debris );
	map.templates = { wall, brick };
	map.instances.push_back( { 0, { 0.0f, 1.5f, 0.0f }, 0.0f, 0.0f } );
	QuantizeLayout( map );

	ItemShape ball;
	ball.kind = 1;
	ball.half = { 0.15f, 0.15f, 0.15f };
	ball.center = {};
	ball.mass = 8.0f;
	ball.restitution = 0.3f;
	ball.bullet = 1;
	ball.breakSpeed = 12.0f;
	ball.breakHole = 0.9f;
	ball.breakThrough = 0.6f;
	ball.breakEvent = 0;

	struct Outcome
	{
		bool whole = true;
		int standing = 0;
		float endZ = 0.0f;
		bool turnedBack = false;
		bool said = false;
	};
	auto throwAt = [&]( float speed ) {
		Outcome out;
		Simulation sim( TestConfig(), map );
		sim.SetItemShapes( { ball } );
		InputFrame f;
		auto step = [&]() {
			f.tick = sim.Tick();
			sim.Step( f );
			f.commands.clear();
		};
		for ( int i = 0; i < 5; ++i )
		{
			step();
		}
		uint32_t item = sim.Globals().nextNetId;
		SimCommand c;
		c.type = CommandType::SpawnItem;
		c.target = 0;
		c.index = 0;
		c.a = { 0.0f, 1.5f, 3.0f };
		c.b = { 0.0f, 0.0f, -speed };
		f.commands.push_back( c );
		for ( int i = 0; i < 60; ++i )
		{
			step();
			const flecs::entity e = sim.FindEntity( item );
			if ( e.is_valid() && e.has<PhysicsBody>() )
			{
				out.turnedBack = out.turnedBack || b3Body_GetLinearVelocity( sim.BodyOf( e.get<PhysicsBody>() ) ).z > 0.5f;
				out.endZ = sim.EntityTransform( item )->position.z;
			}
		}
		// The wall whole (its template's entity), or how many of its bricks stand.
		out.whole = false;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			flecs::entity e( sim.World(), r.entity );
			const TemplateRef* ref = e.try_get<TemplateRef>();
			out.whole = out.whole || ( ref != nullptr && ref->index == 0 );
			out.standing += e.has<WallPiece>() ? 1 : 0;
		}
		for ( uint32_t k = 0; k < std::min<uint32_t>( sim.Globals().modEventCount, kModEventHistory ); ++k )
		{
			const ModEventRecord& record = sim.Globals().modEvents[k];
			out.said = out.said || ( record.type == 0 && record.netIdA == item );
		}
		return out;
	};
	const Outcome fast = throwAt( 30.0f );
	const Outcome slow = throwAt( 8.0f );
	std::printf( "    at 30 m/s: the wall %s, %d bricks stand, the ball %s and is at z %.2f; at 8 m/s: the wall %s, the ball %s\n",
				 fast.whole ? "stands whole" : "broke", fast.standing, fast.turnedBack ? "turned back" : "never turned back", fast.endZ,
				 slow.whole ? "stands whole" : "broke", slow.turnedBack ? "bounced off" : "did not bounce" );
	CHECK( fast.whole == false && fast.standing == 8 && fast.turnedBack == false && fast.endZ < -1.0f && fast.said );
	CHECK( slow.whole && slow.standing == 0 && slow.turnedBack && slow.said == false );
}

// Glue (map version 11: a breakable's glue): its pieces are bodies from the start, welded where they
// touch, the held ones static.
// - A tower of four blocks, held at its foot, strongly glued: it stands still. A blast takes the
//   third block; the top one, glued only to it, is loose and falls; the two below stand.
// - An arm of three blocks out from a held pillar: strongly glued it holds; weakly glued, its welds
//   break under its own weight and it falls.
// The same run twice is the same world, and a copy loaded elsewhere too; the glue survives the file.
void TestGlue()
{
	auto build = [&]( float glue, bool arm ) {
		LevelLayout map;
		map.name = "glue";
		map.spawnCenter = { 0.0f, 1.0f, 12.0f };
		map.spawnRadius = 0.0f;
		map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
		map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 0.5f, 2.0f, 0.5f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );
		map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 0.49f, 0.49f, 0.49f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );
		EntityTemplate wall;
		wall.name = "tower";
		wall.components.push_back( MakeIntComponent( "Body", { { "type", 0 } } ) );
		wall.shapes = 0;
		wall.glue = glue;
		for ( int i = 0; i < 4; ++i )
		{
			// A tower: four up. An arm: the pillar's foot, then three out to the side at its height.
			TemplatePiece piece = { 1, arm && i > 0 ? b3Vec3{ float( i ), 0.0f, 0.0f } : b3Vec3{ 0.0f, float( i ), 0.0f } };
			piece.held = i == 0;
			wall.pieces.push_back( piece );
			if ( i > 0 )
			{
				wall.links.push_back( { uint16_t( i - 1 ), uint16_t( i ) } );
			}
		}
		EntityTemplate block;
		block.name = "block";
		block.shapes = 1;
		AuthoredComponent debris;
		debris.id = Fnv32( "Debris" );
		block.components.push_back( debris );
		map.templates = { wall, block };
		map.instances.push_back( { 0, { 0.0f, arm ? 2.5f : 0.5f, 0.0f }, 0.0f, 0.0f } );
		QuantizeLayout( map );
		return map;
	};
	// The file keeps the glue.
	{
		LevelLayout map = build( 25000.0f, false );
		std::vector<uint8_t> bytes;
		SerializeMap( map, bytes );
		LevelLayout loaded;
		std::string error;
		CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) && loaded.templates[0].glue == 25000.0f );
	}
	auto run = [&]( Simulation& sim, int ticks, std::vector<SimCommand> at60 = {} ) {
		InputFrame f;
		for ( int i = 0; i < ticks; ++i )
		{
			f.tick = sim.Tick();
			if ( i == 60 )
			{
				f.commands = at60;
			}
			sim.Step( f );
			f.commands.clear();
		}
	};
	// The pieces by index: where each is, and whether it is still glued (a WallPiece).
	auto pieces = [&]( Simulation& sim, std::vector<b3Vec3>& where, std::vector<int>& glued ) {
		where.assign( 4, { 0.0f, -100.0f, 0.0f } );
		glued.assign( 4, 0 );
		int i = 0;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			flecs::entity e( sim.World(), r.entity );
			const TemplateRef* ref = e.try_get<TemplateRef>();
			if ( ref != nullptr && ref->index == 1 && i < 4 )
			{
				where[size_t( i )] = e.get<Transform>().position;
				glued[size_t( i )] = e.has<WallPiece>() ? 1 : 0;
				++i;
			}
		}
	};

	// The tower stands; a blast takes its third block, and the top falls.
	{
		const LevelLayout map = build( 25000.0f, false );
		Simulation sim( TestConfig(), map );
		std::vector<b3Vec3> before, after;
		std::vector<int> glued;
		run( sim, 50 );
		pieces( sim, before, glued );
		run( sim, 10 );
		SimCommand blast;
		blast.type = CommandType::Break;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			blast.target = flecs::entity( sim.World(), r.entity ).has<WallPiece>() ? r.netId : blast.target;
		}
		blast.a = { 0.0f, 2.5f, 0.6f };
		blast.b = { 24.0f, 0.8f, 0.0f };
		run( sim, 1, {} );
		std::vector<b3Vec3> justBefore;
		pieces( sim, justBefore, glued );
		InputFrame f;
		f.tick = sim.Tick();
		f.commands = { blast };
		sim.Step( f );
		std::vector<b3Vec3> stood;
		pieces( sim, stood, glued );
		const int gluedAfterBlast = glued[0] + glued[1] + glued[2] + glued[3];
		run( sim, 120 );
		pieces( sim, after, glued );
		float still = 0.0f;
		for ( int i = 0; i < 4; ++i )
		{
			still = std::max( still, b3Distance( before[size_t( i )], justBefore[size_t( i )] ) );
		}
		std::printf( "    a glued tower of 4: still before the blast (%.4f m); after it %d glued; its top fell from %.2f to %.2f m; the two below "
					 "moved %.4f m\n",
					 still, gluedAfterBlast, before[3].y, after[3].y, std::max( b3Distance( before[0], after[0] ), b3Distance( before[1], after[1] ) ) );
		CHECK( still < 0.01f && gluedAfterBlast == 2 && glued[0] == 1 && glued[1] == 1 && glued[2] == 0 && glued[3] == 0 );
		CHECK( after[3].y < before[3].y - 1.5f && b3Distance( before[1], after[1] ) < 0.01f );
		// A copy of it loaded elsewhere is the same world.
		std::vector<uint8_t> image;
		sim.SavePortable( image );
		Simulation other( TestConfig(), map );
		CHECK( other.LoadPortable( image ) && other.ComputeHash() == sim.ComputeHash() );
	}

	// test_ground's glued tower (40 chunks cut by the fracture tool, welded at 20000 N where the bake
	// found them touching) stands on its own for 5 s.
	{
		LevelLayout ground;
		std::vector<uint8_t> bytes;
		std::string error;
		CHECK( LoadMapFile( std::string( CB_SOURCE_DIR ) + "/godot/maps/test_ground.cbmap", ground, bytes, error ) );
		Simulation sim( TestConfig(), ground );
		std::vector<std::pair<uint32_t, b3Vec3>> tower;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			flecs::entity e( sim.World(), r.entity );
			const WallPiece* piece = e.try_get<WallPiece>();
			if ( piece != nullptr && piece->wallTemplate < ground.templates.size() && ground.templates[piece->wallTemplate].glue > 0.0f )
			{
				tower.push_back( { r.netId, e.get<Transform>().position } );
			}
		}
		const auto started = std::chrono::steady_clock::now();
		run( sim, 300 );
		const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();
		float moved = 0.0f;
		int glued = 0;
		for ( const auto& [netId, at] : tower )
		{
			flecs::entity e = sim.FindEntity( netId );
			moved = std::max( moved, e.is_valid() ? b3Distance( e.get<Transform>().position, at ) : 1e9f );
			glued += e.is_valid() && e.has<WallPiece>() ? 1 : 0;
		}
		std::printf( "    test_ground's glued tower: %d pieces, %d still glued after 5 s, the most one moved %.4f m (5 s of the map in %.0f ms)\n",
					 int( tower.size() ), glued, moved, ms );
		CHECK( tower.size() == 40 && glued == 40 && moved < 0.02f );
	}

	// The arm: strong glue holds it out; weak glue breaks under its weight.
	for ( float glue : { 100000.0f, 200.0f } )
	{
		const LevelLayout map = build( glue, true );
		Simulation sim( TestConfig(), map );
		Simulation again( TestConfig(), map );
		std::vector<b3Vec3> start, end;
		std::vector<int> glued;
		run( sim, 2 );
		pieces( sim, start, glued );
		run( sim, 178 );
		run( again, 180 );
		pieces( sim, end, glued );
		const float dropped = start[3].y - end[3].y;
		std::printf( "    an arm of 3 glued at %.0f N: its end dropped %.2f m, %d pieces still glued\n", glue, dropped, glued[0] + glued[1] + glued[2] + glued[3] );
		CHECK( again.ComputeHash() == sim.ComputeHash() );
		if ( glue > 1000.0f )
		{
			CHECK( dropped < 0.05f && glued[3] == 1 );
		}
		else
		{
			CHECK( dropped > 1.0f && glued[3] == 0 );
		}
	}
}

// Slopes (max_slope, 50 degrees by default): on a ramp up to it a player stands still and walks up;
// on a steeper one it slides down and cannot walk up; a mod's 90 lets it stand on a 70° one again
// (as before there was a limit).
void TestSlopes()
{
	struct Case
	{
		float degrees;
		float maxSlope;
	};
	for ( Case k : { Case{ 30.0f, 50.0f }, Case{ 45.0f, 50.0f }, Case{ 55.0f, 50.0f }, Case{ 70.0f, 50.0f }, Case{ 70.0f, 90.0f } } )
	{
		const float a = k.degrees * 3.14159265f / 180.0f;
		LevelLayout ramp;
		ramp.name = "ramp";
		ramp.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 30.0f, 0.5f, 30.0f }, 0.0f, 0.0f } );
		// A slab 20 m long rising toward -Z, its middle at z -10.
		ramp.statics.push_back( { { 0.0f, 10.0f * std::tan( a ) * 0.5f, -10.0f }, { 4.0f, 0.2f, 10.0f / std::cos( a ) }, a, 0.0f } );
		ramp.spawnCenter = { 5.25f, 10.0f * std::tan( a ) * 0.5f + 0.2f / std::cos( a ) + 1.5f, -10.0f }; // (slot 0 stands 5.25 m left)
		ramp.spawnRadius = 0.0f;
		QuantizeLayout( ramp );
		SimConfig config = TestConfig();
		config.move.values[int( MoveParam::MaxSlope )] = k.maxSlope;
		Simulation s( config, ramp );
		InputFrame f;
		f.events.push_back( { PlayerEventType::Join, 0 } );
		auto run = [&]( int ticks ) {
			for ( int i = 0; i < ticks; ++i )
			{
				f.tick = s.Tick();
				s.Step( f );
				f.events.clear();
			}
			return s.EntityTransform( s.PlayerNetId( 0 ) )->position;
		};
		const b3Vec3 landed = run( 30 );
		const b3Vec3 stood = run( 90 );
		f.inputs[0].moveForward = 127;
		f.inputs[0].cameraYaw = 32768; // forward is -Z: up the ramp
		const b3Vec3 walked = run( 90 );
		const bool walkable = k.degrees <= k.maxSlope;
		std::printf( "    %2.0f° with max_slope %2.0f: standing 1.5 s it moved %.2f m down; walking up 1.5 s it rose %.2f m\n", k.degrees,
					 k.maxSlope, landed.y - stood.y, walked.y - stood.y );
		if ( walkable )
		{
			CHECK( std::fabs( stood.y - landed.y ) < 0.05f );			 // stands still
			CHECK( k.degrees > 45.0f || walked.y - stood.y > 1.5f ); // and walks up a slope one can
		}
		else
		{
			CHECK( landed.y - stood.y > 1.0f ); // slides down
			CHECK( walked.y < stood.y + 0.3f ); // and does not walk up
		}
	}
}

// Entities described by components in the editor must reach the simulation exactly as authored:
// the same shape, the same body, the same material, and the same behaviour on every machine.
void TestTemplates()
{
	LevelLayout map;
	map.name = "templates";
	map.spawnCenter = { 0.0f, 1.5f, 6.0f };
	map.spawnRadius = 2.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );

	EntityTemplate crate;
	crate.name = "crate";
	crate.visual = "prop_heavy";
	crate.components.push_back( MakeIntComponent( "Shape", { { "kind", int32_t( ShapeKind::Box ) } } ) );
	AuthoredField size;
	size.id = Fnv32( "size" );
	size.raw[0] = MapQuantize( 1.5f, kMapPositionScale );
	size.raw[1] = MapQuantize( 0.5f, kMapPositionScale );
	size.raw[2] = MapQuantize( 2.0f, kMapPositionScale );
	crate.components[0].fields.push_back( size );
	crate.components.push_back( MakeIntComponent( "Body", { { "type", 2 } } ) );
	crate.components.push_back( MakeComponent( "Prop", { { "lifetime_seconds", 2.0f } } ) );

	EntityTemplate pillar;
	pillar.name = "pillar";
	pillar.visual = "prop_heavy";
	pillar.components.push_back( MakeIntComponent( "Shape", { { "kind", int32_t( ShapeKind::Box ) } } ) );
	pillar.components.push_back( MakeIntComponent( "Body", { { "type", 0 } } ) ); // static

	map.templates.push_back( crate );
	map.templates.push_back( pillar );
	map.instances.push_back( { 0, { 2.0f, 3.0f, 0.0f }, 0.0f, 0.0f } );
	map.instances.push_back( { 1, { -3.0f, 1.0f, 0.0f }, 0.0f, 0.0f } );
	map.spawnTemplate = 0;
	QuantizeLayout( map );

	// A map with templates has to survive the bake -> load round trip like everything else.
	std::vector<uint8_t> bytes;
	SerializeMap( map, bytes );
	LevelLayout loaded;
	std::string error;
	CHECK( DeserializeMap( bytes.data(), bytes.size(), loaded, error ) );
	CHECK( loaded.templates.size() == 2 );
	CHECK( loaded.templates[0].name == "crate" );
	CHECK( loaded.templates[0].visual == "prop_heavy" );
	CHECK( loaded.instances.size() == 2 );
	CHECK( loaded.spawnTemplate == 0 );
	CHECK( TemplateFloat( loaded.templates[0], Fnv32( "Prop" ), Fnv32( "lifetime_seconds" ), 0.0f ) == 2.0f );

	SimConfig config = TestConfig();
	Simulation sim( config, loaded );

	// The authored values became a real entity: half the authored size, and a prop with a lifetime.
	bool sawCrate = false;
	bool sawPillar = false;
	for ( const auto& r : sim.Entities() )
	{
		flecs::entity e( sim.World(), r.entity );
		const TemplateRef* ref = e.try_get<TemplateRef>();
		if ( ref == nullptr )
		{
			continue;
		}
		const Shape& shape = e.get<Shape>();
		if ( ref->index == 0 )
		{
			sawCrate = true;
			CHECK( shape.kind == ShapeKind::Box );
			CHECK( shape.halfExtents.x == 0.75f );
			CHECK( shape.halfExtents.y == 0.25f );
			CHECK( shape.halfExtents.z == 1.0f );
			CHECK( e.has<Prop>() );
			CHECK( e.has<StaticGeometry>() == false );
		}
		if ( ref->index == 1 )
		{
			sawPillar = true;
			// A static body is level geometry, and must not be swept up by the kill plane.
			CHECK( e.has<StaticGeometry>() );
			CHECK( e.has<Prop>() == false );
		}
	}
	CHECK( sawCrate );
	CHECK( sawPillar );

	// A SpawnProp command that names a template makes that template (the props mod sends the
	// map's spawn template this way).
	InputFrame frame;
	frame.events.push_back( { PlayerEventType::Join, 0 } );
	frame.tick = 0;
	sim.Step( frame );
	frame.events.clear();

	size_t before = CountFromTemplate( sim, 0 );
	for ( uint32_t t = 1; t < 30; ++t )
	{
		frame.tick = t;
		frame.commands.clear();
		if ( t % 4 == 0 )
		{
			SimCommand spawn;
			spawn.type = CommandType::SpawnProp;
			spawn.target = SlotTarget( 0 );
			spawn.value = 0;
			spawn.a = { 0.0f, 3.0f, 2.0f + float( t ) * 0.1f };
			frame.commands.push_back( spawn );
		}
		sim.Step( frame );
	}
	size_t spawned = CountFromTemplate( sim, 0 );
	std::printf( "    spawned %zu entities from the map's template\n", spawned - before );
	CHECK( spawned > before );

	// The authored lifetime expires them; the level's own instance stays.
	frame.commands.clear();
	for ( uint32_t t = 30; t < 30 + 2 * config.tickRate + 30; ++t )
	{
		frame.tick = t;
		sim.Step( frame );
	}
	CHECK( CountFromTemplate( sim, 0 ) < spawned );

	// Authored material values really reach Box3D: a bouncy ball ends up higher than a dead one.
	float bouncy = BallHeightAfter( 0.9f, 150 );
	float dead = BallHeightAfter( 0.0f, 150 );
	std::printf( "    after 150 ticks: restitution 0.9 at y=%.3f, restitution 0 at y=%.3f\n", bouncy, dead );
	CHECK( bouncy > dead + 0.1f );

	// Values the registry does not know are dropped, and the rest is clamped into range.
	EntityTemplate hostile;
	hostile.components.push_back( MakeComponent( "NotAComponent", { { "whatever", 1.0f } } ) );
	hostile.components.push_back( MakeComponent( "Material", { { "friction", 9999.0f }, { "nope", 3.0f } } ) );
	SanitizeTemplate( hostile );
	CHECK( hostile.components.size() == 1 );
	CHECK( hostile.components[0].fields.size() == 1 );
	CHECK( TemplateFloat( hostile, Fnv32( "Material" ), Fnv32( "friction" ), 0.0f ) <= 10.0f );
}

// Impacts and footsteps are simulation state, not presentation guesses: they have to be produced
// identically by every build, survive rollback, and stay in the ring long enough to be seen.
void TestSimEvents()
{
	SimConfig config = TestConfig();

	// A ball dropped onto the floor registers an impact, with the entities and a sane speed.
	LevelLayout map;
	map.name = "impacts";
	map.spawnCenter = { 0.0f, 1.5f, 8.0f };
	map.spawnRadius = 2.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 20.0f, 0.5f, 20.0f }, 0.0f, 0.0f } );
	map.props.push_back( { ShapeKind::Sphere, { 0.0f, 5.0f, 0.0f }, { 0.4f, 0.0f, 0.0f } } );
	QuantizeLayout( map );

	Simulation sim( config, map );
	CHECK( sim.Globals().impactCount == 0 );

	InputFrame frame;
	for ( uint32_t t = 0; t < 120; ++t )
	{
		frame.tick = t;
		sim.Step( frame );
	}

	uint32_t impacts = sim.Globals().impactCount;
	std::printf( "    %u impacts after a 5 m drop\n", impacts );
	CHECK( impacts > 0 );

	const ImpactRecord& first = sim.Globals().impacts[0];
	CHECK( first.netIdA != 0 );
	CHECK( first.netIdB != 0 );
	CHECK( first.netIdA != first.netIdB );
	// It fell about 5 m under gravity, so it cannot have been a gentle touch.
	CHECK( first.speed > 2.0f );
	CHECK( first.point.y < 2.0f );

	// Two simulations of the same map must agree on every impact, not just on positions.
	Simulation other( config, map );
	for ( uint32_t t = 0; t < 120; ++t )
	{
		frame.tick = t;
		other.Step( frame );
	}
	CHECK( other.Globals().impactCount == impacts );
	CHECK( other.ComputeHash() == sim.ComputeHash() );

	// Footsteps: a walking player takes steps, a standing one does not, and a sprinting one takes
	// more of them over the same time.
	auto stepsAfter = []( bool sprint, bool move, uint32_t ticks ) {
		SimConfig cfg = TestConfig();
		Simulation s( cfg );
		InputFrame f;
		f.events.push_back( { PlayerEventType::Join, 0 } );
		f.tick = 0;
		s.Step( f );
		f.events.clear();
		for ( uint32_t t = 1; t < ticks; ++t )
		{
			f.tick = t;
			f.inputs[0].moveForward = move ? 127 : 0;
			f.inputs[0].buttons = sprint ? BtnSprint : 0;
			s.Step( f );
		}
		for ( const auto& r : s.Entities() )
		{
			const Character* c = flecs::entity( s.World(), r.entity ).try_get<Character>();
			if ( c != nullptr )
			{
				return c->stepCount;
			}
		}
		return uint32_t( 0 );
	};

	uint32_t standing = stepsAfter( false, false, 180 );
	uint32_t walking = stepsAfter( false, true, 180 );
	uint32_t sprinting = stepsAfter( true, true, 180 );
	std::printf( "    steps in 3 s: standing %u, walking %u, sprinting %u\n", standing, walking, sprinting );
	CHECK( standing == 0 );
	CHECK( walking > 0 );
	CHECK( sprinting > walking );

	// A rollback must not invent or lose either kind of event: restoring an older state restores
	// the counters with it, and re-simulating the same inputs reproduces them exactly.
	Simulation rolled( config, map );
	Snapshot snapshot;
	for ( uint32_t t = 0; t < 40; ++t )
	{
		frame.tick = t;
		rolled.Step( frame );
	}
	rolled.Save( snapshot );
	uint32_t atSave = rolled.Globals().impactCount;
	for ( uint32_t t = 40; t < 120; ++t )
	{
		frame.tick = t;
		rolled.Step( frame );
	}
	uint32_t atEnd = rolled.Globals().impactCount;
	rolled.Load( snapshot );
	CHECK( rolled.Globals().impactCount == atSave );
	for ( uint32_t t = 40; t < 120; ++t )
	{
		frame.tick = t;
		rolled.Step( frame );
	}
	CHECK( rolled.Globals().impactCount == atEnd );
	CHECK( rolled.ComputeHash() == sim.ComputeHash() );
}

void TestRepeatability()
{
	// Two simulations interleaved in one process also proves the arenas are isolated.
	auto frames = test::MakeScenario( {} );
	SimConfig config = TestConfig();
	Simulation a( config );
	Simulation b( config );
	CHECK( a.ComputeHash() == b.ComputeHash() );
	for ( const InputFrame& f : frames )
	{
		a.Step( f );
		b.Step( f );
		if ( a.ComputeHash() != b.ComputeHash() )
		{
			std::printf( "    diverged at tick %u\n", f.tick );
			CHECK( false );
		}
	}

	// Sanity: the scenario must actually exercise the game.
	size_t props = 0;
	for ( const auto& r : a.Entities() )
	{
		props += flecs::entity( a.World(), r.entity ).has<Prop>() ? 1 : 0;
	}
	std::printf( "    %zu entities, %zu props, %zu KB physics at end\n", a.Entities().size(), props, a.PhysicsBytesInUse() / 1024 );
	CHECK( props > 0 );
}

void TestSnapshotRoundTrip()
{
	auto frames = test::MakeScenario( {} );
	Simulation sim( TestConfig() );
	for ( uint32_t t = 0; t < 300; ++t )
	{
		sim.Step( frames[t] );
	}

	Snapshot snap;
	sim.Save( snap );
	CHECK( snap.hash == sim.ComputeHash() );

	std::vector<uint64_t> first;
	for ( uint32_t t = 300; t < 900; ++t )
	{
		sim.Step( frames[t] );
		first.push_back( sim.ComputeHash() );
	}

	sim.Load( snap );
	CHECK( sim.Tick() == 300 );
	CHECK( sim.ComputeHash() == snap.hash );
	for ( uint32_t t = 300; t < 900; ++t )
	{
		sim.Step( frames[t] );
		if ( sim.ComputeHash() != first[t - 300] )
		{
			std::printf( "    diverged after restore at tick %u\n", t );
			CHECK( false );
		}
	}
}

void TestPortableSnapshot()
{
	auto frames = test::MakeScenario( {} );
	SimConfig config = TestConfig();

	Simulation server( config );
	for ( uint32_t t = 0; t < 300; ++t )
	{
		server.Step( frames[t] );
	}

	std::vector<uint8_t> image;
	server.SavePortable( image );
	std::printf( "    portable image: %zu KB\n", image.size() / 1024 );

	// A "joining client": a separate simulation (different arena and Box3D world slot) that has
	// already run a different history.
	Simulation client( config );
	for ( uint32_t t = 0; t < 50; ++t )
	{
		client.Step( frames[t] );
	}
	CHECK( client.LoadPortable( image ) );
	CHECK( client.Tick() == server.Tick() );
	CHECK( client.ComputeHash() == server.ComputeHash() );

	for ( uint32_t t = 300; t < 900; ++t )
	{
		server.Step( frames[t] );
		client.Step( frames[t] );
		if ( client.ComputeHash() != server.ComputeHash() )
		{
			std::printf( "    diverged after portable load at tick %u\n", t );
			CHECK( false );
		}
	}
}

// Fills a stretch of the stack with a byte, so whatever a struct copy picks up from the stack differs
// between two runs.
#if defined( _MSC_VER )
__declspec( noinline )
#else
__attribute__( ( noinline ) )
#endif
void DirtyStack( uint8_t value )
{
	volatile uint8_t junk[64 * 1024];
	for ( size_t i = 0; i < sizeof( junk ); ++i )
	{
		junk[i] = value;
	}
}

// A portable snapshot holds only simulation state: two worlds that ran the same frames give the same
// bytes, whatever the stack held while they ran and saved. (Box3D used to write struct padding and a
// heap pointer; cmake/patches/box3d-snapshot-padding.patch.)
void TestPortableBytes()
{
	auto frames = test::MakeScenario( {} );
	SimConfig config = TestConfig();
	std::vector<uint8_t> images[2];
	const uint8_t patterns[2] = { 0x00, 0xA5 };
	for ( int run = 0; run < 2; ++run )
	{
		Simulation sim( config );
		for ( uint32_t t = 0; t < 300; ++t )
		{
			DirtyStack( patterns[run] );
			sim.Step( frames[t] );
		}
		DirtyStack( patterns[run] );
		sim.SavePortable( images[run] );
	}
	CHECK( images[0].size() == images[1].size() );
	size_t differing = 0;
	for ( size_t i = 0; i < std::min( images[0].size(), images[1].size() ); ++i )
	{
		if ( images[0][i] != images[1][i] )
		{
			if ( differing == 0 )
			{
				std::printf( "    first differing byte at %zu of %zu\n", i, images[0].size() );
			}
			++differing;
		}
	}
	if ( differing != 0 )
	{
		std::printf( "    %zu bytes differ\n", differing );
	}
	CHECK( differing == 0 );
}

// Deliver authoritative frames late and out of step with the client, and have the client
// sometimes sample a local input that the server did not use. Every confirmed state must still
// equal the server's.
// `every`: mispredictions are fixed only every so many ticks (RollbackSession::SetReconcileEvery).
void RunRollbackScenario( uint32_t maxDelay, uint32_t window, uint64_t seed, uint32_t every = 1 )
{
	test::ScenarioOptions opt;
	opt.seed = seed;
	auto frames = test::MakeScenario( opt );
	SimConfig config = TestConfig();
	auto reference = RunReference( frames, config );

	const PlayerSlot local = 0;
	RollbackSession session( config, local, window );
	session.SetReconcileEvery( every );
	uint64_t rng = seed * 31 + 7;

	uint32_t delivered = 0;
	uint32_t total = uint32_t( frames.size() );
	uint32_t checked = 0;
	uint32_t nextCheck = 0;

	for ( uint32_t serverTick = 0; session.CurrentTick() < total || delivered < total; ++serverTick )
	{
		// Network: everything up to (serverTick - delay) has arrived.
		uint32_t delay = uint32_t( NextRandom( rng ) % ( maxDelay + 1 ) );
		uint32_t arrived = serverTick > delay ? std::min( serverTick - delay, total ) : 0;
		if ( serverTick > total + maxDelay )
		{
			arrived = total;
		}
		while ( delivered < arrived )
		{
			session.AddAuthoritativeFrame( frames[delivered] );
			++delivered;
		}

		session.Reconcile();

		// Client advances one tick per frame (a real client runs off wall-clock time).
		uint32_t t = session.CurrentTick();
		if ( t < total )
		{
			PlayerInput localInput = frames[t].inputs[local];
			if ( ( NextRandom( rng ) % 10 ) == 0 )
			{
				// The server will not see this input in time and uses something else.
				localInput.moveForward = int8_t( localInput.moveForward ^ 0x55 );
				localInput.buttons ^= BtnJump;
			}
			session.AdvanceOne( localInput );
		}

		for ( ; nextCheck <= total; ++nextCheck )
		{
			uint64_t h;
			if ( session.GetConfirmedHash( nextCheck, h ) == false )
			{
				if ( nextCheck > session.FinalizedTick() )
				{
					break;
				}
				continue; // final, and not hashed: predicted in a re-simulation and confirmed as it was
			}
			if ( h != reference[nextCheck] )
			{
				std::printf( "    confirmed state mismatch at tick %u\n", nextCheck );
				CHECK( false );
			}
			++checked;
		}
	}

	session.SetReconcileEvery( 1 );
	session.Reconcile();
	CHECK( session.CurrentTick() == total );
	CHECK( session.Sim().ComputeHash() == reference[total] );

	const auto& stats = session.GetStats();
	std::printf( "    delay<=%u window=%u every=%u: %u states verified, %" PRIu64 " rollbacks, %" PRIu64 " resimulated ticks, %" PRIu64
				 " snapshots, %" PRIu64 " stalls\n",
				 maxDelay, window, every, checked, stats.rollbacks, stats.resimulatedTicks, stats.saves, stats.stalls );
	CHECK( checked > total / 2 );
	CHECK( stats.rollbacks > 0 );
}

void TestRollback()
{
	RunRollbackScenario( 4, 8, 11 );
	RunRollbackScenario( 12, 8, 22 ); // delays beyond the window force stalls
	RunRollbackScenario( 2, 2, 33 );
	RunRollbackScenario( 6, 8, 44, 3 ); // fixed every third tick
	RunRollbackScenario( 12, 8, 55, 8 );
}

// The hybrid network model, without a network (M248; ROADMAP step 3). A server's simulation plays
// the scenario (joins and leaves, props, kills with ragdolls, respawns, impulses); every tick its
// physics inputs (the frame's commands, the physics events and pushers it made) and its physics
// checksum go to a client of slot 0, and every second tick its characters' state, each arriving
// some ticks later. The client's HybridSession steps the physics `buffer` ticks behind the server
// and predicts its own character ahead. Checked: the client's physics world is the server's on
// every tick (its checksum); the others stand where the server had them; the own character,
// predicted, is where the server then has it.
void RunHybridScenario( uint32_t maxDelay, uint64_t seed, bool commands )
{
	test::ScenarioOptions opt;
	opt.seed = seed;
	opt.commands = commands;
	opt.churn = commands;
	const auto frames = test::MakeScenario( opt );
	const SimConfig config = TestConfig();
	const uint32_t total = uint32_t( frames.size() );
	const PlayerSlot local = 0;

	auto serverSim = std::make_unique<Simulation>( config ); // (two simulations: not on the stack)
	Simulation& server = *serverSim;
	HybridSession client( config, local, 40 );
	{
		std::vector<uint8_t> image;
		server.SavePortable( image );
		CHECK( client.Sim().LoadPortable( image ) );
		client.Start();
	}
	struct Sent
	{
		uint32_t arrives;
		bool state;
		uint32_t tick;
		InputFrame frame;
		uint64_t checksum;
		std::vector<uint8_t> bytes;
	};
	std::vector<Sent> wire;
	uint64_t rng = seed * 131 + 5;

	// Where slot 0's body is, by tick: the server's, and the client's first prediction of it; and
	// where every character is on the server, by tick.
	struct Seen
	{
		bool has = false;
		uint32_t netId = 0;
		b3Vec3 at = {};
	};
	std::vector<Seen> serverSaw( total + 1 ), clientSaw( total + 1 );
	std::vector<std::map<uint32_t, b3Vec3>> serverCharacters( total + 1 );
	auto ownOf = []( Simulation& sim, Seen& seen ) {
		const uint32_t netId = sim.Globals().playerNetIds[0];
		const Transform* t = netId != 0 ? sim.EntityTransform( netId ) : nullptr;
		if ( t != nullptr )
		{
			seen = { true, netId, t->position };
		}
	};

	size_t stateBytes = 0, states = 0, events = 0;
	uint32_t othersCompared = 0, othersOff = 0;
	const uint32_t buffer = maxDelay + 1;
	const uint32_t lead = maxDelay + 2;
	for ( uint32_t tick = 0; tick < total; ++tick )
	{
		{
			std::map<uint32_t, b3Vec3>& seen = serverCharacters[server.Tick()];
			for ( int slot = 0; slot < kMaxPlayers; ++slot )
			{
				const uint32_t netId = server.Globals().playerNetIds[slot];
				if ( const Transform* t = netId != 0 ? server.EntityTransform( netId ) : nullptr )
				{
					seen[netId] = t->position;
				}
			}
		}
		if ( server.Tick() % 2 == 0 )
		{
			Sent sent{ tick + uint32_t( NextRandom( rng ) % maxDelay ), true, server.Tick(), {}, 0, {} };
			server.SaveState( sent.bytes, true );
			stateBytes += sent.bytes.size();
			states += 1;
			wire.push_back( std::move( sent ) );
		}
		server.Step( frames[tick] );
		ownOf( server, serverSaw[server.Tick()] );
		{
			Sent sent{ tick + uint32_t( NextRandom( rng ) % maxDelay ), false, tick, frames[tick], server.PhysicsHash(), {} };
			sent.frame.physics = server.PhysicsMade().physics;
			sent.frame.pushers = server.PhysicsMade().pushers;
			events += sent.frame.physics.size();
			wire.push_back( std::move( sent ) );
		}

		// What has arrived (in any order).
		for ( size_t i = 0; i < wire.size(); )
		{
			if ( wire[i].arrives <= tick )
			{
				if ( wire[i].state )
				{
					client.AddState( wire[i].tick, wire[i].bytes.data(), wire[i].bytes.size() );
				}
				else
				{
					client.AddChecksum( wire[i].tick, wire[i].checksum );
					client.AddFrame( wire[i].frame );
				}
				wire.erase( wire.begin() + std::ptrdiff_t( i ) );
			}
			else
			{
				++i;
			}
		}
		// The physics `buffer` ticks behind the server; the others where the server had them then.
		if ( tick + 1 > buffer )
		{
			client.AdvancePhysics( tick + 1 - buffer );
		}
		if ( const uint32_t p = client.PlacedTick(); p != UINT32_MAX )
		{
			for ( const auto& [netId, at] : serverCharacters[p] )
			{
				if ( netId == client.Sim().Globals().playerNetIds[local] || client.Sim().IsStandIn( netId ) == false )
				{
					continue;
				}
				const Transform* t = client.Sim().EntityTransform( netId );
				othersCompared += 1;
				// (Rounded to 1/1024 m on the way.)
				othersOff += t == nullptr || b3Length( b3Sub( t->position, at ) ) > 0.002f ? 1 : 0;
			}
		}
		// The own character ahead of the server by about the delay, a tick or two a frame.
		for ( int step = 0; step < 2 && client.PredictedTick() < std::min( tick + 1 + lead, total ); ++step )
		{
			if ( client.Predict( frames[client.PredictedTick()].inputs[local] ) == false )
			{
				break;
			}
			if ( clientSaw[client.PredictedTick()].has == false )
			{
				ownOf( client.Sim(), clientSaw[client.PredictedTick()] );
			}
		}
	}

	// How far the prediction of its own body was from where the server then had it.
	uint32_t compared = 0, nearby = 0;
	float worst = 0.0f, sum = 0.0f;
	for ( uint32_t t = 0; t <= total; ++t )
	{
		const Seen& a = serverSaw[t];
		const Seen& b = clientSaw[t];
		if ( a.has == false || b.has == false || a.netId != b.netId )
		{
			continue;
		}
		const float off = b3Length( b3Sub( a.at, b.at ) );
		compared += 1;
		nearby += off < 0.05f ? 1 : 0;
		sum += off;
		worst = std::max( worst, off );
	}
	const auto& stats = client.GetStats();
	std::printf( "    delay<=%u%s: %zu physics events; %zu states of %zu bytes on average, %" PRIu64 " taken; physics %" PRIu64
				 " ticks (%" PRIu64 " checksums, %" PRIu64 " different, the first at %u), %.3f ms a step; others placed %u, off %u; "
				 "own body within 5 cm %u of %u ticks, %.3f m on average, %.3f at most (%" PRIu64 " ticks predicted again)\n",
				 maxDelay, commands ? ", commands" : "", events, states, states > 0 ? stateBytes / states : 0, stats.states,
				 stats.physicsSteps, stats.checksums, stats.desyncs, client.DesyncTick(),
				 stats.physicsSteps > 0 ? double( stats.physicsNs ) / 1e6 / double( stats.physicsSteps ) : 0.0, othersCompared, othersOff,
				 nearby, compared, compared > 0 ? sum / float( compared ) : 0.0f, worst, stats.resimulatedTicks );
	CHECK( stats.rejected == 0 );
	CHECK( stats.physicsSteps + buffer + 2 >= total );
	CHECK( stats.checksums + buffer + 2 >= total );
	CHECK( stats.desyncs == 0 );
	CHECK( othersCompared > 200 );
	CHECK( othersOff == 0 );
	CHECK( compared > 300 );
	if ( commands == false )
	{
		// Only bumping into the others (placed by the states, in the past) can put it off. With
		// commands, a kill, a respawn or an impulse is the server's, seen a state later.
		CHECK( nearby * 10 >= compared * 9 );
	}
}

void TestHybridModel()
{
	RunHybridScenario( 3, 7, false );
	RunHybridScenario( 10, 8, false );
	RunHybridScenario( 3, 9, true );
	RunHybridScenario( 10, 10, true );
}

void TestRollbackReset()
{
	// Join mid-game via portable snapshot, then keep predicting.
	auto frames = test::MakeScenario( {} );
	SimConfig config = TestConfig();
	auto reference = RunReference( frames, config );

	Simulation server( config );
	for ( uint32_t t = 0; t < 400; ++t )
	{
		server.Step( frames[t] );
	}
	std::vector<uint8_t> image;
	server.SavePortable( image );

	RollbackSession session( config, 3, 8 );
	CHECK( session.Sim().LoadPortable( image ) );
	Snapshot snap;
	session.Sim().Save( snap );
	session.Reset( snap, frames[399].inputs );
	CHECK( session.CurrentTick() == 400 );

	for ( uint32_t t = 400; t < frames.size(); ++t )
	{
		session.AddAuthoritativeFrame( frames[t] );
		session.Reconcile();
		session.AdvanceOne( frames[t].inputs[3] );
	}
	session.Reconcile();
	CHECK( session.Sim().ComputeHash() == reference.back() );
}

// The physics on several threads: what is simulated must not depend on how many workers a world has
// or on how many threads run them, and a snapshot taken between threaded steps must restore.
void TestPhysicsThreads()
{
	test::ScenarioOptions opt;
	opt.initialPlayers = 24;
	opt.maxPlayers = 32;
	opt.ticks = 900;
	auto frames = test::MakeScenario( opt );
	SimConfig config = TestConfig();

	auto run = [&]( uint32_t workers, int threads ) {
		Simulation::SetWorkersForNewSimulations( workers );
		PhysicsThreads::Start( threads );
		Simulation sim( config );
		sim.SetPhysicsThreaded( threads > 0 );
		std::vector<uint64_t> hashes;
		Snapshot snap;
		for ( const InputFrame& f : frames )
		{
			// Every so often: a few ticks on, back, and the same ticks again.
			if ( f.tick % 97 == 50 && f.tick + 5 < frames.size() )
			{
				sim.Save( snap );
				for ( uint32_t k = 0; k < 5; ++k )
				{
					sim.Step( frames[f.tick + k] );
				}
				sim.Load( snap );
			}
			sim.Step( f );
			hashes.push_back( sim.ComputeHash() );
		}
		return hashes;
	};

	auto reference = run( 1, 0 );
	struct Case
	{
		uint32_t workers;
		int threads;
	};
	for ( Case c : { Case{ kPhysicsWorkers, 0 }, Case{ kPhysicsWorkers, 3 }, Case{ 2, 1 }, Case{ 8, 3 }, Case{ 8, 7 }, Case{ kPhysicsWorkers, 3 } } )
	{
		auto hashes = run( c.workers, c.threads );
		size_t first = 0;
		while ( first < hashes.size() && hashes[first] == reference[first] )
		{
			++first;
		}
		std::printf( "    %u workers on %d threads: %s\n", c.workers, c.threads + 1, first == hashes.size() ? "the same" : "DIFFERENT" );
		if ( first != hashes.size() )
		{
			std::printf( "    first difference at tick %zu\n", first );
		}
		CHECK( first == hashes.size() );
	}
	Simulation::SetWorkersForNewSimulations( kPhysicsWorkers );
	PhysicsThreads::Start( 0 );
}

// What two loops cost with a full server: 64 players with eight items each, props about (the
// scenario's). The slots' part of a step, and a ray (a shot, a probe), timed; a report, no limits.
void TestLoops()
{
	test::ScenarioOptions opt;
	opt.initialPlayers = kMaxPlayers;
	opt.maxPlayers = kMaxPlayers;
	opt.churn = false;
	opt.ticks = 900;
	auto frames = test::MakeScenario( opt );
	// Eight items for everyone, a while after joining.
	for ( int slot = 0; slot < kMaxPlayers; ++slot )
	{
		for ( int i = 0; i < 8; ++i )
		{
			SimCommand c;
			c.type = CommandType::SpawnItem;
			c.target = SlotTarget( PlayerSlot( slot ) );
			c.index = uint16_t( i % 2 );
			c.mode = kNoSocket;
			c.value = 1;
			frames[40].commands.push_back( c );
		}
	}

	SimConfig config = TestConfig();
	config.physicsArenaMB = 128;
	config.slots = 8;
	config.slotHand = 0;
	Simulation sim( config );
	ItemShape stick, rock;
	stick.holster = 1;
	sim.SetItemShapes( { stick, rock } );

	// (Timed as the game runs: without the tests' check of every index answer.)
	struct Unchecked
	{
		bool was = Simulation::s_checkIndexes;
		Unchecked() { Simulation::s_checkIndexes = false; }
		~Unchecked() { Simulation::s_checkIndexes = was; }
	} unchecked;
	for ( size_t t = 0; t < 300; ++t )
	{
		sim.Step( frames[t] );
	}
	Simulation::StepProfile before = sim.Profile();
	auto t0 = Clock::now();
	for ( size_t t = 300; t < frames.size(); ++t )
	{
		sim.Step( frames[t] );
	}
	double steps = double( frames.size() - 300 );
	double stepMs = MsSince( t0 ) / steps;
	double slotsMs = double( sim.Profile().slotsNs - before.slotsNs ) / 1e6 / steps;

	size_t held = 0;
	for ( const auto& r : sim.Entities() )
	{
		held += flecs::entity( sim.World(), r.entity ).has<HeldItem>() ? 1 : 0;
	}
	// Rays from each player, as many as a busy tick might cast (everyone shooting twice).
	const int rays = 2 * kMaxPlayers;
	t0 = Clock::now();
	int hits = 0;
	for ( int i = 0; i < rays; ++i )
	{
		uint32_t netId = sim.PlayerNetId( PlayerSlot( i % kMaxPlayers ) );
		const Transform* from = netId != 0 ? sim.EntityTransform( netId ) : nullptr;
		RayHit hit;
		if ( from != nullptr && sim.CastRay( from->position, b3Vec3{ 30.0f * float( ( i % 7 ) - 3 ), -1.0f, 30.0f }, netId, hit ) )
		{
			hits += 1;
		}
	}
	double rayMs = MsSince( t0 ) / double( rays );
	std::printf( "    %zu entities (%zu items held); a step %.3f ms, of it the slots %.3f ms; a ray %.4f ms (%d of %d hit)\n",
				 sim.Entities().size(), held, stepMs, slotsMs, rayMs, hits, rays );
	CHECK( held == size_t( 8 * kMaxPlayers ) );
}

void TestStress()
{
	// 64 players, lots of props: timing report for the rollback budget.
	test::ScenarioOptions opt;
	opt.initialPlayers = kMaxPlayers;
	opt.maxPlayers = kMaxPlayers;
	opt.churn = false;
	opt.ticks = 900;
	auto frames = test::MakeScenario( opt );

	SimConfig config = TestConfig();
	config.physicsArenaMB = 128;
	Simulation sim( config );

	double stepMs = 0.0, maxStepMs = 0.0, saveMs = 0.0, loadMs = 0.0;
	Snapshot snap;
	for ( const InputFrame& f : frames )
	{
		auto t0 = Clock::now();
		sim.Step( f );
		double ms = MsSince( t0 );
		stepMs += ms;
		maxStepMs = std::max( maxStepMs, ms );

		t0 = Clock::now();
		sim.Save( snap );
		saveMs += MsSince( t0 );
	}
	for ( int i = 0; i < 20; ++i )
	{
		auto t0 = Clock::now();
		sim.Load( snap );
		loadMs += MsSince( t0 );
	}

	size_t props = 0;
	for ( const auto& r : sim.Entities() )
	{
		props += flecs::entity( sim.World(), r.entity ).has<Prop>() ? 1 : 0;
	}
	double n = double( frames.size() );
	std::printf( "    64 players, %zu props: step avg %.3f ms (max %.3f), save %.3f ms, load %.3f ms, snapshot %zu KB\n", props,
				 stepMs / n, maxStepMs, saveMs / n, loadMs / 20.0, ( snap.ecs.size() + snap.physics.size() ) / 1024 );
	std::printf( "    worst-case 8-tick rollback ~ %.2f ms\n", 8.0 * ( stepMs / n + saveMs / n ) + loadMs / 20.0 );
	CHECK( props > 0 );

	// The same steps with the physics on four threads: the physics part of a step, side by side.
	for ( int threads : { 0, 3 } )
	{
		PhysicsThreads::Start( threads );
		Simulation other( config );
		other.SetPhysicsThreaded( threads > 0 );
		for ( const InputFrame& f : frames )
		{
			other.Step( f );
		}
		const Simulation::StepProfile& sp = other.Profile();
		std::printf( "    physics on %d threads: %.3f ms a step (collide %.3f, solve %.3f)%s\n", threads + 1,
					 double( sp.physicsNs ) / 1e6 / n, sp.collideMs / n, sp.solveMs / n, other.ComputeHash() == sim.ComputeHash() ? "" : " DIFFERENT" );
		CHECK( other.ComputeHash() == sim.ComputeHash() );
	}
	PhysicsThreads::Start( 0 );
}

void TestGameplaySanity()
{
	Simulation sim( TestConfig() );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
		}
	};
	auto player = [&]() { return flecs::entity( sim.World(), sim.FindEntity( sim.Globals().playerNetIds[0] ) ); };

	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 90 );
	CHECK( sim.IsPlayerActive( 0 ) );
	const Character& idle = player().get<Character>();
	b3Vec3 start = player().get<Transform>().position;
	std::printf( "    idle: y=%.3f grounded=%d\n", start.y, idle.grounded );
	CHECK( idle.grounded == 1 );
	CHECK( start.y > 0.5f && start.y < 2.5f );

	// Walk "forward" (+Z at yaw 0) for 2 seconds.
	f.inputs[0].moveForward = 127;
	step( 120 );
	b3Vec3 walked = player().get<Transform>().position;
	float dz = walked.z - start.z;
	std::printf( "    walked dz=%.3f dx=%.3f facing=%.3f\n", dz, walked.x - start.x, player().get<Character>().facingYaw );
	CHECK( dz > 4.0f && dz < 6.5f );
	CHECK( b3AbsFloat( walked.x - start.x ) < 0.01f );

	// Turn the camera 90 degrees: forward is now +X, and the character turns to face it.
	f.inputs[0].cameraYaw = 16384;
	step( 60 );
	b3Vec3 turned = player().get<Transform>().position;
	std::printf( "    turned dx=%.3f facing=%.3f\n", turned.x - walked.x, player().get<Character>().facingYaw );
	CHECK( turned.x - walked.x > 1.5f );
	CHECK( b3AbsFloat( player().get<Character>().facingYaw - 1.5708f ) < 0.01f );

	// Jump: goes up, comes back down, lands.
	f.inputs[0] = {};
	step( 30 );
	float groundY = player().get<Transform>().position.y;
	f.inputs[0].buttons = BtnJump;
	step( 1 );
	f.inputs[0].buttons = 0;
	float peak = groundY;
	uint32_t maxAir = 0;
	for ( int i = 0; i < 90; ++i )
	{
		step( 1 );
		peak = std::max( peak, player().get<Transform>().position.y );
		maxAir = std::max( maxAir, player().get<Character>().airTicks );
	}
	std::printf( "    jump: ground %.3f peak %.3f air ticks %u\n", groundY, peak, maxAir );
	CHECK( peak - groundY > 0.8f );
	CHECK( player().get<Character>().grounded == 1 );
	CHECK( b3AbsFloat( player().get<Transform>().position.y - groundY ) < 0.05f );

	// Spawn 15 props for the player: the per-player cap keeps 10.
	for ( int i = 0; i < 15; ++i )
	{
		SimCommand spawn;
		spawn.type = CommandType::SpawnProp;
		spawn.target = SlotTarget( 0 );
		spawn.other = sim.Config().PropLifetimeTicks();
		spawn.value = -1;
		spawn.a = { float( i ) * 0.6f - 4.0f, 2.0f, 4.0f };
		spawn.c = { 0.25f, 0.25f, 0.25f };
		f.commands.push_back( spawn );
		step( 1 );
		f.commands.clear();
		step( 1 );
	}
	uint32_t owned = 0;
	for ( const auto& r : sim.Entities() )
	{
		const Prop* p = flecs::entity( sim.World(), r.entity ).try_get<Prop>();
		owned += ( p != nullptr && p->owner == sim.Globals().playerNetIds[0] ) ? 1 : 0;
	}
	std::printf( "    props owned after 15 spawns: %u\n", owned );
	CHECK( owned == 10 );

	// Props expire after their lifetime.
	step( int( sim.Config().PropLifetimeTicks() ) + 1 );
	owned = 0;
	for ( const auto& r : sim.Entities() )
	{
		const Prop* p = flecs::entity( sim.World(), r.entity ).try_get<Prop>();
		owned += ( p != nullptr && p->owner != 0 ) ? 1 : 0;
	}
	CHECK( owned == 0 );

	// Leaving removes the player.
	f.events.push_back( { PlayerEventType::Leave, 0 } );
	step( 1 );
	CHECK( sim.IsPlayerActive( 0 ) == false );
}

// A flat, empty floor: nothing in the way of what a test wants to measure.
LevelLayout FlatMap()
{
	LevelLayout map;
	map.name = "flat";
	map.spawnCenter = { 0.0f, 1.5f, 0.0f };
	map.spawnRadius = 2.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 30.0f, 0.5f, 30.0f }, 0.0f, 0.0f } );
	QuantizeLayout( map );
	return map;
}

uint32_t CountWith( Simulation& sim, bool ( *pred )( flecs::entity ) )
{
	uint32_t n = 0;
	for ( const auto& r : sim.Entities() )
	{
		n += pred( flecs::entity( sim.World(), r.entity ) ) ? 1 : 0;
	}
	return n;
}

flecs::entity NewestRagdoll( Simulation& sim )
{
	flecs::entity found;
	for ( const auto& r : sim.Entities() )
	{
		flecs::entity e( sim.World(), r.entity );
		if ( e.has<Ragdoll>() )
		{
			found = e;
		}
	}
	return found;
}

// Commands are how the server's mods change the world. Each one has to do exactly its job, and
// anything aimed at something that does not exist has to be ignored the same way everywhere.
void TestCommands()
{
	Simulation sim( TestConfig(), FlatMap() );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto command = [&]( CommandType type, uint32_t target ) {
		SimCommand c;
		c.type = type;
		c.target = target;
		f.commands.push_back( c );
		return &f.commands.back();
	};

	f.events.push_back( { PlayerEventType::Join, 0 } );
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 60 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	uint32_t p1 = sim.PlayerNetId( 1 );
	CHECK( p0 != 0 && p1 != 0 );

	// Fields: per entity by slot or NetId, global with target 0; bad slots and targets do nothing.
	command( CommandType::SetField, SlotTarget( 0 ) )->value = 42;
	{
		SimCommand* c = command( CommandType::SetField, p1 );
		c->index = 5;
		c->value = -7;
	}
	{
		SimCommand* c = command( CommandType::SetField, 0 );
		c->index = 2;
		c->value = 99;
	}
	command( CommandType::SetField, SlotTarget( 0 ) )->index = test::kScenarioFields;
	command( CommandType::SetField, 123456 )->value = 1;
	command( CommandType::SetField, SlotTarget( 9 ) )->value = 1;
	step( 1 );
	CHECK( sim.BoardValue( p0, 0 ) == 42 );
	CHECK( sim.BoardValue( p1, 5 ) == -7 );
	CHECK( sim.GlobalBoardValue( 2 ) == 99 );

	// How many fields there are is the config's (what the server's mods declared), not a constant:
	// three hundred of them, written, read, and through a snapshot; one past the last does nothing.
	{
		SimConfig wide = TestConfig();
		wide.fields = 300;
		wide.globalFields = 70;
		Simulation big( wide );
		InputFrame bf;
		auto bstep = [&]( int n ) {
			for ( int i = 0; i < n; ++i )
			{
				bf.tick = big.Tick();
				big.Step( bf );
				bf.events.clear();
				bf.commands.clear();
			}
		};
		auto set = [&]( uint32_t target, uint16_t index, int32_t value ) {
			SimCommand c;
			c.type = CommandType::SetField;
			c.target = target;
			c.index = index;
			c.value = value;
			bf.commands.push_back( c );
		};
		bf.events.push_back( { PlayerEventType::Join, 0 } );
		bstep( 30 );
		uint32_t who = big.PlayerNetId( 0 );
		set( SlotTarget( 0 ), 299, 7 );
		set( SlotTarget( 0 ), 300, 8 );
		set( 0, 69, 9 );
		set( 0, 70, 10 );
		bstep( 1 );
		CHECK( big.BoardValue( who, 299 ) == 7 && big.BoardValue( who, 300 ) == 0 );
		CHECK( big.GlobalBoardValue( 69 ) == 9 && big.GlobalBoardValue( 70 ) == 0 );
		CHECK( big.GetBoard( big.FindEntity( who ) ).values.size() == 300 );
		Snapshot kept;
		big.Save( kept );
		uint64_t hashThen = big.ComputeHash();
		set( SlotTarget( 0 ), 299, 1 );
		set( 0, 69, 1 );
		bstep( 5 );
		CHECK( big.BoardValue( who, 299 ) == 1 && big.ComputeHash() != hashThen );
		big.Load( kept );
		CHECK( big.BoardValue( who, 299 ) == 7 && big.GlobalBoardValue( 69 ) == 9 && big.ComputeHash() == hashThen );
		// A simulation whose mods declared none: a field command does nothing, and nothing is kept.
		SimConfig none = TestConfig();
		none.fields = 0;
		none.globalFields = 0;
		Simulation bare( none );
		bf = {};
		bf.events.push_back( { PlayerEventType::Join, 0 } );
		bf.tick = bare.Tick();
		bare.Step( bf );
		bf.events.clear();
		set( SlotTarget( 0 ), 0, 5 );
		set( 0, 0, 5 );
		bf.tick = bare.Tick();
		bare.Step( bf );
		CHECK( bare.BoardValue( bare.PlayerNetId( 0 ), 0 ) == 0 && bare.GlobalBoardValue( 0 ) == 0 );
		CHECK( bare.HasBoard( bare.FindEntity( bare.PlayerNetId( 0 ) ) ) == false );
	}

	// Events land in the ring with their targets resolved to NetIds.
	uint32_t eventsBefore = sim.Globals().modEventCount;
	{
		SimCommand* c = command( CommandType::Event, SlotTarget( 0 ) );
		c->index = 3;
		c->other = SlotTarget( 1 );
		c->value = 11;
		c->a = { 1.0f, 2.0f, 3.0f };
	}
	uint32_t eventTick = sim.Tick();
	step( 1 );
	CHECK( sim.Globals().modEventCount == eventsBefore + 1 );
	const ModEventRecord& ev = sim.Globals().modEvents[eventsBefore % kModEventHistory];
	CHECK( ev.type == 3 && ev.netIdA == p0 && ev.netIdB == p1 && ev.value == 11 && ev.tick == eventTick );
	CHECK( ev.point.y == 2.0f );

	// A prop, then its destruction. Players cannot be destroyed by command.
	uint32_t nextId = sim.Globals().nextNetId;
	{
		SimCommand* c = command( CommandType::SpawnProp, SlotTarget( 0 ) );
		c->value = -1;
		c->a = { 3.0f, 2.0f, 3.0f };
		c->c = { 0.3f, 0.3f, 0.3f };
	}
	step( 1 );
	CHECK( sim.FindEntity( nextId ).is_valid() );
	CHECK( sim.FindEntity( nextId ).get<Prop>().owner == p0 );
	command( CommandType::Destroy, nextId );
	command( CommandType::Destroy, SlotTarget( 1 ) );
	step( 1 );
	CHECK( sim.FindEntity( nextId ).is_valid() == false );
	CHECK( sim.IsPlayerActive( 1 ) );

	// Non-finite values never reach Box3D.
	nextId = sim.Globals().nextNetId;
	{
		SimCommand* c = command( CommandType::SpawnProp, 0 );
		c->value = -1;
		c->a = { 0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f };
	}
	step( 1 );
	CHECK( sim.Globals().nextNetId == nextId );

	// Knockback on a character is a change of velocity.
	float before = sim.EntityTransform( p1 )->position.y;
	command( CommandType::Impulse, p1 )->b = { 0.0f, 6.0f, 0.0f };
	step( 10 );
	float after = sim.EntityTransform( p1 )->position.y;
	std::printf( "    knockback lifted the player %.2f m\n", after - before );
	CHECK( after - before > 0.3f );
	step( 60 );

	// Rays name what they hit, can skip one entity, and skip dead players.
	b3Vec3 above = b3Add( sim.EntityTransform( p0 )->position, b3Vec3{ 0.0f, 3.0f, 0.0f } );
	RayHit hit;
	CHECK( sim.CastRay( above, { 0.0f, -10.0f, 0.0f }, 0, hit ) );
	CHECK( hit.netId == p0 );
	CHECK( sim.CastRay( above, { 0.0f, -10.0f, 0.0f }, p0, hit ) );
	CHECK( hit.netId != p0 && hit.netId != 0 );

	// Death without a ragdoll: the body is gone from the world and input does nothing.
	command( CommandType::Kill, SlotTarget( 0 ) );
	step( 1 );
	CHECK( sim.PlayerCharacter( 0 )->dead == 1 );
	CHECK( NewestRagdoll( sim ).is_valid() == false );
	CHECK( sim.CastRay( above, { 0.0f, -10.0f, 0.0f }, 0, hit ) );
	CHECK( hit.netId != p0 );
	b3Vec3 deadAt = sim.EntityTransform( p0 )->position;
	f.inputs[0].moveForward = 127;
	step( 30 );
	CHECK( b3Distance( sim.EntityTransform( p0 )->position, deadAt ) == 0.0f );

	// Respawn brings it back at a chosen place.
	{
		SimCommand* c = command( CommandType::Respawn, SlotTarget( 0 ) );
		c->mode = 1;
		c->a = { 5.0f, 1.5f, -5.0f };
	}
	step( 1 );
	CHECK( sim.PlayerCharacter( 0 )->dead == 0 );
	CHECK( b3Distance( sim.EntityTransform( p0 )->position, b3Vec3{ 5.0f, 1.5f, -5.0f } ) < 0.5f );
	f.inputs[0].moveForward = 0;

	// Frozen: movement input does nothing; released, it moves again.
	{
		SimCommand* c = command( CommandType::Freeze, SlotTarget( 0 ) );
		c->mode = 1;
	}
	step( 1 );
	CHECK( sim.PlayerCharacter( 0 )->frozen == 1 );
	b3Vec3 frozenAt = sim.EntityTransform( p0 )->position;
	f.inputs[0].moveForward = 127;
	f.inputs[0].buttons = BtnJump;
	step( 30 );
	b3Vec3 stillAt = sim.EntityTransform( p0 )->position;
	CHECK( b3Distance( b3Vec3{ stillAt.x, 0.0f, stillAt.z }, b3Vec3{ frozenAt.x, 0.0f, frozenAt.z } ) < 0.01f );
	CHECK( stillAt.y < frozenAt.y + 0.05f );
	command( CommandType::Freeze, SlotTarget( 0 ) );
	f.inputs[0].buttons = 0;
	step( 30 );
	CHECK( b3Distance( sim.EntityTransform( p0 )->position, stillAt ) > 0.5f );
	f.inputs[0].moveForward = 0;

	// Aim: the flag follows the command, and the look direction follows the input, relative to
	// where the body faces.
	{
		SimCommand* c = command( CommandType::Aim, SlotTarget( 0 ) );
		c->mode = 1;
	}
	f.inputs[0].cameraPitch = 4096; // 22.5 degrees up
	step( 1 );
	AnimStateCopy look = sim.EntityAnimState( p0 );
	CHECK( look != nullptr && look->aiming == 1 );
	CHECK( std::fabs( look->aimPitch - 0.25f * detmath::kPi / 2.0f ) < 1e-4f );
	float facing = sim.PlayerCharacter( 0 )->facingYaw;
	CHECK( std::fabs( detmath::WrapAngle( look->aimYaw + facing - detmath::YawToRadians( f.inputs[0].cameraYaw ) ) ) < 1e-4f );
	// Facing: freelook turns toward the direction of travel; camera-facing snaps to the camera and
	// the legs turn toward where the body goes instead (backwards past ~100 degrees).
	{
		SimCommand* c = command( CommandType::Facing, SlotTarget( 0 ) );
		c->mode = 1;
	}
	f.inputs[0].cameraYaw = 16384; // a quarter turn
	f.inputs[0].moveRight = 127;   // strafing: sideways relative to the camera
	step( 40 );
	CHECK( sim.PlayerCharacter( 0 )->faceCamera == 1 );
	CHECK( std::fabs( detmath::WrapAngle( sim.PlayerCharacter( 0 )->facingYaw - detmath::YawToRadians( 16384 ) ) ) < 1e-4f );
	// Facing the camera, the upper body follows its pitch (fully, a moment after the switch).
	CHECK( sim.EntityAnimState( p0 )->look == 255 );
	AnimStateCopy legs = sim.EntityAnimState( p0 );
	std::printf( "    strafing: legYaw %.2f backward %d\n", legs->legYaw, int( legs->legsBackward ) );
	CHECK( std::fabs( std::fabs( legs->legYaw ) - 0.5f * detmath::kPi ) < 0.2f );
	f.inputs[0].moveRight = 0;
	f.inputs[0].moveForward = -127; // backing away from where it faces
	step( 40 );
	legs = sim.EntityAnimState( p0 );
	std::printf( "    backing up: legYaw %.2f backward %d\n", legs->legYaw, int( legs->legsBackward ) );
	CHECK( legs->legsBackward == 1 );
	CHECK( std::fabs( legs->legYaw ) < 0.2f );
	CHECK( std::fabs( detmath::WrapAngle( sim.PlayerCharacter( 0 )->facingYaw - detmath::YawToRadians( 16384 ) ) ) < 1e-4f );
	command( CommandType::Facing, SlotTarget( 0 ) );
	step( 60 );
	// Freelook again: it turns around to face the way it walks.
	CHECK( sim.PlayerCharacter( 0 )->faceCamera == 0 );
	CHECK( sim.EntityAnimState( p0 )->look == 0 );
	{
		// It comes and goes over a fifth of a second, not at once.
		SimCommand* c = command( CommandType::Facing, SlotTarget( 0 ) );
		c->mode = 1;
		step( 3 );
		int rising = sim.EntityAnimState( p0 )->look;
		CHECK( rising > 0 && rising < 255 );
		command( CommandType::Facing, SlotTarget( 0 ) );
		step( 60 );
		CHECK( sim.EntityAnimState( p0 )->look == 0 );
	}
	CHECK( std::fabs( detmath::WrapAngle( sim.PlayerCharacter( 0 )->facingYaw - detmath::YawToRadians( 16384 ) ) ) > 2.5f );
	CHECK( sim.EntityAnimState( p0 )->legsBackward == 0 );
	// First person faces the camera too, with no mod saying so: the body turns with the view at
	// once and the upper body follows its pitch; back behind the player it is freelook again.
	f.inputs[0].view = uint8_t( ViewMode::FirstPerson );
	f.inputs[0].cameraYaw = 40000;
	step( 30 );
	CHECK( sim.PlayerCharacter( 0 )->faceCamera == 0 );
	CHECK( std::fabs( detmath::WrapAngle( sim.PlayerCharacter( 0 )->facingYaw - detmath::YawToRadians( 40000 ) ) ) < 1e-4f );
	CHECK( sim.EntityAnimState( p0 )->look == 255 );
	f.inputs[0].view = uint8_t( ViewMode::ThirdPerson );
	f.inputs[0].cameraYaw = 16384;
	step( 60 );
	CHECK( sim.EntityAnimState( p0 )->look == 0 );
	CHECK( std::fabs( detmath::WrapAngle( sim.PlayerCharacter( 0 )->facingYaw - detmath::YawToRadians( 16384 ) ) ) > 1.0f );
	f.inputs[0].moveForward = 0;
	f.inputs[0].cameraYaw = 0;
	step( 30 );

	// Stances: a layer takes the stance a mod sets.
	{
		SimCommand* c = command( CommandType::Stance, SlotTarget( 0 ) );
		c->index = 1;
		c->value = 3;
	}
	step( 5 );
	{
		AnimStateCopy a = sim.EntityAnimState( p0 );
		CHECK( a->stances[1] == 3 && a->stances[0] == 0 );
	}
	{
		SimCommand* c = command( CommandType::Stance, SlotTarget( 0 ) );
		c->index = 1;
		c->value = 2;
	}
	{
		SimCommand* bad = command( CommandType::Stance, SlotTarget( 0 ) );
		bad->index = test::kScenarioLayers; // out of range: ignored
		bad->value = 1;
	}
	step( 1 );
	CHECK( sim.EntityAnimState( p0 )->stances[1] == 2 );

	// How many layers a player has is the config's (the server's character and mods), not a
	// constant: nine of them, the ninth set, kept through a snapshot; a tenth does nothing. And a
	// player of a simulation with none still has the rest of its animation.
	{
		SimConfig tall = TestConfig();
		tall.layers = 9;
		Simulation nine( tall );
		InputFrame nf;
		auto nstep = [&]( int n ) {
			for ( int i = 0; i < n; ++i )
			{
				nf.tick = nine.Tick();
				nine.Step( nf );
				nf.events.clear();
				nf.commands.clear();
			}
		};
		auto stance = [&]( uint16_t layer, int32_t value ) {
			SimCommand c;
			c.type = CommandType::Stance;
			c.target = SlotTarget( 0 );
			c.index = layer;
			c.value = value;
			nf.commands.push_back( c );
		};
		nf.events.push_back( { PlayerEventType::Join, 0 } );
		nstep( 30 );
		uint32_t who = nine.PlayerNetId( 0 );
		stance( 8, 5 );
		stance( 9, 6 );
		nstep( 1 );
		AnimStateCopy a = nine.EntityAnimState( who );
		CHECK( a != nullptr && a->stances.size() == 9 && a->graph.size() == 9 && a->stances[8] == 5 && a->stances[9] == 0 );
		Snapshot kept;
		nine.Save( kept );
		uint64_t hashThen = nine.ComputeHash();
		stance( 8, 1 );
		nstep( 3 );
		CHECK( nine.EntityAnimState( who )->stances[8] == 1 && nine.ComputeHash() != hashThen );
		nine.Load( kept );
		CHECK( nine.EntityAnimState( who )->stances[8] == 5 && nine.ComputeHash() == hashThen );
		// As words and back, for any number of layers.
		for ( size_t layers : { size_t( 0 ), size_t( 1 ), size_t( 4 ), size_t( 9 ) } )
		{
			AnimState in = *a;
			in.groundSpeed = 3.5f;
			in.moveRight = -1.25f;
			in.graph[2].time = 0.75f;
			std::vector<uint8_t> bytes( AnimStateBytes( layers ) );
			PackAnimState( in, layers, bytes.data() );
			AnimState out;
			UnpackAnimState( bytes.data(), layers, out );
			CHECK( bytes.size() % 4 == 0 && out.groundSpeed == 3.5f && out.moveRight == -1.25f && out.Layers() == layers );
			CHECK( layers < 9 || out == in );
			CHECK( layers < 3 || out.graph[2].time == 0.75f );
		}
		SimConfig flat = TestConfig();
		flat.layers = 0;
		Simulation bare( flat );
		nf = {};
		nf.events.push_back( { PlayerEventType::Join, 0 } );
		for ( int i = 0; i < 30; ++i )
		{
			nf.tick = bare.Tick();
			nf.inputs[0].moveForward = 127;
			bare.Step( nf );
			nf.events.clear();
		}
		AnimStateCopy none = bare.EntityAnimState( bare.PlayerNetId( 0 ) );
		CHECK( none != nullptr && none->Layers() == 0 && none->groundSpeed > 1.0f );
	}

	// Being placed (a respawn, falling out of the world) keeps the aim a mod asked for.
	{
		SimCommand* c = command( CommandType::Respawn, SlotTarget( 0 ) );
		c->mode = 1;
		c->a = { 4.0f, 1.5f, -4.0f };
	}
	step( 1 );
	CHECK( sim.EntityAnimState( p0 )->aiming == 1 );
	CHECK( sim.EntityAnimState( p0 )->stances[1] == 2 ); // and the stances
	command( CommandType::Aim, SlotTarget( 0 ) );
	step( 1 );
	CHECK( sim.EntityAnimState( p0 )->aiming == 0 );
	f.inputs[0].cameraPitch = 0;

	// Falling out of the world puts the player back and counts it, for a mod to see.
	uint32_t falls = sim.PlayerCharacter( 1 )->fallCount;
	{
		SimCommand* c = command( CommandType::Respawn, SlotTarget( 1 ) );
		c->mode = 1;
		c->a = { 0.0f, sim.Config().killY - 5.0f, 0.0f };
	}
	step( 2 );
	CHECK( sim.PlayerCharacter( 1 )->fallCount == falls + 1 );
	CHECK( sim.EntityTransform( p1 )->position.y > 0.0f );
}

// Ragdolls are simulation state: they fall the same way on every machine, respect their joint
// limits, and go away by lifetime or cap as the mod asked.
// Movement parameters: the server's (SimConfig::move), and one player's by SetMove commands.
void TestMoveParams()
{
	Simulation sim( TestConfig(), FlatMap() );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto setMove = [&]( uint32_t target, MoveParam param, float value ) {
		SimCommand c;
		c.type = CommandType::SetMove;
		c.mode = 1;
		c.index = uint16_t( param );
		c.target = target;
		c.a = { value, 0.0f, 0.0f };
		f.commands.push_back( c );
	};
	auto resetMove = [&]( uint32_t target, MoveParam param ) {
		SimCommand c;
		c.type = CommandType::SetMove;
		c.index = uint16_t( param );
		c.target = target;
		f.commands.push_back( c );
	};
	auto ground = [&]( PlayerSlot slot ) {
		b3Vec3 p = sim.EntityTransform( sim.PlayerNetId( slot ) )->position;
		return b3Vec3{ p.x, 0.0f, p.z };
	};

	f.events.push_back( { PlayerEventType::Join, 0 } );
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 60 );
	const MoveParams defaults;
	CHECK( sim.PlayerMove( 0 ) == defaults && sim.PlayerMove( 1 ) == defaults );
	CHECK( sim.FindEntity( sim.PlayerNetId( 0 ) ).has<MoveOverrides>() == false );

	// One player walks twice as fast; the other is untouched.
	setMove( SlotTarget( 0 ), MoveParam::WalkSpeed, 6.0f );
	step( 1 );
	CHECK( sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == 6.0f );
	CHECK( sim.PlayerMove( 1 )[MoveParam::WalkSpeed] == 3.0f );
	b3Vec3 from0 = ground( 0 ), from1 = ground( 1 );
	f.inputs[0].moveForward = 127;
	f.inputs[1].moveForward = 127;
	step( 120 );
	float walked0 = b3Distance( ground( 0 ), from0 );
	float walked1 = b3Distance( ground( 1 ), from1 );
	std::printf( "    2 s of walking: %.2f m at walk_speed 6, %.2f m at 3\n", walked0, walked1 );
	CHECK( walked1 > 5.0f && walked1 < 6.1f );
	CHECK( walked0 > 1.9f * walked1 && walked0 < 2.1f * walked1 );

	// The same speed as the server's own parameter gives the same walk.
	{
		SimConfig fast = TestConfig();
		fast.move.values[int( MoveParam::WalkSpeed )] = 6.0f;
		Simulation other( fast, FlatMap() );
		InputFrame g;
		g.events.push_back( { PlayerEventType::Join, 0 } );
		for ( int i = 0; i < 181; ++i )
		{
			g.tick = other.Tick();
			g.inputs[0].moveForward = i >= 61 ? int8_t( 127 ) : int8_t( 0 );
			other.Step( g );
			g.events.clear();
		}
		b3Vec3 p = other.EntityTransform( other.PlayerNetId( 0 ) )->position;
		float walked = b3Distance( b3Vec3{ p.x, 0.0f, p.z }, from0 );
		std::printf( "    the same from the config: %.2f m\n", walked );
		CHECK( std::fabs( walked - walked0 ) < 0.01f );
		CHECK( other.FindEntity( other.PlayerNetId( 0 ) ).has<MoveOverrides>() == false );
	}
	f.inputs[0].moveForward = 0;
	f.inputs[1].moveForward = 0;
	step( 60 );

	// Values are clamped to the parameter's range; what is not a number, not a parameter or not a
	// player changes nothing.
	setMove( SlotTarget( 0 ), MoveParam::WalkSpeed, 1.0e6f );
	step( 1 );
	CHECK( sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == MoveParamInfoOf( int( MoveParam::WalkSpeed ) ).max );
	setMove( SlotTarget( 0 ), MoveParam::WalkSpeed, -4.0f );
	step( 1 );
	CHECK( sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == 0.0f );
	setMove( SlotTarget( 0 ), MoveParam::WalkSpeed, 6.0f );
	step( 1 );
	setMove( SlotTarget( 0 ), MoveParam::WalkSpeed, std::numeric_limits<float>::quiet_NaN() );
	setMove( SlotTarget( 0 ), MoveParam( kMoveParams ), 5.0f );
	setMove( SlotTarget( 7 ), MoveParam::WalkSpeed, 5.0f );
	uint32_t propId = sim.Globals().nextNetId;
	{
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.value = -1;
		c.a = { 6.0f, 2.0f, 6.0f };
		c.c = { 0.3f, 0.3f, 0.3f };
		f.commands.push_back( c );
	}
	step( 1 );
	setMove( propId, MoveParam::WalkSpeed, 5.0f );
	step( 1 );
	CHECK( sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == 6.0f );
	CHECK( sim.FindEntity( propId ).is_valid() && sim.FindEntity( propId ).has<MoveOverrides>() == false );
	MoveParams only = defaults;
	only.values[int( MoveParam::WalkSpeed )] = 6.0f;
	CHECK( sim.PlayerMove( 0 ) == only );

	// A jump twice as fast goes higher.
	setMove( SlotTarget( 0 ), MoveParam::JumpSpeed, 13.0f );
	step( 1 );
	float floor0 = sim.EntityTransform( sim.PlayerNetId( 0 ) )->position.y;
	float floor1 = sim.EntityTransform( sim.PlayerNetId( 1 ) )->position.y;
	float peak0 = 0.0f, peak1 = 0.0f;
	f.inputs[0].buttons = BtnJump;
	f.inputs[1].buttons = BtnJump;
	for ( int i = 0; i < 120; ++i )
	{
		step( 1 );
		f.inputs[0].buttons = 0;
		f.inputs[1].buttons = 0;
		peak0 = std::max( peak0, sim.EntityTransform( sim.PlayerNetId( 0 ) )->position.y - floor0 );
		peak1 = std::max( peak1, sim.EntityTransform( sim.PlayerNetId( 1 ) )->position.y - floor1 );
	}
	std::printf( "    jumps: %.2f m at jump_speed 13, %.2f m at 6.5\n", peak0, peak1 );
	CHECK( peak1 > 0.8f && peak1 < 1.4f );
	CHECK( peak0 > 3.0f * peak1 );

	// A fall with a limit never goes faster; one without does.
	setMove( SlotTarget( 0 ), MoveParam::MaxFall, 2.0f );
	for ( PlayerSlot slot = 0; slot < 2; ++slot )
	{
		SimCommand c;
		c.type = CommandType::Respawn;
		c.mode = 1;
		c.target = SlotTarget( slot );
		c.a = { 4.0f * float( slot ), 12.0f, -4.0f };
		f.commands.push_back( c );
	}
	step( 1 );
	// A respawn keeps what a mod set.
	CHECK( sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == 6.0f && sim.PlayerMove( 0 )[MoveParam::MaxFall] == 2.0f );
	float fastest0 = 0.0f, fastest1 = 0.0f;
	for ( int i = 0; i < 60; ++i )
	{
		step( 1 );
		fastest0 = std::min( fastest0, sim.PlayerCharacter( 0 )->velocity.y );
		fastest1 = std::min( fastest1, sim.PlayerCharacter( 1 )->velocity.y );
	}
	std::printf( "    falling: %.2f m/s with max_fall 2, %.2f m/s without\n", fastest0, fastest1 );
	CHECK( fastest0 >= -2.0f && fastest0 < -1.9f );
	CHECK( fastest1 < -10.0f );

	// It is state: a joining client gets it and stays in step, and a rollback takes a change back.
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation client( TestConfig(), FlatMap() );
	CHECK( client.LoadPortable( image ) );
	CHECK( client.PlayerMove( 0 ) == sim.PlayerMove( 0 ) && client.ComputeHash() == sim.ComputeHash() );
	f.inputs[0].moveForward = 127;
	for ( int i = 0; i < 120; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		client.Step( f );
	}
	CHECK( client.ComputeHash() == sim.ComputeHash() );

	Snapshot before;
	sim.Save( before );
	uint64_t hashBefore = sim.ComputeHash();
	setMove( SlotTarget( 1 ), MoveParam::Gravity, 4.0f );
	step( 5 );
	CHECK( sim.PlayerMove( 1 )[MoveParam::Gravity] == 4.0f && sim.ComputeHash() != hashBefore );
	sim.Load( before );
	CHECK( sim.PlayerMove( 1 ) == defaults && sim.ComputeHash() == hashBefore );

	// Given back, the parameter is the server's again; the others a mod set stay.
	resetMove( SlotTarget( 0 ), MoveParam::WalkSpeed );
	step( 1 );
	CHECK( sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == 3.0f );
	CHECK( sim.PlayerMove( 0 )[MoveParam::JumpSpeed] == 13.0f );

	// Names and ranges, as options and baked files use them.
	for ( int i = 0; i < kMoveParams; ++i )
	{
		const MoveParamInfo& info = MoveParamInfoOf( i );
		CHECK( info.name[0] != '\0' && MoveParamByName( info.name ) == i );
		CHECK( defaults.values[i] >= info.min && defaults.values[i] <= info.max );
	}
	CHECK( MoveParamByName( "fly_speed" ) == -1 && MoveParamInfoOf( kMoveParams ).name[0] == '\0' );
	CHECK( ValidMoveParams( defaults ) );
	MoveParams bad = defaults;
	bad.values[int( MoveParam::Gravity )] = std::numeric_limits<float>::quiet_NaN();
	CHECK( ValidMoveParams( bad ) == false );
}

// Motions: what mods add to movement, run by the simulation from the input (sim/motions.h).
void TestMotions()
{
	ModSchema schema;
	schema.fields.push_back( { "dash.charges", BoardType::Int, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "dash.fuel", BoardType::Float, BoardScope::Entity, 1 } );
	schema.fields.push_back( { "round.time", BoardType::Float, BoardScope::Global, 0 } );
	schema.events = { "dash.started", "dash.double_jump" };
	schema.actions.push_back( { "dash", 0, "Alt" } );
	schema.actions.push_back( { "blink", 3, "V" } );
	const std::string text = "cinderbox_motions\t2\n"
							 "# a comment\n"
							 "motion\tDash\n"
							 "when\tpress\tdash\n"
							 "if\tdash.charges > 0\n"
							 "cooldown\t0.5\n"
							 "duration\t0.25\n"
							 "impulse\tself\t12\tmove\thorizontal\n"
							 "param\tfriction\t0\n"
							 "change\tdash.charges\t-=\t1\n"
							 "change\tdash.fuel\t+=\t0.5\n"
							 "emit\tdash.started\n"
							 "motion\tDoubleJump\n"
							 "when\tpress\tjump\n"
							 "if\tnot grounded\n"
							 "uses\t1\tground\n"
							 "impulse\tself\t6.5\tup\tvertical\n"
							 "emit\tdash.double_jump\n"
							 "motion\tBlink\n"
							 "when\tpress\tblink\n"
							 "uses\t2\t1\n"
							 "impulse\tself\t5\tworld\tall\t0\t0\t1\n";
	schema.motionSets.push_back( { "dash", "dash.moves", text } );

	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() );
	if ( motions == nullptr )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	CHECK( motions->list.size() == 3 && motions->list[0].name == "dash.moves/Dash" );
	CHECK( motions->list[0].action == 0 && motions->list[1].action == kMotionActionJump && motions->list[2].action == 3 );
	CHECK( motions->list[0].First().changes.size() == 2 && motions->list[0].First().emits.size() == 1 && motions->list[0].First().emits[0].event == 0 &&
		   motions->list[1].First().emits[0].event == 1 );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto setField = [&]( int slot, int32_t value ) {
		SimCommand c;
		c.type = CommandType::SetField;
		c.target = SlotTarget( 0 );
		c.index = uint16_t( slot );
		c.value = value;
		f.commands.push_back( c );
	};
	auto speed = [&] {
		b3Vec3 v = sim.PlayerCharacter( 0 )->velocity;
		return b3Length( b3Vec3{ v.x, 0.0f, v.z } );
	};
	const uint16_t dash = 1, blink = 8;
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	CHECK( sim.HasMotionState( sim.FindEntity( p0 ) ) );

	// No charges: the press does nothing.
	f.inputs[0].actions = dash;
	step( 1 );
	CHECK( speed() == 0.0f && sim.Globals().modEventCount == 0 );
	f.inputs[0].actions = 0;
	step( 1 );

	// With charges: on the tick of the press the player moves at the impulse, where it faces (it
	// stands still), a charge is taken, the float field counts, and the event is recorded.
	setField( 0, 2 );
	step( 1 );
	f.inputs[0].actions = dash;
	uint32_t pressTick = sim.Tick();
	b3Vec3 from = sim.EntityTransform( p0 )->position;
	step( 1 );
	std::printf( "    dash: %.2f m/s on the tick of the press\n", speed() );
	CHECK( speed() > 11.9f && speed() <= 12.0f );
	CHECK( sim.BoardValue( p0, 0 ) == 1 && BoardToFloat( sim.BoardValue( p0, 1 ) ) == 0.5f );
	CHECK( sim.Globals().modEventCount == 1 );
	const ModEventRecord& ev = sim.Globals().modEvents[0];
	CHECK( ev.type == 0 && ev.netIdA == p0 && ev.tick == pressTick );
	// While it lasts there is no friction (its parameter), so the speed holds; afterwards it is the
	// server's again and the player stops.
	CHECK( sim.PlayerMove( 0 )[MoveParam::Friction] == 0.0f );
	step( 13 );
	CHECK( speed() > 11.9f );
	step( 2 );
	CHECK( sim.PlayerMove( 0 )[MoveParam::Friction] == 6.0f );
	// Held, it is not pressed again; pressed again inside the cooldown, nothing; after it, the second.
	step( 5 );
	CHECK( sim.BoardValue( p0, 0 ) == 1 );
	f.inputs[0].actions = 0;
	step( 1 );
	f.inputs[0].actions = dash;
	step( 1 );
	CHECK( sim.BoardValue( p0, 0 ) == 1 );
	f.inputs[0].actions = 0;
	step( 30 );
	float travelled = b3Distance( sim.EntityTransform( p0 )->position, from );
	std::printf( "    one dash carried the player %.2f m\n", travelled );
	CHECK( travelled > 3.0f && travelled < 6.0f );
	// Along the movement input when there is one: to the right of the camera.
	f.inputs[0].actions = dash;
	f.inputs[0].moveRight = 127;
	step( 1 );
	b3Vec3 v = sim.PlayerCharacter( 0 )->velocity;
	b3Vec3 right = detmath::YawRight( detmath::YawToRadians( f.inputs[0].cameraYaw ) );
	CHECK( sim.BoardValue( p0, 0 ) == 0 && b3Dot( v, right ) > 11.9f );
	f.inputs[0].actions = 0;
	f.inputs[0].moveRight = 0;
	step( 90 );

	// A double jump: once in the air, not twice, and again after landing. A press on the ground is
	// the engine's jump alone.
	float floor = sim.EntityTransform( p0 )->position.y;
	auto press = [&]( uint8_t buttons ) {
		f.inputs[0].buttons = buttons;
		step( 1 );
		f.inputs[0].buttons = 0;
		step( 1 );
	};
	press( BtnJump );
	CHECK( sim.Globals().modEventCount == 2 ); // the two dashes
	step( 15 );
	press( BtnJump );
	CHECK( sim.Globals().modEventCount == 3 && sim.PlayerCharacter( 0 )->velocity.y > 5.5f );
	step( 10 );
	press( BtnJump );
	CHECK( sim.Globals().modEventCount == 3 );
	float peak = 0.0f;
	for ( int i = 0; i < 150; ++i )
	{
		step( 1 );
		peak = std::max( peak, sim.EntityTransform( p0 )->position.y - floor );
	}
	std::printf( "    a double jump peaked at %.2f m\n", peak );
	CHECK( peak > 1.6f && sim.PlayerCharacter( 0 )->grounded == 1 );
	press( BtnJump );
	step( 15 );
	press( BtnJump );
	CHECK( sim.Globals().modEventCount == 4 );
	step( 150 );

	// Uses that come back by time: two, then none until a second after the last.
	auto tap = [&]( uint16_t action ) {
		f.inputs[0].actions = action;
		step( 1 );
		bool moved = sim.PlayerCharacter( 0 )->velocity.z > 4.0f; // friction has had a tick
		f.inputs[0].actions = 0;
		step( 9 );
		return moved;
	};
	CHECK( tap( blink ) && tap( blink ) );
	CHECK( tap( blink ) == false );
	step( 60 );
	CHECK( tap( blink ) );

	// A frozen player does none, and a key held through the freeze is not a press when it ends.
	setField( 0, 5 );
	{
		SimCommand c;
		c.type = CommandType::Freeze;
		c.mode = 1;
		c.target = SlotTarget( 0 );
		f.commands.push_back( c );
	}
	step( 60 );
	f.inputs[0].actions = dash;
	step( 5 );
	CHECK( sim.BoardValue( p0, 0 ) == 5 );
	{
		SimCommand c;
		c.type = CommandType::Freeze;
		c.target = SlotTarget( 0 );
		f.commands.push_back( c );
	}
	step( 5 );
	CHECK( sim.BoardValue( p0, 0 ) == 5 );
	f.inputs[0].actions = 0;
	step( 1 );

	// It is state: a joining client that has the same motions stays in step through a dash, and a
	// rollback takes a dash back.
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation client( TestConfig(), FlatMap() );
	client.SetMotions( motions );
	CHECK( client.LoadPortable( image ) && client.ComputeHash() == sim.ComputeHash() );
	Snapshot before;
	sim.Save( before );
	uint64_t hashBefore = sim.ComputeHash();
	f.inputs[0].actions = dash;
	for ( int i = 0; i < 40; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		client.Step( f );
	}
	CHECK( sim.BoardValue( p0, 0 ) == 4 && client.ComputeHash() == sim.ComputeHash() );
	sim.Load( before );
	CHECK( sim.BoardValue( p0, 0 ) == 5 && sim.ComputeHash() == hashBefore );

	// What a file may not say fails with the line; what no mod declares is said and does nothing.
	auto compile = [&]( const std::string& body, std::string& error, std::string& warned ) {
		std::vector<Motion> out;
		error.clear();
		warned.clear();
		return CompileMotionSet( "t", "cinderbox_motions\t2\n" + body, schema, out, error, warned ) ? int( out.size() ) : -1;
	};
	std::string error, warned;
	CHECK( compile( "motion\tA\nwhen\tpress\tdash\n", error, warned ) == 1 && warned.empty() );
	CHECK( compile( "when\tpress\tdash\n", error, warned ) == -1 && error.find( "line 2" ) != std::string::npos );
	CHECK( compile( "motion\tA\nimpulse\tself\t5\tsideways\tnone\n", error, warned ) == -1 );
	CHECK( compile( "motion\tA\nimpulse\tself\t5000\tup\tnone\n", error, warned ) == -1 );
	CHECK( compile( "motion\tA\nimpulse\tself\t5\tworld\tnone\t0\t0\t0\n", error, warned ) == -1 );
	CHECK( compile( "motion\tA\nparam\tfly_speed\t3\n", error, warned ) == -1 );
	CHECK( compile( "motion\tA\nparam\tgravity\t-3\n", error, warned ) == -1 );
	CHECK( compile( "motion\tA\nif\tgrounded and\n", error, warned ) == -1 );
	CHECK( compile( "motion\tA\nchange\tdash.charges\t*=\t2\n", error, warned ) == -1 );
	CHECK( compile( "motion\tA\nteleport\t3\n", error, warned ) == -1 );
	{
		std::vector<Motion> out;
		CHECK( CompileMotionSet( "t", "motion\tA\n", schema, out, error, warned ) == false );
	}
	std::vector<Motion> inert;
	error.clear();
	warned.clear();
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t2\nmotion\tA\nwhen\tpress\tfly\nchange\tround.time\t=\t1\nchange\tfly.fuel\t-=\t1\nemit\tfly.up\n",
							 schema, inert, error, warned ) );
	std::printf( "    %s\n", warned.c_str() );
	CHECK( inert.size() == 1 && inert[0].action == -1 && inert[0].First().changes.empty() &&
		   inert[0].First().emits.size() == 1 && inert[0].First().emits[0].event == -1 );
	CHECK( warned.find( "\"fly\"" ) != std::string::npos && warned.find( "round.time" ) != std::string::npos &&
		   warned.find( "fly.fuel" ) != std::string::npos && warned.find( "fly.up" ) != std::string::npos );

	// How many motions a server has is its mods' business: a hundred of them compile, a player of a
	// simulation told so has a slot for each, and the last one works like the first.
	{
		ModSchema many = schema;
		// (And an action far up the word: the forty-first bit.)
		many.actions.push_back( { "far", 40, "X" } );
		std::string big = "cinderbox_motions\t2\n";
		for ( int i = 0; i < 100; ++i )
		{
			// (Only the hundredth can happen: the others wait for a fall nobody has.)
			big += "motion\tM" + std::to_string( i ) + "\nwhen\tpress\t" + ( i == 99 ? "far\n" : "dash\nif\tairborne_time > 100\n" );
		}
		many.motionSets = { { "dash", "dash.many", big } };
		warned.clear();
		auto all = CompileMotions( many, warned );
		CHECK( all != nullptr && all->list.size() == 100 );
		if ( all != nullptr && all->list.size() == 100 )
		{
			SimConfig wide = TestConfig();
			wide.motions = 100;
			Simulation hundred( wide );
			hundred.SetMotions( all );
			InputFrame hf;
			hf.events.push_back( { PlayerEventType::Join, 0 } );
			for ( int i = 0; i < 60; ++i )
			{
				hf.tick = hundred.Tick();
				hundred.Step( hf );
				hf.events.clear();
			}
			CHECK( many.ActionMask( "far" ) == ActionBits( 1 ) << 40 );
			hf.inputs[0].actions = dash;
			hf.tick = hundred.Tick();
			hundred.Step( hf );
			CHECK( hundred.GetMotionState( hundred.FindEntity( hundred.PlayerNetId( 0 ) ) ).slots[99].lastTick == 0 );
			hf.inputs[0].actions = many.ActionMask( "far" );
			hf.tick = hundred.Tick();
			hundred.Step( hf );
			MotionState state = hundred.GetMotionState( hundred.FindEntity( hundred.PlayerNetId( 0 ) ) );
			std::printf( "    a hundred motions: a player's state has %zu slots, the last one used at tick %u\n", state.slots.size(),
						 unsigned( state.slots[99].lastTick ) );
			CHECK( state.slots.size() == 100 && state.slots[99].lastTick != 0 && state.slots[98].lastTick == 0 );
			Snapshot kept;
			hundred.Save( kept );
			uint64_t hashThen = hundred.ComputeHash();
			hf.inputs[0].actions = 0;
			for ( int i = 0; i < 5; ++i )
			{
				hf.tick = hundred.Tick();
				hundred.Step( hf );
			}
			hundred.Load( kept );
			CHECK( hundred.ComputeHash() == hashThen );
		}
	}
	// A server without motions has none, and its players carry nothing for them.
	ModSchema none;
	CHECK( CompileMotions( none, warned ) == nullptr );
	Simulation plain( TestConfig(), FlatMap() );
	InputFrame g;
	g.events.push_back( { PlayerEventType::Join, 0 } );
	plain.Step( g );
	CHECK( plain.HasMotionState( plain.FindEntity( plain.PlayerNetId( 0 ) ) ) == false );

	// The schema carries the text.
	std::vector<uint8_t> bytes;
	EncodeSchema( schema, bytes );
	ModSchema back;
	CHECK( DecodeSchema( bytes.data(), bytes.size(), back ) && back == schema );
}

// Motions that hold while conditions do (flight, a jetpack, a glide), and ones a mod event starts.
// An item's own motions: the bake writes each with the condition that its player has the item, in
// the hand (the kind's name) or anywhere (carried.<kind>).
void TestItemMotions()
{
	ModSchema schema;
	schema.fields.push_back( { "t.hand", BoardType::Float, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "t.carried", BoardType::Float, BoardScope::Entity, 1 } );
	schema.itemKinds = { "dash.boots", "gun.pistol" };
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tInHand\nwhen\twhile\nif\tdash.boots\nchange\tt.hand\t+=\t60\n"
							 "motion\tCarried\nwhen\twhile\nif\tcarried.dash.boots and ( grounded )\nchange\tt.carried\t+=\t60\n";
	schema.motionSets.push_back( { "dash", "dash.boots", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 2 );
	if ( motions == nullptr || warnings.empty() == false )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetItemShapes( { ItemShape{}, ItemShape{} } );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto give = [&]( uint16_t kind, bool stowed ) {
		SimCommand c;
		c.type = CommandType::SpawnItem;
		c.target = SlotTarget( 0 );
		c.index = kind;
		c.mode = stowed ? kNoSocket : uint8_t( 0 );
		c.value = stowed ? 1 : 0;
		f.commands.push_back( c );
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	auto hand = [&] { return BoardToFloat( sim.BoardValue( p0, 0 ) ); };
	auto carried = [&] { return BoardToFloat( sim.BoardValue( p0, 1 ) ); };

	// Without the item, neither runs; another kind in the hand changes nothing.
	give( 1, false );
	step( 30 );
	CHECK( hand() == 0.0f && carried() == 0.0f );

	// Put away: the carried one runs, the hand's does not.
	give( 0, true );
	step( 31 );
	float carriedThen = carried();
	std::printf( "    boots put away: hand %.1f, carried %.1f\n", hand(), carriedThen );
	CHECK( hand() == 0.0f && carriedThen > 20.0f );

	// Another player's item is not this one's: a second player with nothing has neither.
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 30 );
	uint32_t p1 = sim.PlayerNetId( 1 );
	CHECK( BoardToFloat( sim.BoardValue( p1, 0 ) ) == 0.0f && BoardToFloat( sim.BoardValue( p1, 1 ) ) == 0.0f );
	CHECK( carried() > carriedThen );
}

// A motion's steps (a rules sheet's motion block, docs/rules-language.md): a wait for a condition, a
// wait that gives up, a wait by time, guards decided when a step starts, an end, an edge trigger.
void TestMotionSteps()
{
	ModSchema schema;
	schema.fields.push_back( { "slam.big", BoardType::Int, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "t.count", BoardType::Int, BoardScope::Entity, 1 } );
	schema.events = { "slam.started", "slam.landed", "slam.big_landed", "slam.small_landed", "landed",
					  "hang.gave_up", "hang.held", "later.a", "later.b" };
	schema.actions.push_back( { "slam", 0, "Q" } );
	schema.actions.push_back( { "hang", 1, "E" } );
	schema.actions.push_back( { "later", 2, "R" } );
	schema.actions.push_back( { "stop", 3, "F" } );
	const std::string text = "cinderbox_motions\t3\n"
							 "motion\tSlam\nwhen\tpress\tslam\nif\tnot grounded\nemit\tslam.started\n"
							 "impulse\tself\t25\tworld\tvertical\t0\t-1\t0\n"
							 "step\t0\t3\tgrounded\nemit\tslam.landed\n"
							 "guard\tslam.big > 0\nemit\tslam.big_landed\n"
							 "guard\tnot ( slam.big > 0 )\nemit\tslam.small_landed\n"
							 "guard\nparam\twalk_speed\t0\t1\n"
							 "motion\tLand\nwhen\tedge\tgrounded\nemit\tlanded\n"
							 "motion\tHang\nwhen\tpress\thang\nstep\t0\t0.5\tslam.big > 5\nemit\thang.held\ntimeout\nemit\thang.gave_up\n"
							 "motion\tLater\nwhen\tpress\tlater\nemit\tlater.a\nstep\t0.5\t0\nemit\tlater.b\nchange\tt.count\t+=\t1\n"
							 "motion\tStop\nwhen\tpress\tstop\nduration\t2\nparam\twalk_speed\t1\nguard\tgrounded\nend\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 5 );
	if ( motions == nullptr || motions->list.size() != 5 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	const Motion& slam = motions->list[0];
	CHECK( slam.steps.size() == 2 && slam.steps[0].next == 1 && slam.steps[1].next == -1 && slam.steps[1].guards.size() == 2 &&
		   slam.steps[1].emits.size() == 3 && slam.steps[1].length == 1.0f && slam.Held() );
	const Motion& hang = motions->list[2];
	CHECK( hang.steps.size() == 3 && hang.steps[0].next == 1 && hang.steps[1].onTimeout == 2 && hang.steps[2].timeout );
	CHECK( motions->list[1].when == Motion::When::Edge );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	std::vector<ModEventRecord> seen;
	uint32_t counted = 0;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
			for ( ; counted < sim.Globals().modEventCount; ++counted )
			{
				seen.push_back( sim.Globals().modEvents[counted % kModEventHistory] );
			}
		}
	};
	auto count = [&]( int type ) {
		return int( std::count_if( seen.begin(), seen.end(), [&]( const ModEventRecord& e ) { return int( e.type ) == type; } ) );
	};
	auto tickOf = [&]( int type ) {
		for ( size_t i = seen.size(); i-- > 0; )
		{
			if ( int( seen[i].type ) == type )
			{
				return seen[i].tick;
			}
		}
		return 0u;
	};
	auto tap = [&]( uint16_t action ) {
		f.inputs[0].actions = action;
		step( 1 );
		f.inputs[0].actions = 0;
	};
	const uint16_t slamKey = 1, hangKey = 2, laterKey = 4, stopKey = 8;
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	const uint32_t p0 = sim.PlayerNetId( 0 );
	auto grounded = [&] { return sim.PlayerCharacter( 0 )->grounded != 0; };
	auto on = [&]( int motion ) { return ( sim.EntityMotionsOn( p0 ) >> motion & 1u ) != 0; };
	// A player who spawns standing has not landed: the edge's first answer is only remembered.
	const int spawnLandings = count( 4 );
	std::printf( "    landings by the spawn: %d\n", spawnLandings );
	CHECK( grounded() && spawnLandings <= 1 );

	// On the ground the slam does not start (its condition); in the air it dives, waits, and on the
	// tick after it lands takes its second step: the landing said, the small one (its guard), and
	// no walking for a second.
	tap( slamKey );
	CHECK( count( 0 ) == 0 );
	f.inputs[0].buttons = BtnJump;
	step( 1 );
	f.inputs[0].buttons = 0;
	step( 20 );
	CHECK( grounded() == false );
	tap( slamKey );
	CHECK( count( 0 ) == 1 && sim.PlayerCharacter( 0 )->velocity.y < -20.0f && on( 0 ) );
	uint32_t landTick = 0;
	for ( int i = 0; i < 120 && count( 1 ) == 0; ++i )
	{
		step( 1 );
		landTick = grounded() && landTick == 0 ? sim.Tick() - 1 : landTick;
	}
	std::printf( "    the slam landed on tick %u, its step started on tick %u\n", landTick, tickOf( 1 ) );
	CHECK( count( 1 ) == 1 && tickOf( 1 ) == landTick + 1 && count( 3 ) == 1 && count( 2 ) == 0 );
	CHECK( count( 4 ) == spawnLandings + 1 ); // the edge: one landing, one start
	CHECK( on( 0 ) && sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == 0.0f );
	step( 58 );
	CHECK( on( 0 ) && sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == 0.0f );
	step( 2 );
	CHECK( on( 0 ) == false && sim.PlayerMove( 0 )[MoveParam::WalkSpeed] != 0.0f );

	// Its guard is decided when the step starts: with the field set, the big landing instead.
	SimCommand set;
	set.type = CommandType::SetField;
	set.target = SlotTarget( 0 );
	set.index = 0;
	set.value = 1;
	f.commands.push_back( set );
	f.inputs[0].buttons = BtnJump;
	step( 1 );
	f.inputs[0].buttons = 0;
	step( 20 );
	tap( slamKey );
	step( 60 );
	CHECK( count( 1 ) == 2 && count( 2 ) == 1 && count( 3 ) == 1 && count( 4 ) == spawnLandings + 2 );
	step( 60 );

	// A wait that gives up: within half a second the field never passes 5, so its timeout runs and
	// the motion ends.
	tap( hangKey );
	CHECK( on( 2 ) );
	step( 29 );
	CHECK( count( 5 ) == 0 && on( 2 ) );
	step( 2 );
	CHECK( count( 5 ) == 1 && count( 6 ) == 0 );
	step( 1 );
	CHECK( on( 2 ) == false );
	// ... and a wait that is met in time: the field set, the held step instead.
	tap( hangKey );
	set.value = 9;
	f.commands.push_back( set );
	step( 3 );
	CHECK( count( 6 ) == 1 && count( 5 ) == 1 );
	step( 60 );

	// A wait by time: half a second after the press, its second step (and its change, once).
	tap( laterKey );
	const uint32_t pressed = tickOf( 7 );
	step( 40 );
	CHECK( count( 7 ) == 1 && count( 8 ) == 1 && tickOf( 8 ) == pressed + 30 && sim.BoardValue( p0, 1 ) == 1 );

	// It is state: a copy made while a step waits goes on the same, and a rollback takes it back.
	tap( laterKey );
	step( 5 );
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation client( TestConfig(), FlatMap() );
	client.SetMotions( motions );
	CHECK( client.LoadPortable( image ) && client.ComputeHash() == sim.ComputeHash() );
	Snapshot before;
	sim.Save( before );
	const uint64_t hashBefore = sim.ComputeHash();
	for ( int i = 0; i < 40; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		client.Step( f );
	}
	CHECK( sim.BoardValue( p0, 1 ) == 2 && client.ComputeHash() == sim.ComputeHash() );
	sim.Load( before );
	CHECK( sim.BoardValue( p0, 1 ) == 1 && sim.ComputeHash() == hashBefore );
	step( 40 );
	CHECK( sim.BoardValue( p0, 1 ) == 2 );

	// An end under a guard: standing, it ends after the tick it starts; in the air it lasts.
	tap( stopKey );
	CHECK( on( 4 ) );
	step( 1 );
	CHECK( on( 4 ) == false );
	step( 120 );
	f.inputs[0].buttons = BtnJump;
	step( 1 );
	f.inputs[0].buttons = 0;
	step( 10 );
	tap( stopKey );
	step( 5 );
	CHECK( on( 4 ) && sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == 1.0f );

	// What a file may not say.
	auto fails = [&]( const std::string& body ) {
		std::vector<Motion> out;
		std::string error, warned;
		bool ok = CompileMotionSet( "t", "cinderbox_motions\t3\n" + body, schema, out, error, warned );
		return ok == false;
	};
	CHECK( fails( "motion\tA\nwhen\twhile\nstep\t1\t0\n" ) );					   // a while has no steps
	CHECK( fails( "motion\tA\nwhen\tpress\tslam\nstep\t0\t1\n" ) );			   // giving up, waiting for nothing
	CHECK( fails( "motion\tA\nwhen\tpress\tslam\ntimeout\n" ) );				   // a timeout of nothing
	CHECK( fails( "motion\tA\nwhen\tpress\tslam\nstep\t1\t0\ncooldown\t1\n" ) ); // the motion's, after a step
	CHECK( fails( "motion\tA\nwhen\tpress\tslam\nstep\t0\t1\tgrounded\ntimeout\nparam\twalk_speed\t0\n" ) );
	CHECK( fails( "motion\tA\nwhen\tedge\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\tslam\nguard\tnot (\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\tslam\nparam\twalk_speed\t1\t-1\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\tslam\nstep\t0\t0\n" ) == false );
}

// A motion's finds (a sheet's "find SHAPE", docs/rules-language.md): what a shape overlaps when its
// step starts, each in turn: thrown away from the player, slowed for a while, said with who it was.
// Which slot a player has changes nothing (M212): every player's motions run on the world as the
// tick found it, and what they do to other players lands after the targets' own motions, on the
// same tick. Two players 2 m apart, face to face: both shove at once; then one shoves while the
// other dashes at it. Each is played twice, with the two players' slots swapped, and the players
// end where they did, to the bit.
void TestMotionOrder()
{
	ModSchema schema;
	schema.actions.push_back( { "shove", 0, "Q" } );
	schema.actions.push_back( { "lunge", 1, "E" } );
	const std::string text = "cinderbox_motions\t3\n"
							 "motion\tShove\nwhen\tpress\tshove\n"
							 "find\tplayers\t0\t16\tfacing\nshape\t3\t0\t1\t0\n"
							 "impulse\tfound\t9\taway\tnone\nimpulse\tfound\t3\tup\tnone\nendfind\n"
							 "motion\tLunge\nwhen\tpress\tlunge\nimpulse\tself\t11\tfacing\thorizontal\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 2 );
	if ( motions == nullptr || motions->list.size() != 2 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}

	struct Ends
	{
		b3Vec3 west, east;		   // where each is after the press and half a second
		b3Vec3 westFirst, eastFirst; // its velocity on the tick of the press
	};
	// `westSlot`: the slot of the player at z 0 facing +z (the other, at z 2 facing -z, has the other).
	auto play = [&]( int westSlot, ActionBits westPress, ActionBits eastPress ) {
		Simulation sim( TestConfig(), FlatMap() );
		sim.SetMotions( motions );
		InputFrame f;
		auto step = [&]( int n ) {
			for ( int i = 0; i < n; ++i )
			{
				f.tick = sim.Tick();
				sim.Step( f );
				f.events.clear();
				f.commands.clear();
			}
		};
		auto place = [&]( int slot, b3Vec3 at, uint16_t yaw ) {
			SimCommand c;
			c.type = CommandType::Respawn;
			c.target = sim.PlayerNetId( PlayerSlot( slot ) );
			c.mode = 1;
			c.a = { at.x, at.y, at.z };
			c.index = yaw;
			f.commands.push_back( c );
		};
		const int eastSlot = 1 - westSlot;
		f.events.push_back( { PlayerEventType::Join, 0 } );
		f.events.push_back( { PlayerEventType::Join, 1 } );
		step( 2 );
		place( westSlot, { 0.0f, 1.0f, 0.0f }, 0 );
		place( eastSlot, { 0.0f, 1.0f, 2.0f }, 32768 );
		f.inputs[size_t( westSlot )].cameraYaw = 0;
		f.inputs[size_t( eastSlot )].cameraYaw = 32768;
		step( 60 );
		f.inputs[size_t( westSlot )].actions = westPress;
		f.inputs[size_t( eastSlot )].actions = eastPress;
		step( 1 );
		Ends ends;
		ends.westFirst = sim.PlayerCharacter( PlayerSlot( westSlot ) )->velocity;
		ends.eastFirst = sim.PlayerCharacter( PlayerSlot( eastSlot ) )->velocity;
		f.inputs[size_t( westSlot )].actions = 0;
		f.inputs[size_t( eastSlot )].actions = 0;
		step( 30 );
		ends.west = sim.EntityTransform( sim.PlayerNetId( PlayerSlot( westSlot ) ) )->position;
		ends.east = sim.EntityTransform( sim.PlayerNetId( PlayerSlot( eastSlot ) ) )->position;
		return ends;
	};
	auto same = []( b3Vec3 a, b3Vec3 b ) {
		return a.x == b.x && a.y == b.y && a.z == b.z;
	};

	// Both shove: each is thrown away from the other on the tick of the press, whatever its slot.
	const Ends both0 = play( 0, 1, 1 );
	const Ends both1 = play( 1, 1, 1 );
	std::printf( "    both shove: west at z %.3f, east at z %.3f (west in slot 0); %.3f, %.3f (west in slot 1)\n", both0.west.z, both0.east.z,
				 both1.west.z, both1.east.z );
	CHECK( both0.westFirst.z < -8.0f && both0.eastFirst.z > 8.0f && both1.westFirst.z < -8.0f && both1.eastFirst.z > 8.0f );
	CHECK( same( both0.west, both1.west ) && same( both0.east, both1.east ) );
	// And the two are mirror images of each other.
	CHECK( std::fabs( both0.west.z + ( both0.east.z - 2.0f ) ) < 1e-3f && std::fabs( both0.west.y - both0.east.y ) < 1e-4f );

	// West shoves while east lunges at it: east's lunge (its own, replacing its horizontal speed) comes
	// first, the shove after it, so east goes on toward west at 11 - 9 = 2 m/s, whatever the slots.
	const Ends lunge0 = play( 0, 1, 2 );
	const Ends lunge1 = play( 1, 1, 2 );
	std::printf( "    a shove on a lunge: east at %.2f m/s on the tick, ends at z %.3f (west in slot 0); %.2f m/s, z %.3f (west in slot 1)\n",
				 lunge0.eastFirst.z, lunge0.east.z, lunge1.eastFirst.z, lunge1.east.z );
	CHECK( same( lunge0.eastFirst, lunge1.eastFirst ) && same( lunge0.east, lunge1.east ) && same( lunge0.west, lunge1.west ) );
	CHECK( lunge0.eastFirst.z < -1.0f && lunge0.eastFirst.z > -3.0f && lunge0.eastFirst.y > 2.0f );
}

// M213: a find's change is its owner's field, once for each thing found; and a motion of one step
// with no lasts is on for its longest move's seconds (its parameters hold that long).
void TestMotionRegular()
{
	ModSchema schema;
	schema.fields.push_back( { "t.hits", BoardType::Int, BoardScope::Entity, 0 } );
	schema.actions.push_back( { "wave", 0, "Q" } );
	schema.actions.push_back( { "slide", 1, "E" } );
	const std::string text = "cinderbox_motions\t3\n"
							 "motion\tWave\nwhen\tpress\twave\n"
							 "find\tplayers\t0\t16\tfacing\nshape\t4\t0\t0\t0\nchange\tt.hits\t+=\t1\nendfind\n"
							 "motion\tSlide\nwhen\tpress\tslide\nparam\tfriction\t0\t0.2\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 2 );
	if ( motions == nullptr || motions->list.size() != 2 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	CHECK( motions->list[0].First().finds.at( 0 ).changes.size() == 1 );
	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto place = [&]( int slot, b3Vec3 at ) {
		SimCommand c;
		c.type = CommandType::Respawn;
		c.target = sim.PlayerNetId( PlayerSlot( slot ) );
		c.mode = 1;
		c.a = { at.x, at.y, at.z };
		f.commands.push_back( c );
	};
	for ( int slot = 0; slot < 4; ++slot )
	{
		f.events.push_back( { PlayerEventType::Join, PlayerSlot( slot ) } );
	}
	step( 2 );
	place( 0, { 0.0f, 1.0f, 0.0f } );
	place( 1, { 2.0f, 1.0f, 0.0f } );
	place( 2, { -2.0f, 1.0f, 1.0f } );
	place( 3, { 20.0f, 1.0f, 0.0f } ); // far
	step( 60 );
	f.inputs[0].actions = 1;
	step( 1 );
	f.inputs[0].actions = 0;
	const int hits = sim.GetBoard( sim.FindEntity( sim.PlayerNetId( 0 ) ) ).values[0];
	std::printf( "    a wave that found two players: t.hits %d\n", hits );
	CHECK( hits == 2 );

	// The slide: friction 0 for 0.2 s (12 ticks), then the server's again.
	const float friction = sim.PlayerMove( 0 )[MoveParam::Friction];
	f.inputs[0].actions = 2;
	step( 1 );
	f.inputs[0].actions = 0;
	int held = 0;
	for ( int i = 0; i < 20; ++i )
	{
		held += sim.PlayerMove( 0 )[MoveParam::Friction] == 0.0f ? 1 : 0;
		step( 1 );
	}
	std::printf( "    a slide of one step, no lasts: friction 0 for %d ticks (the server's %.1f after)\n", held, sim.PlayerMove( 0 )[MoveParam::Friction] );
	CHECK( friction > 0.0f && held == 11 && sim.PlayerMove( 0 )[MoveParam::Friction] == friction );
}

// M214: every amount may be an expression. A move's value is read when its step starts and holds
// as it was (a field changed after does not move it); a change's value, a move given by a find (by
// how far what it found is), a force's top speed are read when they act.
void TestMotionAmounts()
{
	ModSchema schema;
	schema.fields.push_back( { "t.power", BoardType::Int, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "t.n", BoardType::Int, BoardScope::Entity, 1 } );
	schema.actions.push_back( { "boost", 0, "Q" } );
	schema.actions.push_back( { "wave", 1, "E" } );
	schema.actions.push_back( { "rise", 2, "R" } );
	const std::string text = "cinderbox_motions\t3\n"
							 "motion\tBoost\nwhen\tpress\tboost\nduration\t1\nparam\twalk_speed\t2 * t.power\nchange\tt.n\t+=\tt.power + 1\n"
							 "motion\tWave\nwhen\tpress\twave\n"
							 "find\tplayers\t0\t16\tfacing\nshape\t6\t0\t0\t0\nparam\twalk_speed\tdistance\t1\nendfind\n"
							 "motion\tRise\nwhen\tpress\trise\nduration\t1\nparam\tgravity\t0\nforce\tself\tup\tvelocity\t60\tt.power\t0\t0\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 3 );
	if ( motions == nullptr || motions->list.size() != 3 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto set = [&]( uint16_t index, int32_t value ) {
		SimCommand c;
		c.type = CommandType::SetField;
		c.target = SlotTarget( 0 );
		c.index = index;
		c.value = value;
		f.commands.push_back( c );
	};
	auto place = [&]( int slot, b3Vec3 at ) {
		SimCommand c;
		c.type = CommandType::Respawn;
		c.target = sim.PlayerNetId( PlayerSlot( slot ) );
		c.mode = 1;
		c.a = { at.x, at.y, at.z };
		f.commands.push_back( c );
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 2 );
	place( 0, { 0.0f, 1.0f, 0.0f } );
	place( 1, { 4.0f, 1.0f, 0.0f } );
	set( 0, 3 );
	step( 60 );
	const flecs::entity me = sim.FindEntity( sim.PlayerNetId( 0 ) );

	// The boost: walk speed 2 * 3, and t.n += 3 + 1; the power changed to 5 after: still 6.
	f.inputs[0].actions = 1;
	step( 1 );
	f.inputs[0].actions = 0;
	const float boosted = sim.PlayerMove( 0 )[MoveParam::WalkSpeed];
	const int n = sim.GetBoard( me ).values[1];
	set( 0, 5 );
	step( 10 );
	const float later = sim.PlayerMove( 0 )[MoveParam::WalkSpeed];
	std::printf( "    a boost of 2 * t.power (3): walk speed %.1f, still %.1f with t.power 5; t.n += t.power + 1: %d\n", boosted, later, n );
	CHECK( boosted == 6.0f && later == 6.0f && n == 4 );
	CHECK( me.has<MoveValues>() && sim.FindEntity( sim.PlayerNetId( 1 ) ).has<MoveValues>() == false );
	step( 60 );
	CHECK( sim.PlayerMove( 0 )[MoveParam::WalkSpeed] != 6.0f );

	// The wave: the player 4 m away given walk speed 4 (its distance) for a second.
	f.inputs[0].actions = 2;
	step( 1 );
	f.inputs[0].actions = 0;
	const float given = sim.PlayerMove( 1 )[MoveParam::WalkSpeed];
	std::printf( "    a wave giving walk speed = distance: %.2f to the player 4 m away\n", given );
	CHECK( given > 3.8f && given < 4.2f );

	// The rise: a force toward t.power (5) m/s up.
	f.inputs[0].actions = 4;
	step( 1 );
	f.inputs[0].actions = 0;
	step( 30 );
	const float up = sim.PlayerCharacter( 0 )->velocity.y;
	std::printf( "    a rise toward t.power m/s: %.2f m/s up\n", up );
	CHECK( up > 4.9f && up < 5.1f );
}

// M215: the debug HUD's motion lines (DescribeMotions): a motion waiting at its step says what for;
// a press refused says why (its cooldown, its if); the simulation's state does not change for it.
void TestMotionInspector()
{
	ModSchema schema;
	schema.fields.push_back( { "t.ready", BoardType::Int, BoardScope::Entity, 0 } );
	schema.actions.push_back( { "dive", 0, "Q" } );
	schema.actions.push_back( { "zap", 1, "E" } );
	const std::string text = "cinderbox_motions\t3\n"
							 "motion\tDive\nwhen\tpress\tdive\ncooldown\t2\nimpulse\tself\t3\tup\tnone\nstep\t0\t0\tgrounded and t.ready > 0\n"
							 "motion\tZap\nwhen\tpress\tzap\nif\tt.ready > 0\nimpulse\tself\t1\tup\tnone\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && motions->list.size() == 2 );
	if ( motions == nullptr || motions->list.size() != 2 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	CHECK( sim.DescribeMotions( 0 ).empty() );
	f.inputs[0].actions = 1;
	step( 1 );
	f.inputs[0].actions = 0;
	step( 10 );
	const std::string waiting = sim.DescribeMotions( 0 );
	std::printf( "    a dive waiting:\n      %s", waiting.c_str() );
	CHECK( waiting.find( "t.moves/Dive  step 1 of 2  waits for grounded and t.ready > 0  for 0.2 s" ) != std::string::npos );
	f.inputs[0].actions = 1; // again, in its cooldown (and on: it is held on while it waits)
	step( 1 );
	f.inputs[0].actions = 2; // the zap, whose if does not hold
	step( 1 );
	f.inputs[0].actions = 0;
	const std::string refused = sim.DescribeMotions( 0 );
	std::printf( "    a zap refused:\n      %s", refused.c_str() );
	CHECK( refused.find( "t.moves/Zap did not start 0.0 s ago: its if: t.ready > 0" ) != std::string::npos );
}

// M216: characters per body. Two characters, each its capsule and its state machine: a body made the
// second (SetCharacter) stands on its own capsule with its feet where they were, runs the second's
// state machine, and a snapshot taken then and loaded into another simulation goes on the same.
void TestCharacters()
{
	const char* small = "cinderbox_graph\t1\nclip\tidle\t1\t1\nlayer\tbase\nstate\tIdle\tclip\tidle\t0\nstart\tIdle\n";
	const char* big = "cinderbox_graph\t1\nclip\trest\t2\t1\nclip\troar\t1\t0\nlayer\tbase\nstate\tRest\tclip\trest\t0\n"
					  "state\tRoar\tclip\troar\t0\nstart\tRest\ntransition\tRest\tRoar\t0\t0\timmediate\t1\troar\n";
	ModSchema schema;
	schema.events = { "roar" };
	std::string error, warnings;
	auto smallGraph = CompileAnimGraph( small, schema, error, warnings );
	auto bigGraph = CompileAnimGraph( big, schema, error, warnings );
	CHECK( smallGraph != nullptr && bigGraph != nullptr );
	if ( smallGraph == nullptr || bigGraph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	auto make = [&]( Simulation& sim ) {
		sim.SetCharacters( { { smallGraph, kDefaultCapsuleRadius, kDefaultCapsuleHalfHeight }, { bigGraph, 0.8f, 1.2f } } );
	};
	Simulation sim( TestConfig(), FlatMap() );
	make( sim );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 90 );
	const uint32_t dragon = sim.PlayerNetId( 1 );
	const float smallCentre = sim.EntityTransform( dragon )->position.y;
	CHECK( sim.CharacterOf( sim.FindEntity( dragon ) ) == 0 );

	SimCommand become;
	become.type = CommandType::SetCharacter;
	become.target = SlotTarget( 1 );
	become.index = 1;
	f.commands.push_back( become );
	step( 90 );
	const flecs::entity e = sim.FindEntity( dragon );
	const float bigCentre = sim.EntityTransform( dragon )->position.y;
	const Shape shape = e.get<Shape>();
	std::printf( "    a body made the second character: capsule %.2f / %.2f, its centre %.2f m up (was %.2f): its feet on the ground\n",
				 double( shape.halfExtents.x ), double( shape.halfExtents.y ), double( bigCentre ), double( smallCentre ) );
	CHECK( sim.CharacterOf( e ) == 1 && shape.halfExtents.x == 0.8f && shape.halfExtents.y == 1.2f );
	// Feet below the centre: 0.5 + 0.9 - 0.02 for the first, 1.2 + 2.4 - 0.02 for the second.
	CHECK( std::fabs( ( bigCentre - smallCentre ) - ( 3.58f - 1.38f ) ) < 0.05f );
	CHECK( sim.CharacterOf( sim.FindEntity( sim.PlayerNetId( 0 ) ) ) == 0 );

	// Its own state machine: a roar plays the second's Roar.
	SimCommand roar;
	roar.type = CommandType::Event;
	roar.index = 0;
	roar.target = dragon;
	f.commands.push_back( roar );
	step( 2 );
	const AnimState anim = sim.GetAnimState( e );
	int roaring = -1;
	for ( size_t s = 0; s < bigGraph->layers[0].states.size(); ++s )
	{
		roaring = bigGraph->layers[0].states[s].name == "Roar" ? int( s ) : roaring;
	}
	std::printf( "    its state machine: state %d (Roar is %d)\n", int( anim.graph[0].state ), roaring );
	CHECK( roaring >= 0 && int( anim.graph[0].state ) == roaring );

	// A copy (a joining client's: the portable snapshot) goes on the same.
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation copy( TestConfig(), FlatMap() );
	make( copy );
	CHECK( copy.LoadPortable( image ) );
	for ( int i = 0; i < 60; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		copy.Step( f );
	}
	CHECK( sim.ComputeHash() == copy.ComputeHash() && copy.CharacterOf( copy.FindEntity( dragon ) ) == 1 );

	// Back to the first: the component goes, the capsule is the default again.
	become.index = 0;
	f.commands.push_back( become );
	step( 60 );
	CHECK( sim.FindEntity( dragon ).has<CharacterKind>() == false && sim.FindEntity( dragon ).get<Shape>().halfExtents.x == kDefaultCapsuleRadius );
	CHECK( std::fabs( sim.EntityTransform( dragon )->position.y - smallCentre ) < 0.05f );
}

// M217: the viewer draws each body as its own character. A body made the second character says so
// in its frame, through the viewer's bytes too, and the mirror poses it on that character's
// skeleton (and the others on the first's); made the first again, it goes back.
void TestViewerCharacters()
{
	Simulation sim( TestConfig(), FlatMap() );
	sim.SetCharacters( { { nullptr, kDefaultCapsuleRadius, kDefaultCapsuleHalfHeight }, { nullptr, 0.8f, 1.2f } } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 30 );
	SimCommand become;
	become.type = CommandType::SetCharacter;
	become.target = SlotTarget( 1 );
	become.index = 1;
	f.commands.push_back( become );
	step( 30 );
	const uint32_t first = sim.PlayerNetId( 0 ), second = sim.PlayerNetId( 1 );

	// Captured, and through the bytes a viewer reads.
	auto viewed = [&]() -> present::ViewFrame {
		present::ViewFrame frame;
		frame.serial = 1;
		frame.state = "playing";
		frame.hasWorld = true;
		present::CaptureFrame( sim, frame.frame );
		frame.frame.resetGeneration = 1;
		std::vector<uint8_t> bytes;
		present::EncodeView( frame, nullptr, 0.0, bytes );
		present::ViewFrame back;
		bool decoded = present::DecodeView( bytes.data(), bytes.size(), nullptr, back );
		return decoded ? back : present::ViewFrame{};
	};
	auto characterIn = [&]( const present::ViewFrame& frame, uint32_t netId ) {
		for ( const present::FrameEntity& e : frame.frame.entities )
		{
			if ( e.netId == netId )
			{
				return int( e.character );
			}
		}
		return -1;
	};
	present::ViewFrame frame = viewed();
	std::printf( "    in the frame: the first player is character %d, the second %d\n", characterIn( frame, first ),
				 characterIn( frame, second ) );
	CHECK( characterIn( frame, first ) == 0 && characterIn( frame, second ) == 1 );

	// Posed on its own character's skeleton.
	std::shared_ptr<const anim::AnimSet> small = anim::AnimSet::CreateProcedural(), big = anim::AnimSet::CreateProcedural();
	present::Mirror mirror( small );
	std::vector<present::CharacterAnim> characters( 2 );
	characters[0].set = small;
	characters[1].set = big;
	characters[1].feet = 3.58f;
	mirror.SetCharacters( characters );
	auto skeletonOf = [&]( uint32_t netId ) {
		const anim::AnimSet* set = nullptr;
		mirror.ForEach( [&]( uint64_t, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim* a,
							 const present::RagdollAnim* ) {
			if ( v.netId == netId && a != nullptr && a->evaluator )
			{
				set = &a->evaluator->Set();
			}
		} );
		return set;
	};
	mirror.Update( frame.frame, 0.0f, 1.0f / 60.0f );
	CHECK( skeletonOf( first ) == small.get() && skeletonOf( second ) == big.get() );
	CHECK( mirror.Library().Of( 1 ).feet == 3.58f && mirror.Library().Of( 7 ).set == small );

	become.index = 0;
	f.commands.push_back( become );
	step( 10 );
	frame = viewed();
	mirror.Update( frame.frame, 0.0f, 1.0f / 60.0f );
	std::printf( "    made the first again: character %d, posed on the %s skeleton\n", characterIn( frame, second ),
				 skeletonOf( second ) == small.get() ? "first's" : "second's" );
	CHECK( characterIn( frame, second ) == 0 && skeletonOf( second ) == small.get() );
}

// Found in roadmap step 1 (after M234): both games of a waves session crashed at once. A NetId
// comes from a counter in the simulation's state, so a rollback can give the number of something
// the client predicted (a piece of a wall, a prop) to something else (a ragdoll). The mirror kept
// the visual it had for the number and wrote a ragdoll's pose into a prop's visual, which has no
// room for one (a null component in a release build). A number that names something else now gets
// a visual of its own: the old one is removed (its node freed) and the new one made.
void TestMirrorNetIdReused()
{
	std::shared_ptr<const anim::AnimSet> rig = anim::AnimSet::CreateProcedural();
	present::Mirror mirror( rig );
	present::PresentationFrame frame;
	auto entity = [&]( present::VisualKind kind ) {
		present::FrameEntity e;
		e.netId = 7;
		e.kind = kind;
		e.transform.position = { 1.0f, 2.0f, 3.0f };
		if ( kind == present::VisualKind::Player )
		{
			e.hasAnim = true;
		}
		return e;
	};
	struct Seen
	{
		uint64_t id = 0;
		present::VisualKind kind = present::VisualKind::Static;
		bool ragdollAnim = false, playerAnim = false;
		float yaw = 0.0f;
		int count = 0; // visuals of the number: one
	};
	auto seen = [&]() {
		Seen out;
		mirror.ForEach( [&]( uint64_t id, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim* a,
							 const present::RagdollAnim* r ) {
			if ( v.netId == 7 )
			{
				out = { id, v.kind, r != nullptr, a != nullptr, r != nullptr ? r->yaw : 0.0f, out.count + 1 };
			}
		} );
		return out;
	};
	auto removedOrSpawned = [&]( uint64_t old ) {
		bool removed = false, spawned = false;
		for ( const present::Event& e : mirror.Events() )
		{
			removed |= e.type == present::EventType::Removed && e.visual == old;
			spawned |= e.type == present::EventType::Spawned && e.netId == 7 && e.visual != old;
		}
		return removed && spawned;
	};
	auto step = [&]( present::FrameEntity e ) {
		frame.tick += 1;
		frame.rolledBack = frame.tick > 1;
		frame.entities = { e };
		mirror.Update( frame, 0.5f, 1.0f / 60.0f );
	};

	step( entity( present::VisualKind::Prop ) );
	const Seen prop = seen();
	CHECK( prop.count == 1 && prop.kind == present::VisualKind::Prop && prop.ragdollAnim == false );
	// The rollback's frame: 7 is a ragdoll now.
	present::FrameRagdoll ragdoll;
	ragdoll.netId = 7;
	ragdoll.yaw = 1.25f;
	frame.ragdolls = { ragdoll };
	present::FrameEntity asRagdoll = entity( present::VisualKind::Ragdoll );
	asRagdoll.ragdoll = 0;
	step( asRagdoll );
	const Seen rag = seen();
	std::printf( "    7 a prop (visual %llu), then a ragdoll (visual %llu, posed: %s, yaw %.2f)\n", (unsigned long long)prop.id,
				 (unsigned long long)rag.id, rag.ragdollAnim ? "yes" : "no", double( rag.yaw ) );
	CHECK( rag.count == 1 && rag.kind == present::VisualKind::Ragdoll && rag.ragdollAnim && rag.yaw == 1.25f && rag.id != prop.id );
	CHECK( removedOrSpawned( prop.id ) );
	// Then an item, then a player's body: each its own visual, the player's animated.
	frame.ragdolls.clear();
	present::FrameEntity asItem = entity( present::VisualKind::Item );
	asItem.itemKind = 2;
	step( asItem );
	const Seen item = seen();
	CHECK( item.count == 1 && item.kind == present::VisualKind::Item && item.id != rag.id && removedOrSpawned( rag.id ) );
	asItem.itemKind = 3; // another kind of item under the same number
	step( asItem );
	const Seen other = seen();
	CHECK( other.count == 1 && other.id != item.id && removedOrSpawned( item.id ) );
	step( entity( present::VisualKind::Player ) );
	const Seen player = seen();
	CHECK( player.count == 1 && player.kind == present::VisualKind::Player && player.playerAnim && player.id != other.id );
	// The same thing again keeps its visual.
	step( entity( present::VisualKind::Player ) );
	CHECK( seen().id == player.id );
}

// M218: a character's eyes and whether it may be seen in first person. The mannequin's scene has a
// first-person camera on its head: its eye is there, where the game's eye always was (9 cm above
// the head bone); a character whose files give an eye on the body and no camera has that eye and
// no first person; and a body of such a character is seen from behind in every simulation: a
// first-person view in its input does not turn it to face the camera, while one that may does.
void TestFirstPersonCamera()
{
	namespace fs = std::filesystem;
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin";
	std::string error, warnings;
	auto mannequin = anim::AnimSet::Load( dir, error, warnings );
	CHECK( mannequin != nullptr );
	if ( mannequin == nullptr )
	{
		return;
	}
	const int head = anim::FindJoint( *mannequin, "Head" );
	float v[4];
	ozz::math::StorePtrU( mannequin->RestModels()[size_t( head )].cols[3], v );
	std::printf( "    the mannequin: first person %s, its eye on joint %d (the head %d), %.4f m up (the head + kEyeUp: %.4f)\n",
				 mannequin->FirstPerson() ? "yes" : "no", mannequin->EyeJoint(), head, double( anim::EyeHeight( *mannequin ) ),
				 double( v[1] + kEyeUp ) );
	CHECK( mannequin->FirstPerson() && mannequin->EyeJoint() == head && std::fabs( anim::EyeHeight( *mannequin ) - ( v[1] + kEyeUp ) ) < 1e-3f );
	auto builtIn = anim::AnimSet::CreateProcedural();
	CHECK( builtIn->FirstPerson() && builtIn->EyeJoint() == -2 );

	// Its files with an eye on the body and no camera; then an eye on a bone it has not.
	const fs::path copy = fs::temp_directory_path() / "cb_test_first_person";
	fs::remove_all( copy );
	fs::copy( dir, copy, fs::copy_options::recursive );
	auto rewrite = [&]( const std::string& eye ) {
		std::ifstream in( copy / "anim.cfg" );
		std::string line, text;
		while ( std::getline( in, line ) )
		{
			if ( line.rfind( "eye =", 0 ) != 0 && line.rfind( "first_person =", 0 ) != 0 )
			{
				text += line + "\n";
			}
		}
		in.close();
		std::ofstream( copy / "anim.cfg" ) << text << eye << "\n";
	};
	rewrite( "eye = . 0.0 1.2 0.1" );
	warnings.clear();
	auto marked = anim::AnimSet::Load( copy.string(), error, warnings );
	CHECK( marked != nullptr && marked->FirstPerson() == false && marked->EyeJoint() == -1 && std::fabs( anim::EyeHeight( *marked ) - 1.2f ) < 1e-5f );
	rewrite( "eye = Tail 0 0 0" );
	warnings.clear();
	auto wrong = anim::AnimSet::Load( copy.string(), error, warnings );
	CHECK( wrong != nullptr && wrong->EyeJoint() == -2 && warnings.find( "eye = Tail" ) != std::string::npos );
	fs::remove_all( copy );

	// In the simulation: the second character may not be seen in first person.
	Simulation sim( TestConfig(), FlatMap() );
	Simulation::CharacterBody seen, behind;
	behind.firstPerson = false;
	sim.SetCharacters( { seen, behind } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	f.events.push_back( { PlayerEventType::Join, 1 } );
	SimCommand become;
	become.type = CommandType::SetCharacter;
	become.target = SlotTarget( 1 );
	become.index = 1;
	f.commands.push_back( become );
	step( 30 );
	const float before = sim.PlayerCharacter( PlayerSlot( 1 ) )->facingYaw;
	for ( int slot = 0; slot < 2; ++slot )
	{
		f.inputs[slot].view = uint8_t( ViewMode::FirstPerson );
		f.inputs[slot].cameraYaw = 16384; // a quarter turn
	}
	step( 10 );
	const float yaw = detmath::WrapAngle( detmath::YawToRadians( 16384 ) );
	const float first = sim.PlayerCharacter( PlayerSlot( 0 ) )->facingYaw, second = sim.PlayerCharacter( PlayerSlot( 1 ) )->facingYaw;
	std::printf( "    looking a quarter turn away in first person: the one that may faces %.3f (the camera %.3f), the other %.3f (was %.3f)\n",
				 double( first ), double( yaw ), double( second ), double( before ) );
	CHECK( std::fabs( first - yaw ) < 1e-4f && second == before );
}

// M220: bodies apart from players. A body of a second character spawned in the air, nobody's,
// falls and stands with its feet on the ground (a physics body of its capsule, reaching down to its
// feet, its rotation locked); a slot possesses it: the slot drives it (it walks on the input), and
// the body it drove stands where it was, nobody's, the item it held put away, not sunk into the
// floor and not tipped over; it is pushed like any loose body; another slot cannot take a body
// someone drives; a joining client's copy (the portable snapshot) and a rollback go on the same.
void TestPossession()
{
	Simulation sim( TestConfig(), FlatMap() );
	Simulation::CharacterBody small, big;
	big.radius = 0.5f;
	big.halfHeight = 1.0f;
	sim.SetCharacters( { small, big } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 30 );
	const uint32_t first = sim.PlayerNetId( 0 );
	const float standing = sim.EntityTransform( first )->position.y;
	{
		SimCommand give;
		give.type = CommandType::SpawnItem;
		give.target = SlotTarget( 0 );
		give.index = 0;
		give.mode = uint8_t( kSocketRightHand );
		f.commands.push_back( give );
	}
	step( 2 );
	const std::vector<uint32_t> carried = sim.CarriedItemsOf( first );

	// A body nobody drives, spawned in the air.
	const uint32_t body = sim.Globals().nextNetId;
	SimCommand spawn;
	spawn.type = CommandType::SpawnBody;
	spawn.index = 1;
	spawn.a = { 3.0f, 2.0f, 0.0f };
	spawn.value = 16384;
	f.commands.push_back( spawn );
	step( 150 );
	flecs::entity idle = sim.FindEntity( body );
	const float feet = mover::FeetBelowCenter( big.radius, big.halfHeight );
	const float landed = idle.is_valid() ? sim.EntityTransform( body )->position.y : -1.0f;
	std::printf( "    a body nobody drives, spawned 2 m up: its centre %.3f m up (its feet %.3f below it); character %u\n", double( landed ),
				 double( feet ), unsigned( sim.CharacterOf( idle ) ) );
	CHECK( idle.is_valid() && idle.has<IdleBody>() && idle.has<Character>() == false && sim.CharacterOf( idle ) == 1 &&
		   std::fabs( landed - feet ) < 0.05f );

	// Slot 0 possesses it.
	SimCommand possess;
	possess.type = CommandType::Possess;
	possess.target = SlotTarget( 0 );
	possess.other = body;
	f.commands.push_back( possess );
	step( 2 );
	flecs::entity left = sim.FindEntity( first );
	CHECK( sim.PlayerNetId( 0 ) == body && sim.FindEntity( body ).has<Character>() && sim.FindEntity( body ).get<Character>().slot == 0 &&
		   sim.HasAnimState( sim.FindEntity( body ) ) );
	CHECK( left.is_valid() && left.has<IdleBody>() && left.has<Character>() == false && sim.CharacterOf( left ) == 0 );
	bool putAway = carried.empty() == false;
	for ( uint32_t item : carried )
	{
		const HeldItem* held = sim.FindEntity( item ).try_get<HeldItem>();
		putAway = putAway && held != nullptr && held->holder == first && held->stowed == 1;
	}
	CHECK( putAway );
	f.inputs[0].moveForward = 127;
	const b3Vec3 from = sim.EntityTransform( body )->position;
	step( 60 );
	f.inputs[0].moveForward = 0;
	const b3Vec3 to = sim.EntityTransform( body )->position;
	const Transform stood = *sim.EntityTransform( first );
	std::printf( "    driven, it walked %.2f m; the body left stands %.3f m up (driven it stood %.3f), turned %.4f off upright\n",
				 double( b3Distance( from, to ) ), double( stood.position.y ), double( standing ),
				 double( std::fabs( stood.rotation.v.x ) + std::fabs( stood.rotation.v.z ) ) );
	CHECK( b3Distance( from, to ) > 1.0f && std::fabs( stood.position.y - standing ) < 0.05f &&
		   std::fabs( stood.rotation.v.x ) + std::fabs( stood.rotation.v.z ) < 1e-4f );

	// Slot 1 cannot take the body slot 0 drives.
	possess.target = SlotTarget( 1 );
	f.commands.push_back( possess );
	step( 2 );
	CHECK( sim.PlayerNetId( 0 ) == body && sim.PlayerNetId( 1 ) != body );

	// Pushed like any loose body.
	SimCommand push;
	push.type = CommandType::Impulse;
	push.target = first;
	push.mode = ImpulseVelocity;
	push.b = { 3.0f, 0.0f, 0.0f };
	f.commands.push_back( push );
	step( 20 );
	CHECK( sim.EntityTransform( first )->position.x > stood.position.x + 0.3f );

	// A joining client's copy, and a rollback, go on the same.
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation copy( TestConfig(), FlatMap() );
	copy.SetCharacters( { small, big } );
	CHECK( copy.LoadPortable( image ) );
	Snapshot snap;
	sim.Save( snap );
	f.inputs[0].moveRight = 127;
	for ( int i = 0; i < 60; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		copy.Step( f );
	}
	const uint64_t ahead = sim.ComputeHash();
	CHECK( ahead == copy.ComputeHash() );
	sim.Load( snap );
	for ( int i = 0; i < 60; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
	}
	CHECK( sim.ComputeHash() == ahead );
}

// M221: driving a prop. A wall with a doorway 1 m wide. A player possesses a crate 0.6 m across:
// it walks on the input, upright, its bottom on the ground, and goes through the doorway; the body
// the player left stands nobody's. It then possesses a crate 1.6 m across, whose walking capsule
// (0.4 m) would fit: its bounds go along and stop it at the wall, while the first crate is a loose
// body again. A shot hits the driven crate as its own shape; a joining client's copy goes on the
// same; the player leaving gives the crate back to the world.
void TestPossessProp()
{
	LevelLayout map = FlatMap();
	map.statics.push_back( { { -5.5f, 1.5f, 6.0f }, { 5.0f, 1.5f, 0.25f }, 0.0f, 0.0f } );
	map.statics.push_back( { { 5.5f, 1.5f, 6.0f }, { 5.0f, 1.5f, 0.25f }, 0.0f, 0.0f } );
	QuantizeLayout( map );
	Simulation sim( TestConfig(), map );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto crate = [&]( b3Vec3 at, b3Vec3 half ) {
		const uint32_t id = sim.Globals().nextNetId;
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.value = -1;
		c.mode = uint8_t( ShapeKind::Box );
		c.a = { at.x, at.y, at.z };
		c.c = { half.x, half.y, half.z };
		f.commands.push_back( c );
		step( 1 );
		return id;
	};
	auto possess = [&]( uint32_t body ) {
		SimCommand c;
		c.type = CommandType::Possess;
		c.target = SlotTarget( 0 );
		c.other = body;
		f.commands.push_back( c );
		step( 2 );
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	const uint32_t first = sim.PlayerNetId( 0 );
	const uint32_t small = crate( { 0.0f, 0.4f, 2.0f }, { 0.3f, 0.3f, 0.3f } );
	const uint32_t big = crate( { 0.0f, 0.6f, -3.0f }, { 0.8f, 0.5f, 0.8f } );
	step( 60 );

	possess( small );
	CHECK( sim.PlayerNetId( 0 ) == small && sim.FindEntity( small ).has<DrivenProp>() && sim.FindEntity( first ).has<IdleBody>() );
	step( 30 );
	present::PresentationFrame frame;
	auto drawn = [&]( uint32_t netId ) {
		present::CaptureFrame( sim, frame );
		for ( const present::FrameEntity& e : frame.entities )
		{
			if ( e.netId == netId )
			{
				return e;
			}
		}
		return present::FrameEntity{};
	};
	const present::FrameEntity standing = drawn( small );
	std::printf( "    the small crate driven: drawn as a %s at %.3f m up (its half height 0.3), turned %.4f off upright\n",
				 standing.kind == present::VisualKind::Prop ? "prop" : "player", double( standing.transform.position.y ),
				 double( std::fabs( standing.transform.rotation.v.x ) + std::fabs( standing.transform.rotation.v.z ) ) );
	CHECK( standing.kind == present::VisualKind::Prop && std::fabs( standing.transform.position.y - 0.3f ) < 0.05f &&
		   std::fabs( standing.transform.rotation.v.x ) + std::fabs( standing.transform.rotation.v.z ) < 1e-3f );
	f.inputs[0].moveForward = 127;
	f.inputs[0].cameraYaw = 0;
	step( 180 );
	const b3Vec3 through = drawn( small ).transform.position;
	std::printf( "    walked to the doorway: it is at z %.2f (the wall at 6), x %.2f\n", double( through.z ), double( through.x ) );
	CHECK( through.z > 7.0f );

	// The big one: through the same doorway it does not go.
	f.inputs[0].moveForward = 0;
	possess( big );
	CHECK( sim.PlayerNetId( 0 ) == big && sim.FindEntity( small ).has<DrivenProp>() == false && sim.FindEntity( small ).has<Character>() == false );
	const DrivenProp walker = sim.FindEntity( big ).get<DrivenProp>();
	f.inputs[0].moveForward = 127;
	step( 300 );
	f.inputs[0].moveForward = 0;
	const b3Vec3 held = drawn( big ).transform.position;
	std::printf( "    the big crate (its walking capsule %.2f m round): held at z %.2f, x %.2f, by its bounds\n", double( walker.radius ),
				 double( held.z ), double( held.x ) );
	CHECK( walker.radius * 2.0f < 1.0f && held.z < 5.4f && held.z > 4.0f );

	// A shot hits it as its shape.
	RayHit hit;
	const bool shot = sim.CastRay( b3Vec3{ held.x, 0.5f, held.z - 6.0f }, b3Vec3{ 0.0f, 0.0f, 12.0f }, 0, hit, true );
	CHECK( shot && hit.netId == big );

	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation copy( TestConfig(), map );
	CHECK( copy.LoadPortable( image ) );
	f.inputs[0].moveRight = 127;
	for ( int i = 0; i < 60; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		copy.Step( f );
	}
	CHECK( sim.ComputeHash() == copy.ComputeHash() );

	// The player leaves: the crate stays, a loose body.
	f.inputs[0] = {};
	f.events.push_back( { PlayerEventType::Leave, 0 } );
	step( 2 );
	flecs::entity left = sim.FindEntity( big );
	CHECK( left.is_valid() && left.has<Character>() == false && left.has<DrivenProp>() == false && sim.PlayerNetId( 0 ) == 0 );
}

// M222: bodies a mod drives. An input survives being packed into a command exactly. A body nobody
// drives, driven by a Drive command: it walks on that input and goes on with it with no further
// command (as a client predicts it), plays its walk (its animation state follows), stops on a new
// input; a crate driven the same way walks too; a portable copy and a rollback go on the same; told
// to stop, it stands nobody's.
void TestModDriven()
{
	PlayerInput odd;
	odd.moveRight = -127;
	odd.moveForward = 93;
	odd.cameraYaw = 65535;
	odd.cameraPitch = -16000;
	odd.buttons = 0xA5;
	odd.view = 3;
	odd.actions = ActionBits( 0xF00DBEEFu );
	odd.intent = 2;
	odd.intentA = 255;
	odd.intentSeq = 200;
	odd.slotKeys = 0x8001;
	SimCommand packed;
	PackInput( odd, packed );
	CHECK( UnpackInput( packed ) == odd );

	Simulation sim( TestConfig(), FlatMap() );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto drive = [&]( uint32_t body, int8_t forward, int mode = 1 ) {
		PlayerInput in;
		in.moveForward = forward;
		SimCommand c;
		c.type = CommandType::Drive;
		c.target = body;
		c.mode = uint8_t( mode );
		PackInput( in, c );
		f.commands.push_back( c );
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 10 );
	const uint32_t npc = sim.Globals().nextNetId;
	SimCommand spawn;
	spawn.type = CommandType::SpawnBody;
	spawn.a = { 4.0f, 0.0f, -6.0f };
	f.commands.push_back( spawn );
	step( 30 );
	const b3Vec3 start = sim.EntityTransform( npc )->position;
	drive( npc, 127 );
	step( 40 );
	const flecs::entity e = sim.FindEntity( npc );
	const b3Vec3 walked = sim.EntityTransform( npc )->position;
	const float speed = sim.GetAnimState( e ).groundSpeed;
	CHECK( e.has<ModDriven>() && e.has<Character>() && e.get<Character>().slot == kNoSlot && e.has<IdleBody>() == false );
	step( 40 ); // no command: on with the same input
	const b3Vec3 further = sim.EntityTransform( npc )->position;
	std::printf( "    driven: %.2f m in 40 ticks, %.2f more in 40 with no command; its walk at %.2f m/s\n", double( walked.z - start.z ),
				 double( further.z - walked.z ), double( speed ) );
	CHECK( walked.z - start.z > 1.0f && further.z - walked.z > 1.5f && speed > 1.0f );
	drive( npc, 0 );
	step( 40 );
	const b3Vec3 v = e.get<Character>().velocity;
	std::printf( "    a new input (none): its velocity %.3f %.3f %.3f\n", double( v.x ), double( v.y ), double( v.z ) );
	CHECK( b3Length( b3Vec3{ v.x, 0.0f, v.z } ) < 0.1f );

	// A crate, driven.
	const uint32_t crate = sim.Globals().nextNetId;
	SimCommand box;
	box.type = CommandType::SpawnProp;
	box.value = -1;
	box.mode = uint8_t( ShapeKind::Box );
	box.a = { -4.0f, 0.4f, -6.0f };
	box.c = { 0.3f, 0.3f, 0.3f };
	f.commands.push_back( box );
	step( 30 );
	const float crateStart = sim.EntityTransform( crate )->position.z;
	drive( crate, 127 );
	step( 60 );
	CHECK( sim.FindEntity( crate ).has<DrivenProp>() && sim.EntityTransform( crate )->position.z > crateStart + 1.5f );

	// A copy and a rollback go on the same, with both driven.
	drive( npc, 100 );
	step( 1 );
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation copy( TestConfig(), FlatMap() );
	CHECK( copy.LoadPortable( image ) );
	Snapshot snap;
	sim.Save( snap );
	for ( int i = 0; i < 60; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		copy.Step( f );
	}
	const uint64_t ahead = sim.ComputeHash();
	CHECK( ahead == copy.ComputeHash() );
	sim.Load( snap );
	step( 60 );
	CHECK( sim.ComputeHash() == ahead );

	// Driven no more: alive, it stands nobody's.
	drive( npc, 0, 0 );
	drive( crate, 0, 0 );
	step( 2 );
	CHECK( sim.FindEntity( npc ).has<IdleBody>() && sim.FindEntity( npc ).has<Character>() == false && sim.FindEntity( npc ).has<ModDriven>() == false );
	CHECK( sim.FindEntity( crate ).has<DrivenProp>() == false && sim.FindEntity( crate ).has<Character>() == false );
	// Killed (it fell as its ragdoll), then given back: the body goes, it does not stand up again.
	drive( npc, 50 );
	step( 2 );
	SimCommand kill;
	kill.type = CommandType::Kill;
	kill.target = npc;
	kill.mode = 1;
	f.commands.push_back( kill );
	step( 3 );
	drive( npc, 0, 0 );
	step( 2 );
	CHECK( sim.FindEntity( npc ).is_valid() == false );
}

// The physics world stands alone (M247; ROADMAP step 3): bodies go between it and the characters'
// world by its events. A body nobody drives taken over (and the player's own left behind), a crate
// driven, then let go when its driver takes over the body left behind, an item spawned, picked up and
// dropped, a player killed into a ragdoll. A simulation with no characters at all (no joins, no
// inputs), given the first's physics events and pushers, has its physics world on every tick, by
// its entities and by Box3D's own bytes.
void TestPhysicsWithoutCharacters()
{
	const SimConfig config = TestConfig();
	Simulation sim( config, FlatMap() );
	std::vector<InputFrame> frames;
	std::vector<InputFrame> made;
	std::vector<std::pair<uint64_t, uint64_t>> hashes;
	uint32_t idle = 0, crate = 0, item = 0;
	for ( uint32_t t = 0; t < 300; ++t )
	{
		InputFrame f;
		f.tick = t;
		if ( t == 0 )
		{
			f.events.push_back( { PlayerEventType::Join, 0 } );
			f.events.push_back( { PlayerEventType::Join, 1 } );
		}
		SimCommand c;
		switch ( t )
		{
			case 5:
				c.type = CommandType::SpawnBody;
				c.a = { 3.0f, 0.0f, 3.0f };
				idle = sim.Globals().nextNetId;
				f.commands.push_back( c );
				break;
			case 10:
				c.type = CommandType::SpawnProp;
				c.value = -1;
				c.mode = uint8_t( ShapeKind::Box );
				c.a = { -3.0f, 0.5f, 3.0f };
				c.c = { 0.4f, 0.4f, 0.4f };
				crate = sim.Globals().nextNetId;
				f.commands.push_back( c );
				break;
			case 30:
				c.type = CommandType::Possess;
				c.target = SlotTarget( 0 );
				c.other = idle;
				f.commands.push_back( c );
				break;
			case 60:
				c.type = CommandType::Possess;
				c.target = SlotTarget( 1 );
				c.other = crate;
				f.commands.push_back( c );
				break;
			case 120:
				// Slot 1 takes over the body slot 0 left: the crate is let go.
				c.type = CommandType::Possess;
				c.target = SlotTarget( 1 );
				c.other = kCharacterIdBase; // slot 0's first body (the first character's NetId)
				f.commands.push_back( c );
				break;
			case 150:
				c.type = CommandType::SpawnItem;
				c.a = { 0.0f, 1.0f, 2.0f };
				item = sim.Globals().nextNetId;
				f.commands.push_back( c );
				break;
			case 180:
				c.type = CommandType::PickUpItem;
				c.target = SlotTarget( 0 );
				c.other = item;
				f.commands.push_back( c );
				break;
			case 210:
				c.type = CommandType::DropItem;
				c.target = item;
				c.a = { 1.0f, 1.5f, 1.0f };
				f.commands.push_back( c );
				break;
			case 240:
				c.type = CommandType::Kill;
				c.target = SlotTarget( 1 );
				c.mode = 1;
				c.b = { 0.0f, 2.0f, 4.0f };
				f.commands.push_back( c );
				break;
			case 260:
			{
				// One tick, one body crossing twice: slot 0 takes a new body, leaving the one it drove,
				// which slot 1 takes at once (each command's events are applied before the next runs).
				const uint32_t left = sim.PlayerNetId( 0 );
				c.type = CommandType::SpawnBody;
				c.a = { 5.0f, 0.0f, 5.0f };
				const uint32_t fresh = sim.Globals().nextNetId;
				f.commands.push_back( c );
				SimCommand take;
				take.type = CommandType::Possess;
				take.target = SlotTarget( 0 );
				take.other = fresh;
				f.commands.push_back( take );
				take.target = SlotTarget( 1 );
				take.other = left;
				f.commands.push_back( take );
				break;
			}
			default:
				break;
		}
		f.inputs[0].moveForward = t > 40 && t < 100 ? int8_t( 90 ) : int8_t( 0 );
		f.inputs[1].moveForward = t > 70 && t < 110 ? int8_t( 127 ) : int8_t( 0 );
		f.inputs[1].cameraYaw = 16384;
		sim.Step( f );
		if ( t == 261 )
		{
			// (Both drive a body of their own, each with its body: a character without one crashed M247's
			// first version.)
			for ( PlayerSlot slot : { PlayerSlot( 0 ), PlayerSlot( 1 ) } )
			{
				flecs::entity e = sim.FindEntity( sim.PlayerNetId( slot ) );
				CHECK( e.is_valid() && e.has<Character>() && e.has<PhysicsBody>() && e.get<PhysicsBody>().body.world0 == 1 );
			}
			CHECK( sim.PlayerNetId( 0 ) != sim.PlayerNetId( 1 ) );
		}
		frames.push_back( f );
		made.push_back( sim.PhysicsMade() );
		hashes.push_back( { sim.PhysicsHash(), sim.PhysicsWorldHash() } );
	}
	size_t kinds[16] = {};
	for ( const InputFrame& m : made )
	{
		for ( const PhysicsEvent& e : m.physics )
		{
			kinds[size_t( e.type ) & 15] += 1;
		}
	}
	std::printf( "    events: %zu take-overs, %zu let go, %zu left behind, %zu into the world, %zu ragdolls, %zu spawns\n",
				 kinds[size_t( PhysicsEventType::TakeOver )], kinds[size_t( PhysicsEventType::Enter )], kinds[size_t( PhysicsEventType::Idle )],
				 kinds[size_t( PhysicsEventType::IntoWorld )], kinds[size_t( PhysicsEventType::Ragdoll )], kinds[size_t( PhysicsEventType::Spawn )] );
	CHECK( kinds[size_t( PhysicsEventType::TakeOver )] >= 4 ); // the body, the crate, the left body, the item
	CHECK( kinds[size_t( PhysicsEventType::Enter )] == 1 );
	CHECK( kinds[size_t( PhysicsEventType::Idle )] >= 1 );
	CHECK( kinds[size_t( PhysicsEventType::IntoWorld )] == 1 );
	CHECK( kinds[size_t( PhysicsEventType::Ragdoll )] == 1 );

	Simulation alone( config, FlatMap() );
	alone.SetFollowsPhysics( true );
	uint32_t wrong = UINT32_MAX, wrongWorld = UINT32_MAX;
	for ( size_t i = 0; i < frames.size(); ++i )
	{
		InputFrame f = frames[i];
		f.events.clear();
		f.inputs = {};
		f.pushers = made[i].pushers;
		f.physics = made[i].physics;
		alone.Step( f );
		if ( alone.PhysicsHash() != hashes[i].first && wrong == UINT32_MAX )
		{
			wrong = f.tick;
		}
		if ( alone.PhysicsWorldHash() != hashes[i].second && wrongWorld == UINT32_MAX )
		{
			wrongWorld = f.tick;
		}
	}
	std::printf( "    with no characters: entities %s, Box3D world %s\n", wrong == UINT32_MAX ? "the same" : ( "differ from tick " + std::to_string( wrong ) ).c_str(),
				 wrongWorld == UINT32_MAX ? "the same" : ( "differs from tick " + std::to_string( wrongWorld ) ).c_str() );
	CHECK( wrong == UINT32_MAX );
	CHECK( wrongWorld == UINT32_MAX );
}

// Found after M222 (hunting bugs): what a slot drives can be removed: a mod's Destroy, the props
// cap (the player spawned the crate it drives). The body a slot drives stays; anything else goes.
void TestDrivenBodyKept()
{
	SimConfig config = TestConfig();
	config.propsGlobal = 2;
	Simulation sim( config, FlatMap() );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto crate = [&]( float x ) {
		const uint32_t id = sim.Globals().nextNetId;
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.target = SlotTarget( 0 ); // the player's own: it counts against its cap
		c.value = -1;
		c.mode = uint8_t( ShapeKind::Box );
		c.a = { x, 0.4f, 3.0f };
		c.c = { 0.3f, 0.3f, 0.3f };
		f.commands.push_back( c );
		step( 1 );
		return id;
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 10 );
	const uint32_t mine = crate( 0.0f );
	step( 30 );
	SimCommand possess;
	possess.type = CommandType::Possess;
	possess.target = SlotTarget( 0 );
	possess.other = mine;
	f.commands.push_back( possess );
	step( 2 );
	CHECK( sim.PlayerNetId( 0 ) == mine );
	// Over the cap: the oldest goes, but not the one it drives.
	crate( 1.0f );
	crate( 2.0f );
	crate( 3.0f );
	step( 5 );
	CHECK( sim.PlayerNetId( 0 ) == mine && sim.FindEntity( mine ).is_valid() );
	// A mod's Destroy: not the body a slot drives.
	SimCommand destroy;
	destroy.type = CommandType::Destroy;
	destroy.target = mine;
	f.commands.push_back( destroy );
	step( 5 );
	f.inputs[0].moveForward = 127;
	step( 30 );
	CHECK( sim.PlayerNetId( 0 ) == mine && sim.FindEntity( mine ).is_valid() && sim.FindEntity( mine ).has<Character>() );

	// Killed while it drives a crate, then gone: the crate stays, a loose body that collides and is
	// hit again (not left disabled, as a dead body is). (A world of its own: no caps.)
	Simulation world( TestConfig(), FlatMap() );
	auto stepWorld = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = world.Tick();
			world.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f = InputFrame{};
	f.tick = 0;
	f.events.push_back( { PlayerEventType::Join, 0 } );
	stepWorld( 10 );
	const uint32_t box = world.Globals().nextNetId;
	SimCommand spawnBox;
	spawnBox.type = CommandType::SpawnProp;
	spawnBox.value = -1;
	spawnBox.mode = uint8_t( ShapeKind::Box );
	spawnBox.a = { 0.0f, 0.4f, 3.0f };
	spawnBox.c = { 0.3f, 0.3f, 0.3f };
	f.commands.push_back( spawnBox );
	stepWorld( 30 );
	possess.other = box;
	f.commands.push_back( possess );
	stepWorld( 2 );
	CHECK( world.PlayerNetId( 0 ) == box );
	SimCommand kill;
	kill.type = CommandType::Kill;
	kill.target = SlotTarget( 0 );
	kill.mode = 1;
	f.commands.push_back( kill );
	stepWorld( 5 );
	f.events.push_back( { PlayerEventType::Leave, 0 } );
	stepWorld( 30 );
	const flecs::entity left = world.FindEntity( box );
	CHECK( left.is_valid() && left.has<Character>() == false );
	const b3Vec3 at = left.is_valid() ? world.EntityTransform( box )->position : b3Vec3{};
	RayHit hit;
	const bool seen = world.CastRay( b3Vec3{ at.x, at.y, at.z - 3.0f }, b3Vec3{ 0.0f, 0.0f, 6.0f }, 0, hit );
	std::printf( "    the crate after its driver died and left: at %.2f m up, a ray %s it\n", double( at.y ), seen && hit.netId == box ? "hits" : "misses" );
	CHECK( seen && hit.netId == box );
}

// Found after M224: a driven crate walked into walls. A crate 1 m across driven at a wall, at an
// angle and with its look swinging (so it turns as it goes): no corner of it gets into the wall.
void TestPropWalls()
{
	LevelLayout map = FlatMap();
	map.statics.push_back( { { 0.0f, 1.5f, 6.25f }, { 10.0f, 1.5f, 0.25f }, 0.0f, 0.0f } ); // its face at z = 6
	QuantizeLayout( map );
	Simulation sim( TestConfig(), map );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 10 );
	const uint32_t crate = sim.Globals().nextNetId;
	SimCommand c;
	c.type = CommandType::SpawnProp;
	c.value = -1;
	c.mode = uint8_t( ShapeKind::Box );
	c.a = { 0.0f, 0.6f, 2.0f };
	c.c = { 0.5f, 0.5f, 0.5f };
	f.commands.push_back( c );
	step( 30 );
	SimCommand possess;
	possess.type = CommandType::Possess;
	possess.target = SlotTarget( 0 );
	possess.other = crate;
	f.commands.push_back( possess );
	step( 2 );
	float deepest = -10.0f;
	for ( int i = 0; i < 400; ++i )
	{
		f.inputs[0].moveForward = 127;
		f.inputs[0].moveRight = int8_t( i % 120 < 60 ? 60 : -60 );
		f.inputs[0].cameraYaw = uint16_t( int( 3000.0f * std::sin( float( i ) * 0.05f ) ) );
		step( 1 );
		const Transform& t = *sim.EntityTransform( crate );
		const DrivenProp& prop = sim.FindEntity( crate ).get<DrivenProp>();
		for ( int k = 0; k < 8; ++k )
		{
			const b3Vec3 corner = { ( k & 1 ) ? prop.high[0] : prop.low[0], ( k & 2 ) ? prop.high[1] : prop.low[1], ( k & 4 ) ? prop.high[2] : prop.low[2] };
			const b3Vec3 at = b3Add( b3Vec3{ t.position.x, t.position.y - prop.drop, t.position.z }, b3RotateVector( t.rotation, corner ) );
			deepest = std::max( deepest, at.z - 6.0f );
		}
	}
	std::printf( "    the crate's deepest corner into the wall: %.3f m\n", double( deepest ) );
	CHECK( deepest < 0.02f );

	// Against the wall, it leaves it again: straight back, and along it.
	f.inputs[0] = {};
	f.inputs[0].moveForward = 127;
	step( 60 ); // (pressed to it, facing it)
	const b3Vec3 pressed = sim.EntityTransform( crate )->position;
	f.inputs[0].moveForward = -127;
	step( 30 );
	const b3Vec3 backed = sim.EntityTransform( crate )->position;
	f.inputs[0].moveForward = 127;
	step( 60 );
	f.inputs[0].moveForward = 0;
	f.inputs[0].moveRight = 127;
	step( 30 );
	const b3Vec3 slid = sim.EntityTransform( crate )->position;
	std::printf( "    against the wall at z %.2f: backed off to %.2f; along it %.2f m\n", double( pressed.z ), double( backed.z ),
				 double( std::fabs( slid.x - pressed.x ) ) );
	CHECK( pressed.z - backed.z > 0.5f && std::fabs( slid.x - pressed.x ) > 0.5f );
	f.inputs[0] = {};

	// Against a player, both ways as between two players: neither shoves the other.
	f.inputs[0] = {};
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 30 );
	const uint32_t other = sim.PlayerNetId( 1 );
	const b3Vec3 stands = sim.EntityTransform( other )->position;
	const b3Vec3 box = sim.EntityTransform( crate )->position;
	// The crate drives at the player (its look turned toward it), and on.
	const b3Vec3 toward = b3Sub( stands, box );
	f.inputs[0].cameraYaw = detmath::RadiansToYaw( detmath::Atan2( toward.x, toward.z ) );
	f.inputs[0].moveForward = 127;
	step( 150 );
	f.inputs[0].moveForward = 0;
	const float shoved = b3Length( b3Sub( sim.EntityTransform( other )->position, stands ) );
	// The player walks into the crate.
	const b3Vec3 boxNow = sim.EntityTransform( crate )->position;
	const b3Vec3 back = b3Sub( boxNow, sim.EntityTransform( other )->position );
	f.inputs[1].cameraYaw = detmath::RadiansToYaw( detmath::Atan2( back.x, back.z ) );
	f.inputs[1].moveForward = 127;
	step( 150 );
	const float pushed = b3Length( b3Sub( sim.EntityTransform( crate )->position, boxNow ) );
	std::printf( "    the crate driven into a player moved it %.3f m; the player walking into the crate moved it %.3f m\n", double( shoved ),
				 double( pushed ) );
	CHECK( shoved < 0.05f && pushed < 0.05f );
}

// Found after M225: a driven crate next to a wall could not leave it. Walls turned 30 degrees, a
// corner; a crate pressed into them at random, then walked away: it gets away every time.
void TestPropLeavesWalls()
{
	LevelLayout map = FlatMap();
	map.statics.push_back( { { 0.0f, 1.5f, 8.0f }, { 6.0f, 1.5f, 0.25f }, 0.0f, 0.5236f } ); // turned 30 degrees
	map.statics.push_back( { { -8.0f, 1.5f, 0.0f }, { 0.25f, 1.5f, 6.0f }, 0.0f, 0.0f } );	// and a corner with
	map.statics.push_back( { { -4.0f, 1.5f, -6.0f }, { 4.0f, 1.5f, 0.25f }, 0.0f, 0.0f } ); // this one
	QuantizeLayout( map );
	Simulation sim( TestConfig(), map );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 10 );
	const uint32_t crate = sim.Globals().nextNetId;
	SimCommand c;
	c.type = CommandType::SpawnProp;
	c.value = -1;
	c.mode = uint8_t( ShapeKind::Box );
	c.a = { 0.0f, 0.6f, 2.0f };
	c.c = { 0.5f, 0.5f, 0.5f };
	f.commands.push_back( c );
	step( 30 );
	SimCommand possess;
	possess.type = CommandType::Possess;
	possess.target = SlotTarget( 0 );
	possess.other = crate;
	f.commands.push_back( possess );
	step( 2 );
	uint64_t rng = 12345;
	auto next = [&]() {
		rng ^= rng << 13;
		rng ^= rng >> 7;
		rng ^= rng << 17;
		return rng;
	};
	int stuck = 0, tries = 0;
	// Toward each wall in turn, jiggled into it, then away in four directions.
	const float aims[3] = { 0.0f, -1.5708f, 3.1416f };
	for ( int round = 0; round < 30; ++round )
	{
		f.inputs[0] = {};
		f.inputs[0].cameraYaw = detmath::RadiansToYaw( aims[round % 3] + float( int( next() % 60 ) - 30 ) * 0.01f );
		for ( int i = 0; i < 150; ++i )
		{
			f.inputs[0].moveForward = 127;
			f.inputs[0].moveRight = int8_t( int( next() % 255 ) - 127 );
			step( 1 );
		}
		const b3Vec3 at = sim.EntityTransform( crate )->position;
		// Back, or off to either side, each from the same moment: stuck is when none of them gets
		// away (in a corner one side is the other wall).
		Snapshot pressed;
		sim.Save( pressed );
		const uint32_t tick = f.tick;
		float gone = 0.0f;
		for ( int way = 0; way < 3; ++way )
		{
			sim.Load( pressed );
			f.tick = tick;
			f.inputs[0].moveRight = way == 1 ? 127 : way == 2 ? -127 : 0;
			f.inputs[0].moveForward = way == 0 ? -127 : 0;
			step( 40 );
			gone = std::max( gone, b3Length( b3Sub( sim.EntityTransform( crate )->position, at ) ) );
		}
		++tries;
		if ( gone < 0.5f )
		{
			++stuck;
			std::printf( "    stuck at (%.2f, %.2f) after round %d: moved %.3f m\n", double( at.x ), double( at.z ), round, double( gone ) );
		}
	}
	std::printf( "    %d of %d times it could not leave a wall\n", stuck, tries );
	CHECK( stuck == 0 );
}

void TestMotionFinds()
{
	ModSchema schema;
	schema.events = { "wave.hit", "poke.hit" };
	schema.actions.push_back( { "wave", 0, "Q" } );
	schema.actions.push_back( { "poke", 1, "E" } );
	// A wave: a ball of 4 m around a point a metre up; everything in it (not the player) thrown
	// away along the ground and up, a player found slowed to nothing for a second. A poke: a box
	// from half a metre to 3 m ahead, the nearest player only.
	const std::string text = "cinderbox_motions\t3\n"
							 "motion\tWave\nwhen\tpress\twave\n"
							 "find\tall\t0\t16\tfacing\nshape\t4\t0\t1\t0\n"
							 "impulse\tfound\t9\taway\tnone\nimpulse\tfound\t4\tup\tnone\nparam\twalk_speed\t0\t1\nemit\twave.hit\nendfind\n"
							 "motion\tPoke\nwhen\tpress\tpoke\n"
							 "find\tplayers\t0\t1\tfacing\nshape\t0\t-0.5\t0\t0.5\t0.5\t0\t0.5\t-0.5\t2\t0.5\t0.5\t2\t0.5\t-0.5\t0\t3\t0.5\t0\t3\t-0.5\t2\t3\t0.5\t2\t3\n"
							 "emit\tpoke.hit\nendfind\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 2 );
	if ( motions == nullptr || motions->list.size() != 2 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	const MotionFind& wave = motions->list[0].First().finds.at( 0 );
	CHECK( wave.pieces.size() == 1 && wave.pieces[0].points.size() == 1 && wave.pieces[0].radius == 4.0f && wave.effects.size() == 2 &&
		   wave.effects[0].target.kind == MotionTarget::Kind::Found && wave.effects[0].frame == MotionFrame::Away && wave.moves.size() == 1 &&
		   wave.emits.size() == 1 && wave.self == false && wave.most == 16 );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	std::vector<ModEventRecord> seen;
	uint32_t counted = 0;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
			for ( ; counted < sim.Globals().modEventCount; ++counted )
			{
				seen.push_back( sim.Globals().modEvents[counted % kModEventHistory] );
			}
		}
	};
	auto place = [&]( int slot, b3Vec3 at, uint16_t yaw ) {
		SimCommand c;
		c.type = CommandType::Respawn;
		c.target = sim.PlayerNetId( PlayerSlot( slot ) );
		c.mode = 1;
		c.a = { at.x, at.y, at.z };
		c.index = yaw;
		f.commands.push_back( c );
	};
	auto crate = [&]( b3Vec3 at ) {
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.mode = uint8_t( ShapeKind::Box );
		c.value = -1;
		c.a = { at.x, at.y, at.z };
		c.c = { 0.4f, 0.4f, 0.4f };
		f.commands.push_back( c );
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 2 );
	const uint32_t p0 = sim.PlayerNetId( 0 ), p1 = sim.PlayerNetId( 1 );
	place( 0, { 0.0f, 1.0f, 0.0f }, 0 );
	place( 1, { 0.0f, 1.0f, 2.0f }, 0 );
	crate( { -3.0f, 0.4f, 0.0f } );
	crate( { 10.0f, 0.4f, 0.0f } );
	step( 60 );
	std::vector<uint32_t> crates;
	for ( const auto& r : sim.Entities() )
	{
		flecs::entity e( sim.World(), r.entity );
		if ( e.has<Prop>() )
		{
			crates.push_back( r.netId );
		}
	}
	CHECK( crates.size() == 2 && sim.PlayerCharacter( 1 )->grounded == 1 );
	if ( crates.size() != 2 )
	{
		return;
	}
	auto events = [&]( int type ) {
		std::vector<uint32_t> who;
		for ( const ModEventRecord& e : seen )
		{
			if ( int( e.type ) == type )
			{
				who.push_back( e.netIdB );
			}
		}
		return who;
	};
	auto bodyVelocity = [&]( uint32_t netId ) {
		const PhysicsBody* pb = sim.FindEntity( netId ).try_get<PhysicsBody>();
		return pb != nullptr ? b3Body_GetLinearVelocity( sim.BodyOf( *pb ) ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
	};

	// The wave: the player ahead (2 m) and the crate beside (3 m) are found, nearest first; the far
	// crate and the player itself are not. Each is thrown away along the ground, and up.
	const float walk = sim.PlayerMove( 1 )[MoveParam::WalkSpeed];
	b3Vec3 before = sim.PlayerCharacter( 0 )->velocity;
	f.inputs[0].actions = 1;
	step( 1 );
	f.inputs[0].actions = 0;
	const b3Vec3 v1 = sim.PlayerCharacter( 1 )->velocity;
	const b3Vec3 vCrate = bodyVelocity( crates[0] );
	std::printf( "    the wave: the player ahead at (%.2f, %.2f, %.2f) m/s, the crate beside at (%.2f, %.2f, %.2f)\n", v1.x, v1.y, v1.z,
				 vCrate.x, vCrate.y, vCrate.z );
	CHECK( v1.z > 8.0f && std::fabs( v1.x ) < 0.5f && v1.y > 3.0f );
	CHECK( vCrate.x < -8.0f && vCrate.y > 3.0f && b3Length( bodyVelocity( crates[1] ) ) < 0.01f );
	CHECK( b3Length( b3Sub( sim.PlayerCharacter( 0 )->velocity, before ) ) < 0.01f );
	CHECK( ( events( 0 ) == std::vector<uint32_t>{ p1, crates[0] } ) );
	CHECK( seen.size() == 2 && seen[0].netIdA == p0 && seen[0].tick == sim.Tick() - 1 );
	// The player found walks at nothing for a second (its own movement after the throw), then again.
	CHECK( sim.PlayerMove( 1 )[MoveParam::WalkSpeed] == 0.0f && sim.PlayerMove( 0 )[MoveParam::WalkSpeed] == walk );
	step( 58 );
	CHECK( sim.PlayerMove( 1 )[MoveParam::WalkSpeed] == 0.0f );
	step( 3 );
	CHECK( sim.PlayerMove( 1 )[MoveParam::WalkSpeed] == walk );
	step( 120 );

	// The poke: a box ahead, the nearest player only; facing away, nobody.
	place( 1, { 0.0f, 1.0f, 2.0f }, 0 );
	place( 0, { 0.0f, 1.0f, 0.0f }, 0 );
	step( 60 );
	f.inputs[0].actions = 2;
	step( 1 );
	f.inputs[0].actions = 0;
	CHECK( ( events( 1 ) == std::vector<uint32_t>{ p1 } ) );
	place( 0, { 0.0f, 1.0f, 0.0f }, 32768 );
	step( 30 );
	f.inputs[0].actions = 2;
	step( 1 );
	f.inputs[0].actions = 0;
	CHECK( events( 1 ).size() == 1 );
	step( 30 );

	// It is state: a copy made while a slow holds goes on the same, and a rollback takes the wave back.
	place( 0, { 0.0f, 1.0f, 0.0f }, 0 );
	place( 1, { 0.0f, 1.0f, 2.0f }, 0 );
	step( 60 );
	f.inputs[0].actions = 1;
	step( 1 );
	f.inputs[0].actions = 0;
	step( 10 );
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation client( TestConfig(), FlatMap() );
	client.SetMotions( motions );
	CHECK( client.LoadPortable( image ) && client.ComputeHash() == sim.ComputeHash() && client.PlayerMove( 1 )[MoveParam::WalkSpeed] == 0.0f );
	for ( int i = 0; i < 70; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		client.Step( f );
	}
	CHECK( client.ComputeHash() == sim.ComputeHash() && client.PlayerMove( 1 )[MoveParam::WalkSpeed] == walk );
	place( 1, { 0.0f, 1.0f, 2.0f }, 0 );
	step( 60 );
	Snapshot quiet;
	sim.Save( quiet );
	const uint64_t hashQuiet = sim.ComputeHash();
	f.inputs[0].actions = 1;
	step( 1 );
	f.inputs[0].actions = 0;
	CHECK( sim.PlayerMove( 1 )[MoveParam::WalkSpeed] == 0.0f );
	sim.Load( quiet );
	CHECK( sim.ComputeHash() == hashQuiet && sim.PlayerMove( 1 )[MoveParam::WalkSpeed] == walk );

	// What a file may not say.
	auto fails = [&]( const std::string& body ) {
		std::vector<Motion> out;
		std::string error, warned;
		return CompileMotionSet( "t", "cinderbox_motions\t3\n" + body, schema, out, error, warned ) == false;
	};
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nfind\tall\t0\t16\tfacing\nendfind\n" ) );						// no shape
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nfind\tall\t0\t16\tfacing\nshape\t1\t0\t0\t0\n" ) );			// not ended
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nfind\tall\t0\t16\tfacing\nshape\t1\t0\t0\t0\ncooldown\t1\nendfind\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nfind\tall\t0\t16\tfacing\nshape\t1\t0\t0\t0\nparam\twalk_speed\t0\nendfind\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nimpulse\tfound\t5\tup\tnone\n" ) );							   // found, outside
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nimpulse\tself\t5\taway\tnone\n" ) );							   // away, outside
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nfind\tsome\t0\t16\tfacing\nshape\t1\t0\t0\t0\nendfind\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nfind\tall\t0\t17\tfacing\nshape\t1\t0\t0\t0\nendfind\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nfind\tall\t0\t16\tfacing\nshape\t1\t0\t0\nendfind\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nfind\tall\t0\t16\tfacing\nshape\t1\t0\t0\t0\nendfind\n" ) == false );
}

// A probe with a shape (a sheet's "cast sphere R", or a node's shape): swept along its line instead of
// a ray, so it catches what passes beside the line; turned to the line, so what is level stays level.
void TestMotionCasts()
{
	ModSchema schema;
	schema.events = { "hooked" };
	schema.actions.push_back( { "ray", 0, "Q" } );
	schema.actions.push_back( { "ball", 1, "E" } );
	schema.actions.push_back( { "fly", 2, "R" } );
	schema.actions.push_back( { "blade", 3, "F" } );
	schema.actions.push_back( { "post", 4, "G" } );
	const std::string text = "cinderbox_motions\t3\n"
							 "motion\tRay\nwhen\tpress\tray\nprobe\t10\t0\nhold_emit\thooked\nimpulse\thit\t5\tlook\tnone\n"
							 "motion\tBall\nwhen\tpress\tball\nprobe\t10\t0\nprobe_shape\t0.7\t0\t0\t0\nhold_emit\thooked\nimpulse\thit\t5\tlook\tnone\n"
							 "motion\tFly\nwhen\tpress\tfly\nprobe\t10\t40\nprobe_shape\t0.7\t0\t0\t0\nhold_emit\thooked\n"
							 // A blade: 2 m wide, flat (5 cm), along the line.
							 "motion\tBlade\nwhen\tpress\tblade\nprobe\t10\t0\nprobe_shape\t0\t-1\t-0.025\t-0.025\t1\t-0.025\t-0.025\t-1\t0.025\t-0.025\t1\t0.025\t-0.025"
							 "\t-1\t-0.025\t0.025\t1\t-0.025\t0.025\t-1\t0.025\t0.025\t1\t0.025\t0.025\nhold_emit\thooked\n"
							 // The same blade stood on its end: 2 m tall, 5 cm wide.
							 "motion\tPost\nwhen\tpress\tpost\nprobe\t10\t0\nprobe_shape\t0\t-0.025\t-1\t-0.025\t0.025\t-1\t-0.025\t-0.025\t1\t-0.025\t0.025\t1\t-0.025"
							 "\t-0.025\t-1\t0.025\t0.025\t-1\t0.025\t-0.025\t1\t0.025\t0.025\t1\t0.025\nhold_emit\thooked\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 5 );
	if ( motions == nullptr || motions->list.size() != 5 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	CHECK( motions->list[0].probePoints.empty() && motions->list[1].probePoints.size() == 1 && motions->list[1].probeRadius == 0.7f &&
		   motions->list[3].probePoints.size() == 8 );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	std::vector<ModEventRecord> seen;
	uint32_t counted = 0;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
			for ( ; counted < sim.Globals().modEventCount; ++counted )
			{
				seen.push_back( sim.Globals().modEvents[counted % kModEventHistory] );
			}
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 2 );
	const uint32_t p0 = sim.PlayerNetId( 0 );
	auto place = [&]( b3Vec3 at, uint16_t yaw ) {
		SimCommand c;
		c.type = CommandType::Respawn;
		c.target = p0;
		c.mode = 1;
		c.a = { at.x, at.y, at.z };
		c.index = yaw;
		f.commands.push_back( c );
		f.inputs[0].cameraYaw = yaw;
		f.inputs[0].cameraPitch = 0;
	};
	// A post 3 m tall standing beside the line, 5 m ahead: its near side 0.5 m from it.
	auto post = [&]( b3Vec3 at ) {
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.mode = uint8_t( ShapeKind::Box );
		c.value = -1;
		c.a = { at.x, at.y, at.z };
		c.c = { 0.4f, 1.5f, 0.4f };
		f.commands.push_back( c );
	};
	auto hooked = [&] { return int( seen.size() ); };
	auto press = [&]( uint16_t action ) {
		f.inputs[0].actions = action;
		step( 1 );
		f.inputs[0].actions = 0;
		step( 1 );
	};
	place( { 0.0f, 1.0f, 0.0f }, 0 );
	post( { 0.9f, 1.5f, 5.0f } );
	step( 30 );
	uint32_t postId = 0;
	for ( const auto& r : sim.Entities() )
	{
		postId = flecs::entity( sim.World(), r.entity ).has<Prop>() ? r.netId : postId;
	}
	CHECK( postId != 0 );

	// The line passes beside it and finds nothing: the motion does not happen.
	press( 1 );
	CHECK( hooked() == 0 );
	step( 10 );
	// A ball 0.7 m round swept along the same line catches it, and the impulse moves it.
	press( 2 );
	CHECK( hooked() == 1 && seen[0].netIdB == postId );
	const PhysicsBody* body = sim.FindEntity( postId ).try_get<PhysicsBody>();
	const b3Vec3 moving = body != nullptr ? b3Body_GetLinearVelocity( sim.BodyOf( *body ) ) : b3Vec3{};
	std::printf( "    the ball caught the post 0.5 m beside the line; it moves at (%.2f, %.2f, %.2f) m/s\n", moving.x, moving.y, moving.z );
	CHECK( moving.z > 1.0f );
	step( 120 );

	// Thrown to fly, the same ball catches it on its way (5 m at 40 m/s: within ten ticks).
	place( { 0.0f, 1.0f, 0.0f }, 0 );
	SimCommand gone;
	gone.type = CommandType::Destroy;
	gone.target = postId;
	f.commands.push_back( gone );
	post( { 0.9f, 1.5f, 5.0f } );
	step( 30 );
	for ( const auto& r : sim.Entities() )
	{
		postId = flecs::entity( sim.World(), r.entity ).has<Prop>() ? r.netId : postId;
	}
	f.inputs[0].actions = 4;
	step( 1 );
	f.inputs[0].actions = 0;
	CHECK( hooked() == 1 );
	step( 10 );
	CHECK( hooked() == 2 && seen[1].netIdB == postId );
	step( 150 );

	// Turned to a line along +X: a flat blade 2 m wide still lies flat, so it catches a post 0.5 m
	// beside the line; the same blade on its end, 5 cm wide, does not.
	place( { 0.0f, 1.0f, 0.0f }, 16384 );
	f.commands.push_back( gone );
	gone.target = postId;
	f.commands.back().target = postId;
	post( { 5.0f, 1.5f, 0.9f } );
	step( 30 );
	for ( const auto& r : sim.Entities() )
	{
		postId = flecs::entity( sim.World(), r.entity ).has<Prop>() ? r.netId : postId;
	}
	press( 16 );
	CHECK( hooked() == 2 );
	step( 10 );
	press( 8 );
	CHECK( hooked() == 3 && seen[2].netIdB == postId );

	// What a file may not say.
	auto fails = [&]( const std::string& body ) {
		std::vector<Motion> out;
		std::string error, warned;
		return CompileMotionSet( "t", "cinderbox_motions\t3\n" + body, schema, out, error, warned ) == false;
	};
	CHECK( fails( "motion\tA\nwhen\tpress\tray\nprobe_shape\t0.5\t0\t0\t0\n" ) ); // no probe
	CHECK( fails( "motion\tA\nwhen\tpress\tray\nprobe\t10\t0\nprobe_shape\t0.5\t0\t0\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\tray\nprobe\t10\t0\nprobe_shape\t50\t0\t0\t0\n" ) );
}

// Numbers that are expressions (docs/rules-language.md): a strength read when it acts, from a field,
// from a curve over how far out a find's target is ("reach"), and "time" since a step started.
void TestMotionCurves()
{
	ModSchema schema;
	schema.fields.push_back( { "t.power", BoardType::Float, BoardScope::Entity, 0 } );
	schema.events = { "later.a", "later.b" };
	schema.actions.push_back( { "wave", 0, "Q" } );
	schema.actions.push_back( { "jump2", 1, "E" } );
	schema.actions.push_back( { "lift", 2, "R" } );
	schema.actions.push_back( { "later", 3, "F" } );
	const std::string text = "cinderbox_motions\t3\n"
							 // A wave 6 m round: what it finds is thrown at 10 m/s at the player, nothing at its edge.
							 "motion\tWave\nwhen\tpress\twave\ncurve\tFall\t0\t1\t1\t0\n"
							 "find\tbodies\t0\t16\tfacing\nshape\t6\t0\t1\t0\nimpulse\tfound\t10 * curve(Fall, reach)\taway\tnone\nendfind\n"
							 "motion\tJump2\nwhen\tpress\tjump2\nimpulse\tself\t2 * t.power\tup\tvertical\n"
							 "motion\tLift\nwhen\tpress\tlift\nduration\t0.5\nforce\tself\tup\taccel\tt.power * 10\t0\t0\t0\n"
							 "motion\tLater\nwhen\tpress\tlater\nemit\tlater.a\nstep\t0\t0\ttime >= 0.5\nemit\tlater.b\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 4 );
	if ( motions == nullptr || motions->list.size() != 4 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	const Motion& wave = motions->list[0];
	CHECK( wave.curves.size() == 1 && wave.curves[0].samples.size() == 2 && wave.exprs.size() == 1 && wave.First().finds[0].effects[0].strengthExpr == 0 &&
		   wave.First().finds[0].extent == 6.0f );
	CHECK( SampleAnimCurve( wave.curves[0], 0.25f ) == 0.75f && SampleAnimCurve( wave.curves[0], -1.0f ) == 1.0f && SampleAnimCurve( wave.curves[0], 2.0f ) == 0.0f );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	std::vector<ModEventRecord> seen;
	uint32_t counted = 0;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
			for ( ; counted < sim.Globals().modEventCount; ++counted )
			{
				seen.push_back( sim.Globals().modEvents[counted % kModEventHistory] );
			}
		}
	};
	auto press = [&]( uint16_t action ) {
		f.inputs[0].actions = action;
		step( 1 );
		f.inputs[0].actions = 0;
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 2 );
	const uint32_t p0 = sim.PlayerNetId( 0 );
	SimCommand at;
	at.type = CommandType::Respawn;
	at.target = p0;
	at.mode = 1;
	at.a = { 0.0f, 1.0f, 0.0f };
	f.commands.push_back( at );
	for ( float x : { 1.5f, 4.5f } )
	{
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.mode = uint8_t( ShapeKind::Box );
		c.value = -1;
		c.a = { x, 0.4f, 0.0f };
		c.c = { 0.3f, 0.3f, 0.3f };
		f.commands.push_back( c );
	}
	step( 60 );
	std::vector<uint32_t> crates;
	for ( const auto& r : sim.Entities() )
	{
		if ( flecs::entity( sim.World(), r.entity ).has<Prop>() )
		{
			crates.push_back( r.netId );
		}
	}
	CHECK( crates.size() == 2 );
	if ( crates.size() != 2 )
	{
		return;
	}
	auto velocity = [&]( uint32_t netId ) {
		const PhysicsBody* pb = sim.FindEntity( netId ).try_get<PhysicsBody>();
		return pb != nullptr ? b3Body_GetLinearVelocity( sim.BodyOf( *pb ) ) : b3Vec3{};
	};

	// The near crate (a quarter of the way out) is thrown three times as hard as the far one (three
	// quarters out): 7.5 and 2.5 m/s.
	press( 1 );
	const float near = velocity( crates[0] ).x, far = velocity( crates[1] ).x;
	std::printf( "    a wave's falloff: the crate 1.5 m out at %.2f m/s, the one 4.5 m out at %.2f m/s\n", near, far );
	CHECK( std::fabs( near - 7.5f ) < 0.5f && std::fabs( far - 2.5f ) < 0.5f );
	step( 120 );

	// A strength from a field: twice t.power (3), up.
	SimCommand set;
	set.type = CommandType::SetField;
	set.target = SlotTarget( 0 );
	set.index = 0;
	set.value = BoardFromFloat( 3.0f );
	f.commands.push_back( set );
	step( 1 );
	press( 2 );
	std::printf( "    an impulse of 2 * t.power: %.2f m/s up\n", sim.PlayerCharacter( 0 )->velocity.y );
	CHECK( sim.PlayerCharacter( 0 )->velocity.y > 5.5f && sim.PlayerCharacter( 0 )->velocity.y <= 6.0f );
	step( 120 );

	// A force of t.power * 10 (30 m/s², above gravity) lifts the player off; at 1 it would not.
	press( 4 );
	step( 20 );
	CHECK( sim.PlayerCharacter( 0 )->grounded == 0 );
	step( 120 );
	set.value = BoardFromFloat( 1.0f );
	f.commands.push_back( set );
	step( 1 );
	press( 4 );
	step( 20 );
	CHECK( sim.PlayerCharacter( 0 )->grounded == 1 );
	step( 60 );

	// "time" in a wait: half a second after the step started.
	press( 8 );
	step( 40 );
	uint32_t a = 0, b = 0;
	for ( const ModEventRecord& e : seen )
	{
		a = int( e.type ) == 0 ? e.tick : a;
		b = int( e.type ) == 1 ? e.tick : b;
	}
	CHECK( a != 0 && b == a + 30 );

	// What a file may not say.
	auto fails = [&]( const std::string& body ) {
		std::vector<Motion> out;
		std::string error, warned;
		return CompileMotionSet( "t", "cinderbox_motions\t3\n" + body, schema, out, error, warned ) == false;
	};
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nimpulse\tself\tcurve(Nope, 1)\tup\tnone\n" ) ); // no such curve
	CHECK( fails( "motion\tA\nwhen\tpress\twave\ncurve\tC\t1\t1\t0\t1\n" ) );						// its end not past its start
	CHECK( fails( "motion\tA\nwhen\tpress\twave\ncurve\tC\t0\t1\t0\n" ) );							// one value
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nimpulse\tself\t500\tup\tnone\n" ) );				// a number past the limit
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nimpulse\tself\t2 *\tup\tnone\n" ) );				// not an expression
	CHECK( fails( "motion\tA\nwhen\tpress\twave\ncurve\tC\t0\t1\t0\t1\ncurve\tC\t0\t1\t0\t1\n" ) );
	CHECK( fails( "motion\tA\nwhen\tpress\twave\nimpulse\tself\tcurve(C, 1)\tup\tnone\ncurve\tC\t0\t1\t0\t1\n" ) ); // named before it is
}

// Directions and places drawn in the scene (docs/rules-language.md): a marker's forward, turned with
// the look; a direction turned by degrees; a launch from a marker's place; a leap along a path.
void TestMotionMarkers()
{
	ModSchema schema;
	schema.itemKinds = { "t.ball" };
	schema.fields.push_back( { "t.power", BoardType::Float, BoardScope::Entity, 0 } );
	schema.actions.push_back( { "mark", 0, "Q" } );
	schema.actions.push_back( { "right", 1, "E" } );
	schema.actions.push_back( { "up", 2, "R" } );
	schema.actions.push_back( { "throw", 3, "F" } );
	schema.actions.push_back( { "leap", 4, "G" } );
	const std::string text = "cinderbox_motions\t3\n"
							 // A marker drawn ahead and up (0.6 up, 0.8 ahead).
							 "motion\tMark\nwhen\tpress\tmark\nimpulse\tself\t10\tmarker\tall\t0\t0.6\t0.8\n"
							 "motion\tRight\nwhen\tpress\tright\nimpulse\tself\t10\tlook\tall\nturn\t90\t0\n"
							 "motion\tUp\nwhen\tpress\tup\nimpulse\tself\t10\tlook\tall\nturn\t0\t30\n"
							 // From a marker 1 m to the left, 1.5 m up, half a metre ahead; at twice t.power.
							 "motion\tThrow\nwhen\tpress\tthrow\nlaunch\tt.ball\t2 * t.power\tlook\t0\t0.7\t2\nfrom\t1\t1.5\t0.5\n"
							 // An arc: a metre up and 4 m ahead, in a second.
							 "motion\tLeap\nwhen\tpress\tleap\nduration\t1\npath\tArc\t0\t0\t0\t0\t0.75\t1\t0\t1\t2\t0\t0.75\t3\t0\t0\t4\n"
							 "param\tgravity\t0\nfollow\tArc\t1\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 5 );
	if ( motions == nullptr || motions->list.size() != 5 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	CHECK( motions->list[0].First().effects[0].frame == MotionFrame::Marker && motions->list[1].First().effects[0].turned &&
		   motions->list[3].First().effects[0].fromMarker && motions->list[3].First().effects[0].strengthExpr == 0 &&
		   motions->list[4].paths.size() == 1 && motions->list[4].First().effects[0].kind == MotionEffect::Kind::Follow );

	SimConfig config = TestConfig();
	config.slots = 2;
	ItemShape ball;
	ball.kind = 1;
	ball.half = { 0.1f, 0.1f, 0.1f };
	ball.center = { 0.0f, 0.0f, 0.0f };
	Simulation sim( config, FlatMap() );
	sim.SetItemShapes( { ball } );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 2 );
	const uint32_t p0 = sim.PlayerNetId( 0 );
	auto place = [&]( uint16_t yaw ) {
		SimCommand c;
		c.type = CommandType::Respawn;
		c.target = p0;
		c.mode = 1;
		c.a = { 0.0f, 1.0f, 0.0f };
		c.index = yaw;
		f.commands.push_back( c );
		f.inputs[0].cameraYaw = yaw;
		f.inputs[0].cameraPitch = 0;
		step( 60 );
	};
	auto press = [&]( uint16_t action ) {
		f.inputs[0].actions = action;
		step( 1 );
		f.inputs[0].actions = 0;
		return sim.PlayerCharacter( 0 )->velocity;
	};
	auto near = []( b3Vec3 a, b3Vec3 b, float within ) { return b3Distance( a, b ) < within; };

	// A marker's direction, as drawn: ahead and up; looking along +X, it turns with the look.
	place( 0 );
	b3Vec3 v = press( 1 );
	std::printf( "    a marker ahead and up: (%.2f, %.2f, %.2f) m/s\n", v.x, v.y, v.z );
	CHECK( near( v, { 0.0f, 6.0f, 8.0f }, 0.6f ) );
	place( 16384 );
	v = press( 1 );
	CHECK( near( v, { 8.0f, 6.0f, 0.0f }, 0.6f ) );
	// Turned 90 degrees right of the look (+Z): right is -X. Turned 30 degrees up.
	place( 0 );
	v = press( 2 );
	std::printf( "    the look turned 90 degrees right: (%.2f, %.2f, %.2f) m/s\n", v.x, v.y, v.z );
	CHECK( v.x < -8.5f && std::fabs( v.z ) < 0.3f ); // (a tick of ground friction on the way)
	place( 0 );
	v = press( 4 );
	std::printf( "    the look turned 30 degrees up: (%.2f, %.2f, %.2f) m/s\n", v.x, v.y, v.z );
	CHECK( near( v, { 0.0f, 5.0f, 8.66f }, 0.6f ) );

	// A launch from a marker's place, at twice t.power (5): 10 m/s along the look.
	place( 0 );
	SimCommand set;
	set.type = CommandType::SetField;
	set.target = SlotTarget( 0 );
	set.index = 0;
	set.value = BoardFromFloat( 5.0f );
	f.commands.push_back( set );
	step( 1 );
	const b3Vec3 feet = sim.EntityTransform( p0 )->position;
	press( 8 );
	uint32_t thrown = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
		thrown = item != nullptr && item->holder == 0 ? r.netId : thrown;
	}
	CHECK( thrown != 0 );
	if ( thrown != 0 )
	{
		const b3Vec3 at = sim.EntityTransform( thrown )->position;
		const PhysicsBody* pb = sim.FindEntity( thrown ).try_get<PhysicsBody>();
		const b3Vec3 speed = pb != nullptr ? b3Body_GetLinearVelocity( sim.BodyOf( *pb ) ) : b3Vec3{};
		std::printf( "    thrown from the marker: at (%.2f, %.2f, %.2f) from the feet, at (%.2f, %.2f, %.2f) m/s\n", at.x - feet.x, at.y - feet.y,
					 at.z - feet.z, speed.x, speed.y, speed.z );
		CHECK( near( b3Sub( at, feet ), { 1.0f, 1.5f, 0.5f + 10.0f / 60.0f }, 0.15f ) && std::fabs( speed.z - 10.0f ) < 0.3f );
	}

	// A leap along a path: a metre up halfway, 4 m ahead at its end, a second.
	place( 0 );
	const b3Vec3 start = sim.EntityTransform( p0 )->position;
	press( 16 );
	step( 29 );
	const b3Vec3 half = sim.EntityTransform( p0 )->position;
	step( 30 );
	const b3Vec3 end = sim.EntityTransform( p0 )->position;
	std::printf( "    the leap: halfway %.2f m up and %.2f ahead, at its end %.2f ahead\n", half.y - start.y, half.z - start.z, end.z - start.z );
	CHECK( std::fabs( half.y - start.y - 1.0f ) < 0.15f && std::fabs( half.z - start.z - 2.0f ) < 0.15f && std::fabs( end.z - start.z - 4.0f ) < 0.2f );

	// What a file may not say.
	auto fails = [&]( const std::string& body ) {
		std::vector<Motion> out;
		std::string error, warned;
		return CompileMotionSet( "t", "cinderbox_motions\t3\n" + body, schema, out, error, warned ) == false;
	};
	CHECK( fails( "motion\tA\nwhen\tpress\tmark\nturn\t90\t0\n" ) );												 // no effect to turn
	CHECK( fails( "motion\tA\nwhen\tpress\tmark\nimpulse\tself\t1\tup\tnone\nfrom\t0\t1\t0\n" ) );					 // from, not a launch
	CHECK( fails( "motion\tA\nwhen\tpress\tmark\nfollow\tNope\t1\n" ) );												 // no such path
	CHECK( fails( "motion\tA\nwhen\tpress\tmark\npath\tP\t0\t0\t0\n" ) );												 // one point
	CHECK( fails( "motion\tA\nwhen\tpress\tmark\nimpulse\tself\t1\tmarker\tnone\n" ) );								 // no direction
	CHECK( fails( "motion\tA\nwhen\tpress\tmark\nimpulse\tself\t1\tup\tnone\nturn\t0\t100\n" ) );						 // past straight up
}

// An item's own motions (a sheet's "owner item", docs/rules-language.md): a bomb whose fuse runs out
// throws what is near it, harder the nearer, said as its thrower's; one a mod pokes removes itself.
// Run by the simulation where the item is, so every machine has the blast on the same tick.
void TestItemBlast()
{
	ModSchema schema;
	schema.itemKinds = { "t.bomb" };
	schema.events = { "t.bomb.expired", "t.boom", "t.hit", "t.poke" };
	schema.actions.push_back( { "throw", 0, "Q" } );
	schema.fields.push_back( { "t.push", BoardType::Float, BoardScope::Global, 0 } );
	// The bomb's own set (named by its kind): a blast 4 m round, its push a field the mod sets.
	schema.motionSets.push_back( { "t", "t.bomb",
								   "cinderbox_motions\t3\n"
								   "motion\tBlast\nwhen\tevent\tt.bomb.expired\nowner\titem\nemit\tt.boom\n"
								   "find\tall\t0\t16\tfacing\nshape\t4\t0\t0\t0\n"
								   "impulse\tfound\t(distance < 4) * t.push * (1 - distance / 4)\taway\tnone\nemit\tt.hit\nendfind\n"
								   "motion\tPoke\nwhen\tevent\tt.poke\nowner\titem\nremove\n" } );
	// The player's: the bomb dropped where it stands, half a second to live.
	schema.motionSets.push_back( { "t", "t.moves", "cinderbox_motions\t3\nmotion\tThrow\nwhen\tpress\tthrow\nlaunch\tt.bomb\t0\tfacing\t0\t0.7\t0.5\n" } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 3 );
	if ( motions == nullptr || motions->list.size() != 3 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	CHECK( motions->list[0].ownerItem && motions->list[0].itemKind == 0 && motions->list[1].First().removes.size() == 1 &&
		   motions->list[2].ownerItem == false );

	SimConfig config = TestConfig();
	config.slots = 2;
	config.globalFields = 1;
	ItemShape bomb;
	bomb.kind = 1;
	bomb.half = { 0.1f, 0.1f, 0.1f };
	bomb.center = { 0.0f, 0.0f, 0.0f };
	bomb.expiredEvent = 0; // ("t.bomb.expired")
	Simulation sim( config, FlatMap() );
	sim.SetItemShapes( { bomb } );
	sim.SetMotions( motions );
	InputFrame f;
	std::vector<ModEventRecord> seen;
	uint32_t counted = 0;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
			for ( ; counted < sim.Globals().modEventCount; ++counted )
			{
				seen.push_back( sim.Globals().modEvents[counted % kModEventHistory] );
			}
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 2 );
	const uint32_t p0 = sim.PlayerNetId( 0 );
	SimCommand at;
	at.type = CommandType::Respawn;
	at.target = p0;
	at.mode = 1;
	at.a = { 0.0f, 1.0f, 0.0f };
	f.commands.push_back( at );
	SimCommand set;
	set.type = CommandType::SetField;
	set.target = 0;
	set.index = 0;
	set.value = BoardFromFloat( 12.0f );
	f.commands.push_back( set );
	// A crate 2.5 m ahead of where the bomb lands, one 6 m off.
	for ( float z : { 3.2f, 6.7f } )
	{
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.mode = uint8_t( ShapeKind::Box );
		c.value = -1;
		c.a = { 0.0f, 0.3f, z };
		c.c = { 0.3f, 0.3f, 0.3f };
		f.commands.push_back( c );
	}
	step( 60 );
	std::vector<uint32_t> crates;
	for ( const auto& r : sim.Entities() )
	{
		if ( flecs::entity( sim.World(), r.entity ).has<Prop>() )
		{
			crates.push_back( r.netId );
		}
	}
	CHECK( crates.size() == 2 );
	if ( crates.size() != 2 )
	{
		return;
	}
	auto velocity = [&]( uint32_t netId ) {
		const PhysicsBody* pb = sim.FindEntity( netId ).try_get<PhysicsBody>();
		return pb != nullptr ? b3Body_GetLinearVelocity( sim.BodyOf( *pb ) ) : b3Vec3{};
	};

	// Dropped, it lies there; when its time is up its blast throws the near crate away (+Z) and the
	// player back (-Z), not the far crate; said as the player's, with what each was.
	f.inputs[0].actions = 1;
	step( 1 );
	f.inputs[0].actions = 0;
	uint32_t thrown = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
		thrown = item != nullptr && item->holder == 0 ? r.netId : thrown;
	}
	CHECK( thrown != 0 );
	step( 28 );
	CHECK( seen.empty() && b3Length( velocity( crates[0] ) ) < 0.01f );
	step( 2 );
	const b3Vec3 crate = velocity( crates[0] ), player = sim.PlayerCharacter( 0 )->velocity;
	std::printf( "    the blast: the crate ahead at (%.2f, %.2f, %.2f) m/s, the player at (%.2f, %.2f, %.2f) m/s, the far crate %.2f\n", crate.x,
				 crate.y, crate.z, player.x, player.y, player.z, b3Length( velocity( crates[1] ) ) );
	CHECK( crate.z > 3.0f && crate.z < 9.0f && player.z < -3.0f && b3Length( velocity( crates[1] ) ) < 0.01f );
	CHECK( sim.FindEntity( thrown ).is_valid() == false );
	int booms = 0, hits = 0;
	bool hitCrate = false, hitPlayer = false;
	for ( const ModEventRecord& e : seen )
	{
		booms += e.type == 1 && e.netIdA == p0 && e.netIdB == thrown ? 1 : 0;
		hits += e.type == 2 && e.netIdA == p0 ? 1 : 0;
		hitCrate = hitCrate || ( e.type == 2 && e.netIdB == crates[0] );
		hitPlayer = hitPlayer || ( e.type == 2 && e.netIdB == p0 );
	}
	CHECK( booms == 1 && hits == 2 && hitCrate && hitPlayer );

	// A bomb lying in the world that a mod pokes (an event said at it) removes itself, that tick.
	SimCommand lay;
	lay.type = CommandType::SpawnItem;
	lay.target = 0;
	lay.index = 0;
	lay.a = { 5.0f, 0.5f, 0.0f };
	lay.c = { 0.0f, 0.0f, 0.0f };
	f.commands.push_back( lay );
	step( 5 );
	uint32_t lying = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
		lying = item != nullptr && item->holder == 0 ? r.netId : lying;
	}
	CHECK( lying != 0 );
	SimCommand poke;
	poke.type = CommandType::Event;
	poke.index = 3;
	poke.target = lying;
	f.commands.push_back( poke );
	step( 1 );
	CHECK( lying != 0 && sim.FindEntity( lying ).is_valid() == false );

	// What a file may not say.
	auto fails = [&]( const std::string& body ) {
		std::vector<Motion> out;
		std::string error, warned;
		return CompileMotionSet( "t.bomb", "cinderbox_motions\t3\n" + body, schema, out, error, warned ) == false;
	};
	CHECK( fails( "motion\tA\nwhen\tevent\tt.poke\nowner\titem\nstep\t1\t0\nemit\tt.boom\n" ) );		  // a step
	CHECK( fails( "motion\tA\nwhen\tpress\tthrow\nowner\titem\n" ) );									  // a press
	CHECK( fails( "motion\tA\nwhen\tevent\tt.poke\nowner\titem\nimpulse\tself\t3\tlook\tnone\n" ) );	  // a look
	CHECK( fails( "motion\tA\nwhen\tevent\tt.poke\nowner\titem\nparam\twalk_speed\t1\n" ) );			  // a move
	CHECK( fails( "motion\tA\nwhen\tpress\tthrow\nremove\n" ) );										  // remove self, a player's
	CHECK( fails( "motion\tA\nwhen\tevent\tt.poke\nowner\titem\nbreak\t30\t1\t0\n" ) );				  // a break too wide
	CHECK( fails( "motion\tA\nwhen\tevent\tt.poke\nowner\titem\nbreak\t2\t12\t0\n" ) == false );
}

// An impulse of momentum (heavy things move less) and a step's repeat ("every S": the lines after it,
// again and again while it is the step).
void TestMotionRepeats()
{
	ModSchema schema;
	schema.fields.push_back( { "t.n", BoardType::Int, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "t.m", BoardType::Int, BoardScope::Entity, 1 } );
	schema.fields.push_back( { "t.k", BoardType::Int, BoardScope::Entity, 2 } );
	schema.events = { "p.start", "p.tick" };
	schema.actions.push_back( { "shove", 0, "Q" } );
	schema.actions.push_back( { "hop", 1, "E" } );
	schema.actions.push_back( { "pulse", 2, "R" } );
	schema.actions.push_back( { "burn", 3, "F" } );
	schema.actions.push_back( { "rapid", 4, "G" } );
	const std::string text = "cinderbox_motions\t3\n"
							 "motion\tShove\nwhen\tpress\tshove\nfind\tbodies\t0\t16\tfacing\nshape\t4\t0\t1\t0\nimpulse_momentum\tfound\t40\taway\tnone\nendfind\n"
							 "motion\tHop\nwhen\tpress\thop\nimpulse_momentum\tself\t400\tup\tvertical\n"
							 "motion\tPulse\nwhen\tpress\tpulse\nduration\t1\nemit\tp.start\nevery\t0.25\nemit\tp.tick\nchange\tt.n\t+=\t1\n"
							 "motion\tBurn\nwhen\twhile\nif\theld.burn\nevery\t0.5\nchange\tt.m\t+=\t1\n"
							 "motion\tRapid\nwhen\tpress\trapid\nduration\t3\nevery\t0.0167\nchange\tt.k\t+=\t1\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 5 );
	if ( motions == nullptr || motions->list.size() != 5 || warnings.empty() == false )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	const MotionStep& pulse = motions->list[2].First();
	CHECK( motions->list[1].First().effects[0].momentum && pulse.every == 0.25f && pulse.repeats.size() == 1 &&
		   pulse.emits.size() == 1 && pulse.repeats[0].emits.size() == 1 && pulse.repeats[0].changes.size() == 1 );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	std::vector<ModEventRecord> seen;
	uint32_t counted = 0;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
			for ( ; counted < sim.Globals().modEventCount; ++counted )
			{
				seen.push_back( sim.Globals().modEvents[counted % kModEventHistory] );
			}
		}
	};
	auto press = [&]( uint16_t action ) {
		f.inputs[0].actions = action;
		step( 1 );
		f.inputs[0].actions = 0;
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 2 );
	const uint32_t p0 = sim.PlayerNetId( 0 );
	SimCommand at;
	at.type = CommandType::Respawn;
	at.target = p0;
	at.mode = 1;
	at.a = { 0.0f, 1.0f, 0.0f };
	f.commands.push_back( at );
	// A small crate and a big one, either side of the player.
	for ( float x : { 2.0f, -2.0f } )
	{
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.mode = uint8_t( ShapeKind::Box );
		c.value = -1;
		c.a = { x, 0.6f, 0.0f };
		c.c = x > 0.0f ? Float3{ 0.2f, 0.2f, 0.2f } : Float3{ 0.5f, 0.5f, 0.5f };
		f.commands.push_back( c );
	}
	step( 60 );
	std::vector<uint32_t> crates;
	for ( const auto& r : sim.Entities() )
	{
		if ( flecs::entity( sim.World(), r.entity ).has<Prop>() )
		{
			crates.push_back( r.netId );
		}
	}
	CHECK( crates.size() == 2 );
	if ( crates.size() != 2 )
	{
		return;
	}
	auto body = [&]( uint32_t netId ) { return sim.BodyOf( sim.FindEntity( netId ).get<PhysicsBody>() ); };

	// The same momentum: the light crate flies off as much faster as it is lighter.
	press( 1 );
	const float light = std::fabs( b3Body_GetLinearVelocity( body( crates[0] ) ).x ), heavy = std::fabs( b3Body_GetLinearVelocity( body( crates[1] ) ).x );
	const float lightMass = b3Body_GetMass( body( crates[0] ) ), heavyMass = b3Body_GetMass( body( crates[1] ) );
	std::printf( "    40 N s: the %.1f kg crate at %.2f m/s, the %.1f kg one at %.2f m/s\n", lightMass, light, heavyMass, heavy );
	CHECK( std::fabs( light - 40.0f / lightMass ) < 0.3f && std::fabs( heavy - 40.0f / heavyMass ) < 0.3f ); // (a tick of ground friction on the way)
	step( 60 );
	// On the player: 400 N s over its mass.
	const float mass = sim.PlayerMove( 0 )[MoveParam::Mass];
	press( 2 );
	std::printf( "    400 N s up on a player of %.0f kg: %.2f m/s\n", mass, sim.PlayerCharacter( 0 )->velocity.y );
	CHECK( std::fabs( sim.PlayerCharacter( 0 )->velocity.y - 400.0f / mass ) < 0.4f );
	step( 120 );

	// Every quarter second for a second: 4 times (0, 15, 30, 45 ticks after the press), its start once.
	const uint32_t pressed = sim.Tick();
	press( 4 );
	step( 70 );
	std::vector<uint32_t> ticks;
	int starts = 0;
	for ( const ModEventRecord& e : seen )
	{
		ticks.push_back( e.type == 1 ? e.tick - pressed : 9999u );
		starts += e.type == 0 ? 1 : 0;
	}
	ticks.erase( std::remove( ticks.begin(), ticks.end(), 9999u ), ticks.end() );
	CHECK( starts == 1 && ( ticks == std::vector<uint32_t>{ 0, 15, 30, 45 } ) && sim.BoardValue( p0, 0 ) == 4 );
	// In a while: every half second it holds (1.2 s held: at 0, 30, 60 ticks).
	f.inputs[0].actions = 8;
	step( 72 );
	f.inputs[0].actions = 0;
	step( 30 );
	CHECK( sim.BoardValue( p0, 1 ) == 3 );
	// Every tick for three seconds stops at 64.
	press( 16 );
	step( 200 );
	std::printf( "    every tick for 3 s: %d times\n", sim.BoardValue( p0, 2 ) );
	CHECK( sim.BoardValue( p0, 2 ) == int32_t( kMotionRepeats ) );

	// What a file may not say.
	auto fails = [&]( const std::string& body ) {
		std::vector<Motion> out;
		std::string error, warned;
		return CompileMotionSet( "t", "cinderbox_motions\t3\n" + body, schema, out, error, warned ) == false;
	};
	CHECK( fails( "motion\tA\nwhen\tpress\tpulse\nevery\t0.5\nevery\t0.5\n" ) );						// twice in a step
	CHECK( fails( "motion\tA\nwhen\tpress\tpulse\nevery\t0.5\nparam\twalk_speed\t1\n" ) );			// a parameter
	CHECK( fails( "motion\tA\nwhen\tpress\tpulse\nevery\t0\n" ) );									// no time
	CHECK( fails( "motion\tA\nwhen\tpress\tpulse\nfind\tall\t0\t16\tfacing\nshape\t1\t0\t0\t0\nevery\t1\nendfind\n" ) ); // in a find
	CHECK( fails( "motion\tA\nwhen\tpress\tpulse\nimpulse_momentum\tself\t200000\tup\tnone\n" ) );	// past its limit
	CHECK( fails( "motion\tA\nwhen\tpress\tpulse\nevery\t1\nemit\tp.tick\nstep\t1\t0\nevery\t1\nemit\tp.tick\n" ) == false ); // one each step
}

// "motion.<name>" in a condition: that motion of the player's is on. A motion's own conditions read
// it as the tick found it; the name is the set's and the node's, or the node's alone when it is
// the only one.
void TestMotionConditions()
{
	ModSchema schema;
	schema.fields.push_back( { "t.short", BoardType::Float, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "t.full", BoardType::Float, BoardScope::Entity, 1 } );
	schema.fields.push_back( { "t.never", BoardType::Float, BoardScope::Entity, 2 } );
	schema.actions.push_back( { "go", 2, "V" } );
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tHold\nwhen\tpress\tgo\nduration\t0.5\nparam\tfriction\t0\n"
							 "motion\tShort\nwhen\twhile\nif\tmotion.Hold\nchange\tt.short\t+=\t60\n"
							 "motion\tFull\nwhen\twhile\nif\tmotion.a.moves.Hold and not motion.Twin\nchange\tt.full\t+=\t60\n"
							 "motion\tTwin\nwhen\twhile\nif\tmotion.Nobody\nchange\tt.never\t+=\t60\n";
	schema.motionSets.push_back( { "a", "a.moves", text } );
	schema.motionSets.push_back( { "b", "b.moves", "cinderbox_motions\t2\nmotion\tTwin\nwhen\twhile\nif\tmotion.b.moves.Twin\n" } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && motions->list.size() == 5 );
	// The names it could not find are said: a node name two sets have, and one nobody has.
	std::printf( "    %s\n", warnings.c_str() );
	CHECK( warnings.find( "'motion.Twin'" ) != std::string::npos && warnings.find( "several" ) != std::string::npos );
	CHECK( warnings.find( "'motion.Nobody'" ) != std::string::npos );
	CHECK( FindMotionByName( schema, "Hold" ) == 0 && FindMotionByName( schema, "a.moves.Hold" ) == 0 );
	CHECK( FindMotionByName( schema, "Twin" ) == -2 && FindMotionByName( schema, "b.moves.Twin" ) == 4 && FindMotionByName( schema, "Nobody" ) == -1 );

	// The same name in a state machine's condition.
	AnimExpr expr;
	std::string error, exprWarnings;
	CHECK( CompileAnimExpr( "motion.Hold and not motion.b.moves.Twin", schema, expr, error, exprWarnings ) && exprWarnings.empty() );
	AnimGraphInputs in;
	CHECK( EvaluateAnimExpr( expr, in, 0.0f ) == 0.0f );
	in.motionsOn = 1;
	CHECK( EvaluateAnimExpr( expr, in, 0.0f ) != 0.0f );
	in.motionsOn = 1 | ( uint64_t( 1 ) << 4 );
	CHECK( EvaluateAnimExpr( expr, in, 0.0f ) == 0.0f );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	auto value = [&]( int slot ) { return BoardToFloat( sim.BoardValue( p0, slot ) ); };
	CHECK( value( 0 ) == 0.0f && value( 1 ) == 0.0f && sim.EntityMotionsOn( p0 ) == 0 );

	// A press: Hold is on for half a second, and the two that ask for it run that long (a tick
	// behind it: they read it as the tick found it).
	f.inputs[0].actions = 4;
	step( 1 );
	f.inputs[0].actions = 0;
	CHECK( ( sim.EntityMotionsOn( p0 ) & 1 ) != 0 );
	step( 10 );
	CHECK( ( sim.EntityMotionsOn( p0 ) & 1 ) != 0 && value( 0 ) > 5.0f && value( 1 ) == value( 0 ) );
	step( 60 );
	float after = value( 0 );
	step( 30 );
	std::printf( "    while Hold was on: %.1f and %.1f; a motion nobody has: %.1f\n", value( 0 ), value( 1 ), value( 2 ) );
	CHECK( ( sim.EntityMotionsOn( p0 ) & 1 ) == 0 && value( 0 ) == after && after > 25.0f && after < 35.0f );
	CHECK( value( 2 ) == 0.0f );
}

void TestMotionsWhile()
{
	ModSchema schema;
	schema.fields.push_back( { "flight.on", BoardType::Bool, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "flight.fuel", BoardType::Float, BoardScope::Entity, 1 } );
	schema.fields.push_back( { "flight.count", BoardType::Int, BoardScope::Entity, 2 } );
	schema.events = { "flight.started", "flight.stopped", "flight.thrust", "stun.hit" };
	schema.actions.push_back( { "fly", 2, "T" } );
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tFlyOn\nwhen\tpress\tfly\nif\tnot flight.on\nchange\tflight.on\t=\t1\nemit\tflight.started\n"
							 "motion\tFlyOff\nwhen\tpress\tfly\nif\tflight.on\nchange\tflight.on\t=\t0\nemit\tflight.stopped\n"
							 "motion\tFlying\nwhen\twhile\nif\tflight.on\n"
							 "param\twalk_speed\t7\nparam\tair_control\t1\nparam\tgravity\t0\nparam\tair_friction\t3\nparam\tmove_frame\t1\n"
							 "motion\tThrust\nwhen\twhile\nif\theld.jump and not grounded and not flight.on and flight.fuel > 0\n"
							 "force\tself\tup\taccel\t32\t0\t0\t0\nchange\tflight.fuel\t-=\t30\nchange\tflight.count\t=\t7\nemit\tflight.thrust\n"
							 "motion\tRefuel\nwhen\twhile\nif\tgrounded and flight.fuel < 100\nchange\tflight.fuel\t+=\t40\n"
							 "motion\tGlide\nwhen\twhile\nif\theld.sprint and not grounded and vertical_speed < 0 and not flight.on\n"
							 "param\tgravity\t3\nparam\tmax_fall\t2.5\n"
							 "motion\tStun\nwhen\tevent\tstun.hit\nduration\t0.5\nparam\twalk_speed\t0\nparam\tsprint_speed\t0\n";
	schema.motionSets.push_back( { "flight", "flight.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 7 );
	if ( motions == nullptr || motions->list.size() != 7 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	CHECK( motions->list[2].when == Motion::When::While && motions->list[6].when == Motion::When::Event && motions->list[6].trigger == 3 );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	const uint16_t fly = 4;
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	auto height = [&] { return sim.EntityTransform( p0 )->position.y; };
	auto fuel = [&] { return BoardToFloat( sim.BoardValue( p0, 1 ) ); };
	auto tap = [&]( uint16_t action ) {
		f.inputs[0].actions = action;
		step( 1 );
		f.inputs[0].actions = 0;
		step( 1 );
	};
	const float floor = height();

	// Standing, the tank fills to full and then the refuel stops.
	step( 200 );
	std::printf( "    fuel after standing: %.2f\n", fuel() );
	CHECK( fuel() >= 100.0f && fuel() < 100.7f );

	// One press is one switch: both toggles read the field as the tick found it.
	tap( fly );
	CHECK( sim.BoardValue( p0, 0 ) == 1 && sim.Globals().modEventCount == 1 );
	CHECK( sim.PlayerMove( 0 )[MoveParam::Gravity] == 18.0f ); // between ticks nothing is on: a while says so each tick
	// Flying: looking up and walking forward rises, with no gravity pulling back.
	f.inputs[0].cameraPitch = 8192; // 45 degrees up
	f.inputs[0].moveForward = 127;
	step( 120 );
	float climbed = height() - floor;
	std::printf( "    flew %.2f m up in 2 s\n", climbed );
	CHECK( climbed > 6.0f && sim.PlayerCharacter( 0 )->grounded == 0 );
	// Letting go stops: air friction, and still no fall.
	f.inputs[0].moveForward = 0;
	step( 90 );
	float hover = height();
	step( 60 );
	CHECK( b3Length( sim.PlayerCharacter( 0 )->velocity ) < 0.05f && std::fabs( height() - hover ) < 0.05f );
	// Straight down again, and off: it lands and walks.
	f.inputs[0].cameraPitch = 0;
	tap( fly );
	CHECK( sim.BoardValue( p0, 0 ) == 0 && sim.Globals().modEventCount == 2 );
	step( 180 );
	CHECK( sim.PlayerCharacter( 0 )->grounded == 1 && std::fabs( height() - floor ) < 0.05f );
	step( 200 );

	// The jetpack: Space held in the air thrusts upward and burns 30 fuel a second; one event per
	// hold; a field set with = is set when it starts.
	uint32_t eventsBefore = sim.Globals().modEventCount;
	f.inputs[0].buttons = BtnJump;
	step( 61 );
	float used = 100.0f - fuel();
	std::printf( "    a second of thrust: %.2f m up, %.2f fuel\n", height() - floor, used );
	CHECK( height() - floor > 4.0f );
	CHECK( used > 28.0f && used < 31.0f );
	CHECK( sim.Globals().modEventCount == eventsBefore + 1 && sim.BoardValue( p0, 2 ) == 7 );
	// With the tank empty it stops, and the player comes down; on the ground the tank fills again.
	step( 240 );
	CHECK( fuel() <= 0.0f );
	f.inputs[0].buttons = 0;
	step( 1500 ); // it went a long way up
	CHECK( sim.PlayerCharacter( 0 )->grounded == 1 && fuel() >= 100.0f );

	// A glide: Shift held while falling limits the fall.
	{
		SimCommand c;
		c.type = CommandType::Respawn;
		c.mode = 1;
		c.target = SlotTarget( 0 );
		c.a = { 0.0f, 14.0f, 0.0f };
		f.commands.push_back( c );
	}
	f.inputs[0].buttons = BtnSprint;
	step( 1 );
	float fastest = 0.0f;
	for ( int i = 0; i < 90; ++i )
	{
		step( 1 );
		fastest = std::min( fastest, sim.PlayerCharacter( 0 )->velocity.y );
	}
	std::printf( "    gliding: falls at %.2f m/s at most\n", fastest );
	CHECK( fastest >= -2.5f && fastest < -2.4f );
	f.inputs[0].buttons = 0;
	step( 400 );

	// A mod event starts a motion: a stun the server sends holds the player still for half a second.
	b3Vec3 at = sim.EntityTransform( p0 )->position;
	{
		SimCommand c;
		c.type = CommandType::Event;
		c.index = 3;
		c.target = SlotTarget( 0 );
		f.commands.push_back( c );
	}
	f.inputs[0].moveForward = 127;
	step( 28 );
	CHECK( b3Distance( sim.EntityTransform( p0 )->position, at ) < 0.02f );
	step( 60 );
	CHECK( b3Distance( sim.EntityTransform( p0 )->position, at ) > 1.0f );
	f.inputs[0].moveForward = 0;

	// It is state: a joining client stays in step through a flight.
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation client( TestConfig(), FlatMap() );
	client.SetMotions( motions );
	CHECK( client.LoadPortable( image ) && client.ComputeHash() == sim.ComputeHash() );
	f.inputs[0].cameraPitch = 6000;
	f.inputs[0].moveForward = 100;
	for ( int i = 0; i < 120; ++i )
	{
		f.inputs[0].actions = i == 5 ? fly : uint16_t( 0 );
		f.inputs[0].buttons = i > 60 ? uint8_t( BtnJump ) : uint8_t( 0 );
		f.tick = sim.Tick();
		sim.Step( f );
		client.Step( f );
	}
	CHECK( sim.BoardValue( p0, 0 ) == 1 && client.ComputeHash() == sim.ComputeHash() );

	// Per second, a whole number would never move: said, and skipped. An action nobody declares
	// reads as not held.
	std::vector<Motion> out;
	std::string error, warned;
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t2\nmotion\tA\nwhen\twhile\nif\theld.warp\nchange\tflight.count\t+=\t1\n", schema, out, error,
							 warned ) );
	std::printf( "    %s\n", warned.c_str() );
	CHECK( out.size() == 1 && out[0].First().changes.empty() );
	CHECK( warned.find( "per second" ) != std::string::npos && warned.find( "held.warp" ) != std::string::npos );
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t2\nmotion\tA\nwhen\tevent\n", schema, out, error, warned ) == false );

	// A state machine has no input: held.* reads 0 there.
	AnimExpr held;
	CHECK( CompileAnimExpr( "held.jump", schema, held, error, warned ) );
	AnimGraphInputs none;
	CHECK( EvaluateAnimExpr( held, none, 0.0f ) == 0.0f );
	PlayerInput down;
	down.buttons = BtnJump;
	none.input = &down;
	CHECK( EvaluateAnimExpr( held, none, 0.0f ) == 1.0f );
}

// What a probe may hold on to (holds): a hook that holds only the level is stopped by a crate in its
// way and holds nothing; one that holds at once is not thrown at a crate at all; both hold the wall.
// The crate is not moved. A holds line that names nothing does not compile.
void TestProbeHolds()
{
	ModSchema schema;
	schema.actions.push_back( { "hook", 0, "X" } );
	schema.actions.push_back( { "grab", 1, "Z" } );
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tHook\nwhen\tpress\thook\nprobe\t40\t30\nholds\tlevel\nuntil\tnot held.hook\n"
							 "force\tself\tto\tforce\t1920\t0\t0\t1\nlink\thit\t0\t3\n"
							 "motion\tGrab\nwhen\tpress\tgrab\nprobe\t40\t0\nholds\tlevel\nlink\thit\t0\t0\nuntil\tnot held.grab\n";
	schema.motionSets.push_back( { "hooks", "hooks.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 2 && motions->list[0].probeHolds == kProbeHoldsLevel &&
		   motions->list[1].probeHolds == kProbeHoldsLevel );
	if ( motions == nullptr || motions->list.size() != 2 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	{
		ModSchema bad = schema;
		bad.motionSets[0].text = "cinderbox_motions\t2\nmotion\tHook\nwhen\tpress\thook\nprobe\t40\t30\nholds\tclouds\n";
		std::vector<Motion> out;
		std::string error, warned;
		CHECK( CompileMotionSet( "hooks", bad.motionSets[0].text, bad, out, error, warned ) == false && error.find( "holds" ) != std::string::npos );
	}

	Simulation sim( TestConfig() );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	const uint16_t hook = 1, grab = 2;
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	auto holdOf = [&]() -> const MotionHold* { return sim.FindEntity( p0 ).try_get<MotionHold>(); };
	auto position = [&] { return sim.EntityTransform( p0 )->position; };

	// A crate on the floor 6 m ahead, between the player and the wall.
	uint32_t crate = sim.Globals().nextNetId;
	b3Vec3 here = position();
	{
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.value = -1;
		c.a = { here.x, here.y + 0.3f, here.z + 6.0f };
		c.c = { 0.4f, 0.4f, 0.4f };
		f.commands.push_back( c );
	}
	step( 90 );
	b3Vec3 crateAt = sim.EntityTransform( crate )->position;
	b3Vec3 eye = b3Add( position(), b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } );
	f.inputs[0].cameraPitch = int16_t( std::atan2( crateAt.y - eye.y, crateAt.z - eye.z ) * 65536.0f / 6.2831853f );

	// The hook flies, meets the crate and stops there, holding nothing.
	f.inputs[0].actions = hook;
	step( 30 );
	const MotionHold* hold = holdOf();
	CHECK( hold != nullptr && hold->on == 0 && hold->missed == 1 && hold->anchor == 0 && b3Distance( hold->point, crateAt ) < 0.6f );
	f.inputs[0].actions = 0;
	step( 15 );
	// The grab holds at once or not at all: at the crate, not at all.
	f.inputs[0].actions = grab;
	step( 2 );
	hold = holdOf();
	CHECK( hold != nullptr && hold->on == 0 );
	f.inputs[0].actions = 0;
	step( 30 );
	float moved = b3Distance( sim.EntityTransform( crate )->position, crateAt );
	std::printf( "    a hook that holds the level only, at a crate: held nothing; the crate moved %.3f m\n", moved );
	CHECK( moved < 0.01f && b3Distance( position(), here ) < 0.05f );

	// Over the crate, at the wall: it holds there and pulls.
	f.inputs[0].cameraPitch = 500;
	f.inputs[0].actions = hook;
	step( 120 );
	hold = holdOf();
	CHECK( hold != nullptr && hold->on == 1 && hold->anchor == 0 && hold->missed == 0 );
	std::printf( "    at the wall: it holds; the player came %.2f m\n", b3Distance( position(), here ) );
	CHECK( b3Distance( position(), here ) > 1.0f );
}

// A rope and a pull that do not react (link ... 0, force ... react 0): the hook holds a crate, and
// only the player moves, toward it; the crate stays until the player reaches it. A link's react is
// 0 or 1.
void TestRopeAlone()
{
	ModSchema schema;
	schema.actions.push_back( { "hook", 0, "X" } );
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tHook\nwhen\tpress\thook\nprobe\t40\t30\nuntil\tnot held.hook\n"
							 "force\tself\tto\taccel\t40\t12\t0\t0\nlink\thit\t0\t3\t0\nparam\tairborne\t1\nparam\tgravity\t0\n";
	schema.motionSets.push_back( { "hooks", "hooks.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 1 && motions->list[0].First().effects.size() == 2 &&
		   motions->list[0].First().effects[0].react == false && motions->list[0].First().effects[1].react == false );
	if ( motions == nullptr || motions->list.size() != 1 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	{
		std::vector<Motion> out;
		std::string error, warned;
		CHECK( CompileMotionSet( "hooks", "cinderbox_motions\t2\nmotion\tA\nwhen\tpress\thook\nprobe\t40\t0\nlink\thit\t0\t0\t2\n", schema, out, error, warned ) ==
			   false );
		out.clear();
		CHECK( CompileMotionSet( "hooks", "cinderbox_motions\t2\nmotion\tA\nwhen\tpress\thook\nprobe\t40\t0\nlink\thit\t0\t0\n", schema, out, error, warned ) &&
			   out.size() == 1 && out[0].First().effects[0].react );
	}

	Simulation sim( TestConfig() );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	auto position = [&] { return sim.EntityTransform( p0 )->position; };
	uint32_t crate = sim.Globals().nextNetId;
	b3Vec3 here = position();
	{
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.value = -1;
		c.a = { here.x, here.y + 0.3f, here.z + 8.0f };
		c.c = { 0.4f, 0.4f, 0.4f };
		f.commands.push_back( c );
	}
	step( 90 );
	b3Vec3 crateAt = sim.EntityTransform( crate )->position;
	b3Vec3 eye = b3Add( position(), b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } );
	f.inputs[0].cameraPitch = int16_t( std::atan2( crateAt.y - eye.y, crateAt.z - eye.z ) * 65536.0f / 6.2831853f );
	f.inputs[0].actions = 1;
	float drift = 0.0f;
	float nearest = 1e9f;
	bool held = false;
	for ( int i = 0; i < 120; ++i )
	{
		step( 1 );
		const MotionHold* hold = sim.FindEntity( p0 ).try_get<MotionHold>();
		held = held || ( hold != nullptr && hold->on != 0 && hold->flying == 0 && hold->anchor == crate );
		float apart = b3Distance( position(), sim.EntityTransform( crate )->position );
		nearest = std::min( nearest, apart );
		if ( apart > 1.5f )
		{
			drift = std::max( drift, b3Distance( sim.EntityTransform( crate )->position, crateAt ) );
		}
	}
	std::printf( "    a rope that does not react, on a crate: the player came to %.2f m of it; the crate moved %.3f m before\n", nearest, drift );
	CHECK( held && nearest < 2.0f && drift < 0.01f );
}

// Probes, forces and links: a motion throws a line at what the player looks at; it flies, a
// projectile traced tick by tick, and takes hold of what it meets first; a force pulls the player
// toward that (which is pulled back), and a link is a rope that keeps the two within its length (a
// grappling hook).
void TestLinks()
{
	ModSchema schema;
	schema.events = { "grapple.fired", "grapple.hooked" };
	schema.actions.push_back( { "grapple", 0, "X" } );
	schema.actions.push_back( { "leash", 1, "Z" } );
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tHook\nwhen\tpress\tgrapple\ncooldown\t0.2\nprobe\t40\t30\nhold_emit\tgrapple.hooked\nuntil\tnot held.grapple\n"
							 // 1920 N on an 80 kg player: 24 m/s^2; what it holds is pulled back by the same.
							 "force\tself\tto\tforce\t1920\t0\t0\t1\nlink\thit\t0\t3\n"
							 "param\tair_control\t0.6\nparam\tairborne\t1\nemit\tgrapple.fired\n"
							 // No pull, no reel, at once: a rope and nothing else.
							 "motion\tLeash\nwhen\tpress\tleash\nprobe\t40\t0\nlink\thit\t0\t0\nuntil\tnot held.leash\n";
	schema.motionSets.push_back( { "grapple", "grapple.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 2 && motions->list[0].probe && motions->list[0].First().effects.size() == 2 &&
		   motions->list[0].First().effects[1].kind == MotionEffect::Kind::Link );
	if ( motions == nullptr || motions->list.size() != 2 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}

	// The sandbox: walls 4 m high around the arena; the player looks down +Z at one.
	Simulation sim( TestConfig() );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	const uint16_t grapple = 1, leash = 2;
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	auto holdOf = [&]() -> const MotionHold* { return sim.FindEntity( p0 ).try_get<MotionHold>(); };
	auto position = [&] { return sim.EntityTransform( p0 )->position; };
	CHECK( holdOf() == nullptr );

	// At the sky: the line finds nothing, and is thrown all the same (its event says so). It flies
	// out its range (40 m at 30 m/s: 80 ticks), holds nothing, moves nobody, and is gone when it
	// gets there.
	b3Vec3 end;
	bool holds = true;
	uint16_t which = 9;
	f.inputs[0].cameraPitch = 14000;
	f.inputs[0].actions = grapple;
	b3Vec3 stood = position();
	step( 1 );
	const MotionHold* miss = holdOf();
	CHECK( miss != nullptr && miss->on == 1 && miss->flying == 1 && miss->holdTick - miss->startTick == 80 && sim.Globals().modEventCount == 1 );
	step( 40 );
	CHECK( sim.EntityHold( p0, end, holds, which ) && holds == false && end.y > stood.y + 10.0f );
	CHECK( b3Distance( position(), stood ) < 0.01f && sim.PlayerCharacter( 0 )->grounded == 1 );
	step( 41 );
	std::printf( "    at the sky: flew out %.1f m, and was gone after %u ticks\n", b3Distance( miss->point, miss->origin ), miss->holdTick - miss->startTick );
	CHECK( holdOf()->missed == 1 && holdOf()->on == 0 && sim.EntityHold( p0, end, holds, which ) == false && b3Distance( position(), stood ) < 0.01f );
	f.inputs[0].actions = 0;
	step( 15 ); // (its cooldown)

	// At the wall, a little upward: it is thrown, flies (nothing pulls yet), then holds and pulls.
	f.inputs[0].cameraPitch = 500;
	f.inputs[0].actions = grapple;
	b3Vec3 from = position();
	step( 1 );
	const MotionHold* tether = holdOf();
	CHECK( tether != nullptr && tether->on == 1 && tether->flying == 1 && tether->motion == 0 );
	if ( tether == nullptr || tether->on == 0 )
	{
		return;
	}
	const uint32_t thrownAt = tether->startTick;
	// While it flies its end is on the way, and the player has not moved; its parameters wait for
	// it to hold (the motion is in effect from the tick its probe takes hold).
	step( 10 );
	CHECK( sim.EntityHold( p0, end, holds, which ) && holds == false && which == 0 );
	CHECK( end.z > from.z + 1.0f && b3Distance( position(), from ) < 0.01f );
	CHECK( sim.GetMotionState( sim.FindEntity( p0 ) ).slots[0].sinceTick > sim.Tick() && sim.PlayerCharacter( 0 )->grounded == 1 );
	// It flies until it meets the wall: then it holds there, from that tick, and its parameters
	// with it.
	while ( holdOf()->flying == 1 && sim.Tick() < thrownAt + 90 )
	{
		step( 1 );
	}
	b3Vec3 anchor = holdOf()->point;
	float reach = b3Distance( anchor, b3Add( from, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) );
	uint32_t flight = holdOf()->holdTick - thrownAt;
	std::printf( "    thrown %.2f m at the wall: %u ticks of flight at 30 m/s\n", reach, flight );
	CHECK( holdOf()->on == 1 && holdOf()->missed == 0 && holdOf()->anchor == 0 && reach > 5.0f && reach < 40.0f && anchor.z > from.z + 5.0f );
	// (A tick's stretch is traced at a time: it holds on the tick its tip passes the wall.)
	CHECK( flight >= uint32_t( reach / 30.0f * 60.0f ) && flight <= uint32_t( reach / 30.0f * 60.0f ) + 1 );
	// In effect from then; and it says so, there (one event at the throw, one where it holds).
	CHECK( sim.GetMotionState( sim.FindEntity( p0 ) ).slots[0].sinceTick == holdOf()->holdTick );
	const ModEventRecord& hooked = sim.Globals().modEvents[( sim.Globals().modEventCount - 1 ) % kModEventHistory];
	CHECK( sim.Globals().modEventCount == 3 && hooked.type == 1 && hooked.tick == holdOf()->holdTick && b3Distance( hooked.point, anchor ) < 0.001f );
	step( 2 );
	CHECK( sim.EntityHold( p0, end, holds, which ) && holds && b3Distance( end, anchor ) < 0.001f );
	// Holding, its parameters hold until something ends it: the player is airborne on the hook,
	// though the floor is right under it.
	CHECK( sim.GetMotionState( sim.FindEntity( p0 ) ).slots[0].sinceTick <= sim.Tick() &&
		   sim.GetMotionState( sim.FindEntity( p0 ) ).slots[0].untilTick > sim.Tick() );
	CHECK( sim.PlayerCharacter( 0 )->grounded == 0 );
	step( 45 );
	float closer = b3Distance( anchor, b3Add( position(), b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) );
	std::printf( "    after 0.75 s of pull: %.2f m from the point (rope %.2f)\n", closer, holdOf()->length );
	CHECK( closer < reach - 3.0f && holdOf()->length < reach - 2.0f && holdOf()->length > reach - 2.5f );
	// Still in the air, and not sunk into the floor it is dragged over.
	CHECK( sim.PlayerCharacter( 0 )->grounded == 0 && sim.PlayerCharacter( 0 )->airTicks > 30 );
	CHECK( position().y > from.y - 0.05f );
	// Letting the key go lets it go, and the parameters are the server's again.
	f.inputs[0].actions = 0;
	step( 1 );
	CHECK( holdOf()->on == 0 && sim.EntityHold( p0, end, holds, which ) == false );
	step( 1 );
	CHECK( sim.GetMotionState( sim.FindEntity( p0 ) ).slots[0].untilTick < sim.Tick() );
	// Let go, it comes down and stands again.
	step( 180 );
	CHECK( sim.PlayerCharacter( 0 )->grounded == 1 );

	// A rope alone: the player cannot walk further from the point than the rope is long.
	f.inputs[0].cameraPitch = 500;
	f.inputs[0].actions = leash;
	step( 1 );
	CHECK( holdOf()->on == 1 && holdOf()->motion == 1 && holdOf()->holdTick == holdOf()->startTick );
	b3Vec3 post = holdOf()->point;
	float rope = holdOf()->length;
	f.inputs[0].moveForward = -127; // away from the wall
	float furthest = 0.0f;
	for ( int i = 0; i < 240; ++i )
	{
		step( 1 );
		furthest = std::max( furthest, b3Distance( post, b3Add( position(), b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) ) );
	}
	std::printf( "    on a %.2f m rope, walking away for 4 s: never past %.2f m\n", rope, furthest );
	CHECK( furthest < rope + 0.35f && holdOf()->length == rope );
	f.inputs[0].moveForward = 0;
	f.inputs[0].actions = 0;
	step( 120 );
	CHECK( holdOf()->on == 0 );

	// On a prop: the point is on the body, the prop is pulled toward the player, and when the prop is
	// gone the tether is.
	uint32_t crate = sim.Globals().nextNetId;
	b3Vec3 here = position();
	{
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.value = -1;
		c.a = { here.x, here.y + 0.3f, here.z + 6.0f };
		c.c = { 0.4f, 0.4f, 0.4f };
		f.commands.push_back( c );
	}
	step( 90 );
	b3Vec3 crateAt = sim.EntityTransform( crate )->position;
	// Aim at it: it rests on the floor ahead.
	b3Vec3 eye = b3Add( position(), b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } );
	float down = std::atan2( crateAt.y - eye.y, crateAt.z - eye.z );
	f.inputs[0].cameraPitch = int16_t( down * 65536.0f / 6.2831853f );
	f.inputs[0].actions = grapple;
	step( 15 ); // (6 m at 30 m/s)
	CHECK( holdOf()->on == 1 && holdOf()->anchor == crate );
	step( 46 );
	float pulled = crateAt.z - sim.EntityTransform( crate )->position.z;
	std::printf( "    a crate on the hook came %.2f m closer in a second\n", pulled );
	CHECK( pulled > 0.3f && holdOf()->on == 1 );
	{
		SimCommand c;
		c.type = CommandType::Destroy;
		c.target = crate;
		f.commands.push_back( c );
	}
	step( 2 );
	CHECK( holdOf()->on == 0 );
	f.inputs[0].actions = 0;
	step( 60 );

	// A projectile: thrown at the wall, a crate dropped into its way while it flies is what it
	// meets, and what it holds (aimed at, the wall was all there was at the throw).
	{
		f.inputs[0].cameraPitch = 500;
		f.inputs[0].actions = grapple;
		step( 3 );
		const MotionHold* out = holdOf();
		CHECK( out != nullptr && out->flying == 1 );
		if ( out == nullptr )
		{
			return;
		}
		b3Vec3 along = b3Normalize( b3Sub( out->point, out->origin ) );
		b3Vec3 inTheWay = b3MulAdd( out->origin, 8.0f, along );
		uint32_t blocker = sim.Globals().nextNetId;
		SimCommand c;
		c.type = CommandType::SpawnProp;
		c.value = -1;
		c.a = { inTheWay.x, inTheWay.y, inTheWay.z };
		c.c = { 0.4f, 0.4f, 0.4f };
		f.commands.push_back( c );
		const uint32_t thrownAt = out->startTick;
		while ( holdOf()->flying == 1 && sim.Tick() < thrownAt + 90 )
		{
			step( 1 );
		}
		b3Vec3 held;
		CHECK( sim.EntityHold( p0, held, holds, which ) && holds );
		std::printf( "    a crate dropped into the line's way: it holds the crate (%d), %.1f m out, after %u ticks\n", int( holdOf()->anchor == blocker ),
					 b3Distance( held, holdOf()->origin ), holdOf()->holdTick - thrownAt );
		CHECK( holdOf()->on == 1 && holdOf()->anchor == blocker && b3Distance( held, holdOf()->origin ) < 8.0f );
		f.inputs[0].actions = 0;
		step( 1 );
		c.type = CommandType::Destroy;
		c.target = blocker;
		f.commands.push_back( c );
		step( 60 );
	}

	// It is state: a joining client stays in step through a throw and a pull, and a rollback takes
	// a throw back.
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	Simulation client( TestConfig() );
	client.SetMotions( motions );
	CHECK( client.LoadPortable( image ) && client.ComputeHash() == sim.ComputeHash() );
	Snapshot before;
	sim.Save( before );
	uint64_t hashBefore = sim.ComputeHash();
	f.inputs[0].cameraPitch = 500;
	f.inputs[0].actions = grapple;
	for ( int i = 0; i < 90; ++i )
	{
		f.tick = sim.Tick();
		sim.Step( f );
		client.Step( f );
	}
	CHECK( holdOf()->on == 1 && client.ComputeHash() == sim.ComputeHash() );
	sim.Load( before );
	CHECK( holdOf()->on == 0 && sim.ComputeHash() == hashBefore );

	// Press to throw, press again to let go: "linked" keeps a second press from throwing a second
	// line, and "pressed.<action>" is the press that lets the first one go.
	{
		ModSchema toggled = schema;
		toggled.motionSets = { { "grapple", "grapple.moves",
								 "cinderbox_motions\t2\nmotion\tHook\nwhen\tpress\tgrapple\nif\tnot linked\n"
								 "probe\t40\t0\nforce\tself\tto\taccel\t10\t0\t0\t0\nlink\thit\t0\t0\nuntil\tpressed.grapple\nemit\tgrapple.fired\n" } };
		std::string toggleWarnings;
		auto toggle = CompileMotions( toggled, toggleWarnings );
		CHECK( toggle != nullptr && toggleWarnings.empty() );
		Simulation other( TestConfig() );
		other.SetMotions( toggle );
		InputFrame g;
		g.events.push_back( { PlayerEventType::Join, 0 } );
		auto run = [&]( int n, uint16_t actions ) {
			for ( int i = 0; i < n; ++i )
			{
				g.tick = other.Tick();
				g.inputs[0].cameraPitch = 500;
				g.inputs[0].actions = actions;
				other.Step( g );
				g.events.clear();
			}
		};
		auto out = [&] {
			const MotionHold* t = other.FindEntity( other.PlayerNetId( 0 ) ).try_get<MotionHold>();
			return t != nullptr && t->on != 0;
		};
		run( 60, 0 );
		run( 4, grapple ); // a press, held a few ticks: thrown, and still out
		CHECK( out() && other.Globals().modEventCount == 1 );
		run( 30, 0 );
		CHECK( out() ); // the key is up: it stays
		run( 1, grapple ); // the next press lets it go, and throws no second one
		CHECK( out() == false && other.Globals().modEventCount == 1 );
		run( 3, grapple );
		CHECK( out() == false );
		run( 5, 0 );
		run( 1, grapple ); // and the one after that throws again
		CHECK( out() && other.Globals().modEventCount == 2 );
	}

	// A zip line: no gravity while it holds, a steady speed straight to the point, and it lets go by
	// itself on arrival ("link_distance"), with the speed kept.
	{
		ModSchema zipSchema;
		zipSchema.actions.push_back( { "zip", 0, "X" } );
		zipSchema.motionSets.push_back( { "zip", "zip.moves",
										  "cinderbox_motions\t2\nmotion\tZip\nwhen\tpress\tzip\nif\tnot linked\nuntil\tlink_distance < 1.8\n"
										  "param\tgravity\t0\nparam\tairborne\t1\nprobe\t40\t0\nforce\tself\tto\tvelocity\t160\t20\t0\t1\n" } );
		std::string zipWarnings;
		std::shared_ptr<const Motions> zip = CompileMotions( zipSchema, zipWarnings );
		CHECK( zip != nullptr && zipWarnings.empty() );
		if ( zip != nullptr )
		{
			Simulation zipSim( TestConfig() );
			zipSim.SetMotions( zip );
			InputFrame zf;
			auto zstep = [&]( int n ) {
				for ( int i = 0; i < n; ++i )
				{
					zf.tick = zipSim.Tick();
					zipSim.Step( zf );
					zf.events.clear();
					zf.commands.clear();
				}
			};
			zf.events.push_back( { PlayerEventType::Join, 0 } );
			zstep( 60 );
			uint32_t zp = zipSim.PlayerNetId( 0 );
			b3Vec3 start = zipSim.EntityTransform( zp )->position;
			zf.inputs[0].cameraPitch = 500;
			zf.inputs[0].actions = 1;
			zstep( 1 );
			const MotionHold* zhold = zipSim.FindEntity( zp ).try_get<MotionHold>();
			CHECK( zhold != nullptr && zhold->on == 1 );
			b3Vec3 point = zhold != nullptr ? zhold->point : b3Vec3{};
			float reach = b3Distance( point, b3Add( start, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) );
			// Half a second in: at its speed, along the line, and rising with it from the first tick
			// (the line climbs: gravity would have kept the player on the floor).
			zstep( 30 );
			const Character* zc = zipSim.PlayerCharacter( 0 );
			b3Vec3 line = b3Normalize( b3Sub( point, b3Add( start, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) ) );
			float alongLine = b3Dot( zc->velocity, line );
			float rise = zipSim.EntityTransform( zp )->position.y - start.y;
			std::printf( "    a zip line of %.1f m: %.1f m/s along it after half a second, %.2f m higher\n", reach, alongLine, rise );
			CHECK( alongLine > 19.5f && alongLine < 20.5f && b3Length( zc->velocity ) < 20.6f );
			CHECK( line.y > 0.02f && rise > 0.7f * 9.5f * line.y && zipSim.FindEntity( zp ).get<MotionHold>().on == 1 );
			// It lets go by itself when it has arrived, and the player keeps its speed.
			int ticks = 0;
			while ( zipSim.FindEntity( zp ).get<MotionHold>().on != 0 && ticks < 300 )
			{
				zstep( 1 );
				++ticks;
			}
			float left = b3Distance( point, b3Add( zipSim.EntityTransform( zp )->position, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) );
			std::printf( "    let go by itself %.2f m from the point, at %.1f m/s\n", left, b3Length( zipSim.PlayerCharacter( 0 )->velocity ) );
			CHECK( ticks < 300 && left < 1.8f && left > 1.0f && b3Length( zipSim.PlayerCharacter( 0 )->velocity ) > 15.0f );
		}
	}

	// What is loose gives way before a player does, by what the two weigh: a ball of a tenth of a
	// kilogram thrown at a standing player at 17 m/s does not move it; a crate of 40 kg does.
	{
		auto hitBy = [&]( b3Vec3 half, ShapeKind kind ) {
			Simulation sim( TestConfig() );
			InputFrame f;
			f.events.push_back( { PlayerEventType::Join, 0 } );
			for ( int i = 0; i < 60; ++i )
			{
				f.tick = sim.Tick();
				sim.Step( f );
				f.events.clear();
			}
			b3Vec3 stood = sim.EntityTransform( sim.PlayerNetId( 0 ) )->position;
			SimCommand spawn;
			spawn.type = CommandType::SpawnProp;
			spawn.mode = uint8_t( kind );
			spawn.value = -1;
			spawn.a = { stood.x, stood.y + 0.1f, stood.z + 2.5f };
			spawn.b = { 0.0f, 0.0f, -17.0f };
			spawn.c = { half.x, half.y, half.z };
			f.commands.push_back( spawn );
			float moved = 0.0f;
			for ( int i = 0; i < 40; ++i )
			{
				f.tick = sim.Tick();
				sim.Step( f );
				f.commands.clear();
				moved = std::max( moved, b3Distance( sim.EntityTransform( sim.PlayerNetId( 0 ) )->position, stood ) );
			}
			return moved;
		};
		// (A small fast body is inside the player's capsule before the solver stops it, and the
		// mover used to step the player out of it by the whole overlap: 0.14 m here.)
		float byBall = hitBy( { 0.09f, 0.0f, 0.0f }, ShapeKind::Sphere );
		std::printf( "    hit by a ball of 0.12 kg at 17 m/s, a standing player is moved %.3f m\n", byBall );
		CHECK( byBall < 0.03f );
	}

	// A throw (ImpulseThrow) moves the whole of a fallen body; a hit moves the part it lands on.
	{
		auto fallen = [&]( uint8_t mode ) {
			Simulation sim( TestConfig() );
			InputFrame f;
			f.events.push_back( { PlayerEventType::Join, 0 } );
			for ( int i = 0; i < 60; ++i )
			{
				f.tick = sim.Tick();
				sim.Step( f );
				f.events.clear();
			}
			b3Vec3 stood = sim.EntityTransform( sim.PlayerNetId( 0 ) )->position;
			SimCommand kill;
			kill.type = CommandType::Kill;
			kill.target = SlotTarget( 0 );
			kill.mode = 1;
			kill.a = { stood.x, stood.y, stood.z };
			f.commands.push_back( kill );
			f.tick = sim.Tick();
			sim.Step( f );
			f.commands.clear();
			uint32_t body = 0;
			for ( const Simulation::EntityRef& r : sim.Entities() )
			{
				body = sim.FindEntity( r.netId ).has<Ragdoll>() ? r.netId : body;
			}
			SimCommand push;
			push.type = CommandType::Impulse;
			push.target = body;
			push.mode = mode;
			push.a = { stood.x, stood.y - 0.9f, stood.z - 1.0f }; // a blast on the floor, a metre off
			push.b = { 0.0f, 6.0f, 8.0f };
			f.commands.push_back( push );
			b3Vec3 from = sim.EntityTransform( body )->position;
			float far = 0.0f;
			for ( int i = 0; i < 45; ++i )
			{
				f.tick = sim.Tick();
				sim.Step( f );
				f.commands.clear();
				far = std::max( far, b3Distance( sim.EntityTransform( body )->position, from ) );
			}
			return body != 0 ? far : -1.0f;
		};
		float hit = fallen( ImpulseVelocity );
		float thrown = fallen( ImpulseThrow );
		std::printf( "    a fallen body given 10 m/s: as a hit (one part) it goes %.2f m in 0.75 s, as a throw (all of it) %.2f m\n", hit, thrown );
		CHECK( hit >= 0.0f && thrown > 3.0f && thrown > 2.0f * hit );
	}

	// A launch: a motion throws an item of a kind into the world on the tick of the press, in every
	// simulation alike. It knows who threw it, flies as its body says, and goes when its time is up.
	{
		ModSchema throwSchema;
		throwSchema.actions.push_back( { "throw", 0, "T" } );
		throwSchema.itemKinds = { "ball" };
		ItemShape ball;
		ball.kind = 1;
		ball.half = { 0.12f, 0.12f, 0.12f };
		ball.center = { 0.0f, 0.0f, 0.0f };
		ball.mass = 0.5f;
		ball.restitution = 0.5f;
		throwSchema.itemShapes = { ball };
		throwSchema.motionSets.push_back( { "throw", "throw.moves",
											"cinderbox_motions\t2\nmotion\tThrow\nwhen\tpress\tthrow\ncooldown\t0.5\n"
											"launch\tball\t15\tlook\t2\t0.7\t1.5\n" } );
		std::string throwWarnings;
		std::shared_ptr<const Motions> throwing = CompileMotions( throwSchema, throwWarnings );
		CHECK( throwing != nullptr && throwWarnings.empty() );
		if ( throwing != nullptr )
		{
			auto make = [&]() {
				auto made = std::make_unique<Simulation>( TestConfig() );
				made->SetMotions( throwing );
				made->SetItemShapes( throwSchema.itemShapes );
				return made;
			};
			auto server = make();
			auto client = make();
			InputFrame tf;
			auto tstep = [&]( int n ) {
				for ( int i = 0; i < n; ++i )
				{
					tf.tick = server->Tick();
					server->Step( tf );
					client->Step( tf );
					tf.events.clear();
				}
			};
			auto items = [&]( const Simulation& in ) {
				std::vector<uint32_t> found;
				for ( const Simulation::EntityRef& r : in.Entities() )
				{
					if ( in.FindEntity( r.netId ).has<HeldItem>() )
					{
						found.push_back( r.netId );
					}
				}
				return found;
			};
			tf.events.push_back( { PlayerEventType::Join, 0 } );
			tstep( 60 );
			uint32_t thrower = server->PlayerNetId( 0 );
			CHECK( items( *server ).empty() );
			Snapshot before;
			server->Save( before );
			uint64_t hashBefore = server->ComputeHash();
			tf.inputs[0].actions = 1;
			tf.inputs[0].cameraPitch = 2000; // a little upward
			tstep( 1 );
			std::vector<uint32_t> thrown = items( *server );
			CHECK( thrown.size() == 1 && items( *client ) == thrown && client->ComputeHash() == server->ComputeHash() );
			if ( thrown.size() == 1 )
			{
				uint32_t it = thrown[0];
				b3Vec3 at = server->EntityTransform( it )->position;
				b3Vec3 from = server->EntityTransform( thrower )->position;
				const Velocity velocity = server->FindEntity( it ).get<Velocity>();
				std::printf( "    a ball launched at 15 m/s with 2 m/s of lift: %.2f m from the player, going %.1f m/s (%.1f up)\n",
							 b3Distance( at, from ), b3Length( velocity.linear ), velocity.linear.y );
				CHECK( server->LaunchedBy( it ) == thrower && server->LaunchedBy( thrower ) == 0 );
				CHECK( b3Length( velocity.linear ) > 14.0f && b3Length( velocity.linear ) < 17.0f && velocity.linear.y > 2.0f );
				// Held, the key throws nothing more; it flies, and both simulations agree where.
				tstep( 20 );
				CHECK( items( *server ).size() == 1 && client->ComputeHash() == server->ComputeHash() );
				CHECK( b3Distance( server->EntityTransform( it )->position, from ) > 3.0f );
				// A second press after the cooldown: a second ball. Then each goes when its time is up.
				tf.inputs[0].actions = 0;
				tstep( 20 );
				tf.inputs[0].actions = 1;
				tstep( 1 );
				tf.inputs[0].actions = 0;
				CHECK( items( *server ).size() == 2 );
				tstep( 60 );
				CHECK( items( *server ).size() == 1 && server->EntityTransform( it ) == nullptr );
				tstep( 60 );
				CHECK( items( *server ).empty() && client->ComputeHash() == server->ComputeHash() );
				// Rolled back to before the first press, there was never a ball.
				server->Load( before );
				CHECK( items( *server ).empty() && server->ComputeHash() == hashBefore );
			}
		}
		// What no mod declares is said, and nothing is thrown.
		ModSchema none = throwSchema;
		none.itemKinds.clear();
		none.itemShapes.clear();
		std::string said;
		auto inert = CompileMotions( none, said );
		CHECK( inert != nullptr && said.find( "ball" ) != std::string::npos );
		auto bad = []( const char* line ) {
			ModSchema s;
			s.actions.push_back( { "throw", 0, "T" } );
			s.itemKinds = { "ball" };
			s.motionSets.push_back( { "throw", "throw.moves", std::string( "cinderbox_motions\t2\nmotion\tThrow\nwhen\tpress\tthrow\n" ) + line } );
			std::string w;
			auto compiled = CompileMotions( s, w );
			return compiled == nullptr || compiled->list.empty();
		};
		CHECK( bad( "launch\tball\t15\n" ) );
		CHECK( bad( "launch\tball\t15\tto\t0\t0.7\t0\n" ) );   // there is nothing to throw it "to"
		CHECK( bad( "launch\tball\t15\tlook\t0\t9\t0\n" ) );   // nine metres ahead
		CHECK( bad( "launch\tball\t15\tlook\t0\t0.7\t0\n" ) == false );
	}

	// A while motion cannot throw a probe; a probe needs both its numbers; a rope goes to something else.
	std::vector<Motion> out;
	std::string error, warned;
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t2\nmotion\tA\nwhen\twhile\nprobe\t40\t30\n", schema, out, error, warned ) == false );
	CHECK( error.find( "while" ) != std::string::npos );
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t2\nmotion\tA\nwhen\tpress\tgrapple\nprobe\t40\n", schema, out, error, warned ) == false );
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t2\nmotion\tA\nwhen\tpress\tgrapple\nprobe\t40\t30\nlink\tself\t0\t3\n", schema, out, error, warned ) == false );
	// An effect on what a probe found needs a probe.
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t2\nmotion\tA\nwhen\tpress\tgrapple\nimpulse\thit\t5\tup\tnone\n", schema, out, error, warned ) == false );
	CHECK( error.find( "probe" ) != std::string::npos );
}

void TestRagdoll()
{
	Simulation sim( TestConfig(), FlatMap() );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto kill = [&]( uint32_t lifetime, int32_t cap ) {
		SimCommand c;
		c.type = CommandType::Kill;
		c.mode = 1;
		c.target = SlotTarget( 0 );
		c.other = lifetime;
		c.value = cap;
		{ b3Vec3 p = sim.EntityTransform( sim.PlayerNetId( 0 ) )->position; c.a = { p.x, p.y + 0.3f, p.z }; }
		c.b = { 0.0f, 1.0f, -4.0f };
		f.commands.push_back( c );
	};
	auto respawn = [&]() {
		SimCommand c;
		c.type = CommandType::Respawn;
		c.target = SlotTarget( 0 );
		f.commands.push_back( c );
	};

	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	uint32_t player = sim.PlayerNetId( 0 );

	kill( 600, 2 );
	step( 1 );
	flecs::entity body = NewestRagdoll( sim );
	CHECK( body.is_valid() );
	CHECK( body.get<Ragdoll>().owner == player );
	CHECK( sim.PlayerCharacter( 0 )->dead == 1 );
	float startY = body.get<RagdollPose>().part[ragdoll::Pelvis].position.y;

	step( 240 );
	body = NewestRagdoll( sim );
	const RagdollPose& pose = body.get<RagdollPose>();
	float restY = pose.part[ragdoll::Pelvis].position.y;
	std::printf( "    pelvis fell from %.2f to %.2f\n", startY, restY );
	CHECK( restY < startY - 0.4f );
	CHECK( restY > 0.0f && restY < 0.5f );

	// Every joint still holds together, and knees and elbows only bend their own way.
	for ( int i = 0; i < ragdoll::PartCount; ++i )
	{
		const ragdoll::PartDef& part = ragdoll::kParts[i];
		if ( part.parent < 0 )
		{
			continue;
		}
		const Transform& child = pose.part[i];
		const Transform& parent = pose.part[part.parent];
		const ragdoll::PartDef& parentDef = ragdoll::kParts[part.parent];
		b3Vec3 fromChild = b3Add( child.position, b3RotateVector( child.rotation, b3Sub( part.anchor, part.center ) ) );
		b3Vec3 fromParent = b3Add( parent.position, b3RotateVector( parent.rotation, b3Sub( part.anchor, parentDef.center ) ) );
		float gap = b3Distance( fromChild, fromParent );
		if ( gap > 0.05f )
		{
			std::printf( "    %s separated by %.3f m\n", part.name, gap );
		}
		CHECK( gap < 0.05f );

		if ( part.joint == ragdoll::JointKind::Hinge )
		{
			b3Quat relative = b3InvMulQuat( parent.rotation, child.rotation );
			if ( relative.s < 0.0f )
			{
				relative = b3NegateQuat( relative );
			}
			float angle = 2.0f * detmath::Atan2( relative.v.x, relative.s );
			std::printf( "    %s bent %.2f rad (limits %.2f .. %.2f)\n", part.name, angle, part.lower, part.upper );
			CHECK( angle > part.lower - 0.15f && angle < part.upper + 0.15f );
		}
	}

	// A shot moves it.
	b3Vec3 torsoBefore = body.get<RagdollPose>().part[ragdoll::Torso].position;
	{
		SimCommand c;
		c.type = CommandType::Impulse;
		c.mode = ImpulseVelocity;
		c.target = body.get<NetId>().value;
		c.a = { torsoBefore.x, torsoBefore.y, torsoBefore.z };
		c.b = { 0.0f, 5.0f, 0.0f };
		f.commands.push_back( c );
	}
	step( 10 );
	CHECK( NewestRagdoll( sim ).get<RagdollPose>().part[ragdoll::Torso].position.y > torsoBefore.y + 0.1f );

	// The cap keeps the newest two.
	uint32_t firstBody = body.get<NetId>().value;
	for ( int i = 0; i < 2; ++i )
	{
		respawn();
		step( 30 );
		kill( 600, 2 );
		step( 30 );
	}
	auto isRagdoll = []( flecs::entity e ) { return e.has<Ragdoll>(); };
	CHECK( CountWith( sim, isRagdoll ) == 2 );
	CHECK( sim.FindEntity( firstBody ).is_valid() == false );

	// Lifetimes expire them, and they never outlive the kill plane either.
	respawn();
	step( 601 );
	CHECK( CountWith( sim, isRagdoll ) == 0 );
	CHECK( sim.PlayerCharacter( 0 )->dead == 0 );
}

// Presentation pose tools: an aimed arm points where it was told, and a ragdoll lying exactly in
// its rest pose puts every joint of the skeleton back where the rig's rest has it.
void TestPoseTools()
{
	auto set = anim::AnimSet::CreateProcedural();
	anim::PoseEvaluator eval( *set );
	eval.Evaluate( AnimState{} );

	auto position = []( const ozz::math::Float4x4& m ) {
		float v[4];
		ozz::math::StorePtrU( m.cols[3], v );
		return b3Vec3{ v[0], v[1], v[2] };
	};

	int shoulder = present::FindJoint( *set, "RightUpperArm" );
	int hand = present::FindJoint( *set, "RightHand" );
	CHECK( shoulder >= 0 && hand >= 0 );
	// The character's right is -X (left is +X, facing +Z).
	CHECK( position( eval.Models()[size_t( shoulder )] ).x < 0.0f );

	for ( b3Vec3 dir : { b3Vec3{ 0.0f, 0.0f, 1.0f }, b3Vec3{ 1.0f, 0.0f, 0.0f }, b3Normalize( b3Vec3{ -0.3f, 0.5f, 0.8f } ) } )
	{
		present::Models models = eval.Models();
		anim::AimChain( *set, models, { { shoulder, 1.0f } }, hand, dir );
		b3Vec3 arm = b3Normalize( b3Sub( position( models[size_t( hand )] ), position( models[size_t( shoulder )] ) ) );
		std::printf( "    aim (%.2f %.2f %.2f) -> arm (%.2f %.2f %.2f)\n", dir.x, dir.y, dir.z, arm.x, arm.y, arm.z );
		CHECK( b3Dot( arm, dir ) > 0.999f );
		// The shoulder itself stays put.
		CHECK( b3Distance( position( models[size_t( shoulder )] ), position( eval.Models()[size_t( shoulder )] ) ) < 1e-5f );
	}

	// Aiming is part of the pose now: an aiming state points the arm where the player looks,
	// relative to the body, with no presentation step involved (the server's hit tests use it).
	AnimState aiming;
	aiming.aiming = 1;
	aiming.aimYaw = 0.5f;
	aiming.aimPitch = 0.3f;
	anim::PoseEvaluator aimed( *set );
	aimed.Evaluate( aiming );
	{
		b3CosSin p = detmath::CosSin( 0.3f );
		b3CosSin y = detmath::CosSin( 0.5f );
		b3Vec3 look = { y.sine * p.cosine, p.sine, y.cosine * p.cosine };
		b3Vec3 arm = b3Normalize( b3Sub( position( aimed.Models()[size_t( hand )] ), position( aimed.Models()[size_t( shoulder )] ) ) );
		CHECK( b3Dot( arm, look ) > 0.999f );
		CHECK( set->AimJoints().size() == 1 && set->AimTip() == hand );
	}
	aiming.aiming = 0;
	aimed.Evaluate( aiming );
	CHECK( b3Distance( position( aimed.Models()[size_t( hand )] ), position( eval.Models()[size_t( hand )] ) ) < 1e-5f );

	// The upper body follows the camera's pitch while the character faces the camera: looking up
	// leans it back, looking down bows it, and the hips and legs stay where they were.
	{
		int head = present::FindJoint( *set, "Head" );
		int hips = present::FindJoint( *set, "Hips" );
		int foot = present::FindJoint( *set, "LeftFoot" );
		float shares = 0.0f;
		for ( const auto& [joint, share] : set->LookJoints() )
		{
			shares += share;
		}
		std::printf( "    look chain: %d joints, shares add up to %.2f\n", int( set->LookJoints().size() ), shares );
		CHECK( set->LookJoints().size() >= 2 && shares > 0.5f && shares <= 1.001f );
		b3Vec3 restHead = position( eval.Models()[size_t( head )] );
		bool lowerStays = true;
		auto headAt = [&]( float pitch, uint8_t look ) {
			AnimState s;
			s.aimPitch = pitch;
			s.look = look;
			anim::PoseEvaluator e( *set );
			e.Evaluate( s );
			lowerStays &= b3Distance( position( e.Models()[size_t( hips )] ), position( eval.Models()[size_t( hips )] ) ) < 1e-5f;
			lowerStays &= b3Distance( position( e.Models()[size_t( foot )] ), position( eval.Models()[size_t( foot )] ) ) < 1e-5f;
			return position( e.Models()[size_t( head )] );
		};
		b3Vec3 up = headAt( 0.7f, 255 );
		b3Vec3 down = headAt( -0.7f, 255 );
		std::printf( "    head at rest (%.2f %.2f %.2f), looking up (%.2f %.2f %.2f), looking down (%.2f %.2f %.2f)\n", restHead.x, restHead.y,
					 restHead.z, up.x, up.y, up.z, down.x, down.y, down.z );
		CHECK( lowerStays );
		CHECK( up.z < restHead.z - 0.03f );						  // leans back
		CHECK( down.z > restHead.z + 0.03f && down.y < restHead.y ); // bows forward and down
		CHECK( std::fabs( up.x - restHead.x ) < 1e-4f && std::fabs( down.x - restHead.x ) < 1e-4f );
		// Not facing the camera (freelook): the pitch is the camera's alone.
		CHECK( b3Distance( headAt( 0.7f, 0 ), restHead ) < 1e-5f );
		// Half way in: about half as far.
		float half = b3Distance( headAt( -0.7f, 128 ), restHead );
		float whole = b3Distance( down, restHead );
		CHECK( half > 0.35f * whole && half < 0.65f * whole );
		// The head ends up turned by the whole of the shares: its own forward follows the camera.
		{
			AnimState s;
			s.aimPitch = 0.7f;
			s.look = 255;
			anim::PoseEvaluator e( *set );
			e.Evaluate( s );
			b3Vec3 p;
			b3Quat was, is;
			float scale;
			anim::Decompose( eval.Models()[size_t( head )], p, was, scale );
			anim::Decompose( e.Models()[size_t( head )], p, is, scale );
			b3Vec3 forward = b3RotateVector( b3MulQuat( is, b3Quat{ { -was.v.x, -was.v.y, -was.v.z }, was.s } ), { 0.0f, 0.0f, 1.0f } );
			b3CosSin turned = detmath::CosSin( 0.7f * shares );
			std::printf( "    head forward after looking up 0.7: (%.3f %.3f %.3f), expected (0 %.3f %.3f)\n", forward.x, forward.y, forward.z, turned.sine, turned.cosine );
			// (Within the deterministic sine's error: five small turns, each a little off.)
			CHECK( std::fabs( forward.y - turned.sine ) < 0.02f && std::fabs( forward.z - turned.cosine ) < 0.02f );
		}
		// With a gun out the arm still ends up exactly on the line of sight.
		{
			AnimState s;
			s.aiming = 1;
			s.look = 255;
			s.aimPitch = -0.6f;
			anim::PoseEvaluator e( *set );
			e.Evaluate( s );
			b3CosSin p = detmath::CosSin( -0.6f );
			b3Vec3 arm = b3Normalize( b3Sub( position( e.Models()[size_t( hand )] ), position( e.Models()[size_t( shoulder )] ) ) );
			CHECK( b3Dot( arm, b3Vec3{ 0.0f, p.sine, p.cosine } ) > 0.999f );
		}
		// A character says which joints, and how much.
		std::string warnings;
		auto stiff = anim::AnimSet::CreateProcedural();
		stiff->SetLook( "", warnings );
		CHECK( stiff->LookJoints().empty() && warnings.empty() );
		stiff->SetLook( "Spine:0.5 Nope:1 Head:x", warnings );
		CHECK( stiff->LookJoints().size() == 1 && warnings.find( "Nope" ) != std::string::npos && warnings.find( "Head" ) != std::string::npos );
	}

	// Moving and turning a part of the pose (what a first-person view does to the viewer's own
	// upper body): everything below the joint goes along, nothing else does.
	{
		int spine = present::FindJoint( *set, "Spine" );
		int head = present::FindJoint( *set, "Head" );
		int foot = present::FindJoint( *set, "LeftFoot" );
		present::Models models = eval.Models();
		anim::TranslateSubtree( *set, models, spine, { 0.1f, 0.2f, -0.3f } );
		b3Vec3 moved = b3Sub( position( models[size_t( head )] ), position( eval.Models()[size_t( head )] ) );
		CHECK( std::fabs( moved.x - 0.1f ) < 1e-5f && std::fabs( moved.y - 0.2f ) < 1e-5f && std::fabs( moved.z + 0.3f ) < 1e-5f );
		CHECK( b3Distance( position( models[size_t( hand )] ), position( eval.Models()[size_t( hand )] ) ) > 0.3f );
		CHECK( b3Distance( position( models[size_t( foot )] ), position( eval.Models()[size_t( foot )] ) ) < 1e-6f );
		// A quarter turn about a point above the head: every joint below the spine keeps its distance
		// from that point, and the feet stay.
		models = eval.Models();
		b3Vec3 pivot = { 0.0f, 1.7f, 0.0f };
		float before = b3Distance( position( models[size_t( hand )] ), pivot );
		anim::RotateSubtreeAbout( *set, models, spine, pivot, b3MakeQuatFromAxisAngle( { 1.0f, 0.0f, 0.0f }, 0.5f * detmath::kPi ) );
		CHECK( std::fabs( b3Distance( position( models[size_t( hand )] ), pivot ) - before ) < 1e-3f );
		CHECK( b3Distance( position( models[size_t( hand )] ), position( eval.Models()[size_t( hand )] ) ) > 0.3f );
		CHECK( b3Distance( position( models[size_t( foot )] ), position( eval.Models()[size_t( foot )] ) ) < 1e-6f );
	}

	// An item held with both hands: the item is where the hand that carries it is, and the other
	// arm bends so that its wrist is on the item's grip.
	{
		const anim::AnimSet::Arm& right = set->ArmJoints( false );
		const anim::AnimSet::Arm& left = set->ArmJoints( true );
		CHECK( right.upper >= 0 && right.lower >= 0 && right.hand == hand && left.upper >= 0 && left.lower >= 0 && left.hand >= 0 );
		AnimState aimingState;
		aimingState.aiming = 1; // the right arm out in front, as with a pistol
		anim::PoseEvaluator carried( *set );
		carried.Evaluate( aimingState );
		// A grip within the other arm's reach: a point in front of its shoulder, said in the item's
		// frame, which is the carrying hand's socket frame.
		b3Vec3 at;
		b3Quat turn;
		float scale;
		anim::Decompose( carried.Models()[size_t( right.hand )], at, turn, scale );
		b3Quat item = b3MulQuat( turn, set->HandSocketOf( false ).rotation );
		b3Vec3 itemAt = b3Add( at, b3RotateVector( turn, set->HandSocketOf( false ).position ) );
		b3Vec3 target = b3Add( position( carried.Models()[size_t( left.upper )] ), b3Vec3{ -0.1f, -0.1f, 0.3f } );
		anim::HandGrip grip;
		const b3Vec3 within = b3RotateVector( b3Quat{ { -item.v.x, -item.v.y, -item.v.z }, item.s }, b3Sub( target, itemAt ) );
		grip.position = within;
		anim::PoseEvaluator both( *set );
		both.Evaluate( aimingState, &grip );
		float off = b3Distance( position( both.Models()[size_t( left.hand )] ), target );
		float before = b3Distance( position( carried.Models()[size_t( left.hand )] ), target );
		std::printf( "    the other hand: %.3f m from the grip without it, %.4f m with\n", before, off );
		CHECK( before > 0.2f && off < 0.002f );
		// The carrying arm and the rest of the body did not move, and the bones kept their lengths.
		int head = present::FindJoint( *set, "Head" );
		CHECK( b3Distance( position( both.Models()[size_t( right.hand )] ), position( carried.Models()[size_t( right.hand )] ) ) < 1e-6f );
		CHECK( b3Distance( position( both.Models()[size_t( head )] ), position( carried.Models()[size_t( head )] ) ) < 1e-6f );
		CHECK( b3Distance( position( both.Models()[size_t( left.upper )] ), position( carried.Models()[size_t( left.upper )] ) ) < 1e-5f );
		auto length = [&]( const present::Models& m, int a, int b ) { return b3Distance( position( m[size_t( a )] ), position( m[size_t( b )] ) ); };
		CHECK( std::fabs( length( both.Models(), left.upper, left.lower ) - length( carried.Models(), left.upper, left.lower ) ) < 1e-4f );
		CHECK( std::fabs( length( both.Models(), left.lower, left.hand ) - length( carried.Models(), left.lower, left.hand ) ) < 1e-4f );
		// Out of reach: the arm goes as far as it can toward it, almost straight.
		grip.position = { 0.0f, 0.0f, -1.5f };
		both.Evaluate( aimingState, &grip );
		float arm = length( both.Models(), left.upper, left.lower ) + length( both.Models(), left.lower, left.hand );
		CHECK( length( both.Models(), left.upper, left.hand ) > 0.99f * arm );
		// The hand takes the grip's turn when it is asked to: as a hand carrying an item there would.
		grip.position = within;
		grip.align = true;
		both.Evaluate( aimingState, &grip );
		b3Quat leftTurn;
		b3Vec3 leftAt;
		anim::Decompose( both.Models()[size_t( left.hand )], leftAt, leftTurn, scale );
		b3Vec3 itemAhead = b3RotateVector( item, { 0.0f, 0.0f, -1.0f } );
		b3Vec3 handAhead = b3RotateVector( b3MulQuat( leftTurn, set->HandSocketOf( true ).rotation ), { 0.0f, 0.0f, -1.0f } );
		CHECK( b3Dot( itemAhead, handAhead ) > 0.999f );
		// ... and it is the hand's own socket (its palm) that is on the grip, not its wrist.
		CHECK( b3Distance( b3Add( leftAt, b3RotateVector( leftTurn, set->HandSocketOf( true ).position ) ), target ) < 0.002f );
		// A character says where its palms are: the grip follows.
		{
			auto palms = anim::AnimSet::CreateProcedural();
			anim::AnimSet::HandSocket palm = palms->HandSocketOf( false );
			palm.position = b3Add( palm.position, b3Vec3{ 0.0f, -0.05f, 0.0f } );
			palms->SetHandSocket( false, palm );
			anim::PoseEvaluator moved( *palms );
			grip.align = false;
			moved.Evaluate( aimingState, &grip );
			b3Vec3 shifted = b3Add( target, b3RotateVector( turn, b3Vec3{ 0.0f, -0.05f, 0.0f } ) );
			CHECK( b3Distance( position( moved.Models()[size_t( left.hand )] ), shifted ) < 0.002f );
			grip.align = true;
		}
		// As animated: no place is given. The other hand stays where the animation has it relative
		// to the carrying hand, however far the aim takes that hand.
		{
			anim::HandGrip keep;
			keep.asAnimated = true;
			AnimState level;
			anim::PoseEvaluator rest( *set );
			rest.Evaluate( level ); // not aiming: the two hands as the animation has them
			anim::HandGrip was = anim::AsAnimated( *set, rest.Models(), false );
			for ( float pitch : { 0.0f, 0.6f, -0.6f } )
			{
				AnimState aimed = aimingState;
				aimed.aimPitch = pitch;
				anim::PoseEvaluator alone( *set );
				alone.Evaluate( aimed );
				anim::PoseEvaluator together( *set );
				together.Evaluate( aimed, &keep );
				anim::HandGrip apart = anim::AsAnimated( *set, alone.Models(), false );
				anim::HandGrip now = anim::AsAnimated( *set, together.Models(), false );
				float reach = b3Length( was.position );
				std::printf( "    as animated, pitch %+.1f: the other hand %.3f m from its place without it, %.4f m with (it is %.2f m from the item)\n",
							 pitch, b3Distance( apart.position, was.position ), b3Distance( now.position, was.position ), reach );
				// (The placeholder's arms hang at its sides: its hands are further apart than an arm
				// is long once one is aimed ahead, so the other reaches as far as it can.)
				CHECK( b3Distance( now.position, was.position ) < b3Distance( apart.position, was.position ) );
				// The carrying arm is where the aim put it.
				CHECK( b3Distance( position( together.Models()[size_t( right.hand )] ), position( alone.Models()[size_t( right.hand )] ) ) < 1e-6f );
			}
			// A relation the arm can keep is kept exactly: the hands as a pose has them, and the
			// same pose again.
			anim::PoseEvaluator same( *set );
			same.Evaluate( level, &keep );
			anim::HandGrip again = anim::AsAnimated( *set, same.Models(), false );
			CHECK( b3Distance( again.position, was.position ) < 1e-3f ); // (a straight arm is kept a hair short of straight)
			CHECK( std::fabs( b3DotQuat( again.rotation, was.rotation ) ) > 0.9999f );
		}
		// The item in the left hand: the right hand reaches for it.
		anim::PoseEvaluator resting( *set );
		resting.Evaluate( AnimState{} );
		anim::Decompose( resting.Models()[size_t( left.hand )], at, turn, scale );
		item = b3MulQuat( turn, set->HandSocketOf( true ).rotation );
		itemAt = b3Add( at, b3RotateVector( turn, set->HandSocketOf( true ).position ) );
		target = b3Add( position( resting.Models()[size_t( right.upper )] ), b3Vec3{ 0.1f, -0.2f, 0.25f } );
		grip.leftCarries = true;
		grip.align = false;
		grip.position = b3RotateVector( b3Quat{ { -item.v.x, -item.v.y, -item.v.z }, item.s }, b3Sub( target, itemAt ) );
		anim::PoseEvaluator mirrored( *set );
		mirrored.Evaluate( AnimState{}, &grip );
		CHECK( b3Distance( position( mirrored.Models()[size_t( right.hand )] ), target ) < 0.002f );
		CHECK( b3Distance( position( mirrored.Models()[size_t( left.hand )] ), position( resting.Models()[size_t( left.hand )] ) ) < 1e-6f );
	}

	// Legs turned toward the direction of travel: the hips and legs turn, the upper body does not.
	{
		AnimState strafing;
		strafing.legYaw = 1.2f;
		anim::PoseEvaluator turned( *set );
		turned.Evaluate( strafing );
		int leftFoot = present::FindJoint( *set, "LeftFoot" );
		int head = present::FindJoint( *set, "Head" );
		int leftHand = present::FindJoint( *set, "LeftHand" );
		CHECK( b3Distance( position( turned.Models()[size_t( leftFoot )] ), position( eval.Models()[size_t( leftFoot )] ) ) > 0.05f );
		CHECK( b3Distance( position( turned.Models()[size_t( head )] ), position( eval.Models()[size_t( head )] ) ) < 1e-4f );
		CHECK( b3Distance( position( turned.Models()[size_t( leftHand )] ), position( eval.Models()[size_t( leftHand )] ) ) < 1e-4f );
	}

	// A chain: the chest leans part of the way, the arm still ends up exactly on the line.
	{
		std::string warnings;
		auto chained = anim::AnimSet::CreateProcedural();
		chained->SetAim( "UpperChest:0.4 RightUpperArm:1", "RightHand", warnings );
		CHECK( warnings.empty() );
		CHECK( chained->AimJoints().size() == 2 );
		anim::PoseEvaluator chainPose( *chained );
		aiming.aiming = 1;
		chainPose.Evaluate( aiming );
		b3CosSin p = detmath::CosSin( 0.3f );
		b3CosSin y = detmath::CosSin( 0.5f );
		b3Vec3 look = { y.sine * p.cosine, p.sine, y.cosine * p.cosine };
		int head = present::FindJoint( *chained, "Head" );
		b3Vec3 arm = b3Normalize( b3Sub( position( chainPose.Models()[size_t( hand )] ), position( chainPose.Models()[size_t( shoulder )] ) ) );
		CHECK( b3Dot( arm, look ) > 0.999f );
		CHECK( b3Distance( position( chainPose.Models()[size_t( head )] ), position( eval.Models()[size_t( head )] ) ) > 0.01f );
		chained->SetAim( "Nope:1", "RightHand", warnings );
		CHECK( warnings.find( "Nope" ) != std::string::npos );
		CHECK( chained->AimJoints().empty() );
	}

	// A ragdoll at rest, facing +Z at the origin, is the rig at rest.
	present::RagdollRig rig = present::BuildRagdollRig( *set );
	Transform parts[kRagdollParts];
	for ( int i = 0; i < kRagdollParts; ++i )
	{
		parts[i] = { ragdoll::kParts[i].center, b3Quat_identity };
	}
	Transform frame = present::RagdollFrame( parts[ragdoll::Pelvis], 0.0f );
	CHECK( b3Length( frame.position ) < 1e-5f );
	present::Models models;
	present::RagdollModels( rig, parts, frame, models );
	float worst = 0.0f;
	for ( size_t j = 0; j < models.size(); ++j )
	{
		worst = std::max( worst, b3Distance( position( models[j] ), position( set->RestModels()[j] ) ) );
	}
	std::printf( "    ragdoll at rest: worst joint off by %.4f m\n", worst );
	CHECK( worst < 1e-4f );

	// Turned and moved, the frame carries the whole pose along: same model-space result.
	b3Quat turn = b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, 1.1f );
	b3Vec3 at = { 4.0f, 2.0f, -3.0f };
	for ( int i = 0; i < kRagdollParts; ++i )
	{
		parts[i] = { b3Add( at, b3RotateVector( turn, ragdoll::kParts[i].center ) ), turn };
	}
	frame = present::RagdollFrame( parts[ragdoll::Pelvis], 1.1f );
	present::Models moved;
	present::RagdollModels( rig, parts, frame, moved );
	worst = 0.0f;
	for ( size_t j = 0; j < models.size(); ++j )
	{
		worst = std::max( worst, b3Distance( position( moved[j] ), position( models[j] ) ) );
	}
	CHECK( worst < 1e-4f );
}

// Conditions and formats presentation evaluates against the mods' board, by name.
void TestFields()
{
	ModSchema schema;
	schema.fields.push_back( { "pistol.ammo", BoardType::Int, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "combat.dead", BoardType::Bool, BoardScope::Entity, 1 } );
	schema.fields.push_back( { "round.time", BoardType::Float, BoardScope::Global, 0 } );
	Blackboard board;
	board.values[0] = 3;
	board.values[1] = 0;
	BoardValues globalValues;
	globalValues[0] = BoardFromFloat( 12.5f );
	const BoardValues* globals = &globalValues;
	auto check = [&]( const char* condition ) { return present::CheckCondition( schema, condition, &board, globals ); };

	CHECK( check( "pistol.ammo" ) );
	CHECK( check( "pistol.ammo > 2" ) && check( "pistol.ammo >= 3" ) && check( "pistol.ammo == 3" ) );
	CHECK( check( "pistol.ammo < 3" ) == false && check( "pistol.ammo != 3" ) == false );
	CHECK( check( "!combat.dead" ) && check( "combat.dead == false" ) );
	CHECK( check( "round.time > 12" ) && check( "round.time < 13" ) );
	CHECK( check( "?pistol.ammo" ) && check( "?nope" ) == false );
	CHECK( check( "!?nope" ) && check( "!?pistol.ammo" ) == false );
	// Not declared reads as zero, so bindings for a mod that is not running never match.
	CHECK( check( "nope" ) == false && check( "!nope" ) );
	CHECK( present::CheckConditions( schema, {}, &board, globals ) );
	CHECK( present::CheckConditions( schema, { "pistol.ammo > 0", "!combat.dead" }, &board, globals ) );
	CHECK( present::CheckConditions( schema, { "pistol.ammo > 0", "combat.dead" }, &board, globals ) == false );
	CHECK( present::FormatFields( schema, "AMMO {pistol.ammo} {{ {round.time} {combat.dead}", &board, globals ) == "AMMO 3 { 12.5 no" );
	// Nothing published yet (no board) reads as zero too.
	CHECK( present::CheckCondition( schema, "pistol.ammo == 0", nullptr, globals ) );
	// The one expression language: logic, arithmetic, a field against a field; and values.
	CHECK( check( "pistol.ammo > 0 and not combat.dead" ) && check( "combat.dead or pistol.ammo == 3" ) );
	CHECK( check( "pistol.ammo * 4 < round.time" ) && check( "pistol.ammo + 10 < round.time" ) == false );
	CHECK( check( "pistol.ammo >" ) == false ); // what does not parse is not true
	{
		float value = 0.0f;
		CHECK( present::EvaluateFields( schema, "round.time * 2 - pistol.ammo", &board, globals, value ) && value == 22.0f );
		CHECK( present::EvaluateFields( schema, "round.time *", &board, globals, value ) == false );
	}
	CHECK( present::FormatFields( schema, "{pistol.ammo * 2} / {round.time / 2} / {pistol.ammo > 2}", &board, globals ) == "6 / 6.2 / 1" );

	// A private field: its value for the viewer's own player, zero for anyone else (declared all the
	// same, so "?name" still says its mod runs).
	schema.fields.push_back( { "cards.role", BoardType::Int, BoardScope::Private, 2 } );
	Blackboard privates;
	privates.values[2] = 7;
	CHECK( present::ReadField( schema, "cards.role", &board, globals, &privates ).raw == 7 );
	CHECK( present::ReadField( schema, "cards.role", &board, globals ).raw == 0 );
	CHECK( present::ReadField( schema, "cards.role", &board, globals ).declared );
	CHECK( present::CheckCondition( schema, "cards.role == 7", &board, globals, nullptr, &privates ) );
	CHECK( present::CheckCondition( schema, "cards.role == 7", &board, globals ) == false );
	CHECK( present::CheckConditions( schema, { "cards.role > 6", "pistol.ammo > 0" }, &board, globals, &privates ) );
	CHECK( present::FormatFields( schema, "role {cards.role}, ammo {pistol.ammo}", &board, globals, &privates ) == "role 7, ammo 3" );
	CHECK( present::FormatFields( schema, "role {cards.role}", &board, globals ) == "role 0" );
	// The board's slot 2 is another field's: a private field never reads the entity's board.
	board.values[2] = 99;
	CHECK( present::ReadField( schema, "cards.role", &board, globals ).raw == 0 );
	// A state machine cannot read one: the simulation does not have it.
	{
		AnimExpr expr;
		std::string error, warnings;
		CHECK( CompileAnimExpr( "cards.role > 0", schema, expr, error, warnings ) );
		CHECK( warnings.find( "private" ) != std::string::npos );
	}
}

// The one expression language: the grammar every reader shares (state machines, HUD nodes, reactions).
void TestExpr()
{
	std::unordered_map<std::string, float> known = { { "a", 3.0f }, { "b", 4.0f }, { "zero", 0.0f }, { "combat.health", 40.0f },
													   { "combat.max_health", 80.0f }, { "^^:combat.dead", 1.0f }, { "$other/Head:x", 2.0f } };
	auto read = [&]( const std::string& name, float& value ) {
		auto found = known.find( name );
		if ( found == known.end() )
		{
			return false;
		}
		value = found->second;
		return true;
	};
	auto value = [&]( const char* text ) {
		expr::Program program;
		std::string error;
		// (What does not compile gives a value no check expects.)
		return expr::Compile( text, program, error ) ? expr::Evaluate( program, read ) : -12345.0f;
	};
	auto fails = [&]( const char* text, const char* saying = nullptr ) {
		expr::Program program;
		std::string error;
		return expr::Compile( text, program, error ) == false && ( saying == nullptr || error.find( saying ) != std::string::npos );
	};

	// Numbers and names.
	CHECK( value( "3" ) == 3.0f && value( "0.25" ) == 0.25f && value( "1e-3" ) == 0.001f && value( ".5" ) == 0.5f );
	CHECK( value( "true" ) == 1.0f && value( "false" ) == 0.0f );
	CHECK( value( "a" ) == 3.0f && value( "combat.health" ) == 40.0f && value( "nope" ) == 0.0f );
	CHECK( value( "" ) == 1.0f && value( "   " ) == 1.0f ); // nothing asked: true
	// Arithmetic, with the usual precedence.
	CHECK( value( "a + b * 2" ) == 11.0f && value( "(a + b) * 2" ) == 14.0f );
	CHECK( value( "a - b - 1" ) == -2.0f && value( "b / 2 / 2" ) == 1.0f );
	CHECK( value( "-a + 1" ) == -2.0f && value( "a - -b" ) == 7.0f && value( "-(a + b)" ) == -7.0f );
	CHECK( value( "combat.health * 100 / combat.max_health" ) == 50.0f );
	CHECK( value( "a / zero" ) == 0.0f ); // never a NaN
	// Comparisons give 1 or 0, and bind looser than arithmetic.
	CHECK( value( "a < b" ) == 1.0f && value( "a >= b" ) == 0.0f && value( "a + 1 == b" ) == 1.0f && value( "a != 3" ) == 0.0f );
	CHECK( value( "combat.health < combat.max_health / 2" ) == 0.0f && value( "combat.health <= combat.max_health / 2" ) == 1.0f );
	// Logic: or, then and, then not; both spellings.
	CHECK( value( "a and b" ) == 1.0f && value( "a && zero" ) == 0.0f && value( "zero or b" ) == 1.0f && value( "zero || zero" ) == 0.0f );
	CHECK( value( "zero and zero or a" ) == 1.0f && value( "zero and (zero or a)" ) == 0.0f );
	CHECK( value( "not zero" ) == 1.0f && value( "!a" ) == 0.0f && value( "!!a" ) == 1.0f );
	CHECK( value( "!a == 3" ) == 0.0f && value( "!a == 4" ) == 1.0f ); // not (a == 3)
	CHECK( value( "not a and b" ) == 0.0f && value( "not (a and zero)" ) == 1.0f );
	// "?name": known or not, whatever its value.
	CHECK( value( "?a" ) == 1.0f && value( "?zero" ) == 1.0f && value( "?nope" ) == 0.0f && value( "!?nope" ) == 1.0f );
	CHECK( value( "?a and ?nope" ) == 0.0f );
	// Names with a path keep it, colon and all, for the reader.
	CHECK( value( "^^:combat.dead" ) == 1.0f && value( "!^^:combat.dead" ) == 0.0f && value( "$other/Head:x + 1" ) == 3.0f );
	CHECK( value( "?^^:combat.dead" ) == 1.0f && value( "?$world:nope" ) == 0.0f );
	{
		expr::Program program;
		std::string error, path, plain;
		CHECK( expr::Compile( "a < $other:combat.health and a > 1", program, error ) );
		CHECK( program.names.size() == 2 ); // a name is listed once
		expr::SplitName( "$other:combat.health", path, plain );
		CHECK( path == "$other" && plain == "combat.health" );
		expr::SplitName( "combat.health", path, plain );
		CHECK( path.empty() && plain == "combat.health" );
	}
	// What is not an expression says why.
	CHECK( fails( "a >" ) && fails( "(a" ) && fails( "a b" ) && fails( "a +* b" ) && fails( "3 4" ) && fails( "and" ) );
	CHECK( fails( "^^combat.dead", "colon" ) && fails( "$other:", "colon" ) );
	CHECK( fails( "((((((((((((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))))))))))))" ) == false );
	{
		// Deeper than the fixed stack: refused, not run.
		std::string deep = "1";
		for ( int i = 0; i < 40; ++i )
		{
			deep = "1 + (" + deep + ")";
		}
		CHECK( fails( deep.c_str(), "nested" ) );
	}

	// The state machine compiles the same text and gets the same answers: one grammar, two readers.
	{
		ModSchema schema;
		const char* same[] = { "3 + 4 * 2", "(3 + 4) * 2", "!3 == 4", "0 and 0 or 3", "not 3 and 4", "8 / 2 / 2", "1 / 0", "-(3 + 4) < -6" };
		for ( const char* text : same )
		{
			AnimExpr compiled;
			std::string error, warnings;
			CHECK( CompileAnimExpr( text, schema, compiled, error, warnings ) );
			AnimGraphInputs inputs;
			CHECK( EvaluateAnimExpr( compiled, inputs, 0.0f ) == value( text ) );
		}
		AnimExpr compiled;
		std::string error, warnings;
		CHECK( CompileAnimExpr( "speed >", schema, compiled, error, warnings ) == false );
	}
}

// Camera collision: how far a third-person camera can back away before the map is in the way.
void TestCameraCollision()
{
	using present::CameraFreeDistance;
	present::PresentationFrame frame;
	auto add = [&frame]( present::VisualKind kind, ShapeKind shape, b3Vec3 at, b3Vec3 half, b3Quat rotation = { { 0.0f, 0.0f, 0.0f }, 1.0f } ) {
		present::FrameEntity e;
		e.netId = uint32_t( frame.entities.size() + 1 );
		e.kind = kind;
		e.shape = shape;
		e.halfExtents = half;
		e.transform = { at, rotation };
		frame.entities.push_back( e );
	};
	const b3Vec3 back = { 0.0f, 0.0f, -1.0f };
	const b3Vec3 eye = { 0.0f, 1.5f, 0.0f };

	// Nothing there: the full distance.
	CHECK( CameraFreeDistance( frame, eye, back, 6.0f, 0.2f ) == 6.0f );

	// A wall 3 m behind (its face at z = -3): the camera's ball stops a radius short of it.
	add( present::VisualKind::Static, ShapeKind::Box, { 0.0f, 2.0f, -3.5f }, { 5.0f, 2.0f, 0.5f } );
	float d = CameraFreeDistance( frame, eye, back, 6.0f, 0.2f );
	std::printf( "    wall at 3 m: free %.3f m\n", d );
	CHECK( std::fabs( d - 2.8f ) < 1e-4f );
	CHECK( std::fabs( CameraFreeDistance( frame, eye, back, 6.0f, 0.0f ) - 3.0f ) < 1e-4f );
	// Closer than the wall anyway, or looking the other way: untouched.
	CHECK( CameraFreeDistance( frame, eye, back, 2.0f, 0.2f ) == 2.0f );
	CHECK( CameraFreeDistance( frame, eye, { 0.0f, 0.0f, 1.0f }, 6.0f, 0.2f ) == 6.0f );
	// Already against it: zero, never negative.
	CHECK( CameraFreeDistance( frame, { 0.0f, 1.5f, -2.9f }, back, 6.0f, 0.2f ) == 0.0f );

	// Props, players and items never block the camera.
	add( present::VisualKind::Prop, ShapeKind::Box, { 0.0f, 1.5f, -1.0f }, { 0.5f, 0.5f, 0.5f } );
	add( present::VisualKind::Player, ShapeKind::Capsule, { 0.0f, 1.5f, -1.5f }, { 0.35f, 0.55f, 0.0f } );
	CHECK( std::fabs( CameraFreeDistance( frame, eye, back, 6.0f, 0.2f ) - 2.8f ) < 1e-4f );

	// A wall turned 45 degrees about Y, 2 m away along the ray.
	frame.entities.clear();
	float half = 0.5f * 0.78539816f;
	add( present::VisualKind::Static, ShapeKind::Box, { 0.0f, 2.0f, -2.0f }, { 5.0f, 2.0f, 0.0f }, { { 0.0f, std::sin( half ), 0.0f }, std::cos( half ) } );
	d = CameraFreeDistance( frame, eye, back, 6.0f, 0.0f );
	CHECK( std::fabs( d - 2.0f ) < 1e-3f );

	// Static spheres and capsules (map templates).
	frame.entities.clear();
	add( present::VisualKind::Static, ShapeKind::Sphere, { 0.0f, 1.5f, -4.0f }, { 1.0f, 0.0f, 0.0f } );
	CHECK( std::fabs( CameraFreeDistance( frame, eye, back, 6.0f, 0.25f ) - 2.75f ) < 1e-4f );
	frame.entities.clear();
	add( present::VisualKind::Static, ShapeKind::Capsule, { 0.0f, 1.5f, -4.0f }, { 0.5f, 1.0f, 0.0f } );
	CHECK( std::fabs( CameraFreeDistance( frame, eye, back, 6.0f, 0.0f ) - 3.5f ) < 1e-4f );
	// Over the top of the capsule's cap: a miss.
	CHECK( CameraFreeDistance( frame, { 0.0f, 3.6f, 0.0f }, back, 6.0f, 0.0f ) == 6.0f );
	// Grazing the rounded cap is later than the side.
	d = CameraFreeDistance( frame, { 0.0f, 2.8f, 0.0f }, back, 6.0f, 0.0f );
	CHECK( d > 3.5f && d < 4.0f );

	// Against the real level: for rays that the simulation says hit the map, the same distance.
	SimConfig config;
	Simulation sim( config );
	present::PresentationFrame level;
	present::CaptureFrame( sim, level );
	int compared = 0;
	float worst = 0.0f;
	for ( int i = 0; i < 64; ++i )
	{
		float yaw = 0.0981748f * float( i );
		float pitch = -0.6f + 0.02f * float( i );
		b3Vec3 dir = { std::cos( pitch ) * std::sin( yaw ), std::sin( pitch ), std::cos( pitch ) * std::cos( yaw ) };
		b3Vec3 from = { 0.5f, 2.0f, -0.5f };
		const float reach = 40.0f;
		RayHit hit;
		bool any = sim.CastRay( from, b3MulSV( reach, dir ), 0, hit, true );
		bool isStatic = false;
		for ( const present::FrameEntity& e : level.entities )
		{
			isStatic |= any && e.netId == hit.netId && e.kind == present::VisualKind::Static;
		}
		if ( isStatic == false )
		{
			continue;
		}
		float mine = CameraFreeDistance( level, from, dir, reach, 0.0f );
		worst = std::max( worst, std::fabs( mine - hit.fraction * reach ) );
		compared += 1;
	}
	std::printf( "    %d rays against the level, worst difference %.5f m\n", compared, worst );
	CHECK( compared > 20 && worst < 0.01f );
}

void TestAnimController()
{
	Simulation sim( TestConfig() );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
		}
	};
	auto state = [&]() { return sim.GetAnimState( sim.FindEntity( sim.Globals().playerNetIds[0] ) ); };

	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 90 );
	CHECK( state().mode == AnimMode::Locomotion );
	CHECK( state().groundSpeed < 0.05f );

	f.inputs[0].moveForward = 127;
	step( 90 );
	std::printf( "    walking: groundSpeed %.3f\n", state().groundSpeed );
	CHECK( b3AbsFloat( state().groundSpeed - 3.0f ) < 0.1f );
	CHECK( b3AbsFloat( state().moveForward - 3.0f ) < 0.1f && b3AbsFloat( state().moveRight ) < 0.1f );

	// Jump: JumpStart -> Fall -> Land -> Locomotion, in that order.
	f.inputs[0] = {};
	step( 30 );
	f.inputs[0].buttons = BtnJump;
	step( 1 );
	f.inputs[0].buttons = 0;
	std::vector<AnimMode> modes = { state().mode };
	for ( int i = 0; i < 120; ++i )
	{
		step( 1 );
		if ( state().mode != modes.back() )
		{
			modes.push_back( state().mode );
		}
	}
	std::printf( "    jump modes:" );
	for ( AnimMode m : modes )
	{
		std::printf( " %d", int( m ) );
	}
	std::printf( "\n" );
	CHECK( modes.size() == 4 );
	CHECK( modes[0] == AnimMode::JumpStart );
	CHECK( modes[1] == AnimMode::Fall );
	CHECK( modes[2] == AnimMode::Land );
	CHECK( modes[3] == AnimMode::Locomotion );
}

// A baked state machine, as the bake writes it: a locomotion blend space and a jump on the base
// layer, and an upper layer that punches when a mod's event says so; the punch's marker emits
// another event.
const char* const kTestGraph = "cinderbox_graph\t1\n"
							   "clip\tidle\t2\t1\n"
							   "clip\twalk\t1\t1\n"
							   "clip\trun\t0.5\t1\n"
							   "clip\tjump\t0.4\t0\n"
							   "clip\tpunch\t0.5\t0\n"
							   "marker\tpunch\t0.2\ttest.impact\n"
							   "layer\tbase\n"
							   "state\tMove\tblend\tspeed\n"
							   "point\t0\tidle\n"
							   "point\t6.5\trun\n"
							   "point\t3\twalk\n"
							   "state\tJump\tclip\tjump\t0\n"
							   "start\tMove\n"
							   "transition\tMove\tJump\t1\t0.1\timmediate\t1\tjumped\n"
							   "transition\tJump\tMove\t1\t0.1\timmediate\t1\tgrounded and state_time > 0.1\n"
							   "layer\tupper\n"
							   "mask\tSpine\n"
							   "weight\ttest.armed\n"
							   "state\tRest\tclip\tidle\t0\n"
							   "state\tPunch\tclip\tpunch\t0\n"
							   "start\tRest\n"
							   "transition\tRest\tPunch\t1\t0.05\timmediate\t1\ttest.punch\n"
							   "transition\tPunch\tRest\t1\t0.1\tat_end\t1\n";

ModSchema TestGraphSchema()
{
	ModSchema schema;
	schema.events = { "test.punch", "test.impact" };
	schema.fields.push_back( { "test.armed", BoardType::Bool, BoardScope::Entity, 0 } );
	return schema;
}
// Stowed items: carried by a player but in no hand. MoveItem puts a held item away and takes a
// carried one out; a stowed item is not what "the item in this socket" means, can be given and
// picked up straight to stowed, drops like any other, and goes when its holder leaves.
void TestStowedItems()
{
	Simulation sim( TestConfig(), FlatMap() );
	sim.SetItemShapes( { ItemShape{}, ItemShape{} } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto command = [&]( CommandType type, uint32_t target ) {
		SimCommand c;
		c.type = type;
		c.target = target;
		f.commands.push_back( c );
		return &f.commands.back();
	};
	auto items = [&]( uint32_t holder ) {
		std::vector<uint32_t> out;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
			if ( item != nullptr && item->holder == holder )
			{
				out.push_back( r.netId );
			}
		}
		return out;
	};
	auto held = [&]( uint32_t netId ) { return sim.FindEntity( netId ).get<HeldItem>(); };
	const uint8_t hand = 0;
	const uint8_t back = 1;
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	uint32_t holder = sim.PlayerNetId( 0 );

	// Given stowed: carried, in no hand, and nothing was dropped for it.
	SimCommand* give = command( CommandType::SpawnItem, SlotTarget( 0 ) );
	give->index = 0;
	give->mode = kNoSocket;
	give->value = 1;
	SimCommand* inHand = command( CommandType::SpawnItem, SlotTarget( 0 ) );
	inHand->index = 1;
	inHand->mode = hand;
	step( 1 );
	std::vector<uint32_t> carried = items( holder );
	CHECK( carried.size() == 2 && items( 0 ).empty() );
	if ( carried.size() != 2 )
	{
		return;
	}
	uint32_t a = carried[0]; // stowed
	uint32_t b = carried[1]; // in the hand
	CHECK( held( a ).stowed == 1 && held( a ).socket == kNoSocket && held( b ).stowed == 0 );
	CHECK( sim.HeldItemOf( holder, hand ) == b && sim.HeldItemOf( holder, kNoSocket ) == 0 );

	// Taking the stowed one out while the hand is full does nothing...
	command( CommandType::MoveItem, a )->mode = hand;
	step( 1 );
	CHECK( held( a ).stowed == 1 && sim.HeldItemOf( holder, hand ) == b );
	// ...stow the other first, in the same frame, and they change places: on the back, in the hand.
	SimCommand* stow = command( CommandType::MoveItem, b );
	stow->mode = back;
	stow->value = 1;
	command( CommandType::MoveItem, a )->mode = hand;
	step( 1 );
	CHECK( sim.HeldItemOf( holder, hand ) == a && held( b ).stowed == 1 && held( b ).socket == back );
	CHECK( sim.HeldItemOf( holder, back ) == 0 ); // a holster is not a hand
	CHECK( items( holder ).size() == 2 && items( 0 ).empty() );
	// A new item into the hand drops what the hand held, never what is stowed.
	uint64_t before = sim.ComputeHash();
	Snapshot snap;
	sim.Save( snap );
	SimCommand* third = command( CommandType::SpawnItem, SlotTarget( 0 ) );
	third->index = 1;
	third->mode = hand;
	step( 1 );
	CHECK( items( 0 ).size() == 1 && items( 0 )[0] == a && held( b ).holder == holder );
	uint64_t after = sim.ComputeHash();
	sim.Load( snap );
	CHECK( sim.ComputeHash() == before ); // stowed state is part of the snapshot
	third = command( CommandType::SpawnItem, SlotTarget( 0 ) );
	third->index = 1;
	third->mode = hand;
	step( 1 );
	CHECK( sim.ComputeHash() == after );

	// From the world straight to stowed, with the hand full.
	SimCommand* pick = command( CommandType::PickUpItem, SlotTarget( 0 ) );
	pick->other = a;
	pick->mode = kNoSocket;
	pick->value = 1;
	step( 1 );
	CHECK( held( a ).holder == holder && held( a ).stowed == 1 && sim.FindEntity( a ).has<PhysicsBody>() == false );
	CHECK( items( holder ).size() == 3 && items( 0 ).empty() );
	// A stowed item drops like any other, and is in use by nobody afterwards.
	command( CommandType::DropItem, b )->a = { 2.0f, 1.0f, 0.0f };
	step( 1 );
	CHECK( held( b ).holder == 0 && held( b ).stowed == 0 && sim.FindEntity( b ).has<PhysicsBody>() );
	// Moving an item that lies in the world does nothing.
	command( CommandType::MoveItem, b )->mode = hand;
	step( 1 );
	CHECK( held( b ).holder == 0 );

	// Leaving takes everything carried along, stowed too; what lies in the world stays.
	f.events.push_back( { PlayerEventType::Leave, 0 } );
	step( 1 );
	CHECK( sim.FindEntity( a ).is_valid() == false && items( holder ).empty() );
	CHECK( items( 0 ).size() == 1 && items( 0 )[0] == b );
}


// Directional clips in a 2D blend space, as Godot triangulates it (idle in the middle, four
// directions around), and the body-frame velocity that drives it.
// Held items: entities of their own, spawned into a player's socket, addressed by holder and
// socket before anyone knows their NetId, following their holder, gone with it.
// Items in the world: spawned on the floor they fall and rest; picked up they lose their body and
// follow the hand; dropped they get it back. All of it rolls back and replays exactly.
void TestWorldItems()
{
	Simulation sim( TestConfig(), FlatMap() );
	ItemShape bat;
	bat.half = { 0.04f, 0.04f, 0.36f };
	bat.center = { 0.0f, 0.0f, -0.36f };
	bat.mass = 1.2f;
	sim.SetItemShapes( { bat } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto command = [&]( CommandType type, uint32_t target ) {
		SimCommand c;
		c.type = type;
		c.target = target;
		f.commands.push_back( c );
		return &f.commands.back();
	};
	auto lying = [&]() {
		std::vector<uint32_t> out;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
			if ( item != nullptr && item->holder == 0 )
			{
				out.push_back( r.netId );
			}
		}
		return out;
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	uint32_t holder = sim.PlayerNetId( 0 );

	// Spawned in the world (no holder): a body at its grip, turned a little, falling.
	SimCommand* spawn = command( CommandType::SpawnItem, 0 );
	spawn->a = { 3.0f, 2.0f, 0.0f };
	spawn->c = { 0.0f, 0.3826834f, 0.0f }; // 45 degrees about Y
	step( 1 );
	std::vector<uint32_t> world = lying();
	CHECK( world.size() == 1 );
	uint32_t item = world.empty() ? 0 : world[0];
	CHECK( sim.FindEntity( item ).has<PhysicsBody>() );
	Snapshot midFall;
	sim.Save( midFall );
	step( 90 );
	uint64_t landed = sim.ComputeHash();
	float restY = sim.EntityTransform( item )->position.y;
	CHECK( restY > 0.0f && restY < 0.2f ); // lying on the floor, its body's centre a few cm up
	sim.Load( midFall );
	step( 90 );
	CHECK( sim.ComputeHash() == landed ); // the fall replays exactly

	// Picked up: no body, in the hand, following the holder.
	SimCommand* pick = command( CommandType::PickUpItem, SlotTarget( 0 ) );
	pick->other = item;
	pick->mode = kSocketRightHand;
	step( 1 );
	CHECK( sim.HeldItemOf( holder, kSocketRightHand ) == item );
	CHECK( sim.FindEntity( item ).has<PhysicsBody>() == false && lying().empty() );
	// A second pick-up of a held item does nothing.
	SimCommand* twice = command( CommandType::PickUpItem, SlotTarget( 0 ) );
	twice->other = item;
	twice->mode = kSocketLeftHand;
	step( 1 );
	CHECK( sim.HeldItemOf( holder, kSocketLeftHand ) == 0 );

	// Dropped: back in the world where the command says, thrown.
	SimCommand* drop = command( CommandType::DropItem, ItemTarget( 0, kSocketRightHand ) );
	drop->a = { 0.0f, 1.5f, -1.0f };
	drop->b = { 0.0f, 1.0f, -3.0f };
	step( 1 );
	CHECK( sim.HeldItemOf( holder, kSocketRightHand ) == 0 && lying().size() == 1 );
	CHECK( sim.EntityTransform( item )->position.z < -1.0f ); // moving away along -Z
	// A rotation that is too long is made a unit one; a NaN one refuses the command.
	SimCommand* odd = command( CommandType::SpawnItem, 0 );
	odd->a = { -2.0f, 1.0f, 0.0f };
	odd->c = { 3.0f, 0.0f, 4.0f };
	SimCommand* broken = command( CommandType::SpawnItem, 0 );
	broken->a = { -3.0f, 1.0f, 0.0f };
	broken->c = { 0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f };
	step( 60 );
	CHECK( lying().size() == 2 );
	for ( uint32_t id : lying() )
	{
		b3Vec3 at = sim.EntityTransform( id )->position;
		CHECK( std::isfinite( at.x ) && std::isfinite( at.y ) && std::isfinite( at.z ) );
	}
}

// Which mod events are news to a viewer that predicts: told by what an event is, not by where it is
// in the ring, because the server's events of a tick arrive after the viewer's own and before it.
void TestEventNews()
{
	auto record = []( uint32_t tick, uint16_t type, uint32_t a, uint32_t b = 0 ) {
		ModEventRecord r;
		r.tick = tick;
		r.type = type;
		r.netIdA = a;
		r.netIdB = b;
		return r;
	};
	const uint16_t used = 1, fired = 2, hit = 3;
	ModEventRecord ring[kModEventHistory] = {};
	uint32_t count = 0;
	auto put = [&]( std::initializer_list<ModEventRecord> events ) {
		for ( const ModEventRecord& e : events )
		{
			ring[count % kModEventHistory] = e;
			count += 1;
		}
	};
	auto types = [&]( const std::vector<uint32_t>& places ) {
		std::vector<uint16_t> out;
		for ( uint32_t at : places )
		{
			out.push_back( ring[at].type );
		}
		return out;
	};
	present::ModEventNews news;
	// A new world: what is in the ring already is not news.
	put( { record( 5, hit, 9 ) } );
	CHECK( news.Sync( ring, count, 10, true ).empty() );
	CHECK( news.Sync( ring, count, 10, false ).empty() );

	// The viewer clicks: its own simulation has the click's event at once, alone.
	put( { record( 20, used, 7, 70 ) } );
	CHECK( types( news.Sync( ring, count, 20, false ) ) == std::vector<uint16_t>{ used } );
	CHECK( news.Sync( ring, count, 21, false ).empty() );

	// The server's frame for that tick arrives: the tick is simulated again, and now the commands'
	// events come first. What the click answers with is news; the click is not, though it moved.
	count -= 1;
	put( { record( 20, fired, 7 ), record( 20, hit, 7, 8 ), record( 20, used, 7, 70 ) } );
	CHECK( types( news.Sync( ring, count, 24, false ) ) == ( std::vector<uint16_t>{ fired, hit } ) );
	CHECK( news.Sync( ring, count, 25, false ).empty() );

	// A rollback that takes an event out of the ring for a moment and brings it back does not show
	// it twice...
	count -= 1;
	CHECK( news.Sync( ring, count, 25, false ).empty() );
	put( { record( 20, used, 7, 70 ) } );
	CHECK( news.Sync( ring, count, 26, false ).empty() );
	// ...and two that are the same on one tick (two pellets on one target) are two.
	put( { record( 30, hit, 7, 8 ), record( 30, hit, 7, 8 ) } );
	CHECK( news.Sync( ring, count, 30, false ).size() == 2 );
	put( { record( 30, hit, 7, 8 ) } );
	CHECK( news.Sync( ring, count, 30, false ).size() == 1 );

	// More than the ring holds since the last look: what is still there is shown, once.
	for ( uint32_t i = 0; i < kModEventHistory + 5; ++i )
	{
		put( { record( 40 + i, fired, 100 + i ) } );
	}
	CHECK( news.Sync( ring, count, 90, false ).size() == kModEventHistory );
	CHECK( news.Sync( ring, count, 91, false ).empty() );
}

// Using an item: any copy from the hand (selected, then the use button), and a kind with its own key
// (ItemShape::useAction, a mod action) by that key where it is. Either way the simulation records
// the kind's own event on that tick.
void TestItemUse()
{
	SimConfig config = TestConfig();
	config.slots = 3;
	config.slotHand = 0;
	ItemShape gun, potion, rock;
	gun.slot = 1;
	gun.usedEvent = 4; // "gun.used" in a schema
	potion.slot = 2;
	potion.use = kItemUsePress; // once a press
	potion.useAction = 0;		// and its own key, the action of bit 0
	potion.usedEvent = 5;
	rock.slot = 3; // used like a tool, and nobody asked to hear of it

	Simulation sim( config );
	sim.SetItemShapes( { gun, potion, rock } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto intent = [&]( SlotIntent what, uint8_t a ) {
		f.inputs[0].intent = uint8_t( what );
		f.inputs[0].intentA = a;
		f.inputs[0].intentSeq = uint8_t( f.inputs[0].intentSeq + 1 );
	};
	// The events of the tick just simulated.
	auto events = [&]( uint16_t type ) {
		int n = 0;
		for ( uint32_t i = 0; i < sim.Globals().modEventCount; ++i )
		{
			const ModEventRecord& e = sim.Globals().modEvents[i];
			n += e.type == type && e.tick + 1 == sim.Tick() ? 1 : 0;
		}
		return n;
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	for ( uint16_t kind : { uint16_t( 0 ), uint16_t( 1 ), uint16_t( 2 ) } )
	{
		SimCommand c;
		c.type = CommandType::SpawnItem;
		c.target = SlotTarget( 0 );
		c.index = kind;
		c.mode = kNoSocket;
		c.value = 1;
		f.commands.push_back( c );
	}
	step( 1 );
	uint32_t gunItem = sim.SlotItem( p0, 0 );
	uint32_t potionItem = sim.SlotItem( p0, 1 );
	CHECK( gunItem != 0 && potionItem != 0 && sim.SlotItem( p0, 2 ) != 0 );

	// The use button with empty hands: nothing.
	f.inputs[0].buttons = BtnUse;
	step( 1 );
	CHECK( events( 4 ) == 0 && events( 5 ) == 0 );
	f.inputs[0].buttons = 0;
	step( 1 );

	// The tool: its key takes it out (that is not a use), then the button uses it, once a press.
	intent( SlotIntent::Select, 0 );
	step( 1 );
	CHECK( sim.SelectedSlot( p0 ) == 0 && events( 4 ) == 0 );
	f.inputs[0].buttons = BtnUse;
	step( 1 );
	CHECK( events( 4 ) == 1 );
	for ( uint32_t i = 0; i < sim.Globals().modEventCount; ++i )
	{
		const ModEventRecord& e = sim.Globals().modEvents[i];
		if ( e.type == 4 && e.tick + 1 == sim.Tick() )
		{
			// At the player, with the item as the other entity.
			CHECK( e.netIdA == p0 && e.netIdB == gunItem );
		}
	}
	step( 5 ); // held: still one use
	CHECK( events( 4 ) == 0 );
	f.inputs[0].buttons = 0;
	step( 1 );

	// The consumable: its own key uses it where it is. The gun stays in the hand.
	f.inputs[0].actions = 1;
	step( 1 );
	CHECK( events( 5 ) == 1 && sim.SelectedSlot( p0 ) == 0 && sim.HeldItemOf( p0, 0 ) == gunItem );
	CHECK( sim.FindEntity( potionItem ).get<HeldItem>().stowed == 1 );
	// Held: still one use.
	step( 3 );
	CHECK( events( 5 ) == 0 );
	f.inputs[0].actions = 0;
	step( 1 );
	// And the use button uses what is in the hand: the gun.
	f.inputs[0].buttons = BtnUse;
	step( 1 );
	CHECK( events( 5 ) == 0 && events( 4 ) == 1 );
	f.inputs[0].buttons = 0;
	step( 1 );
	// Selected, its slot's key takes it out like anything else (that is not a use), and the use
	// button uses it, once a press.
	intent( SlotIntent::Select, 1 );
	step( 1 );
	CHECK( sim.SelectedSlot( p0 ) == 1 && events( 5 ) == 0 );
	f.inputs[0].buttons = BtnUse;
	step( 1 );
	CHECK( events( 5 ) == 1 );
	step( 3 );
	CHECK( events( 5 ) == 0 );
	f.inputs[0].buttons = 0;
	step( 1 );

	// A kind nobody listens for is used all the same (its mod is told), and records nothing.
	intent( SlotIntent::Select, 2 );
	step( 1 );
	uint32_t before = sim.Globals().modEventCount;
	f.inputs[0].buttons = BtnUse;
	step( 1 );
	CHECK( sim.SelectedSlot( p0 ) == 2 && sim.Globals().modEventCount <= before + 0u + ( events( 4 ) + events( 5 ) == 0 ? 0u : 1u ) && events( 4 ) == 0 &&
		   events( 5 ) == 0 );
	f.inputs[0].buttons = 0;
	step( 1 );

	// A frozen player uses nothing.
	SimCommand freeze;
	freeze.type = CommandType::Freeze;
	freeze.target = SlotTarget( 0 );
	freeze.mode = 1;
	f.commands.push_back( freeze );
	step( 1 );
	f.inputs[0].actions = 1;
	step( 1 );
	CHECK( events( 5 ) == 0 );
}

// Slots: what a player carries, in numbered places. Which slot is selected, and moving and dropping,
// are in the player's input, so every simulation carries them out: here one simulation, its
// snapshot, and a second run of the same inputs.
// The keys (held; the simulation sees the presses). A slot's key (PlayerInput::slotKeys) selects it,
// whatever it holds (again: empty hands). A kind's own key (ItemShape::useAction, a mod action)
// uses the first copy carried where it is: one used once a press records its ".used" once a press
// and changes no hands, one used while held is in use for as long as the key is down
// ("using.<kind>"). From the hand, the use button does the same.
void TestSlotKeys()
{
	ModSchema schema;
	schema.itemKinds = { "t.tool", "t.dash", "t.jet" };
	schema.events = { "t.dash.used", "t.jet.used" };
	schema.fields.push_back( { "t.dashes", BoardType::Int, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "t.flying", BoardType::Float, BoardScope::Entity, 1 } );
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tDash\nwhen\tevent\tt.dash.used\nchange\tt.dashes\t+=\t1\n"
							 "motion\tJet\nwhen\twhile\nif\tusing.t.jet\nchange\tt.flying\t+=\t60\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() );
	if ( motions == nullptr )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}

	SimConfig config = TestConfig();
	config.slots = 3;
	config.slotHand = 0;
	ItemShape tool, dash, jet;
	tool.slot = 1;
	dash.slot = 2;
	dash.use = kItemUsePress;
	dash.useAction = 1; // the action of bit 1
	dash.usedEvent = 0;
	jet.slot = 3;
	jet.use = kItemUseHold;
	jet.useAction = 2;
	jet.usedEvent = 1;
	Simulation sim( config, FlatMap() );
	sim.SetItemShapes( { tool, dash, jet } );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	for ( uint16_t kind : { 0, 1, 2 } )
	{
		SimCommand c;
		c.type = CommandType::SpawnItem;
		c.target = SlotTarget( 0 );
		c.index = kind;
		c.mode = kNoSocket;
		c.value = 1;
		f.commands.push_back( c );
	}
	step( 5 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	auto press = [&]( uint16_t keys, int ticks ) {
		f.inputs[0].slotKeys = keys;
		step( ticks );
		f.inputs[0].slotKeys = 0;
		step( 2 );
	};
	auto own = [&]( ActionBits keys, int ticks ) {
		f.inputs[0].actions = keys;
		step( ticks );
		f.inputs[0].actions = 0;
		step( 2 );
	};
	auto dashes = [&] { return sim.BoardValue( p0, 0 ); };
	auto flying = [&] { return BoardToFloat( sim.BoardValue( p0, 1 ) ); };

	CHECK( sim.SelectedSlot( p0 ) == kNoSlot );
	// The tool's key takes it out, and again puts it away. Held, it does it once.
	press( 1, 10 );
	CHECK( sim.SelectedSlot( p0 ) == 0 );
	press( 1, 1 );
	CHECK( sim.SelectedSlot( p0 ) == kNoSlot );
	press( 1, 1 );
	// The dash's own key: one dash a press, however long it is held; the tool stays in the hand.
	own( 2, 20 );
	own( 2, 1 );
	std::printf( "    two presses of the dash's key: %d dashes; the hand still has slot %d\n", int( dashes() ), int( sim.SelectedSlot( p0 ) ) );
	CHECK( dashes() == 2 && sim.SelectedSlot( p0 ) == 0 );
	// The jet's own key: in use while it is held (half a second), and not after.
	own( 4, 30 );
	float flown = flying();
	step( 30 );
	std::printf( "    the jet held half a second: %.1f (and %.1f after)\n", flown, flying() );
	CHECK( flown > 25.0f && flown < 35.0f && flying() == flown && sim.SelectedSlot( p0 ) == 0 );
	// Two keys at once: both, in the same tick.
	own( 2 | 4, 1 );
	CHECK( dashes() == 3 && flying() > flown );
	// The dash's slot's key selects the dash, and the use button uses it from the hand.
	press( 2, 1 );
	CHECK( sim.SelectedSlot( p0 ) == 1 && dashes() == 3 );
	f.inputs[0].buttons = BtnUse;
	step( 10 );
	f.inputs[0].buttons = 0;
	step( 2 );
	CHECK( dashes() == 4 );
	// A slot is a place: the tool moved to the jet's slot and the jet to the tool's, each key
	// selects what is there now, and the jet's own key still finds the jet.
	f.inputs[0].intent = uint8_t( SlotIntent::Move );
	f.inputs[0].intentA = 0;
	f.inputs[0].intentB = 2;
	f.inputs[0].intentSeq = uint8_t( f.inputs[0].intentSeq + 1 );
	step( 2 );
	press( 4, 1 );
	CHECK( sim.SelectedSlot( p0 ) == 2 && sim.FindEntity( sim.SlotItem( p0, 2 ) ).get<HeldItem>().kind == 0 );
	float before = flying();
	own( 4, 10 );
	CHECK( flying() > before && sim.SelectedSlot( p0 ) == 2 );
	// The dash gone (a grenade thrown, waiting for the next): its slot's key empties the hands, and
	// its own key does nothing.
	SimCommand gone;
	gone.type = CommandType::Destroy;
	gone.target = sim.SlotItem( p0, 1 );
	f.commands.push_back( gone );
	step( 2 );
	own( 2, 1 );
	press( 2, 1 );
	std::printf( "    the dash gone: %d dashes; its slot's key selects slot %d\n", int( dashes() ), int( sim.SelectedSlot( p0 ) ) );
	CHECK( sim.SlotItem( p0, 1 ) == 0 && sim.SelectedSlot( p0 ) == 1 && dashes() == 4 );
	// A kind its slot's key uses (ItemShape::slotKeyUses): the jet, now in slot 1. Its key held is
	// the jet in use, and the slot is never selected.
	jet.slotKeyUses = 1;
	sim.SetItemShapes( { tool, dash, jet } );
	float beforeKey = flying();
	press( 1, 30 );
	std::printf( "    the jet's slot key held half a second: %.1f more; the hand still has slot %d\n", flying() - beforeKey,
				 int( sim.SelectedSlot( p0 ) ) );
	CHECK( flying() - beforeKey > 25.0f && flying() - beforeKey < 35.0f && sim.SelectedSlot( p0 ) == 1 );
}

// Two of a kind are two items: each in a slot of its own, each used from the hand, each with its own
// state. The bat that goes to slot 2 (ItemShape::slot) goes to the first free slot when slot 2 has
// one already; moved anywhere, it is taken out and used as well. A kind's own key uses the selected
// copy first, else the lowest slot's.
void TestTwoOfAKind()
{
	SimConfig config = TestConfig();
	config.slots = 8;
	config.slotHand = 0;
	ItemShape bat, frag;
	bat.slot = 2;
	bat.usedEvent = 0;
	frag.slot = 6;
	frag.use = kItemUsePress;
	frag.useAction = 0;
	frag.usedEvent = 1;
	Simulation sim( config, FlatMap() );
	sim.SetItemShapes( { bat, frag } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto give = [&]( uint16_t kind ) {
		SimCommand c;
		c.type = CommandType::SpawnItem;
		c.target = SlotTarget( 0 );
		c.index = kind;
		c.mode = kNoSocket;
		c.value = 1;
		f.commands.push_back( c );
		step( 1 );
	};
	// The items used on the tick just simulated, of a kind's event.
	auto usedItems = [&]( uint16_t type ) {
		std::vector<uint32_t> out;
		for ( uint32_t i = 0; i < sim.Globals().modEventCount; ++i )
		{
			const ModEventRecord& e = sim.Globals().modEvents[i % kModEventHistory];
			if ( e.type == type && e.tick + 1 == sim.Tick() )
			{
				out.push_back( e.netIdB );
			}
		}
		return out;
	};
	auto press = [&]( uint16_t keys ) {
		f.inputs[0].slotKeys = keys;
		step( 1 );
		f.inputs[0].slotKeys = 0;
		step( 1 );
	};
	auto use = [&]( uint16_t type ) {
		f.inputs[0].buttons = BtnUse;
		step( 1 );
		std::vector<uint32_t> used = usedItems( type );
		f.inputs[0].buttons = 0;
		step( 1 );
		return used;
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 10 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	give( 0 );
	give( 0 );
	give( 1 );
	give( 1 );
	uint32_t batA = sim.SlotItem( p0, 1 ), batB = sim.SlotItem( p0, 0 );
	uint32_t fragA = sim.SlotItem( p0, 5 ), fragB = sim.SlotItem( p0, 2 );
	std::printf( "    two bats in slots 2 and 1, two grenades in slots 6 and 3: %u %u %u %u\n", batA, batB, fragA, fragB );
	CHECK( batA != 0 && batB != 0 && batA != batB && fragA != 0 && fragB != 0 && fragA != fragB );
	// Each bat from its own slot, each its own.
	press( 1 << 1 );
	CHECK( use( 0 ) == std::vector<uint32_t>{ batA } );
	press( 1 << 0 );
	CHECK( use( 0 ) == std::vector<uint32_t>{ batB } );
	// Moved to slot 7, a bat is taken out and used there.
	f.inputs[0].intent = uint8_t( SlotIntent::Move );
	f.inputs[0].intentA = 1;
	f.inputs[0].intentB = 6;
	f.inputs[0].intentSeq = uint8_t( f.inputs[0].intentSeq + 1 );
	step( 2 );
	press( 1 << 6 );
	CHECK( sim.SelectedSlot( p0 ) == 6 && use( 0 ) == std::vector<uint32_t>{ batA } );
	// A grenade from the hand: the one selected (slot 6's).
	press( 1 << 5 );
	CHECK( use( 1 ) == std::vector<uint32_t>{ fragA } );
	// Its own key: the selected one first; with a bat selected, the lowest slot's (slot 3's).
	f.inputs[0].actions = 1;
	step( 1 );
	std::vector<uint32_t> selectedFirst = usedItems( 1 );
	f.inputs[0].actions = 0;
	step( 1 );
	press( 1 << 0 );
	f.inputs[0].actions = 1;
	step( 1 );
	std::vector<uint32_t> lowest = usedItems( 1 );
	f.inputs[0].actions = 0;
	step( 1 );
	CHECK( selectedFirst == std::vector<uint32_t>{ fragA } && lowest == std::vector<uint32_t>{ fragB } && sim.SelectedSlot( p0 ) == 0 );
}

// The left hand (ItemShape::hand): a hook is there while it is carried, beside a pistol in the
// slots' hand (whose other hand is only as animated); a rifle with its other hand at a marker puts
// it away, and putting the rifle away gives it back.
// Of two such items the lower slot's is out. Using it by its own key changes nothing in the hands.
void TestLeftHand()
{
	SimConfig config = TestConfig();
	config.slots = 4;
	config.slotHand = 0;
	ItemShape pistol, rifle, hook, shield;
	pistol.slot = 1;
	pistol.grip = 3; // both hands as the animation has them
	rifle.slot = 2;
	rifle.grip = 1; // its other hand on the barrel
	hook.slot = 3;
	hook.use = kItemUsePress;
	hook.useAction = 0;
	hook.hand = kItemHandLeft;
	shield.slot = 4;
	shield.hand = kItemHandLeft;
	Simulation sim( config, FlatMap() );
	sim.SetItemShapes( { pistol, rifle, hook, shield } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 10 );
	for ( uint16_t kind : { 0, 1, 2, 3 } )
	{
		SimCommand c;
		c.type = CommandType::SpawnItem;
		c.target = SlotTarget( 0 );
		c.index = kind;
		c.mode = kNoSocket;
		c.value = 1;
		f.commands.push_back( c );
	}
	step( 3 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	auto press = [&]( uint16_t keys ) {
		f.inputs[0].slotKeys = keys;
		step( 2 );
		f.inputs[0].slotKeys = 0;
		step( 2 );
	};
	auto kindIn = [&]( uint32_t socket ) {
		uint32_t item = sim.HeldItemOf( p0, socket );
		return item != 0 ? int( sim.FindEntity( item ).get<HeldItem>().kind ) : -1;
	};
	auto report = [&]( const char* when ) {
		std::printf( "    %s: right hand %d, left hand %d\n", when, kindIn( kSocketRightHand ), kindIn( kSocketLeftHand ) );
	};
	report( "nothing selected" );
	CHECK( kindIn( kSocketRightHand ) == -1 && kindIn( kSocketLeftHand ) == 2 );
	press( 1 );
	report( "the pistol" );
	CHECK( kindIn( kSocketRightHand ) == 0 && kindIn( kSocketLeftHand ) == 2 );
	f.inputs[0].actions = 1; // the hook's own key: used where it is
	step( 2 );
	f.inputs[0].actions = 0;
	step( 2 );
	CHECK( sim.SelectedSlot( p0 ) == 0 && kindIn( kSocketRightHand ) == 0 && kindIn( kSocketLeftHand ) == 2 );
	press( 2 );
	report( "the rifle" );
	CHECK( kindIn( kSocketRightHand ) == 1 && kindIn( kSocketLeftHand ) == -1 );
	press( 2 );
	report( "the rifle put away" );
	CHECK( kindIn( kSocketRightHand ) == -1 && kindIn( kSocketLeftHand ) == 2 );
	// The shield's slot selected: it is in the left hand itself, the hook put away.
	press( 8 );
	report( "the shield's slot" );
	CHECK( kindIn( kSocketRightHand ) == -1 && kindIn( kSocketLeftHand ) == 3 );
}

// Thrown from the hand (launch_carried): what is thrown is the copy the player carries, out of its
// slot, the same entity with its own fields; with none carried, nothing is. And the release is on
// an event said after the motions ran (a motion's emit, as a throw clip's marker is): the motion
// that waits for it hears it the next tick.
void TestThrownFromHand()
{
	ModSchema schema;
	schema.itemKinds = { "t.ball" };
	schema.events = { "t.release", "t.ball.expired" };
	schema.actions.push_back( { "throw", 0, "X" } );
	schema.fields.push_back( { "t.marks", BoardType::Int, BoardScope::Entity, 0 } );
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tWindUp\nwhen\tpress\tthrow\nemit\tt.release\n"
							 "motion\tRelease\nwhen\tevent\tt.release\nlaunch_carried\tt.ball\t10\tlook\t2\t0.8\t0.5\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 2 && motions->list[1].First().effects.size() == 1 &&
		   motions->list[1].First().effects[0].carried );
	if ( motions == nullptr || motions->list.size() != 2 )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	SimConfig config = TestConfig();
	config.slots = 2;
	config.fields = 1;
	ItemShape ball;
	ball.kind = 1;
	ball.half = { 0.1f, 0.1f, 0.1f };
	ball.center = { 0.0f, 0.0f, 0.0f };
	ball.fieldDefaults = { { 0, 7 } };
	ball.expiredEvent = 1; // ("t.ball.expired")
	Simulation sim( config, FlatMap() );
	sim.SetItemShapes( { ball } );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	SimCommand give;
	give.type = CommandType::SpawnItem;
	give.target = SlotTarget( 0 );
	give.index = 0;
	give.mode = kNoSocket;
	give.value = 1;
	f.commands.push_back( give );
	step( 2 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	uint32_t carried = sim.SlotItem( p0, 0 );
	CHECK( carried != 0 );
	// Its own field changed while it is carried: the thrown one keeps it.
	SimCommand mark;
	mark.type = CommandType::SetField;
	mark.target = carried;
	mark.index = 0;
	mark.value = 42;
	f.commands.push_back( mark );
	step( 1 );
	auto ballsInWorld = [&]() {
		int n = 0;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
			n += item != nullptr && item->holder == 0 ? 1 : 0;
		}
		return n;
	};
	// The press: the wind-up says its event after the motions ran; nothing is thrown that tick.
	f.inputs[0].actions = 1;
	step( 1 );
	const HeldItem* before = sim.FindEntity( carried ).try_get<HeldItem>();
	CHECK( before != nullptr && before->holder == p0 && ballsInWorld() == 0 );
	// The next tick the release hears it: the ball it carried is thrown.
	step( 1 );
	const HeldItem* after = sim.FindEntity( carried ).try_get<HeldItem>();
	const Velocity* v = sim.FindEntity( carried ).try_get<Velocity>();
	float speed = v != nullptr ? b3Length( v->linear ) : 0.0f;
	std::printf( "    the carried ball thrown the tick after the press: in the world %d, the same one %d, at %.1f m/s, its mark %d; its slot now %u\n",
				 ballsInWorld(), int( after != nullptr && after->holder == 0 ), speed, int( sim.BoardValue( carried, 0 ) ), sim.SlotItem( p0, 0 ) );
	CHECK( after != nullptr && after->holder == 0 && after->slot == kNoSlot && ballsInWorld() == 1 && speed > 8.0f );
	CHECK( sim.BoardValue( carried, 0 ) == 42 && sim.SlotItem( p0, 0 ) == 0 && sim.LaunchedBy( carried ) == p0 );
	// Again, with nothing carried: nothing is thrown, and nothing is made.
	f.inputs[0].actions = 0;
	step( 2 );
	f.inputs[0].actions = 1;
	step( 3 );
	CHECK( ballsInWorld() == 1 );
	// Its time is up half a second after it left: it says so (its kind's expired event), where it
	// was, with who threw it, and is gone.
	step( 30 );
	const SimGlobals& g = sim.Globals();
	bool said = false;
	for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
	{
		const ModEventRecord& e = g.modEvents[i];
		said |= e.type == 1 && e.netIdA == carried && e.netIdB == p0 && b3Length( e.point ) > 0.1f;
	}
	std::printf( "    half a second later: its time is up %d, and in the world %d\n", int( said ), ballsInWorld() );
	CHECK( said && ballsInWorld() == 0 && sim.FindEntity( carried ).is_valid() == false );
}

// Stacks (ItemShape::stack): balls given to a player go onto a stack of them with room, one entity
// with a count; what does not fit is a stack of its own in another slot. A throw of the one carried
// splits one off the stack, with the stack's fields, and the rest stays.
void TestStacks()
{
	ModSchema schema;
	schema.itemKinds = { "t.ball" };
	schema.actions.push_back( { "throw", 0, "X" } );
	schema.fields.push_back( { "t.mark", BoardType::Int, BoardScope::Entity, 0 } );
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tThrow\nwhen\tpress\tthrow\nlaunch_carried\tt.ball\t8\tlook\t2\t0.8\t0\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() );
	if ( motions == nullptr )
	{
		return;
	}
	SimConfig config = TestConfig();
	config.slots = 3;
	config.fields = 1;
	ItemShape ball;
	ball.kind = 1;
	ball.half = { 0.1f, 0.1f, 0.1f };
	ball.center = { 0.0f, 0.0f, 0.0f };
	ball.stack = 3;
	Simulation sim( config, FlatMap() );
	sim.SetItemShapes( { ball } );
	sim.SetMotions( motions );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	auto give = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			SimCommand c;
			c.type = CommandType::SpawnItem;
			c.target = SlotTarget( 0 );
			c.index = 0;
			c.mode = kNoSocket;
			c.value = 1;
			f.commands.push_back( c );
		}
		step( 2 );
	};
	// What the player carries: the count in each slot (0: empty), and how many items those are.
	auto carried = [&]( int& entities ) {
		std::array<int, 3> counts = {};
		entities = 0;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
			if ( item != nullptr && item->holder == p0 && item->slot < 3 )
			{
				counts[item->slot] = int( StackOf( *item ) );
				entities += 1;
			}
		}
		return counts;
	};
	int entities = 0;
	give( 2 );
	std::array<int, 3> two = carried( entities );
	CHECK( two[0] == 2 && two[1] == 0 && entities == 1 );
	give( 3 );
	std::array<int, 3> five = carried( entities );
	std::printf( "    two balls given: one stack of %d; three more: stacks of %d and %d (%d items)\n", two[0], five[0], five[1], entities );
	CHECK( five[0] == 3 && five[1] == 2 && five[2] == 0 && entities == 2 );
	// A mark on the first stack; a throw takes one off it, marked the same.
	uint32_t first = sim.SlotItem( p0, 0 );
	SimCommand mark;
	mark.type = CommandType::SetField;
	mark.target = first;
	mark.index = 0;
	mark.value = 9;
	f.commands.push_back( mark );
	step( 1 );
	f.inputs[0].actions = 1;
	step( 1 );
	f.inputs[0].actions = 0;
	step( 2 );
	uint32_t thrown = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
		thrown = item != nullptr && item->holder == 0 ? r.netId : thrown;
	}
	std::array<int, 3> after = carried( entities );
	std::printf( "    thrown from the first stack: one in the world (its mark %d), the stacks now %d and %d\n",
				 thrown != 0 ? int( sim.BoardValue( thrown, 0 ) ) : -1, after[0], after[1] );
	CHECK( thrown != 0 && thrown != first && sim.BoardValue( thrown, 0 ) == 9 && after[0] == 2 && after[1] == 2 && sim.SlotItem( p0, 0 ) == first );
	CHECK( sim.LaunchedBy( thrown ) == p0 && StackOf( sim.FindEntity( thrown ).get<HeldItem>() ) == 1 );

	// The inventory screen's drags (the player's intents): the second stack moved onto the first
	// goes onto it as far as it fits; half of the first moved to the empty slot is a stack of its
	// own, marked as the first is.
	uint8_t seq = 0;
	auto intent = [&]( SlotIntent what, uint8_t a, uint8_t b ) {
		f.inputs[0].intent = uint8_t( what );
		f.inputs[0].intentA = a;
		f.inputs[0].intentB = b;
		f.inputs[0].intentSeq = ++seq;
		step( 2 );
	};
	intent( SlotIntent::Move, 1, 0 );
	std::array<int, 3> merged = carried( entities );
	intent( SlotIntent::Split, 0, 2 );
	std::array<int, 3> split = carried( entities );
	uint32_t part = sim.SlotItem( p0, 2 );
	std::printf( "    the second stack dragged onto the first: %d and %d; half of the first to the third slot: %d, %d, %d (its mark %d)\n",
				 merged[0], merged[1], split[0], split[1], split[2], part != 0 ? int( sim.BoardValue( part, 0 ) ) : -1 );
	CHECK( merged[0] == 3 && merged[1] == 1 );
	CHECK( split[0] == 1 && split[1] == 1 && split[2] == 2 && part != 0 && part != first && sim.BoardValue( part, 0 ) == 9 );
}

// Containers: a chest (a template with a Container component) a command fills, opened by the player
// looking at it within reach; its slots are reached with kContainerSlotBit in the slot intents:
// moved to the player's, back into it, merged onto a stack. Walking away closes it; destroyed, what
// it held falls out of it.
void TestContainers()
{
	LevelLayout map = FlatMap();
	EntityTemplate chest;
	chest.name = "chest";
	chest.visual = "prop_heavy";
	chest.components.push_back( MakeIntComponent( "Shape", { { "kind", int32_t( ShapeKind::Box ) } } ) );
	chest.components.push_back( MakeIntComponent( "Body", { { "type", 0 } } ) ); // static
	chest.components.push_back( MakeIntComponent( "Container", { { "slots", 4 } } ) );
	map.templates.push_back( chest );
	QuantizeLayout( map );
	SimConfig config = TestConfig();
	config.slots = 3;
	ItemShape ball;
	ball.kind = 1;
	ball.half = { 0.1f, 0.1f, 0.1f };
	ball.center = { 0.0f, 0.0f, 0.0f };
	ball.stack = 5;
	ItemShape stone = ball;
	stone.stack = 0;
	Simulation sim( config, map );
	sim.SetItemShapes( { ball, stone } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	uint32_t p0 = sim.PlayerNetId( 0 );
	b3Vec3 at = sim.EntityTransform( p0 )->position;
	// The chest, 1.5 m ahead (the player looks down +Z), at the height of its eyes' line.
	uint32_t box = sim.Globals().nextNetId;
	SimCommand spawn;
	spawn.type = CommandType::SpawnProp;
	spawn.value = 0;
	spawn.a = { at.x, at.y + 0.6f, at.z + 1.5f };
	f.commands.push_back( spawn );
	step( 2 );
	CHECK( sim.FindEntity( box ).is_valid() && sim.FindEntity( box ).has<Container>() );
	// Filled by a command (a mod's): three balls (one stack) and a rock; the player has two balls.
	auto give = [&]( uint32_t target, uint16_t kind, int n ) {
		for ( int i = 0; i < n; ++i )
		{
			SimCommand c;
			c.type = CommandType::SpawnItem;
			c.target = target;
			c.index = kind;
			c.mode = kNoSocket;
			c.value = 1;
			f.commands.push_back( c );
		}
		step( 2 );
	};
	give( box, 0, 3 );
	give( box, 1, 1 );
	give( SlotTarget( 0 ), 0, 2 );
	auto count = [&]( uint32_t holder, uint8_t slot ) {
		flecs::entity item = sim.FindEntity( sim.SlotItem( holder, slot ) );
		return item.is_valid() ? int( StackOf( item.get<HeldItem>() ) ) : 0;
	};
	CHECK( count( box, 0 ) == 3 && count( box, 1 ) == 1 && count( p0, 0 ) == 2 );
	uint8_t seq = 0;
	auto intent = [&]( SlotIntent what, uint8_t a, uint8_t b ) {
		f.inputs[0].intent = uint8_t( what );
		f.inputs[0].intentA = a;
		f.inputs[0].intentB = b;
		f.inputs[0].intentSeq = ++seq;
		step( 2 );
	};
	auto open = [&] { return sim.FindEntity( p0 ).get<Slots>().open; };
	// Not open yet: its slots are not the player's to reach.
	intent( SlotIntent::Move, kContainerSlotBit | 0, 1 );
	CHECK( count( box, 0 ) == 3 && count( p0, 1 ) == 0 );
	// Looked at, opened: the balls onto the player's two (as many as fit: 5), the rock to slot 2.
	intent( SlotIntent::Open, 0, 0 );
	uint32_t opened = open();
	intent( SlotIntent::Move, kContainerSlotBit | 0, 0 );
	intent( SlotIntent::Move, kContainerSlotBit | 1, 2 );
	int balls = count( p0, 0 ), left = count( box, 0 ), rock = count( p0, 2 );
	// Half of the player's balls back into the chest's last slot.
	intent( SlotIntent::Split, 0, kContainerSlotBit | 3 );
	int back = count( box, 3 );
	std::printf( "    opened %d; the chest's balls onto the player's: %d (the chest keeps %d); the rock taken %d; half put back: %d\n",
				 int( opened == box ), balls, left, rock, back );
	CHECK( opened == box && balls == 5 && left == 0 && rock == 1 && back == 3 && count( p0, 0 ) == 2 );
	const HeldItem* inBox = sim.FindEntity( sim.SlotItem( box, 3 ) ).try_get<HeldItem>();
	CHECK( inBox != nullptr && inBox->holder == box && inBox->stowed == 1 && inBox->socket == kNoSocket );
	// Walking away closes it.
	f.inputs[0].moveForward = -127;
	step( 120 );
	f.inputs[0].moveForward = 0;
	CHECK( open() == 0 );
	// Destroyed, what it held falls out.
	SimCommand gone;
	gone.type = CommandType::Destroy;
	gone.target = box;
	f.commands.push_back( gone );
	step( 30 );
	int lying = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
		lying += item != nullptr && item->holder == 0 ? 1 : 0;
	}
	std::printf( "    walked away: closed %d; the chest destroyed: %d lying where it was\n", int( open() == 0 ), lying );
	CHECK( lying == 1 );
}

// Fast and small: a dart launched at 120 m/s (2 m a tick) is stopped by a level's wall 5 cm thick
// (continuous collision against the level is Box3D's own), but passes a loose panel as thin
// between two steps; a bolt, the same but a bullet (ItemShape::bullet: its CbItem's "bullet"),
// stops at the panel too. And an item's own hits are kept for its mod however many other things
// collide that tick: a dart dropped among a dozen crates landing on the same tick.
void TestFastLaunch()
{
	ModSchema schema;
	schema.itemKinds = { "t.dart", "t.bolt" };
	schema.actions.push_back( { "fire", 0, "X" } );
	schema.actions.push_back( { "bolt", 1, "Z" } );
	const std::string text = "cinderbox_motions\t2\n"
							 "motion\tFire\nwhen\tpress\tfire\nlaunch\tt.dart\t120\tlook\t0\t0.8\t5\n"
							 "motion\tBolt\nwhen\tpress\tbolt\nlaunch\tt.bolt\t120\tlook\t0\t0.8\t5\n";
	schema.motionSets.push_back( { "t", "t.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() );
	if ( motions == nullptr )
	{
		return;
	}
	ItemShape dart;
	dart.kind = 1;
	dart.half = { 0.03f, 0.03f, 0.03f };
	dart.center = { 0.0f, 0.0f, 0.0f };
	dart.mass = 0.05f;
	ItemShape bolt = dart;
	bolt.bullet = 1;
	// Where the dart (or the bolt) is after a second, fired down +Z at what stands 10 m ahead.
	auto fire = [&]( bool wall, bool panel, ActionBits key ) {
		LevelLayout map = FlatMap();
		if ( wall )
		{
			map.statics.push_back( { { 0.0f, 2.0f, 10.0f }, { 30.0f, 2.0f, 0.025f }, 0.0f, 0.0f } );
			QuantizeLayout( map );
		}
		Simulation sim( TestConfig(), map );
		sim.SetItemShapes( { dart, bolt } );
		sim.SetMotions( motions );
		InputFrame f;
		auto step = [&]( int n ) {
			for ( int i = 0; i < n; ++i )
			{
				f.tick = sim.Tick();
				sim.Step( f );
				f.events.clear();
				f.commands.clear();
			}
		};
		f.events.push_back( { PlayerEventType::Join, 0 } );
		step( 30 );
		b3Vec3 at = sim.EntityTransform( sim.PlayerNetId( 0 ) )->position;
		if ( panel )
		{
			SimCommand c;
			c.type = CommandType::SpawnProp;
			c.value = -1;
			c.a = { at.x, at.y + 1.2f, at.z + 10.0f };
			c.c = { 1.5f, 1.5f, 0.025f };
			f.commands.push_back( c );
		}
		step( 30 );
		f.inputs[0].actions = key;
		step( 1 );
		f.inputs[0].actions = 0;
		step( 60 );
		float furthest = -1000.0f;
		for ( const Simulation::EntityRef& r : sim.Entities() )
		{
			const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
			if ( item != nullptr && item->holder == 0 )
			{
				furthest = std::max( furthest, sim.EntityTransform( r.netId )->position.z - at.z );
			}
		}
		return furthest;
	};
	float open = fire( false, false, 1 );
	float walled = fire( true, false, 1 );
	float paneled = fire( false, true, 1 );
	float bolted = fire( false, true, 2 );
	std::printf( "    at 120 m/s, after a second: a dart in the open %.1f m, at a 5 cm wall 10 m off %.2f m, at a 5 cm loose panel %.2f m; "
				 "a bolt (a bullet) at the panel %.2f m\n",
				 open, walled, paneled, bolted );
	CHECK( open > 20.0f && walled < 10.0f && bolted < 10.5f );

	// A dart dropped among twelve crates, all landing on the same tick at the same speed: its hit
	// is kept (the ring keeps the hardest eight a tick, and an item's own first).
	{
		Simulation sim( TestConfig(), FlatMap() );
		sim.SetItemShapes( { dart, bolt } );
		InputFrame f;
		auto step = [&]( int n ) {
			for ( int i = 0; i < n; ++i )
			{
				f.tick = sim.Tick();
				sim.Step( f );
				f.events.clear();
				f.commands.clear();
			}
		};
		step( 2 );
		const float drop = 1.0f; // each falls a metre to the floor: 4.4 m/s
		for ( int i = 0; i < 12; ++i )
		{
			SimCommand c;
			c.type = CommandType::SpawnProp;
			c.value = -1;
			c.a = { -6.0f + float( i ), 0.2f + drop, 6.0f };
			c.c = { 0.2f, 0.2f, 0.2f };
			f.commands.push_back( c );
		}
		SimCommand d;
		d.type = CommandType::SpawnItem;
		d.target = 0;
		d.index = 0;
		d.a = { 0.0f, 0.03f + drop, 3.0f };
		f.commands.push_back( d );
		// (The crates are made when the physics events are applied, after the commands: M247.)
		const uint32_t dartId = sim.Globals().nextNetId;
		uint32_t dartHits = 0, mostThatTick = 0;
		uint32_t seen = sim.Globals().impactCount;
		for ( int t = 0; t < 60; ++t )
		{
			step( 1 );
			const SimGlobals& g = sim.Globals();
			uint32_t thisTick = 0;
			for ( ; seen < g.impactCount; ++seen )
			{
				const ImpactRecord& r = g.impacts[seen % kImpactHistory];
				thisTick += 1;
				dartHits += r.netIdA == dartId || r.netIdB == dartId ? 1 : 0;
			}
			mostThatTick = std::max( mostThatTick, thisTick );
		}
		std::printf( "    a dart among twelve crates landing together: its hits kept %u (the most impacts kept in a tick %u)\n", dartHits, mostThatTick );
		CHECK( dartHits >= 1 );
	}
}

// An item's own fields (ItemShape::fieldDefaults, declared by its CbItem): every new copy starts with
// its kind's values, however it is made, and keeps its own from then on.
void TestItemFields()
{
	SimConfig config = TestConfig();
	config.fields = 3;
	ItemShape gun, rock;
	gun.fieldDefaults = { { 1, 12 }, { 2, BoardFromFloat( 0.5f ) } };
	Simulation sim( config, FlatMap() );
	sim.SetItemShapes( { gun, rock } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 10 );
	auto spawn = [&]( uint16_t kind, uint32_t target, uint8_t mode, int32_t value ) {
		SimCommand c;
		c.type = CommandType::SpawnItem;
		c.target = target;
		c.index = kind;
		c.mode = mode;
		c.value = value;
		c.a = { 0.0f, 2.0f, 0.0f };
		f.commands.push_back( c );
	};
	spawn( 0, SlotTarget( 0 ), 0, 0 ); // in the hand
	spawn( 0, 0, 0, 0 );				// lying in the world
	spawn( 1, SlotTarget( 0 ), kNoSocket, 1 );
	step( 2 );
	std::vector<uint32_t> guns, rocks;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		if ( const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>() )
		{
			( item->kind == 0 ? guns : rocks ).push_back( r.netId );
		}
	}
	CHECK( guns.size() == 2 && rocks.size() == 1 );
	if ( guns.size() != 2 || rocks.size() != 1 )
	{
		return;
	}
	for ( uint32_t g : guns )
	{
		CHECK( sim.BoardValue( g, 1 ) == 12 && BoardToFloat( sim.BoardValue( g, 2 ) ) == 0.5f && sim.BoardValue( g, 0 ) == 0 );
	}
	CHECK( sim.BoardValue( rocks[0], 1 ) == 0 );
	// One copy's change is its own.
	SimCommand set;
	set.type = CommandType::SetField;
	set.target = guns[0];
	set.index = 1;
	set.value = 7;
	f.commands.push_back( set );
	step( 1 );
	std::printf( "    two copies: %d and %d rounds\n", int( sim.BoardValue( guns[0], 1 ) ), int( sim.BoardValue( guns[1], 1 ) ) );
	CHECK( sim.BoardValue( guns[0], 1 ) == 7 && sim.BoardValue( guns[1], 1 ) == 12 );
}

void TestSlots()
{
	SimConfig config = TestConfig();
	config.slots = 3;
	config.slotHand = 0; // the hand's socket
	ItemShape gun, bat, rock;
	gun.slot = 1; // the slot it goes to if that is free: the first
	gun.holster = 2;
	bat.slot = 2;
	bat.holster = 1;
	// The rock has no slot of its own: the first free one. And no holster: out of sight.

	auto run = [&]( std::vector<uint64_t>* hashes, const std::function<void( Simulation&, InputFrame&, const std::function<void( int )>& )>& body ) {
		Simulation sim( config );
		sim.SetItemShapes( { gun, bat, rock } );
		InputFrame f;
		auto step = [&]( int n ) {
			for ( int i = 0; i < n; ++i )
			{
				f.tick = sim.Tick();
				sim.Step( f );
				f.events.clear();
				f.commands.clear();
				if ( hashes != nullptr )
				{
					hashes->push_back( sim.ComputeHash() );
				}
			}
		};
		body( sim, f, step );
	};

	std::vector<uint64_t> first, second;
	for ( std::vector<uint64_t>* hashes : { &first, &second } )
	{
		run( hashes, [&]( Simulation& sim, InputFrame& f, const std::function<void( int )>& step ) {
			const bool checking = hashes == &first;
			auto give = [&]( uint16_t kind ) {
				SimCommand c;
				c.type = CommandType::SpawnItem;
				c.target = SlotTarget( 0 );
				c.index = kind;
				c.mode = kNoSocket;
				c.value = 1;
				f.commands.push_back( c );
			};
			auto intent = [&]( SlotIntent what, uint8_t a, uint8_t b = 0 ) {
				f.inputs[0].intent = uint8_t( what );
				f.inputs[0].intentA = a;
				f.inputs[0].intentB = b;
				f.inputs[0].intentSeq = uint8_t( f.inputs[0].intentSeq + 1 );
			};
			auto held = [&]( uint32_t netId ) { return sim.FindEntity( netId ).get<HeldItem>(); };
			auto kindIn = [&]( uint8_t slot ) {
				uint32_t item = sim.SlotItem( sim.PlayerNetId( 0 ), slot );
				return item != 0 ? int( held( item ).kind ) : -1;
			};
			auto lying = [&] {
				int n = 0;
				for ( const Simulation::EntityRef& r : sim.Entities() )
				{
					const HeldItem* item = flecs::entity( sim.World(), r.entity ).try_get<HeldItem>();
					n += item != nullptr && item->holder == 0 ? 1 : 0;
				}
				return n;
			};
			f.events.push_back( { PlayerEventType::Join, 0 } );
			step( 30 );
			uint32_t p0 = sim.PlayerNetId( 0 );
			if ( checking )
			{
				// A player has its slots from its first tick, nothing selected.
				CHECK( sim.FindEntity( p0 ).has<Slots>() && sim.FindEntity( p0 ).get<Slots>().count == 3 && sim.SelectedSlot( p0 ) == kNoSlot );
			}

			// Each kind goes to its slot, free; the rock to the first free one.
			give( 1 );
			give( 0 );
			give( 2 );
			step( 1 );
			if ( checking )
			{
				CHECK( kindIn( 0 ) == 0 && kindIn( 1 ) == 1 && kindIn( 2 ) == 2 );
				// All put away, each in its holster; nothing is in the hand.
				CHECK( held( sim.SlotItem( p0, 0 ) ).stowed == 1 && held( sim.SlotItem( p0, 0 ) ).socket == 2 );
				CHECK( held( sim.SlotItem( p0, 2 ) ).stowed == 1 && held( sim.SlotItem( p0, 2 ) ).socket == kNoSocket );
				CHECK( sim.HeldItemOf( p0, 0 ) == 0 && lying() == 0 );
			}

			// Selecting: the slot's item is in the hand on that tick.
			intent( SlotIntent::Select, 1 );
			step( 1 );
			uint32_t batItem = sim.SlotItem( p0, 1 );
			if ( checking )
			{
				CHECK( sim.SelectedSlot( p0 ) == 1 && sim.HeldItemOf( p0, 0 ) == batItem && held( batItem ).stowed == 0 );
			}
			// The same input again (a late packet, a guess for someone else) is the same intent: nothing.
			step( 5 );
			if ( checking )
			{
				CHECK( sim.SelectedSlot( p0 ) == 1 );
			}
			// Another slot: the bat goes to its holster, the gun comes out. Nothing is made or lost.
			intent( SlotIntent::Select, 0 );
			step( 1 );
			if ( checking )
			{
				CHECK( sim.HeldItemOf( p0, 0 ) == sim.SlotItem( p0, 0 ) && held( batItem ).stowed == 1 && held( batItem ).socket == 1 && lying() == 0 );
			}
			// The selected slot again: empty hands.
			intent( SlotIntent::Select, 0 );
			step( 1 );
			if ( checking )
			{
				CHECK( sim.SelectedSlot( p0 ) == kNoSlot && sim.HeldItemOf( p0, 0 ) == 0 );
			}
			// A slot that is not there: nothing.
			intent( SlotIntent::Select, 7 );
			step( 1 );
			if ( checking )
			{
				CHECK( sim.SelectedSlot( p0 ) == kNoSlot );
			}

			// Moving: two slots trade what they hold; the selected slot stays the selected one, so what
			// is in the hand changes with it.
			intent( SlotIntent::Select, 2 );
			step( 1 );
			intent( SlotIntent::Move, 2, 0 );
			step( 1 );
			if ( checking )
			{
				CHECK( kindIn( 0 ) == 2 && kindIn( 2 ) == 0 && kindIn( 1 ) == 1 );
				CHECK( sim.SelectedSlot( p0 ) == 2 && held( sim.HeldItemOf( p0, 0 ) ).kind == 0 );
			}

			// Dropping the selected one: it lies in the world, in no slot; the slot is empty and stays selected.
			uint32_t gunItem = sim.SlotItem( p0, 2 );
			intent( SlotIntent::Drop, kNoSlot );
			step( 1 );
			if ( checking )
			{
				CHECK( held( gunItem ).holder == 0 && held( gunItem ).slot == kNoSlot && sim.FindEntity( gunItem ).has<PhysicsBody>() );
				CHECK( lying() == 1 && sim.SlotItem( p0, 2 ) == 0 && sim.SelectedSlot( p0 ) == 2 && sim.HeldItemOf( p0, 0 ) == 0 );
			}
			// Dropping a slot by number, not the selected one.
			intent( SlotIntent::Drop, 1 );
			step( 1 );
			if ( checking )
			{
				CHECK( lying() == 2 && sim.SlotItem( p0, 1 ) == 0 && kindIn( 0 ) == 2 );
			}

			// Picked up again: its slot (the first) has the rock now, which stays there (a slot is a
			// place): into the first free one, and out into the hand.
			SimCommand take;
			take.type = CommandType::PickUpItem;
			take.target = SlotTarget( 0 );
			take.other = gunItem;
			take.mode = 0;
			f.commands.push_back( take );
			step( 1 );
			if ( checking )
			{
				CHECK( sim.SlotItem( p0, 1 ) == gunItem && sim.SelectedSlot( p0 ) == 1 && sim.HeldItemOf( p0, 0 ) == gunItem );
				CHECK( kindIn( 0 ) == 2 && lying() == 1 );
			}

			// Full: a fourth item has nowhere to go and lies on the floor.
			give( 2 );
			give( 2 );
			step( 1 );
			give( 2 );
			step( 1 );
			if ( checking )
			{
				CHECK( kindIn( 0 ) == 2 && kindIn( 2 ) == 2 && lying() == 3 );
			}

			// A snapshot has all of it: restored, the same inputs go on to the same state.
			Snapshot image;
			sim.Save( image );
			uint64_t before = sim.ComputeHash();
			intent( SlotIntent::Select, 2 );
			step( 3 );
			uint64_t after = sim.ComputeHash();
			sim.Load( image );
			if ( checking )
			{
				CHECK( sim.ComputeHash() == before && sim.SelectedSlot( p0 ) == 1 );
			}
			f.tick = sim.Tick();
			step( 3 );
			if ( checking )
			{
				CHECK( sim.ComputeHash() == after && sim.SelectedSlot( p0 ) == 2 );
			}

			// A dead player's intents wait: nothing changes hands.
			SimCommand kill;
			kill.type = CommandType::Kill;
			kill.target = SlotTarget( 0 );
			f.commands.push_back( kill );
			step( 1 );
			intent( SlotIntent::Select, 0 );
			step( 2 );
			if ( checking )
			{
				CHECK( sim.SelectedSlot( p0 ) == 2 );
			}
		} );
	}
	// The same inputs, the same states, tick for tick.
	for ( size_t i = 0; i < first.size() && i < second.size(); ++i )
	{
		if ( first[i] != second[i] )
		{
			std::printf( "    runs differ from step %zu of %zu\n", i, first.size() );
			break;
		}
	}
	std::printf( "    %zu and %zu states\n", first.size(), second.size() );
	CHECK( first.size() > 50 && first == second );
}

void TestHeldItems()
{
	Simulation sim( TestConfig(), FlatMap() );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto command = [&]( CommandType type, uint32_t target ) {
		SimCommand c;
		c.type = type;
		c.target = target;
		f.commands.push_back( c );
		return &f.commands.back();
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	f.events.push_back( { PlayerEventType::Join, 1 } );
	step( 30 );
	uint32_t holder = sim.PlayerNetId( 0 );

	// Spawn, and in the same tick set a field on it and send it an event, by holder and socket.
	SimCommand* spawn = command( CommandType::SpawnItem, SlotTarget( 0 ) );
	spawn->index = 2; // kind
	spawn->mode = kSocketRightHand;
	SimCommand* set = command( CommandType::SetField, ItemTarget( 0, kSocketRightHand ) );
	set->index = 3;
	set->value = 77;
	SimCommand* event = command( CommandType::Event, ItemTarget( 0, kSocketRightHand ) );
	event->index = 5;
	step( 1 );
	uint32_t item = sim.HeldItemOf( holder, kSocketRightHand );
	CHECK( item != 0 && item != holder );
	CHECK( sim.HeldItemOf( holder, kSocketLeftHand ) == 0 );
	flecs::entity e = sim.FindEntity( item );
	CHECK( e.get<HeldItem>().kind == 2 && e.get<HeldItem>().holder == holder );
	CHECK( sim.BoardValue( item, 3 ) == 77 );
	const ModEventRecord& last = sim.Globals().modEvents[( sim.Globals().modEventCount - 1 ) % kModEventHistory];
	CHECK( last.type == 5 && last.netIdA == item );

	// It goes where its holder goes.
	f.inputs[0].moveForward = 127;
	step( 30 );
	f.inputs[0] = {};
	b3Vec3 a = sim.EntityTransform( item )->position, b = sim.EntityTransform( holder )->position;
	CHECK( a.x == b.x && a.y == b.y && a.z == b.z );

	// Its state is part of the snapshot and the hash.
	Snapshot snapshot;
	sim.Save( snapshot );
	uint64_t hash = sim.ComputeHash();
	command( CommandType::SetField, ItemTarget( 0, kSocketRightHand ) )->value = 5;
	step( 1 );
	CHECK( sim.ComputeHash() != hash || sim.BoardValue( item, 0 ) == 5 );
	sim.Load( snapshot );
	CHECK( sim.ComputeHash() == hash && sim.HeldItemOf( holder, kSocketRightHand ) == item );

	// A new item in the same socket takes its place (the old one is dropped into the world); Destroy
	// removes one; leaving takes it along.
	SimCommand* again = command( CommandType::SpawnItem, SlotTarget( 0 ) );
	again->index = 4;
	again->mode = kSocketRightHand;
	step( 1 );
	uint32_t second = sim.HeldItemOf( holder, kSocketRightHand );
	CHECK( second != 0 && second != item && sim.FindEntity( item ).is_valid() );
	CHECK( sim.FindEntity( item ).get<HeldItem>().holder == 0 && sim.FindEntity( item ).has<PhysicsBody>() );
	SimCommand* other = command( CommandType::SpawnItem, SlotTarget( 1 ) );
	other->mode = kSocketLeftHand;
	command( CommandType::Destroy, ItemTarget( 0, kSocketRightHand ) );
	step( 1 );
	CHECK( sim.HeldItemOf( holder, kSocketRightHand ) == 0 );
	uint32_t theirs = sim.HeldItemOf( sim.PlayerNetId( 1 ), kSocketLeftHand );
	CHECK( theirs != 0 );
	f.events.push_back( { PlayerEventType::Leave, 1 } );
	step( 1 );
	CHECK( sim.FindEntity( theirs ).is_valid() == false );
	// A spawn for someone who is not there does nothing.
	command( CommandType::SpawnItem, SlotTarget( 5 ) );
	step( 1 );
	std::printf( "    items: %u, then %u replacing it; the other player's %u went with them\n", item, second, theirs );
}

// One "attack" from the server, resolved by the character's state machine: by what the player holds
// (an item kind is a condition) and by the event's value (a heavy attack).
void TestAttackResolve()
{
	const char* text = "cinderbox_graph\t1\n"
					   "clip\tidle\t2\t1\n"
					   "clip\tpunch\t0.5\t0\n"
					   "clip\tswing\t0.6\t0\n"
					   "clip\theavy\t0.8\t0\n"
					   "layer\tbase\n"
					   "state\tIdle\tclip\tidle\t0\n"
					   "state\tPunch\tclip\tpunch\t0\n"
					   "state\tBatSwing\tclip\tswing\t0\n"
					   "state\tHeavy\tclip\theavy\t0\n"
					   "start\tIdle\n"
					   "transition\tIdle\tHeavy\t0\t0\timmediate\t1\tattack == 2\n"
					   "transition\tIdle\tBatSwing\t1\t0\timmediate\t1\tattack and test.bat\n"
					   "transition\tIdle\tPunch\t2\t0\timmediate\t1\tattack\n"
					   "transition\tPunch\tIdle\t1\t0\tat_end\t1\t\n"
					   "transition\tBatSwing\tIdle\t1\t0\tat_end\t1\t\n"
					   "transition\tHeavy\tIdle\t1\t0\tat_end\t1\t\n";
	ModSchema schema;
	schema.events = { "attack" };
	schema.itemKinds = { "test.bat" };
	std::string error, warnings;
	auto graph = CompileAnimGraph( text, schema, error, warnings );
	CHECK( graph != nullptr && warnings.empty() );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto attack = [&]( int32_t value ) {
		SimCommand c;
		c.type = CommandType::Event;
		c.target = SlotTarget( 0 );
		c.index = 0;
		c.value = value;
		f.commands.push_back( c );
		step( 1 );
		const auto& states = graph->layers[0].states;
		std::string now = states[sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) ).graph[0].state].name;
		step( 60 ); // the attack plays out
		return now;
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );

	std::string bare = attack( 0 );
	SimCommand give;
	give.type = CommandType::SpawnItem;
	give.target = SlotTarget( 0 );
	give.index = 0; // test.bat
	give.mode = uint8_t( kSocketRightHand );
	f.commands.push_back( give );
	step( 1 );
	std::string armed = attack( 0 );
	std::string heavy = attack( 2 );
	std::printf( "    \"attack\": empty hands -> %s, holding test.bat -> %s, value 2 -> %s\n", bare.c_str(), armed.c_str(), heavy.c_str() );
	CHECK( bare == "Punch" );
	CHECK( armed == "BatSwing" );
	CHECK( heavy == "Heavy" );
}

void TestAnimBlend2D()
{
	const char* text = "cinderbox_graph\t1\n"
					   "clip\tidle\t2\t1\n"
					   "clip\tfwd\t1\t1\n"
					   "clip\tleft\t1\t1\n"
					   "clip\tright\t1\t1\n"
					   "clip\tback\t1\t1\n"
					   "layer\tbase\n"
					   "state\tMove\tblend2d\tmove_right\tmove_forward\n"
					   "point2\t0\t0\tidle\n"
					   "point2\t0\t3\tfwd\n"
					   "point2\t-3\t0\tleft\n"
					   "point2\t3\t0\tright\n"
					   "point2\t0\t-3\tback\n"
					   "triangle\t0\t1\t3\n"
					   "triangle\t0\t3\t4\n"
					   "triangle\t0\t4\t2\n"
					   "triangle\t0\t2\t1\n"
					   "start\tMove\n";
	std::string error, warnings;
	auto graph = CompileAnimGraph( text, ModSchema{}, error, warnings );
	CHECK( graph != nullptr );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	CHECK( warnings.empty() );
	const AnimGraphState& move = graph->layers[0].states[0];
	CHECK( move.planar && move.points.size() == 5 && move.triangles.size() == 4 );
	AnimBlendWeights w;
	AnimGraphWeights( move, 0.0f, 1.5f, w ); // halfway forward: idle and fwd
	CHECK( std::fabs( w[0] - 0.5f ) < 1e-5f && std::fabs( w[1] - 0.5f ) < 1e-5f );
	AnimGraphWeights( move, 1.0f, 1.0f, w ); // inside the forward-right triangle: a third each
	std::printf( "    weights at (1, 1): idle %.2f fwd %.2f right %.2f\n", w[0], w[1], w[3] );
	CHECK( std::fabs( w[0] - 1.0f / 3.0f ) < 1e-5f && std::fabs( w[1] - 1.0f / 3.0f ) < 1e-5f && std::fabs( w[3] - 1.0f / 3.0f ) < 1e-5f );
	CHECK( w[2] == 0.0f && w[4] == 0.0f );
	AnimGraphWeights( move, 6.0f, 0.0f, w ); // outside: the nearest edge's end, right
	CHECK( std::fabs( w[3] - 1.0f ) < 1e-5f );

	// A camera-facing player strafing right: the body keeps facing, the velocity is to its right.
	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	InputFrame f;
	f.events.push_back( { PlayerEventType::Join, 0 } );
	SimCommand face;
	face.type = CommandType::Facing;
	face.target = SlotTarget( 0 );
	face.mode = 1;
	f.commands.push_back( face );
	for ( int i = 0; i < 90; ++i )
	{
		f.tick = sim.Tick();
		f.inputs[0].moveRight = i >= 30 ? int8_t( 127 ) : int8_t( 0 );
		sim.Step( f );
		f.events.clear();
		f.commands.clear();
	}
	AnimState a = sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) );
	std::printf( "    strafing right: move_right %.2f move_forward %.2f, blend (%.2f, %.2f)\n", a.moveRight, a.moveForward, a.graph[0].blend,
				 a.graph[0].blendY );
	CHECK( a.moveRight > 2.5f && std::fabs( a.moveForward ) < 0.3f );
	CHECK( a.graph[0].blend == a.moveRight && a.graph[0].blendY == a.moveForward );
}

// A state's speed (a TimeScale node, or a stretched custom timeline) and a crossfade's curve: the
// state's clock runs at its speed, so its markers come with it and an at-end transition waits for
// the slower clip; the speed is an expression; a fade through its curve.
void TestStateSpeed()
{
	ModSchema schema = TestGraphSchema();
	const std::string text = "cinderbox_graph\t1\n"
							 "clip\tidle\t1\t1\n"
							 "clip\tswing\t1\t0\n"
							 "marker\tswing\t0.5\ttest.impact\n"
							 "layer\tFullBody\n"
							 "state\tIdle\tclip\tidle\t0\n"
							 "state\tSwing\tclip\tswing\t0\n"
							 "speed\t0.5\n"
							 "state\tFast\tclip\tidle\t0\n"
							 "speed\tspeed * 2\n"
							 "transition\tIdle\tSwing\t1\t0.2\timmediate\t1\ttest.punch\t0,0.9,1\n"
							 "transition\tSwing\tIdle\t1\t0\tat_end\t1\t\n"
							 "transition\tIdle\tFast\t1\t0\timmediate\t1\tspeed > 0\n"
							 "start\tIdle\n";
	std::string error, warnings;
	auto graph = CompileAnimGraph( text, schema, error, warnings );
	CHECK( graph != nullptr );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	const AnimGraphLayer& layer = graph->layers[0];
	CHECK( layer.states[1].speed.Empty() == false && layer.states[0].speed.Empty() );
	CHECK( layer.states[0].transitions[0].curve.size() == 3 );

	AnimState s;
	AnimGraphPacks packs;
	std::vector<int> markers;
	ModEventRecord punch{};
	punch.type = uint16_t( schema.FindEvent( "test.punch" ) );
	punch.netIdA = 7;
	auto step = [&]( bool punched, float speed ) {
		AnimGraphInputs in;
		in.netId = 7;
		in.builtins[AnimExpr::Speed] = speed;
		in.events = punched ? &punch : nullptr;
		in.eventCount = punched ? 1 : 0;
		markers.clear();
		UpdateAnimGraph( s, *graph, packs, in, 0.1f, markers );
	};
	step( false, 0.0f );
	step( true, 0.0f ); // into Swing
	CHECK( s.graph[0].state == 1 && s.graph[0].fadeLength == 0.2f );
	// Its fade through the curve: 0.1 s of 0.2 is the curve's middle sample.
	step( false, 0.0f );
	const float fade = AnimGraphFadeIn( layer, s.graph[0] );
	std::printf( "    the swing faded in %.2f after 0.1 s of a 0.2 s fade (its curve's middle: 0.9)\n", double( fade ) );
	CHECK( std::fabs( fade - 0.9f ) < 1e-4f );
	// Half speed: the 0.5 s marker after 1 s, the 1 s clip's end (and the way back) after 2 s.
	int impactAt = -1, backAt = -1;
	for ( int i = 1; i < 30 && backAt < 0; ++i )
	{
		step( false, 0.0f );
		bool impact = false;
		for ( int m : markers )
		{
			impact |= m == schema.FindEvent( "test.impact" );
		}
		impactAt = impact && impactAt < 0 ? i : impactAt;
		backAt = s.graph[0].state == 0 ? i : backAt;
	}
	std::printf( "    at half speed: the marker %d ticks into the swing, back to idle at %d (0.1 s ticks)\n", impactAt + 1, backAt + 1 );
	CHECK( impactAt + 1 >= 10 && impactAt + 1 <= 11 );
	CHECK( backAt + 1 >= 20 && backAt + 1 <= 21 );
	// An expression: twice the speed.
	step( false, 1.5f ); // into Fast
	CHECK( s.graph[0].state == 2 );
	const float before = s.graph[0].time;
	step( false, 1.5f );
	std::printf( "    speed * 2 at speed 1.5: %.2f s of clip in a 0.1 s tick\n", double( s.graph[0].time - before ) );
	CHECK( std::fabs( s.graph[0].time - before - 0.3f ) < 1e-5f );
	CHECK( CompileAnimGraph( "cinderbox_graph\t1\nclip\ta\t1\t1\nlayer\tx\nspeed\t2\n", schema, error, warnings ) == nullptr );
}

// Godot's OneShot and Add2 (M231). A one-shot layer plays its clip once from the tick its request
// turns true (held, it does not play again; asked again while it plays, it starts over without
// dropping out), fading in and out, its markers with it. An added layer puts its pose's difference
// from the rest on top of what is below, by its weight.
void TestOneShot()
{
	ModSchema schema = TestGraphSchema();
	const std::string text = "cinderbox_graph\t1\n"
							 "clip\tidle\t2\t1\n"
							 "clip\tland\t0.3\t0\n"
							 "marker\tland\t0.15\ttest.impact\n"
							 "layer\tBase\n"
							 "state\tIdle\tclip\tidle\t0\n"
							 "start\tIdle\n"
							 "layer\tKick\n"
							 "shot\t0.05\t0.1\ttest.punch\n"
							 "state\tShot\tclip\tland\t0\n"
							 "start\tShot\n";
	std::string error, warnings;
	auto graph = CompileAnimGraph( text, schema, error, warnings );
	CHECK( graph != nullptr && graph->layers.size() == 2 && graph->layers[1].shot );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	AnimState s;
	AnimGraphPacks packs;
	std::vector<int> markers;
	ModEventRecord punch{};
	punch.type = uint16_t( schema.FindEvent( "test.punch" ) );
	punch.netIdA = 7;
	int impacts = 0;
	auto step = [&]( bool punched ) {
		AnimGraphInputs in;
		in.netId = 7;
		in.events = punched ? &punch : nullptr;
		in.eventCount = punched ? 1 : 0;
		markers.clear();
		UpdateAnimGraph( s, *graph, packs, in, 0.025f, markers );
		for ( int m : markers )
		{
			impacts += m == schema.FindEvent( "test.impact" ) ? 1 : 0;
		}
		return s.graph[1].weight;
	};
	CHECK( step( false ) == 0.0f );
	const float first = step( true );
	const float second = step( false );
	std::printf( "    fired: weight %.2f, then %.2f (in over 0.05 s)\n", double( first ), double( second ) );
	CHECK( std::fabs( first - 0.5f ) < 1e-4f && second == 1.0f );
	int ticks = 2;
	while ( s.graph[1].weight > 0.0f && ticks < 40 )
	{
		const float w = step( false );
		++ticks;
		if ( ticks == 10 )
		{
			std::printf( "    0.25 s in, 0.05 s left: weight %.2f (out over 0.1 s)\n", double( w ) );
			CHECK( std::fabs( w - 0.5f ) < 0.02f );
		}
	}
	std::printf( "    done after %d ticks of 0.025 s (the clip is 0.3 s); its marker %d time\n", ticks, impacts );
	CHECK( ticks == 12 && impacts == 1 );
	// Held: once only.
	for ( int i = 0; i < 20; ++i )
	{
		step( true );
	}
	CHECK( s.graph[1].weight == 0.0f && impacts == 2 );
	// Asked again while it plays: from the start, at full weight.
	step( false );
	step( true );
	for ( int i = 0; i < 8; ++i )
	{
		step( false );
	}
	const float before = s.graph[1].time;
	step( true );
	std::printf( "    fired again at %.3f s in: now %.3f s, weight %.2f\n", double( before ), double( s.graph[1].time ), double( s.graph[1].weight ) );
	CHECK( s.graph[1].time < 0.05f && s.graph[1].weight == 1.0f );
	CHECK( CompileAnimGraph( "cinderbox_graph\t1\nclip\ta\t1\t1\nlayer\tx\nshot\t0\t0\tjumped\nstate\tA\tclip\ta\t0\nstart\tA\n", schema, error,
							 warnings ) == nullptr ); // (the base cannot be one)

	// An added layer on the placeholder rig: nothing at weight 0, a difference at 1, about half of
	// it at 0.5; a layer laid over instead moves the foot elsewhere.
	auto set = anim::AnimSet::CreateProcedural();
	auto footWith = [&]( const char* layerLines, float weight ) {
		const std::string added = std::string( "cinderbox_graph\t1\n"
											   "clip\tidle\t2\t1\n"
											   "clip\twalk\t1\t1\n"
											   "layer\tBase\n"
											   "state\tIdle\tclip\tidle\t0\n"
											   "start\tIdle\n"
											   "layer\tOver\n" ) +
								  layerLines + "state\tWalk\tclip\twalk\t0\nstart\tWalk\n";
		std::string why, warned;
		auto g = CompileAnimGraph( added, ModSchema{}, why, warned );
		if ( g == nullptr )
		{
			std::printf( "    %s\n", why.c_str() );
			return b3Vec3{};
		}
		anim::PoseEvaluator pose( *set );
		pose.SetGraph( g, warned );
		AnimState a;
		a.graph.resize( 2 );
		a.graph[0].started = a.graph[1].started = 1;
		a.graph[1].time = 0.25f;
		a.graph[1].weight = weight;
		pose.Evaluate( a );
		float v[4];
		ozz::math::StorePtrU( pose.Models()[size_t( anim::FindJoint( *set, "LeftFoot" ) )].cols[3], v );
		return b3Vec3{ v[0], v[1], v[2] };
	};
	const b3Vec3 base = footWith( "add\n", 0.0f );
	const b3Vec3 full = footWith( "add\n", 1.0f );
	const b3Vec3 half = footWith( "add\n", 0.5f );
	const b3Vec3 over = footWith( "", 1.0f );
	const b3Vec3 rest = footWith( "", 0.0f );
	std::printf( "    the left foot moved %.3f m added, %.3f m at half, %.3f m laid over\n", double( b3Distance( full, base ) ),
				 double( b3Distance( half, base ) ), double( b3Distance( over, base ) ) );
	CHECK( b3Distance( base, rest ) < 1e-5f );
	CHECK( b3Distance( full, base ) > 0.02f );
	CHECK( std::fabs( b3Distance( half, base ) / b3Distance( full, base ) - 0.5f ) < 0.1f );
	CHECK( b3Distance( full, over ) > 1e-3f );
}

// A nested state machine as the bake flattens it (M232): its states under its name, the way in to
// its start, the way out from where it ends. Played through: into Combat/Slash on the event, on to
// Combat/Recover at the slash's end, back to Idle once Recover has had 0.5 s.
void TestNestedMachine()
{
	ModSchema schema = TestGraphSchema();
	const std::string text = "cinderbox_graph\t1\n"
							 "clip\tidle\t2\t1\n"
							 "clip\tslash\t0.4\t0\n"
							 "layer\tBase\n"
							 "state\tIdle\tclip\tidle\t0\n"
							 "state\tCombat/Slash\tclip\tslash\t0\n"
							 "state\tCombat/Recover\tclip\tidle\t0\n"
							 "transition\tIdle\tCombat/Slash\t1\t0\timmediate\t1\ttest.punch\n"
							 "transition\tCombat/Recover\tIdle\t1\t0.3\timmediate\t1\tstate_time > 0.5\n"
							 "transition\tCombat/Slash\tCombat/Recover\t1\t0\tat_end\t1\t\n"
							 "start\tIdle\n";
	std::string error, warnings;
	auto graph = CompileAnimGraph( text, schema, error, warnings );
	CHECK( graph != nullptr );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	AnimState s;
	AnimGraphPacks packs;
	std::vector<int> markers;
	ModEventRecord punch{};
	punch.type = uint16_t( schema.FindEvent( "test.punch" ) );
	punch.netIdA = 7;
	auto step = [&]( bool punched ) {
		AnimGraphInputs in;
		in.netId = 7;
		in.events = punched ? &punch : nullptr;
		in.eventCount = punched ? 1 : 0;
		UpdateAnimGraph( s, *graph, packs, in, 0.1f, markers );
		return graph->layers[0].states[s.graph[0].state].name;
	};
	step( false );
	CHECK( step( true ) == "Combat/Slash" );
	int recoverAt = -1, idleAt = -1;
	for ( int i = 1; i <= 20 && idleAt < 0; ++i )
	{
		const std::string now = step( false );
		recoverAt = now == "Combat/Recover" && recoverAt < 0 ? i : recoverAt;
		idleAt = now == "Idle" ? i : idleAt;
	}
	std::printf( "    into Combat/Slash, on to Combat/Recover after %d ticks of 0.1 s (a 0.4 s slash), back to Idle after %d\n", recoverAt, idleAt );
	CHECK( recoverAt >= 4 && recoverAt <= 5 );
	CHECK( idleAt - recoverAt >= 5 && idleAt - recoverAt <= 7 );
}

void TestAnimGraph()
{
	// Numbers read the same everywhere.
	float v = 0.0f;
	CHECK( ParseAnimFloat( "0.1", 3, v ) && v == 0.1f );
	CHECK( ParseAnimFloat( "6.5", 3, v ) && v == 6.5f );
	CHECK( ParseAnimFloat( "-3", 2, v ) && v == -3.0f );
	CHECK( ParseAnimFloat( "1e-3", 4, v ) && v == 0.001f );
	CHECK( ParseAnimFloat( "0.333333333", 11, v ) && v == 0.333333333f );
	CHECK( ParseAnimFloat( "abc", 3, v ) == false );
	CHECK( ParseAnimFloat( "1.5x", 4, v ) == false );

	// Expressions: Godot's operators and precedence, simulation values by name.
	ModSchema schema = TestGraphSchema();
	auto eval = [&]( const char* text, const AnimGraphInputs& in ) -> float {
		AnimExpr expr;
		std::string error, warnings;
		if ( CompileAnimExpr( text, schema, expr, error, warnings ) == false )
		{
			std::printf( "    %s\n", error.c_str() );
			return -1000.0f; // fails the comparison
		}
		return EvaluateAnimExpr( expr, in, 0.25f );
	};
	AnimGraphInputs in;
	in.builtins[AnimExpr::Speed] = 4.0f;
	CHECK( eval( "1 + 2 * 3 == 7 and not false", in ) == 1.0f );
	CHECK( eval( "speed > 3 && !(speed >= 5)", in ) == 1.0f );
	CHECK( eval( "-speed / 2", in ) == -2.0f );
	CHECK( eval( "state_time < 0.3 or grounded", in ) == 1.0f );
	CHECK( eval( "", in ) == 0.0f );
	{
		AnimExpr expr;
		std::string error, warnings;
		CHECK( CompileAnimExpr( "speed > (2", schema, expr, error, warnings ) == false && error.empty() == false );
		CHECK( CompileAnimExpr( "nobody.declared > 1", schema, expr, error, warnings ) );
		CHECK( warnings.find( "nobody.declared" ) != std::string::npos );
	}

	std::string error, warnings;
	auto graph = CompileAnimGraph( kTestGraph, schema, error, warnings );
	CHECK( graph != nullptr );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	CHECK( warnings.empty() );
	CHECK( graph->layers.size() == 2 && graph->layers[0].states[0].points[1].clip == graph->FindClip( "walk" ) ); // sorted
	CHECK( graph->EmitsEvent( 1 ) && graph->EmitsEvent( 0 ) == false );
	CHECK( CompileAnimGraph( "cinderbox_graph\t1\nlayer\tx\nstate\tA\tclip\tnothing\t0\n", schema, error, warnings ) == nullptr );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto layer = [&]( int l ) { return sim.GetAnimState( sim.FindEntity( sim.Globals().playerNetIds[0] ) ).graph[l]; };
	auto impacts = [&]() {
		int count = 0;
		const SimGlobals& g = sim.Globals();
		for ( uint32_t i = 0; i < std::min( g.modEventCount, kModEventHistory ); ++i )
		{
			count += g.modEvents[i].type == 1 && g.modEvents[i].netIdA == sim.PlayerNetId( 0 ) ? 1 : 0;
		}
		return count;
	};

	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	CHECK( layer( 0 ).started == 1 && layer( 0 ).state == 0 );
	CHECK( layer( 1 ).state == 0 && layer( 1 ).weight == 0.0f );

	// Walking: the blend space follows the speed, its phase at the blended cycle rate.
	f.inputs[0].moveForward = 127;
	step( 90 );
	std::printf( "    walking: blend %.2f phase %.3f\n", layer( 0 ).blend, layer( 0 ).time );
	CHECK( std::fabs( layer( 0 ).blend - 3.0f ) < 0.1f );

	// A jump: into Jump on the tick it happens, back to Move once grounded again.
	f.inputs[0] = {};
	step( 30 );
	f.inputs[0].buttons = BtnJump;
	step( 1 );
	f.inputs[0].buttons = 0;
	CHECK( layer( 0 ).state == 1 && layer( 0 ).fadeLength == 0.1f );
	step( 120 );
	CHECK( layer( 0 ).state == 0 );

	// A punch: a mod arms the upper layer (a board field) and triggers it (an event at the player).
	SimCommand arm;
	arm.type = CommandType::SetField;
	arm.target = SlotTarget( 0 );
	arm.index = 0;
	arm.value = 1;
	f.commands.push_back( arm );
	step( 30 );
	CHECK( layer( 1 ).weight == 1.0f && layer( 1 ).state == 0 );
	SimCommand punch;
	punch.type = CommandType::Event;
	punch.target = SlotTarget( 0 );
	punch.index = 0;
	f.commands.push_back( punch );
	step( 1 );
	CHECK( layer( 1 ).state == 1 );
	int before = impacts();
	step( 10 ); // 0.17 s: not yet at the marker
	CHECK( impacts() == before );
	step( 5 ); // 0.25 s: past it, once
	CHECK( impacts() == before + 1 );
	step( 30 ); // the punch ends and the layer goes back to rest
	CHECK( impacts() == before + 1 );
	CHECK( layer( 1 ).state == 0 );
}

// Pose hash over a spread of animation states: the same on every compiler/platform.
// `legs` and `aim`: whether the states also turn the legs and aim (the whole pose), or leave the
// pose as the state machine's layers blend it; the parts tell which step differs when a build does.
uint64_t AnimPoseHash( const anim::AnimSet& set, bool legs = true, bool aim = true )
{
	std::string error, warnings;
	auto graph = CompileAnimGraph( set.GraphText(), ModSchema{}, error, warnings );
	anim::PoseEvaluator eval( set );
	eval.SetGraph( graph, warnings );
	uint64_t states = graph != nullptr && graph->layers.empty() == false ? graph->layers[0].states.size() : 1;
	uint64_t hash = kHashSeed;
	uint64_t rng = 99;
	for ( int i = 0; i < 400; ++i )
	{
		AnimState s;
		AnimGraphLayerState& layer = s.graph[0];
		layer.started = 1;
		layer.state = uint8_t( NextRandom( rng ) % states );
		layer.previous = uint8_t( NextRandom( rng ) % states );
		layer.time = RandomUnit( rng );
		layer.previousTime = RandomUnit( rng );
		layer.stateTime = RandomRange( rng, 0.0f, 0.4f );
		layer.fadeLength = 0.2f;
		layer.weight = 1.0f;
		layer.blend = RandomRange( rng, -8.0f, 8.0f );
		layer.previousBlend = RandomRange( rng, -8.0f, 8.0f );
		s.groundSpeed = RandomRange( rng, 0.0f, 8.0f );
		float legYaw = RandomRange( rng, -1.5f, 1.5f );
		uint8_t aiming = uint8_t( NextRandom( rng ) % 2 );
		float aimYaw = RandomRange( rng, -1.0f, 1.0f );
		float aimPitch = RandomRange( rng, -1.0f, 1.0f );
		uint8_t look = uint8_t( NextRandom( rng ) % 256 ); // how far the upper body follows the pitch
		s.legYaw = legs ? legYaw : 0.0f;
		s.aiming = aim ? aiming : 0;
		s.look = aim ? look : 0;
		s.aimYaw = aimYaw;
		s.aimPitch = aimPitch;
		eval.Evaluate( s );
		for ( const auto& m : eval.Models() )
		{
			for ( const auto& col : m.cols )
			{
				float v[4];
				ozz::math::StorePtrU( col, v );
				hash = HashBytes( hash, v, sizeof( v ) );
			}
		}
	}
	return hash;
}

// Hitboxes follow the posed skeleton: a ray finds the zone it passes through, misses the gap between
// the legs, and turns with the body.
void TestHitboxes()
{
	anim::HitboxSet parsed;
	std::string error;
	CHECK( anim::ParseHitboxes( anim::FormatHitboxes( anim::DefaultHitboxes() ), parsed, error ) );
	CHECK( parsed.boxes.size() == anim::DefaultHitboxes().boxes.size() );
	CHECK( anim::ParseHitboxes( "head Head cube 0 0 0 0 0 0 1 1\n", parsed, error ) == false );

	auto set = anim::AnimSet::CreateProcedural();
	anim::HitboxSet hitboxes = anim::DefaultHitboxes();
	std::string warnings;
	anim::BindHitboxes( hitboxes, *set, warnings );
	CHECK( warnings.empty() );
	CHECK( hitboxes.boxes.size() == 10 );

	anim::PoseEvaluator pose( *set );
	pose.Evaluate( AnimState{} );
	const b3Vec3 feet = { 2.0f, 0.0f, 3.0f };
	const b3Quat facing = { { 0.0f, 0.0f, 0.0f }, 1.0f };

	bool pointsMatch = true;
	auto zoneAt = [&]( b3Quat rotation, b3Vec3 from, b3Vec3 translation ) -> std::string {
		anim::HitboxHit hit;
		if ( anim::RayHitboxes( hitboxes, pose.Models(), feet, rotation, from, translation, 1.0f, hit ) == false )
		{
			return "";
		}
		b3Vec3 again = b3MulAdd( from, hit.fraction, translation );
		pointsMatch = pointsMatch && b3Length( b3Sub( again, hit.point ) ) < 1e-4f;
		return hit.box->zone;
	};
	// Shots from behind (-Z) towards +Z, 10 m long.
	const b3Vec3 forward = { 0.0f, 0.0f, 10.0f };
	CHECK( zoneAt( facing, { 2.0f, 1.62f, -2.0f }, forward ) == "head" );
	CHECK( zoneAt( facing, { 2.0f, 1.20f, -2.0f }, forward ) == "torso" );
	CHECK( zoneAt( facing, { 2.1f, 0.40f, -2.0f }, forward ) == "leg" );
	CHECK( zoneAt( facing, { 2.0f, 0.30f, -2.0f }, forward ).empty() );
	CHECK( zoneAt( facing, { 2.0f, 2.20f, -2.0f }, forward ).empty() );
	CHECK( zoneAt( facing, { 2.0f, 1.62f, -2.0f }, { 0.0f, 0.0f, 1.0f } ).empty() ); // too short

	// Turned 90 degrees: the arms now stick out along Z, and a sideways shot at the arm's height
	// that used to pass beside the body hits one.
	const b3Quat turned = b3MakeQuatFromAxisAngle( { 0.0f, 1.0f, 0.0f }, 0.5f * detmath::kPi );
	const b3Vec3 across = { 10.0f, 0.0f, 0.0f };
	CHECK( zoneAt( facing, { -3.0f, 1.20f, 3.20f }, across ).empty() );
	CHECK( zoneAt( turned, { -3.0f, 1.20f, 3.20f }, across ) == "arm" );

	// Aiming straight ahead raises the right arm to shoulder height in front of the body: a shot
	// across there hits it only while aiming, because hit tests pose the same aim.
	AnimState aiming;
	aiming.aiming = 1;
	anim::PoseEvaluator aimed( *set );
	aimed.Evaluate( aiming );
	anim::HitboxHit hit;
	const b3Vec3 armLine = { -3.0f, 1.39f, 3.35f }; // 0.35 m in front of the body, at the shoulder
	CHECK( anim::RayHitboxes( hitboxes, pose.Models(), feet, facing, armLine, across, 1.0f, hit ) == false );
	CHECK( anim::RayHitboxes( hitboxes, aimed.Models(), feet, facing, armLine, across, 1.0f, hit ) && hit.box->zone == "arm" );
	CHECK( pointsMatch );
}

// The placeholder rig, what a server without a character plays: its six procedural clips and the
// one-layer state machine built into it. The simulation runs that machine like any character's:
// standing, walking by speed (backwards in reverse), and a jump through its states. A pose without
// a machine is the skeleton's rest.
void TestPlaceholderGraph()
{
	auto set = anim::AnimSet::CreateProcedural();
	CHECK( set->GraphText().empty() == false );
	for ( const char* clip : { "idle", "walk", "run", "jump_start", "fall", "land" } )
	{
		CHECK( set->NamedClip( clip ) != nullptr );
	}
	std::string error, warnings;
	auto graph = CompileAnimGraph( set->GraphText(), ModSchema{}, error, warnings );
	CHECK( graph != nullptr && warnings.empty() );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	CHECK( graph->layers.size() == 1 && graph->clips.size() == 6 );
	for ( const AnimGraphClip& clip : graph->clips )
	{
		// The lengths in the machine's text are the clips'.
		CHECK( std::fabs( clip.length - set->NamedClip( clip.name )->duration() ) < 1e-4f );
	}
	anim::PoseEvaluator pose( *set );
	pose.SetGraph( graph, warnings );
	CHECK( warnings.empty() );
	auto stateOf = [&]( const char* name ) {
		const auto& states = graph->layers[0].states;
		for ( size_t i = 0; i < states.size(); ++i )
		{
			if ( states[i].name == name )
			{
				return int( i );
			}
		}
		return -1;
	};
	auto at = []( const anim::PoseEvaluator& p, const anim::AnimSet& s, const char* joint ) {
		float v[4];
		ozz::math::StorePtrU( p.Models()[size_t( present::FindJoint( s, joint ) )].cols[3], v );
		return b3Vec3{ v[0], v[1], v[2] };
	};

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
		}
	};
	auto state = [&]() { return sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) ); };

	// Standing.
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	CHECK( state().graph[0].state == stateOf( "Locomotion" ) && std::fabs( state().graph[0].blend ) < 0.1f );
	auto clips = anim::ActiveClips( state(), *graph );
	CHECK( clips.size() == 1 && clips[0].name == "idle" && clips[0].loops );
	pose.Evaluate( state() );
	b3Vec3 standingFoot = at( pose, *set, "LeftFoot" );

	// Walking: the blend follows the speed, the walk clip leads, and the legs move.
	f.inputs[0].moveForward = 127;
	step( 60 );
	CHECK( std::fabs( state().graph[0].blend - 3.0f ) < 0.1f );
	clips = anim::ActiveClips( state(), *graph );
	CHECK( clips.size() == 1 && clips[0].name == "walk" );
	float farthest = 0.0f;
	for ( int i = 0; i < 30; ++i )
	{
		step( 1 );
		pose.Evaluate( state() );
		farthest = std::max( farthest, b3Distance( at( pose, *set, "LeftFoot" ), standingFoot ) );
	}
	std::printf( "    walking: blend %.2f, the foot swings %.2f m from where it stood\n", state().graph[0].blend, farthest );
	CHECK( farthest > 0.1f );

	// A jump: JumpStart, Fall, Land, and back, in that order.
	f.inputs[0] = {};
	step( 30 );
	f.inputs[0].buttons = BtnJump;
	step( 1 );
	f.inputs[0].buttons = 0;
	std::vector<int> states = { int( state().graph[0].state ) };
	for ( int i = 0; i < 120; ++i )
	{
		step( 1 );
		if ( int( state().graph[0].state ) != states.back() )
		{
			states.push_back( int( state().graph[0].state ) );
		}
	}
	CHECK( states.size() == 4 );
	CHECK( states.size() == 4 && states[0] == stateOf( "JumpStart" ) && states[1] == stateOf( "Fall" ) && states[2] == stateOf( "Land" ) &&
		   states[3] == stateOf( "Locomotion" ) );

	// Without a machine there is nothing to play: the skeleton's rest, whatever the state says.
	anim::PoseEvaluator plain( *set );
	plain.Evaluate( AnimState{} );
	b3Vec3 restFoot = at( plain, *set, "LeftFoot" );
	plain.Evaluate( state() );
	CHECK( b3Distance( at( plain, *set, "LeftFoot" ), restFoot ) < 1e-5f );
}

// The default player character, as the editor baked it from the Universal Animation Library's
// mannequin (godot/characters/mannequin): it loads cleanly, stands about 1.8 m tall on its feet,
// and a ray finds each zone where the body is.
// Aiming with the pistol, where an item held in the right hand points: the pistol binding turns
// the gun's -Z (muzzle) onto the hand frame's -Y and its +Y (top) onto +Z. Both should follow the
// line of sight (+Z) and stay upright, whatever the rig's own bone axes.
void CheckHeldItem( const anim::AnimSet& set, anim::PoseEvaluator& pose )
{
	int hand = anim::FindJoint( set, "RightHand" );
	CHECK( hand >= 0 );
	if ( hand < 0 )
	{
		return;
	}
	ozz::math::Float4x4 frame = pose.Models()[size_t( hand )] * set.AttachFrame( hand );
	auto axis = [&]( int column ) {
		float v[4];
		ozz::math::StorePtrU( frame.cols[column], v );
		float length = std::sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
		return b3Vec3{ v[0] / length, v[1] / length, v[2] / length };
	};
	b3Vec3 y = axis( 1 ), z = axis( 2 );
	b3Vec3 muzzle = { -y.x, -y.y, -y.z };
	std::printf( "    %s: held item points (%.2f %.2f %.2f), its top (%.2f %.2f %.2f)\n", set.Description().c_str(), muzzle.x, muzzle.y,
				 muzzle.z, z.x, z.y, z.z );
	CHECK( muzzle.z > 0.9f );
	CHECK( z.y > 0.7f );
}

// The shipped items' animation packs on the mannequin, as a server declares them and a client loads
// them: the bat's (source 1: Ready, Swing on melee.swing, the strike and the end as markers) and the
// pistol's (source 2: Hold, Shoot on item.use). The character's own upper layer is only a rest.
struct ShippedPacks
{
	AnimGraphPacks graphs;
	std::vector<std::shared_ptr<const anim::PackClips>> clips;
	enum Event : int
	{
		Use,
		Strike,
		Fired,
		Swing,
		Done
	};
	static constexpr uint8_t Bat = 1, Pistol = 2;
	bool loaded = false; // LoadShippedPacks went well
	int StateOf( uint8_t source, const char* name ) const
	{
		const auto& states = graphs[size_t( source - 1 )]->layers[0].states;
		for ( size_t i = 0; i < states.size(); ++i )
		{
			if ( states[i].name == name )
			{
				return int( i );
			}
		}
		return -1;
	}
};

void MannequinSchema( ModSchema& schema )
{
	schema.layers = { "full", "upper" };
	schema.stances = { "melee", "melee_swing", "gun", "rifle" };
	schema.events = { "item.use", "melee.strike", "rifle.fired", "melee.swing", "melee.done", "combat.hurt" };
	for ( const char* pack : { "melee/melee.hold", "gun/gun.pistol_hold" } )
	{
		std::string mod = pack, name = mod.substr( mod.find( '/' ) + 1 ), text;
		mod.resize( mod.find( '/' ) );
		CHECK( anim::DiskReader( std::string( CB_SOURCE_DIR ) + "/server_mods/" + mod + "/client/anim/" + name )( "graph.cfg", text ) );
		schema.animPacks.push_back( { mod, name, text } );
	}
}

// `character`: whose clips the packs' are fitted to (null: the simulation's only, no clips).
void LoadShippedPacks( const ModSchema& schema, const anim::AnimSet* character, ShippedPacks& out )
{
	std::string error, warnings;
	out.graphs = CompileAnimPacks( schema, warnings );
	CHECK( out.graphs.size() == 2 && out.graphs[0] != nullptr && out.graphs[1] != nullptr && warnings.empty() );
	if ( out.graphs.size() != 2 || out.graphs[0] == nullptr || out.graphs[1] == nullptr )
	{
		std::printf( "    %s\n", warnings.c_str() );
		return;
	}
	for ( size_t i = 0; character != nullptr && i < out.graphs.size(); ++i )
	{
		const AnimPackInfo& pack = schema.animPacks[i];
		std::shared_ptr<const anim::AnimSet> set = anim::AnimSet::Load(
			anim::DiskReader( std::string( CB_SOURCE_DIR ) + "/server_mods/" + pack.mod + "/client/anim/" + pack.name ), pack.name, error, warnings );
		CHECK( set != nullptr );
		if ( set == nullptr )
		{
			std::printf( "    %s\n", error.c_str() );
			return;
		}
		out.clips.push_back( anim::FitPack( set, *out.graphs[i], *character, warnings ) );
	}
	out.loaded = true;
}

void TestMannequinCharacter()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin";
	std::string error, warnings;
	auto set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr );
	if ( set == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	std::printf( "    %s; warnings: %s\n", set->Description().c_str(), warnings.empty() ? "none" : warnings.c_str() );
	CHECK( warnings.empty() );
	CHECK( set->GraphText().empty() == false ); // it plays its own state machine
	std::string text;
	CHECK( anim::DiskReader( dir )( "hitboxes.cfg", text ) );
	anim::HitboxSet hitboxes;
	CHECK( anim::ParseHitboxes( text, hitboxes, error ) );
	size_t count = hitboxes.boxes.size();
	anim::BindHitboxes( hitboxes, *set, warnings );
	CHECK( hitboxes.boxes.size() == count && count >= 10 );

	// The shipped mods' names and packs, as a server would send them.
	ModSchema schema;
	MannequinSchema( schema );
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	CHECK( graph != nullptr );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	CHECK( warnings.empty() );
	ShippedPacks packs;
	LoadShippedPacks( schema, set.get(), packs );
	if ( packs.loaded == false )
	{
		return;
	}
	// The strike is the bat's pack's marker; the character's own clips have none.
	// (Its layers: FullBody, UpperBody, and the flinch: M242.)
	CHECK( graph->layers.size() == 3 && graph->EmitsEvent( ShippedPacks::Strike ) == false &&
		   packs.graphs[0]->EmitsEvent( ShippedPacks::Strike ) );
	anim::PoseEvaluator pose( *set );
	pose.SetGraph( graph, warnings );
	pose.SetPacks( packs.graphs, packs.clips );
	CHECK( warnings.empty() );
	auto stateOf = [&]( int layer, const char* name ) {
		const auto& states = graph->layers[size_t( layer )].states;
		for ( size_t i = 0; i < states.size(); ++i )
		{
			if ( states[i].name == name )
			{
				return int( i );
			}
		}
		return -1;
	};

	// The simulation runs the state machine; the pose follows it.
	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	sim.SetAnimPacks( packs.graphs );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto state = [&]() { return sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) ); };
	auto command = [&]( CommandType type, uint8_t index, int32_t value, uint8_t mode ) {
		SimCommand c;
		c.type = type;
		c.target = SlotTarget( 0 );
		c.index = index;
		c.value = value;
		c.mode = mode;
		f.commands.push_back( c );
	};
	auto at = [&]( const char* joint ) {
		float v[4];
		ozz::math::StorePtrU( pose.Models()[size_t( anim::FindJoint( *set, joint ) )].cols[3], v );
		return b3Vec3{ v[0], v[1], v[2] };
	};

	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	CHECK( state().graph[0].state == stateOf( 0, "Locomotion" ) );
	pose.Evaluate( state() );
	b3Vec3 head = at( "Head" );
	b3Vec3 foot = at( "LeftFoot" );
	std::printf( "    idle: head (%.2f %.2f %.2f), left foot (%.2f %.2f %.2f)\n", head.x, head.y, head.z, foot.x, foot.y, foot.z );
	CHECK( head.y > 1.4f && head.y < 1.8f );
	CHECK( foot.y > -0.05f && foot.y < 0.25f );
	CHECK( foot.x > 0.0f ); // its left is +X: it faces +Z like every character

	auto zoneAt = [&]( float height ) -> std::string {
		anim::HitboxHit hit;
		b3Quat facing = { { 0.0f, 0.0f, 0.0f }, 1.0f };
		if ( anim::RayHitboxes( hitboxes, pose.Models(), {}, facing, { 0.0f, height, -3.0f }, { 0.0f, 0.0f, 6.0f }, 1.0f, hit ) == false )
		{
			return "";
		}
		return hit.box->zone;
	};
	// (From behind; the head's sphere sits a little above and ahead of the joint, and below it the
	// upper chest's capsule is in the way first.)
	CHECK( zoneAt( head.y + 0.15f ) == "head" );
	CHECK( zoneAt( 1.2f ) == "torso" );
	CHECK( zoneAt( 2.1f ).empty() );

	// Walking: the blend space on forward_speed; a jump goes through JumpStart and lands.
	f.inputs[0].moveForward = 127;
	step( 60 );
	std::printf( "    walking: blend %.2f\n", state().graph[0].blend );
	CHECK( state().graph[0].state == stateOf( 0, "Locomotion" ) && state().graph[0].blend > 2.5f );
	f.inputs[0].buttons = BtnJump;
	step( 1 );
	f.inputs[0].buttons = 0;
	CHECK( state().graph[0].state == stateOf( 0, "JumpStart" ) );
	f.inputs[0] = {};
	step( 120 );
	CHECK( state().graph[0].state == stateOf( 0, "Locomotion" ) );

	// The pistol: its pack's upper layer holds it, aims (the aim chain), and plays a shot on item.use.
	const int pistolHold = packs.StateOf( ShippedPacks::Pistol, "Hold" ), pistolShoot = packs.StateOf( ShippedPacks::Pistol, "Shoot" );
	CHECK( pistolHold >= 0 && pistolShoot >= 0 );
	command( CommandType::SwapLayer, 1, ShippedPacks::Pistol, 0 );
	command( CommandType::Aim, 0, 0, 1 );
	step( 30 );
	CHECK( state().graph[1].source == ShippedPacks::Pistol && state().graph[1].state == pistolHold && state().graph[1].weight == 1.0f );
	pose.Evaluate( state() );
	b3Vec3 shoulder = at( "RightUpperArm" );
	b3Vec3 hand = at( "RightHand" );
	std::printf( "    aiming with the pistol: shoulder (%.2f %.2f %.2f) hand (%.2f %.2f %.2f)\n", shoulder.x, shoulder.y, shoulder.z,
				 hand.x, hand.y, hand.z );
	CHECK( hand.z - shoulder.z > 0.35f );
	CHECK( std::fabs( hand.y - shoulder.y ) < 0.1f );
	CheckHeldItem( *set, pose );
	{
		// Where the gun points and where the hands are, as the stance's animation has them and
		// once the arm is aimed (level: the aim asks for nothing the animation does not do).
		auto gun = [&]( const anim::PoseEvaluator& from, b3Vec3& ahead, b3Vec3& between ) {
			b3Vec3 at, otherAt;
			b3Quat turn, otherTurn;
			float scale;
			anim::Decompose( from.Models()[size_t( set->ArmJoints( false ).hand )], at, turn, scale );
			anim::Decompose( from.Models()[size_t( set->ArmJoints( true ).hand )], otherAt, otherTurn, scale );
			ahead = b3RotateVector( b3MulQuat( turn, set->HandSocketOf( false ).rotation ), { 0.0f, 0.0f, -1.0f } );
			between = b3Sub( otherAt, at );
		};
		AnimState unaimed = state();
		unaimed.aiming = 0;
		anim::PoseEvaluator plain( *set );
		plain.SetGraph( graph, warnings );
		plain.Evaluate( unaimed );
		b3Vec3 asAnimated, asAimed, handsAnimated, handsAimed;
		gun( plain, asAnimated, handsAnimated );
		gun( pose, asAimed, handsAimed );
		std::printf( "    the pistol points (%.2f %.2f %.2f) as animated, (%.2f %.2f %.2f) aimed level; the left hand is (%.2f %.2f %.2f) from the right as animated, (%.2f %.2f %.2f) aimed\n",
					 asAnimated.x, asAnimated.y, asAnimated.z, asAimed.x, asAimed.y, asAimed.z, handsAnimated.x, handsAnimated.y, handsAnimated.z,
					 handsAimed.x, handsAimed.y, handsAimed.z );
	}
	command( CommandType::Event, 0, 0, 0 ); // item.use, by this player
	step( 1 );
	CHECK( state().graph[1].state == pistolShoot );
	step( 60 );
	CHECK( state().graph[1].state == pistolHold );
	// Rapid fire: a shot during the recoil of the last one starts the recoil over (the pistol fires
	// every 0.2 s, the clip is 0.63 s long), and the state lasts until the last shot's has played.
	{
		const float dt = 1.0f / float( sim.Config().tickRate );
		command( CommandType::Event, 0, 0, 0 );
		step( 12 );
		float before = state().graph[1].stateTime;
		command( CommandType::Event, 0, 0, 0 );
		step( 1 );
		std::printf( "    a second shot %.2f s into the first: the state is %.3f s old again\n", before, state().graph[1].stateTime );
		CHECK( state().graph[1].state == pistolShoot && before > 0.15f );
		CHECK( state().graph[1].stateTime <= 1.5f * dt && state().graph[1].time <= 1.5f * dt );
		// It fades from where the last recoil was, not from the idle.
		CHECK( state().graph[1].previous == state().graph[1].state && state().graph[1].previousTime > 0.15f );
		// The viewer is told, so the keys inside the clip (a sound, a flash) are due again; only
		// for the moment of the crossfade.
		auto restarted = [&]() {
			bool any = false;
			for ( const anim::ActiveClip& clip : anim::ActiveClips( state(), *graph, packs.graphs ) )
			{
				any |= clip.channel == 1 && clip.restarted;
			}
			return any;
		};
		CHECK( restarted() );
		step( 11 );
		CHECK( restarted() == false );
		command( CommandType::Event, 0, 0, 0 );
		step( 21 ); // 45 ticks after the first shot: one play of the clip would have ended by now
		CHECK( state().graph[1].state == pistolShoot );
		step( 15 );
		CHECK( state().graph[1].state == pistolHold );
	}

	// The bat: its pack's swing starts on melee.swing, and the clip's marker emits melee.strike once,
	// 0.4 s in.
	command( CommandType::Aim, 0, 0, 0 );
	command( CommandType::SwapLayer, 1, ShippedPacks::Bat, 0 );
	step( 30 );
	CHECK( state().graph[1].source == ShippedPacks::Bat && state().graph[1].state == packs.StateOf( ShippedPacks::Bat, "Ready" ) );
	auto strikes = [&]() {
		int n = 0;
		for ( uint32_t i = 0; i < std::min( sim.Globals().modEventCount, kModEventHistory ); ++i )
		{
			n += sim.Globals().modEvents[i].type == ShippedPacks::Strike ? 1 : 0;
		}
		return n;
	};
	command( CommandType::Event, ShippedPacks::Swing, 0, 0 );
	step( 1 );
	CHECK( state().graph[1].state == packs.StateOf( ShippedPacks::Bat, "Swing" ) );
	step( 20 );
	CHECK( strikes() == 0 );
	step( 10 );
	CHECK( strikes() == 1 );

	// The placeholder rig, for which the bindings were made, holds an item the same way.
	AnimState aiming;
	aiming.aiming = 1;
	auto procedural = anim::AnimSet::CreateProcedural();
	anim::PoseEvaluator placeholder( *procedural );
	placeholder.Evaluate( aiming );
	CheckHeldItem( *procedural, placeholder );
}

// The viewer's own player ahead of the server (present/anim_lead.h), on the mannequin with the
// shipped bat and pistol packs. A press 12 ticks before the server answers: the swing starts on the
// press, its clock runs on evenly through the moment the server's event arrives, and the legs are
// left alone. A predicted event does the same for the pistol's recoil.
// M219: a character that is not a humanoid. The critter (characters/critter: four legs and a tail,
// bones of its own names) loads with no warnings; its aim and look chains and its eye are found by
// its own bone names; it is not taken for a humanoid (the mannequin and the built-in rig are); the
// simulation runs its state machine and the pose follows it (its legs swing as it trots); the
// humanoid packs fit none of its bones; the ragdoll rig finds nothing to hang on it; and a body of
// it, killed with a ragdoll asked for, falls as none.
void TestAnySkeleton()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/characters/critter/client/characters/critter";
	std::string error, warnings;
	std::shared_ptr<const anim::AnimSet> critter = anim::AnimSet::Load( dir, error, warnings );
	CHECK( critter != nullptr );
	if ( critter == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	std::printf( "    %s; warnings: %s\n", critter->Description().c_str(), warnings.empty() ? "none" : warnings.c_str() );
	CHECK( warnings.empty() );
	const int skull = anim::FindJoint( *critter, "Skull" ), neck = anim::FindJoint( *critter, "NeckBase" );
	CHECK( skull >= 0 && neck >= 0 && critter->AimTip() == skull && critter->AimJoints().size() == 2 &&
		   critter->LookJoints().size() == 2 && critter->EyeJoint() == skull && critter->FirstPerson() );
	auto mannequin = anim::AnimSet::Load( std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin", error, warnings );
	CHECK( critter->Humanoid() == false && mannequin != nullptr && mannequin->Humanoid() && anim::AnimSet::CreateProcedural()->Humanoid() );

	// Its state machine, run by the simulation; the pose follows it.
	ModSchema schema;
	MannequinSchema( schema );
	warnings.clear();
	auto graph = CompileAnimGraph( critter->GraphText(), schema, error, warnings );
	CHECK( graph != nullptr && warnings.empty() );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	ShippedPacks packs;
	LoadShippedPacks( schema, critter.get(), packs );
	size_t fitted = 0;
	for ( const auto& clips : packs.clips )
	{
		fitted += clips != nullptr ? 1 : 0;
	}
	std::printf( "    the humanoid packs fitted to it: %zu of %zu\n", fitted, packs.clips.size() );
	CHECK( fitted == 0 && packs.clips.size() == 2 );
	anim::PoseEvaluator pose( *critter );
	pose.SetGraph( graph, warnings );
	pose.SetPacks( packs.graphs, packs.clips );

	Simulation sim( TestConfig(), FlatMap() );
	Simulation::CharacterBody body;
	body.graph = graph;
	body.radius = critter->CapsuleRadius();
	body.halfHeight = critter->CapsuleHalfHeight();
	body.ragdoll = critter->Humanoid();
	body.packs = { 0, 0 }; // (as the server finds them: neither fits)
	sim.SetCharacters( { body } );
	sim.SetAnimPacks( packs.graphs );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	const flecs::entity e = sim.FindEntity( sim.PlayerNetId( 0 ) );
	auto kneeMoved = [&]() {
		pose.Evaluate( sim.GetAnimState( e ), nullptr );
		const int leg = anim::FindJoint( *critter, "LegFL2" );
		float v[4], r[4];
		ozz::math::StorePtrU( pose.Models()[size_t( leg )].cols[3], v );
		ozz::math::StorePtrU( critter->RestModels()[size_t( leg )].cols[3], r );
		return std::sqrt( ( v[0] - r[0] ) * ( v[0] - r[0] ) + ( v[1] - r[1] ) * ( v[1] - r[1] ) + ( v[2] - r[2] ) * ( v[2] - r[2] ) );
	};
	const float standing = kneeMoved();
	f.inputs[0].moveForward = 127;
	float trotting = 0.0f;
	for ( int i = 0; i < 40; ++i )
	{
		step( 1 );
		trotting = std::max( trotting, kneeMoved() );
	}
	std::printf( "    its front left knee from where it rests: %.3f m standing, up to %.3f m trotting\n", double( standing ), double( trotting ) );
	CHECK( trotting > standing + 0.05f );

	// A mod swapping its layer to a pack that does not fit it: its own layer plays on.
	SimCommand swap;
	swap.type = CommandType::SwapLayer;
	swap.target = SlotTarget( 0 );
	swap.index = 0;
	swap.value = ShippedPacks::Pistol;
	f.commands.push_back( swap );
	step( 2 );
	CHECK( sim.GetAnimState( e ).graph[0].source == 0 );

	// No ragdoll to hang on it, and none to fall as.
	const present::RagdollRig rig = present::BuildRagdollRig( *critter );
	CHECK( std::all_of( rig.anchorJoint.begin(), rig.anchorJoint.end(), []( int j ) { return j < 0; } ) );
	SimCommand kill;
	kill.type = CommandType::Kill;
	kill.target = SlotTarget( 0 );
	kill.mode = 1;
	f.commands.push_back( kill );
	step( 2 );
	int ragdolls = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		ragdolls += sim.FindEntity( r.netId ).has<Ragdoll>() ? 1 : 0;
	}
	CHECK( sim.PlayerCharacter( PlayerSlot( 0 ) )->dead && ragdolls == 0 );
}

void TestAnimLead()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin";
	std::string error, warnings;
	auto set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr );
	if ( set == nullptr )
	{
		return;
	}
	ModSchema schema;
	MannequinSchema( schema );
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	ShippedPacks packs;
	CHECK( graph != nullptr );
	LoadShippedPacks( schema, nullptr, packs );
	if ( graph == nullptr || packs.loaded == false )
	{
		return;
	}
	const int ready = packs.StateOf( ShippedPacks::Bat, "Ready" ), swing = packs.StateOf( ShippedPacks::Bat, "Swing" );
	const int hold = packs.StateOf( ShippedPacks::Pistol, "Hold" ), shoot = packs.StateOf( ShippedPacks::Pistol, "Shoot" );
	// What the packs read above the base layer: the swing and the shot, not the strike marker.
	CHECK( UpperLayersRead( *graph, packs.graphs, AnimExpr::VarKind::Event, ShippedPacks::Swing ) );
	CHECK( UpperLayersRead( *graph, packs.graphs, AnimExpr::VarKind::Event, ShippedPacks::Use ) );
	CHECK( UpperLayersRead( *graph, packs.graphs, AnimExpr::VarKind::Event, ShippedPacks::Strike ) == false );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	sim.SetAnimPacks( packs.graphs );
	const float dt = sim.Config().TimeStep();
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto state = [&]() { return sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) ); };
	auto command = [&]( CommandType type, uint8_t index, int32_t value ) {
		SimCommand c;
		c.type = type;
		c.target = SlotTarget( 0 );
		c.index = index;
		c.value = value;
		f.commands.push_back( c );
	};
	present::AnimLead lead;
	lead.netId = sim.PlayerNetId( 0 );
	lead.tickSeconds = dt;

	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	lead.netId = sim.PlayerNetId( 0 );
	command( CommandType::SwapLayer, 1, ShippedPacks::Bat ); // the bat is out
	step( 60 );
	CHECK( state().graph[1].source == ShippedPacks::Bat && state().graph[1].state == ready );

	// No lead: nothing changes.
	{
		AnimState now = state();
		AnimState same = present::LeadAnimState( now, *graph, packs.graphs, lead );
		CHECK( same == now );
	}

	// The press, 12 ticks before the server's stance.
	const int latency = 12;
	float previousTime = -1.0f;
	float worstStep = 0.0f;
	int firstSwing = -1;
	for ( int t = 0; t < 40; ++t )
	{
		if ( t == latency )
		{
			command( CommandType::Event, ShippedPacks::Swing, 0 ); // the server's answer: melee.swing
		}
		if ( t > 0 )
		{
			step( 1 );
		}
		lead.inputs.clear();
		lead.seconds = float( std::min( t, latency ) ) * dt;
		if ( t <= latency ) // still unanswered (the frame of the answer has both, as in the viewer)
		{
			present::AnimLead::Input input;
			input.age = float( t ) * dt;
			input.event = ShippedPacks::Swing;
			lead.inputs.push_back( input );
		}
		AnimState shown = state();
		AnimState led = present::LeadAnimState( shown, *graph, packs.graphs, lead );
		// The legs are the server's.
		CHECK( std::memcmp( &led.graph[0], &shown.graph[0], sizeof( AnimGraphLayerState ) ) == 0 );
		if ( t < latency )
		{
			CHECK( shown.graph[1].state == ready ); // the server has not swung yet
		}
		if ( led.graph[1].state == swing )
		{
			firstSwing = firstSwing < 0 ? t : firstSwing;
			if ( previousTime >= 0.0f && led.graph[1].time < 1.4f )
			{
				worstStep = std::max( worstStep, std::fabs( ( led.graph[1].time - previousTime ) - dt ) );
			}
			previousTime = led.graph[1].time;
		}
	}
	std::printf( "    led swing starts %d tick(s) after the press; its clock is off an even step by at most %.4f s\n", firstSwing, worstStep );
	CHECK( firstSwing == 0 ); // on the press itself
	CHECK( worstStep < 0.5f * dt ); // no jump when the server's stance arrives
	CHECK( previousTime > 35.0f * dt ); // and it ran on from the press, not from the answer

	// A predicted event: the shot's recoil state, before the server says the pistol fired.
	command( CommandType::SwapLayer, 1, ShippedPacks::Pistol );
	step( 60 );
	CHECK( state().graph[1].source == ShippedPacks::Pistol && state().graph[1].state == hold );
	lead.inputs.clear();
	present::AnimLead::Input shot;
	shot.age = 4.0f * dt;
	shot.event = ShippedPacks::Use;
	lead.inputs.push_back( shot );
	lead.seconds = 4.0f * dt;
	AnimState recoil = present::LeadAnimState( state(), *graph, packs.graphs, lead );
	CHECK( recoil.graph[1].state == shoot );
	CHECK( std::fabs( recoil.graph[1].time - 4.0f * dt ) < 0.5f * dt ); // as old as the press
	// An expired or refused prediction: no input, so the server's state as it is, only later.
	lead.inputs.clear();
	AnimState back = present::LeadAnimState( state(), *graph, packs.graphs, lead );
	CHECK( back.graph[1].state == hold );
}

// The character built from the paid animation pack, where it was built (it is never in the
// repository, so CI and fresh clones skip this): strafing plays its sideways jog, hips straight.
// The lead reads the packs too: an item's pack whose upper layer shoots on "test.shot", an event the
// character's own machine never reads, is led on the press like the character's own layers are. A
// pack's layer of the base's name is the base, and leads nothing.
void TestLeadReadsPacks()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin";
	std::string error, warnings;
	auto set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr );
	if ( set == nullptr )
	{
		return;
	}
	ModSchema schema;
	schema.layers = { "full", "upper" };
	schema.stances = { "melee", "melee_swing", "gun", "rifle" };
	schema.events = { "item.use", "melee.strike", "rifle.fired", "test.shot", "test.base" };
	schema.animPacks.push_back( { "test", "test.hold",
								  "cinderbox_graph\t1\n"
								  "clip\tPistol_Idle\t1.0\t1\n"
								  "clip\tPistol_Shoot\t0.5\t0\n"
								  "layer\tFullBody\n"
								  "state\tStand\tclip\tPistol_Idle\t0\n"
								  "transition\tStand\tStand\t1\t0.1\timmediate\t1\ttest.base\n"
								  "start\tStand\n"
								  "layer\tUpperBody\n"
								  "state\tHold\tclip\tPistol_Idle\t0\n"
								  "state\tShoot\tclip\tPistol_Shoot\t1\n"
								  "transition\tHold\tShoot\t1\t0.05\timmediate\t1\ttest.shot\n"
								  "transition\tShoot\tHold\t1\t0.1\tat_end\t1\t\n"
								  "start\tHold\n" } );
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	AnimGraphPacks packs = CompileAnimPacks( schema, warnings );
	CHECK( graph != nullptr && packs.size() == 1 && packs[0] != nullptr );
	if ( graph == nullptr || packs.empty() || packs[0] == nullptr )
	{
		std::printf( "    %s %s\n", error.c_str(), warnings.c_str() );
		return;
	}
	const int use = schema.FindEvent( "test.shot" );
	CHECK( graph->UpperLayersRead( AnimExpr::VarKind::Event, use ) == false ); // the character's own does not
	CHECK( UpperLayersRead( *graph, packs, AnimExpr::VarKind::Event, use ) );  // the pack's upper layer does
	CHECK( UpperLayersRead( *graph, packs, AnimExpr::VarKind::Event, schema.FindEvent( "test.base" ) ) == false ); // its base is not led

	// The pack's upper layer in place of the character's, and a press of "use" predicted.
	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	sim.SetAnimPacks( packs );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	SimCommand swap;
	swap.type = CommandType::SwapLayer;
	swap.target = SlotTarget( 0 );
	swap.index = 1;
	swap.value = 1;
	f.commands.push_back( swap );
	step( 30 );
	AnimState shown = sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) );
	CHECK( shown.graph.size() > 1 && shown.graph[1].source == 1 && shown.graph[1].state == 0 );
	present::AnimLead lead;
	lead.netId = sim.PlayerNetId( 0 );
	lead.tickSeconds = sim.Config().TimeStep();
	lead.seconds = 6.0f * lead.tickSeconds;
	present::AnimLead::Input press;
	press.age = 6.0f * lead.tickSeconds;
	press.event = use;
	lead.inputs.push_back( press );
	AnimState led = present::LeadAnimState( shown, *graph, packs, lead );
	std::printf( "    the pack's upper layer, led 6 ticks from the press: state %u, %.3f s in\n", unsigned( led.graph[1].state ),
				 led.graph[1].time );
	CHECK( led.graph[1].state == 1 && led.graph[1].time > 0.0f ); // Shoot, on the press
}

// Another player's swing first seen late (present/anim_catch_up.h), on the mannequin's machine: the
// server's state is 15 ticks into the swing when the viewer first sees it. It is drawn from its
// start and is level with the state within the catch-up's time; one seen on its first tick, or a
// player first drawn mid-swing, is drawn as it is; a correction that comes back to it goes on.
void TestAnimCatchUp()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin";
	std::string error, warnings;
	auto set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr );
	if ( set == nullptr )
	{
		return;
	}
	ModSchema schema;
	MannequinSchema( schema );
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	ShippedPacks packs;
	CHECK( graph != nullptr );
	LoadShippedPacks( schema, nullptr, packs );
	if ( graph == nullptr || packs.loaded == false )
	{
		return;
	}
	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	sim.SetAnimPacks( packs.graphs );
	const float dt = sim.Config().TimeStep();
	InputFrame f;
	auto step = [&]() {
		f.tick = sim.Tick();
		sim.Step( f );
		f.events.clear();
		f.commands.clear();
	};
	auto command = [&]( CommandType type, uint8_t index, int32_t value ) {
		SimCommand c;
		c.type = type;
		c.target = SlotTarget( 0 );
		c.index = index;
		c.value = value;
		f.commands.push_back( c );
	};
	auto state = [&]() { return sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) ); };
	f.events.push_back( { PlayerEventType::Join, 0 } );
	for ( int i = 0; i < 30; ++i )
	{
		step();
	}
	command( CommandType::SwapLayer, 1, ShippedPacks::Bat ); // the bat is out
	for ( int i = 0; i < 60; ++i )
	{
		step();
	}
	const AnimState ready = state();
	// The swing, tick by tick, as the server has it.
	command( CommandType::Event, ShippedPacks::Swing, 0 );
	std::vector<AnimState> swing;
	for ( int i = 0; i < 40; ++i )
	{
		step();
		swing.push_back( state() );
	}
	const uint8_t swingState = swing[20].graph[1].state;
	CHECK( swingState != ready.graph[1].state );

	// Seen 15 ticks late: standing until then, then the server's state as it is now.
	present::AnimCatchUp memory;
	const int late = 15;
	for ( int i = 0; i < 20; ++i )
	{
		present::CatchUpAnimState( ready, *graph, packs.graphs, memory, dt, dt );
	}
	float previous = -1.0f;
	int level = -1;
	bool forward = true;
	float first = -1.0f;
	for ( int i = late; i < 40; ++i )
	{
		AnimState drawn = present::CatchUpAnimState( swing[size_t( i )], *graph, packs.graphs, memory, dt, dt );
		CHECK( std::memcmp( &drawn.graph[0], &swing[size_t( i )].graph[0], sizeof( AnimGraphLayerState ) ) == 0 ); // the legs as they are
		float t = drawn.graph[1].time;
		first = first < 0.0f ? t : first;
		forward = forward && t > previous;
		previous = t;
		if ( level < 0 && std::fabs( t - swing[size_t( i )].graph[1].time ) < 1e-5f )
		{
			level = i - late;
		}
	}
	std::printf( "    a swing first seen %.3f s in is drawn from %.3f s and level %d tick(s) later\n", swing[late].graph[1].time, first,
				 level );
	CHECK( swing[late].graph[1].time > 0.2f );
	CHECK( first < 0.5f * dt );								 // from its start
	CHECK( forward );										 // never back
	CHECK( level > 0 && float( level ) * dt <= present::kCatchUpSeconds + 1.5f * dt ); // and soon level
	CHECK( memory.started == 1 );

	// A correction that leaves it for a frame and comes back to the same play: on from where it was.
	AnimState back = present::CatchUpAnimState( ready, *graph, packs.graphs, memory, dt, dt );
	(void)back;
	AnimState again = present::CatchUpAnimState( swing[39], *graph, packs.graphs, memory, dt, dt );
	CHECK( std::fabs( again.graph[1].time - swing[39].graph[1].time ) < 1e-5f );
	CHECK( memory.started == 1 );

	// Seen on its first tick: as it is.
	present::AnimCatchUp onTime;
	present::CatchUpAnimState( ready, *graph, packs.graphs, onTime, dt, dt );
	AnimState drawn = present::CatchUpAnimState( swing[0], *graph, packs.graphs, onTime, dt, dt );
	CHECK( drawn == swing[0] );
	CHECK( onTime.started == 0 );

	// A player first drawn mid-swing (a join): as it is.
	present::AnimCatchUp joined;
	drawn = present::CatchUpAnimState( swing[20], *graph, packs.graphs, joined, dt, dt );
	CHECK( drawn == swing[20] );
	CHECK( joined.started == 0 );
}

void TestUalMannequin()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/ual_mannequin";
	if ( std::filesystem::exists( dir + "/anim.cfg" ) == false )
	{
		std::printf( "    skipped: the paid pack's character is not built here\n" );
		return;
	}
	std::string error, warnings;
	auto set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr && warnings.empty() );
	if ( set == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	CHECK( set->TurnLegs() == false );
	ModSchema schema;
	schema.stances = { "melee", "melee_swing", "pistol", "rifle" };
	schema.events = { "pistol.fired", "melee.strike", "rifle.fired", "combat.hurt" };
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	CHECK( graph != nullptr && warnings.empty() );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	anim::PoseEvaluator pose( *set );
	pose.SetGraph( graph, warnings );
	CHECK( warnings.empty() );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	InputFrame f;
	f.events.push_back( { PlayerEventType::Join, 0 } );
	SimCommand face;
	face.type = CommandType::Facing;
	face.target = SlotTarget( 0 );
	face.mode = 1;
	f.commands.push_back( face );
	for ( int i = 0; i < 120; ++i )
	{
		f.tick = sim.Tick();
		f.inputs[0].moveRight = i >= 30 ? int8_t( 127 ) : int8_t( 0 );
		sim.Step( f );
		f.events.clear();
		f.commands.clear();
	}
	AnimState state = sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) );
	auto clips = anim::ActiveClips( state, *graph );
	std::printf( "    strafing right: blend (%.2f, %.2f), leg yaw %.2f, playing %s\n", state.graph[0].blend, state.graph[0].blendY,
				 state.legYaw, clips.empty() ? "nothing" : clips[0].name.c_str() );
	CHECK( clips.empty() == false && clips[0].name == "Jog_Right" );

	// The hips face where the body faces although the legs' yaw says sideways: no twist.
	pose.Evaluate( state );
	float hips[4];
	ozz::math::StorePtrU( pose.Models()[size_t( anim::FindJoint( *set, "Hips" ) )].cols[2], hips );
	std::printf( "    hips z axis (%.2f %.2f %.2f)\n", hips[0], hips[1], hips[2] );
	CHECK( std::fabs( state.legYaw ) > 1.0f ); // the simulation still says the legs go sideways
	CHECK( hips[2] > 0.7f );					 // but the hips only lean as the clip leans them

	// Every direction, camera-facing: the chest and the head look where the body faces, whatever
	// the legs do (the strafe clips turn hips and chest up to 50 degrees; face_forward undoes it).
	CHECK( set->FaceForward() );
	auto yawOf = [&]( const char* joint ) {
		int j = anim::FindJoint( *set, joint );
		float m[3][4], r[3][4];
		for ( int i = 0; i < 3; ++i )
		{
			ozz::math::StorePtrU( pose.Models()[size_t( j )].cols[i], m[i] );
			ozz::math::StorePtrU( set->RestModels()[size_t( j )].cols[i], r[i] );
		}
		float f[3] = { 0.0f, 0.0f, 0.0f };
		for ( int i = 0; i < 3; ++i )
		{
			float nm = std::sqrt( m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2] );
			float nr = std::sqrt( r[i][0] * r[i][0] + r[i][1] * r[i][1] + r[i][2] * r[i][2] );
			for ( int k = 0; k < 3; ++k )
			{
				f[k] += m[i][k] / nm * ( r[i][2] / nr );
			}
		}
		return std::atan2( f[0], f[2] ) * 57.29578f;
	};
	const int8_t directions[8][2] = { { 0, 127 }, { 90, 90 }, { 127, 0 }, { 90, -90 }, { 0, -127 }, { -90, -90 }, { -127, 0 }, { -90, 90 } };
	for ( const auto& d : directions )
	{
		// Settle into the direction, then average over a second: a jog twists the chest back and
		// forth with every stride, so any one instant says little.
		float hipsYaw = 0.0f, chest = 0.0f, head = 0.0f;
		std::string playing;
		for ( int i = 0; i < 120; ++i )
		{
			f.tick = sim.Tick();
			f.inputs[0].moveRight = d[0];
			f.inputs[0].moveForward = d[1];
			sim.Step( f );
			if ( i < 60 )
			{
				continue;
			}
			AnimState now = sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) );
			pose.Evaluate( now );
			hipsYaw += yawOf( "Hips" ) / 60.0f;
			chest += yawOf( "UpperChest" ) / 60.0f;
			head += yawOf( "Head" ) / 60.0f;
			auto clips = anim::ActiveClips( now, *graph );
			playing = clips.empty() ? "-" : clips[0].name;
		}
		std::printf( "    right %4d forward %4d: %-10s average hips %6.1f chest %6.1f head %6.1f\n", d[0], d[1], playing.c_str(), hipsYaw,
					 chest, head );
		CHECK( std::fabs( chest ) < 15.0f );
		CHECK( std::fabs( head ) < 15.0f );
	}
}

// Retargeting: a clip rebuilt for its own skeleton plays the same pose (the math is a turn from
// rest, onto the same rest); for another profile skeleton it keeps that skeleton's proportions and
// moves the same joints.
void TestRetarget()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin";
	std::string error, warnings;
	auto set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr );
	if ( set == nullptr )
	{
		return;
	}
	const ozz::animation::Animation* walk = set->NamedClip( "Walk" );
	CHECK( walk != nullptr );
	CHECK( anim::SameSkeleton( set->Skeleton(), set->Skeleton() ) );
	auto procedural = anim::AnimSet::CreateProcedural();
	CHECK( anim::SameSkeleton( set->Skeleton(), procedural->Skeleton() ) == false );

	auto again = anim::RetargetClip( *walk, set->Skeleton(), set->Skeleton(), 30.0f, error );
	CHECK( again != nullptr );
	if ( again == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	// Both sampled into model space halfway through: joints within a couple of millimetres (the
	// rebuilt clip is resampled at 30 Hz).
	auto models = [&]( const ozz::animation::Animation& clip, const ozz::animation::Skeleton& skeleton, float ratio ) {
		ozz::animation::SamplingJob::Context context( skeleton.num_joints() );
		ozz::vector<ozz::math::SoaTransform> locals( size_t( skeleton.num_soa_joints() ) );
		ozz::vector<ozz::math::Float4x4> out( size_t( skeleton.num_joints() ) );
		ozz::animation::SamplingJob sampling;
		sampling.animation = &clip;
		sampling.context = &context;
		sampling.ratio = ratio;
		sampling.output = ozz::make_span( locals );
		sampling.Run();
		ozz::animation::LocalToModelJob ltm;
		ltm.skeleton = &skeleton;
		ltm.input = ozz::make_span( locals );
		ltm.output = ozz::make_span( out );
		ltm.Run();
		return out;
	};
	float worst = 0.0f;
	auto a = models( *walk, set->Skeleton(), 0.5f );
	auto b = models( *again, set->Skeleton(), 0.5f );
	for ( size_t j = 0; j < a.size(); ++j )
	{
		float va[4], vb[4];
		ozz::math::StorePtrU( a[j].cols[3], va );
		ozz::math::StorePtrU( b[j].cols[3], vb );
		worst = std::max( worst, std::sqrt( ( va[0] - vb[0] ) * ( va[0] - vb[0] ) + ( va[1] - vb[1] ) * ( va[1] - vb[1] ) + ( va[2] - vb[2] ) * ( va[2] - vb[2] ) ) );
	}
	std::printf( "    Walk rebuilt for its own skeleton: worst joint off by %.4f m\n", worst );
	CHECK( worst < 0.005f );

	// Onto the placeholder rig: the same joints move (the legs swing), its own bone lengths stay.
	auto onto = anim::RetargetClip( *walk, set->Skeleton(), procedural->Skeleton(), 30.0f, error );
	CHECK( onto != nullptr && onto->num_tracks() == procedural->Skeleton().num_joints() );
	if ( onto == nullptr )
	{
		return;
	}
	auto early = models( *onto, procedural->Skeleton(), 0.0f );
	auto late = models( *onto, procedural->Skeleton(), 0.5f );
	int foot = anim::FindJoint( *procedural, "LeftFoot" );
	int knee = anim::FindJoint( *procedural, "LeftLowerLeg" );
	float f0[4], f1[4], k1[4];
	ozz::math::StorePtrU( early[size_t( foot )].cols[3], f0 );
	ozz::math::StorePtrU( late[size_t( foot )].cols[3], f1 );
	ozz::math::StorePtrU( late[size_t( knee )].cols[3], k1 );
	float shin = std::sqrt( ( f1[0] - k1[0] ) * ( f1[0] - k1[0] ) + ( f1[1] - k1[1] ) * ( f1[1] - k1[1] ) + ( f1[2] - k1[2] ) * ( f1[2] - k1[2] ) );
	std::printf( "    onto the placeholder rig: left foot z %.2f -> %.2f, shin %.2f m (its own 0.42)\n", f0[2], f1[2], shin );
	CHECK( std::fabs( f1[2] - f0[2] ) > 0.1f );
	CHECK( std::fabs( shin - 0.42f ) < 0.01f );
}

// What the hands hold wins the arms: with an item's hold in the upper body, an overlay of the whole
// body (a dash's pose) moves the body (the head) but leaves both arms as the hold has them; an overlay whose
// filter names the left hand (a throw) takes that arm, and the right stays with the hold; with
// nothing held, the dash's pose has the arms too.
void TestHeldArms()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin";
	std::string error, warnings;
	std::shared_ptr<const anim::AnimSet> set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr );
	if ( set == nullptr )
	{
		return;
	}
	ModSchema schema;
	schema.animPacks.push_back( { "test", "test.hold",
								  "cinderbox_graph\t1\nclip\tSprint\t0.6666667\t1\nlayer\tUpperBody\nstate\tHold\tclip\tSprint\t0\nstart\tHold\n" } );
	schema.animPacks.push_back( { "test", "test.dash",
								  "cinderbox_graph\t1\nclip\tJump_Land\t1.2666667\t0\nlayer\tDash\nstate\tDash\tclip\tJump_Land\t0\nstart\tDash\n" } );
	schema.animPacks.push_back( { "test", "test.throw",
								  "cinderbox_graph\t1\nclip\tJump\t2.5\t1\nlayer\tThrow\n"
								  "mask\tLeftShoulder\tLeftUpperArm\tLeftLowerArm\tLeftHand\n"
								  "state\tThrow\tclip\tJump\t0\nstart\tThrow\n" } );
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	AnimGraphPacks packs = CompileAnimPacks( schema, warnings );
	CHECK( graph != nullptr && packs.size() == 3 && packs[0] != nullptr && packs[1] != nullptr && packs[2] != nullptr );
	if ( graph == nullptr || packs.size() != 3 )
	{
		std::printf( "    %s %s\n", error.c_str(), warnings.c_str() );
		return;
	}
	std::vector<std::shared_ptr<const anim::PackClips>> fitted;
	for ( const auto& pack : packs )
	{
		fitted.push_back( anim::FitPack( set, *pack, *set, warnings ) );
	}
	SimConfig config = TestConfig();
	config.layers = uint8_t( LayerSlots( *graph ) );
	Simulation sim( config, FlatMap() );
	sim.SetAnimGraph( graph );
	sim.SetAnimPacks( packs );
	anim::PoseEvaluator pose( *set );
	pose.SetGraph( graph, warnings );
	pose.SetPacks( packs, fitted );
	InputFrame f;
	auto swap = [&]( size_t layer, int source ) {
		SimCommand c;
		c.type = CommandType::SwapLayer;
		c.target = SlotTarget( 0 );
		c.index = uint16_t( layer );
		c.value = source;
		f.commands.push_back( c );
		for ( int i = 0; i < 45; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	struct Joints
	{
		b3Vec3 left, right, head;
	};
	// The pose of this moment, and of the same moment without the overlay (its weight 0): the hands
	// where the chest has them (a hold's shape, wherever the body bends), the head in the model.
	auto joints = [&]( bool overlaid ) {
		AnimState s = sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) );
		if ( overlaid == false )
		{
			s.graph[graph->layers.size()].weight = 0.0f;
		}
		pose.Evaluate( s );
		const ozz::math::Float4x4 chest = ozz::math::Invert( pose.Models()[size_t( anim::FindJoint( *set, "UpperChest" ) )] );
		auto at = [&]( int j, bool fromChest ) {
			float v[4];
			ozz::math::StorePtrU( ( fromChest ? chest * pose.Models()[size_t( j )] : pose.Models()[size_t( j )] ).cols[3], v );
			return b3Vec3{ v[0], v[1], v[2] };
		};
		return Joints{ at( set->ArmJoints( true ).hand, true ), at( set->ArmJoints( false ).hand, true ),
					   at( anim::FindJoint( *set, "Head" ), false ) };
	};
	const size_t overlay = graph->layers.size();
	f.events.push_back( { PlayerEventType::Join, 0 } );
	swap( 1, 0 );
	swap( overlay, 2 ); // the dash's pose, nothing held
	const Joints bare = joints( false ), bareDash = joints( true );
	swap( 1, 1 ); // the hold, the dash's pose over it
	const Joints held = joints( false ), heldDash = joints( true );
	swap( overlay, 3 ); // the throw over the hold
	const Joints heldThrow = joints( true ), heldNoThrow = joints( false );
	std::printf( "    the dash's pose, nothing held: hands move %.3f / %.3f m. Over a hold: hands %.4f / %.4f m, the head %.3f m. "
				 "A throw over a hold: left hand %.3f m, right %.4f m\n",
				 b3Distance( bare.left, bareDash.left ), b3Distance( bare.right, bareDash.right ), b3Distance( held.left, heldDash.left ),
				 b3Distance( held.right, heldDash.right ), b3Distance( held.head, heldDash.head ), b3Distance( heldNoThrow.left, heldThrow.left ),
				 b3Distance( heldNoThrow.right, heldThrow.right ) );
	CHECK( b3Distance( bare.left, bareDash.left ) > 0.05f && b3Distance( bare.right, bareDash.right ) > 0.05f ); // no hold: its arms
	CHECK( b3Distance( held.head, heldDash.head ) > 0.02f );													   // over a hold: the body
	CHECK( b3Distance( held.left, heldDash.left ) < 0.001f && b3Distance( held.right, heldDash.right ) < 0.001f ); // and the hold's arms
	CHECK( b3Distance( heldNoThrow.left, heldThrow.left ) > 0.05f && b3Distance( heldNoThrow.right, heldThrow.right ) < 0.001f ); // a throw takes its hand
}

// An overlay that moves the other hand takes it off a two-handed item (the grapple's arm while a
// rifle is out): with the overlay in, the grip changes nothing; without it, the hand goes to the grip.
void TestOverlayGrip()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin";
	std::string error, warnings;
	std::shared_ptr<const anim::AnimSet> set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr );
	if ( set == nullptr )
	{
		return;
	}
	ModSchema schema;
	schema.stances = { "melee", "melee_swing", "gun", "rifle" };
	schema.events = { "item.use", "melee.strike", "rifle.fired" };
	// A layer the character does not have, for the left arm alone, always in.
	schema.animPacks.push_back( { "test", "test.arm",
								  "cinderbox_graph\t1\n"
								  "clip\tJump_Land\t1.2666667\t0\n"
								  "layer\tArm\n"
								  "mask\tLeftShoulder\tLeftUpperArm\tLeftLowerArm\tLeftHand\n"
								  "state\tUp\tclip\tJump_Land\t0\n"
								  "start\tUp\n" } );
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	AnimGraphPacks packs = CompileAnimPacks( schema, warnings );
	CHECK( graph != nullptr && packs.size() == 1 && packs[0] != nullptr );
	if ( graph == nullptr || packs.empty() || packs[0] == nullptr )
	{
		std::printf( "    %s %s\n", error.c_str(), warnings.c_str() );
		return;
	}
	auto fitted = anim::FitPack( set, *packs[0], *set, warnings );
	SimConfig config = TestConfig();
	config.layers = uint8_t( LayerSlots( *graph ) );
	Simulation sim( config, FlatMap() );
	sim.SetAnimGraph( graph );
	sim.SetAnimPacks( packs );
	anim::PoseEvaluator pose( *set );
	pose.SetGraph( graph, warnings );
	pose.SetPacks( packs, { fitted } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto overlay = [&]( int source ) {
		SimCommand c;
		c.type = CommandType::SwapLayer;
		c.target = SlotTarget( 0 );
		c.index = uint16_t( graph->layers.size() );
		c.value = source;
		f.commands.push_back( c );
	};
	// The left hand with the grip, and how far that is from where it is without one.
	const int left = set->ArmJoints( true ).hand;
	anim::HandGrip grip;
	grip.position = { 0.15f, 0.0f, -0.35f };
	auto moved = [&]() {
		AnimState s = sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) );
		pose.Evaluate( s );
		float a[4], b[4];
		ozz::math::StorePtrU( pose.Models()[size_t( left )].cols[3], a );
		pose.Evaluate( s, &grip );
		ozz::math::StorePtrU( pose.Models()[size_t( left )].cols[3], b );
		return b3Distance( { a[0], a[1], a[2] }, { b[0], b[1], b[2] } );
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 30 );
	float without = moved();
	overlay( 1 );
	step( 30 );
	AnimState a = sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) );
	size_t slot = graph->layers.size();
	CHECK( a.graph.size() > slot && a.graph[slot].source == 1 && a.graph[slot].weight > 0.99f );
	float with = moved();
	overlay( 0 );
	step( 30 );
	float after = moved();
	std::printf( "    the grip moves the left hand %.3f m; with the arm's overlay in %.4f m; after %.3f m\n", without, with, after );
	CHECK( without > 0.05f && with < 1e-5f && after > 0.05f );
}

// A mod's animation pack: its "FullBody" layer swapped in for a player's own, in the simulation and in
// the pose; its "UpperBody" (which it does not have) stays the character's; restored on command.
void TestLayerSwap()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/godot/characters/mannequin";
	std::string error, warnings;
	std::shared_ptr<const anim::AnimSet> set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr );
	if ( set == nullptr )
	{
		return;
	}
	ModSchema schema;
	schema.stances = { "melee", "melee_swing", "gun", "rifle" };
	schema.events = { "item.use", "melee.strike", "rifle.fired" };
	// The pack's own graph: a crouch (standing in: the landing clip, held) on the FullBody layer.
	schema.animPacks.push_back( { "test", "test.crouch",
								  "cinderbox_graph\t1\n"
								  "clip\tJump_Land\t1.2666667\t0\n"
								  "layer\tFullBody\n"
								  "state\tCrouch\tclip\tJump_Land\t0\n"
								  "start\tCrouch\n" } );
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	AnimGraphPacks packs = CompileAnimPacks( schema, warnings );
	CHECK( graph != nullptr && packs.size() == 1 && packs[0] != nullptr );
	if ( graph == nullptr || packs.empty() || packs[0] == nullptr )
	{
		std::printf( "    %s %s\n", error.c_str(), warnings.c_str() );
		return;
	}
	// The pack's clips come from its own baked files; here the mannequin's stand in for them (same
	// skeleton: they fit as they are).
	auto fitted = anim::FitPack( set, *packs[0], *set, warnings );
	CHECK( fitted->clips.size() == 1 && fitted->clips[0] == set->NamedClip( "Jump_Land" ) );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
	sim.SetAnimPacks( packs );
	anim::PoseEvaluator pose( *set );
	pose.SetGraph( graph, warnings );
	pose.SetPacks( packs, { fitted } );
	InputFrame f;
	auto step = [&]( int n ) {
		for ( int i = 0; i < n; ++i )
		{
			f.tick = sim.Tick();
			sim.Step( f );
			f.events.clear();
			f.commands.clear();
		}
	};
	auto swap = [&]( int layer, int source ) {
		SimCommand c;
		c.type = CommandType::SwapLayer;
		c.target = SlotTarget( 0 );
		c.index = uint16_t( layer );
		c.value = source;
		f.commands.push_back( c );
	};
	auto head = [&]() {
		pose.Evaluate( sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) ) );
		float v[4];
		ozz::math::StorePtrU( pose.Models()[size_t( anim::FindJoint( *set, "Head" ) )].cols[3], v );
		return v[1];
	};
	f.events.push_back( { PlayerEventType::Join, 0 } );
	step( 60 );
	float standing = head();

	swap( 0, 1 );
	swap( 1, 1 ); // the pack has no Upper: that layer stays the character's
	step( 30 );
	AnimState a = sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) );
	CHECK( a.graph[0].source == 1 && a.graph[0].state == 0 );
	std::vector<anim::ActiveClip> clips = anim::ActiveClips( a, *graph, packs );
	CHECK( clips.empty() == false && clips[0].name == "Jump_Land" );
	float crouched = head();

	swap( 0, 0 );
	step( 30 );
	a = sim.GetAnimState( sim.FindEntity( sim.PlayerNetId( 0 ) ) );
	CHECK( a.graph[0].source == 0 );
	float again = head();
	std::printf( "    head: standing %.2f, the pack's crouch %.2f, restored %.2f\n", standing, crouched, again );
	CHECK( crouched < standing - 0.2f );
	CHECK( std::fabs( again - standing ) < 0.1f );
}

void TestAnimPipeline()
{
	auto procedural = anim::AnimSet::CreateProcedural();
	CHECK( procedural != nullptr );
	CHECK( procedural->Skeleton().num_joints() > 20 );
	// Feet on the ground, head up, in its rest.
	anim::PoseEvaluator eval( *procedural );
	eval.Evaluate( AnimState{} );
	float minY = 1e9f, maxY = -1e9f;
	for ( const auto& m : eval.Models() )
	{
		float y = ozz::math::GetY( m.cols[3] );
		minY = std::min( minY, y );
		maxY = std::max( maxY, y );
	}
	std::printf( "    idle pose height: %.3f .. %.3f\n", minY, maxY );
	CHECK( minY > -0.05f && minY < 0.08f );
	CHECK( maxY > 1.6f && maxY < 2.0f );

	// Save to .ozz files and load back through the asset path: identical poses.
	std::filesystem::path dir = std::filesystem::temp_directory_path() / "cinderbox_anim_test";
	std::filesystem::create_directories( dir );
	CHECK( procedural->Save( dir.string() ) );
	std::string error, warnings;
	auto loaded = anim::AnimSet::Load( dir.string(), error, warnings );
	if ( loaded == nullptr )
	{
		std::printf( "    load failed: %s\n", error.c_str() );
	}
	CHECK( loaded != nullptr );
	CHECK( warnings.empty() );
	CHECK( loaded != nullptr && loaded->GraphText() == procedural->GraphText() );
	uint64_t a = AnimPoseHash( *procedural );
	uint64_t b = AnimPoseHash( *loaded );
	std::printf( "    pose hash %016" PRIx64 " (procedural) %016" PRIx64 " (from files)\n", a, b );
	CHECK( a == b );
	CHECK( loaded != nullptr && loaded->Movement().empty() );

	// How a character moves is in its anim.cfg (CbCharacter.movement bakes it): known names, in
	// range; the rest is said and left out or clamped.
	{
		std::ofstream cfg( dir / "anim.cfg", std::ios::app );
		cfg << "\nmove.walk_speed = 2.4\nmove.max_fall = 99999\nmove.fly_speed = 3\nmove.gravity = lots\n";
	}
	warnings.clear();
	loaded = anim::AnimSet::Load( dir.string(), error, warnings );
	CHECK( loaded != nullptr );
	if ( loaded != nullptr )
	{
		const auto& movement = loaded->Movement();
		std::printf( "    movement: %zu values; %s\n", movement.size(), warnings.c_str() );
		CHECK( movement.size() == 2 );
		CHECK( movement.size() == 2 && movement[0].first == int( MoveParam::MaxFall ) && movement[0].second == 1000.0f );
		CHECK( movement.size() == 2 && movement[1].first == int( MoveParam::WalkSpeed ) && movement[1].second == 2.4f );
		CHECK( warnings.find( "move.fly_speed" ) != std::string::npos && warnings.find( "move.gravity" ) != std::string::npos &&
			   warnings.find( "move.max_fall" ) != std::string::npos );
	}
	std::filesystem::remove_all( dir );
}

// The cross-build check also runs a scenario with motions (sim/motions.h): the reference scenario
// has none, and what they compute (directions from the look, parameters while they last, fields)
// has to be the same on every build too. Its hashes follow the reference scenario's.
void AppendMotionReference( std::vector<uint64_t>& hashes )
{
	ModSchema schema;
	schema.fields.push_back( { "m.count", BoardType::Int, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "m.fuel", BoardType::Float, BoardScope::Entity, 1 } );
	schema.events = { "m.dashed" };
	// The scenario's players press the first two action bits at random.
	schema.actions.push_back( { "a", 0, "" } );
	schema.actions.push_back( { "b", 1, "" } );
	schema.motionSets.push_back( { "m", "m.moves",
								   "cinderbox_motions\t2\n"
								   "motion\tDash\nwhen\tpress\ta\nif\tspeed > 0.5 or not grounded\ncooldown\t0.3\nduration\t0.2\n"
								   "impulse\tself\t9\tlook\tnone\nparam\tfriction\t0\nparam\tgravity\t6\n"
								   "change\tm.count\t+=\t1\nchange\tm.fuel\t-=\t0.25\nemit\tm.dashed\n"
								   "motion\tHop\nwhen\tpress\tb\nuses\t2\tground\nduration\t0.5\nimpulse\tself\t5\tmove\tvertical\nparam\tair_control\t1\n"
								   "motion\tBlink\nwhen\tpress\tjump\nif\tnot grounded and vertical_speed < 2\nuses\t1\t0.75\n"
								   "impulse\tself\t7\tworld\tall\t0.3\t1\t-0.2\n"
								   "motion\tBurst\nwhen\tpress\tsprint\ncooldown\t1\nimpulse\tself\t-4\tfacing\thorizontal\n"
								   "motion\tSoar\nwhen\twhile\nif\theld.b and m.fuel > -40\ncooldown\t0.5\n"
								   "force\tself\tlook\taccel\t20\t0\t0\t0\nparam\tmove_frame\t1\nparam\tair_friction\t2\nparam\tgravity\t3\n"
								   "change\tm.fuel\t-=\t3\nchange\tm.count\t=\t0\nemit\tm.dashed\n"
								   "motion\tStun\nwhen\tevent\tm.dashed\nduration\t0.25\nparam\tjump_speed\t9\n"
								   // A probe, a force that reacts and a link: the ray, the pull and the rope on whatever the look finds.
								   "motion\tHook\nwhen\tpress\ta\nif\tm.fuel < -1 and not linked\ncooldown\t1.5\nprobe\t30\t0\nforce\tself\tto\tforce\t1400\t0\t0.2\t1\nlink\thit\t0\t2\n"
								   "until\tairborne_time > 1 or m.count > 6 or ( pressed.b and pressed.jump )\nparam\tfriction\t0\n" } );
	std::string warnings;
	auto motions = CompileMotions( schema, warnings );
	if ( motions == nullptr || motions->list.size() != 7 || warnings.empty() == false )
	{
		std::printf( "the motion reference did not compile: %s\n", warnings.c_str() );
		std::abort();
	}
	test::ScenarioOptions options;
	options.ticks = 600;
	options.seed = 4321;
	auto frames = test::MakeScenario( options );
	Simulation sim( TestConfig() );
	sim.SetMotions( motions );
	for ( const InputFrame& frame : frames )
	{
		sim.Step( frame );
		hashes.push_back( sim.ComputeHash() );
	}
}

// And a scenario with slots: players who come and go are given items (tools, stacks, ones used once
// a press or while held by their own keys, one for the left hand), press the slots' keys, move,
// split, drop and use them, and open chests (containers) that were filled and one that is
// destroyed. What slots compute (which copy a key finds, merges, splits, what falls where) has to
// be the same on every build too. Its hashes follow the motion scenario's.
void AppendSlotsReference( std::vector<uint64_t>& hashes )
{
	LevelLayout map = FlatMap();
	EntityTemplate chest;
	chest.name = "chest";
	chest.visual = "prop_heavy";
	chest.components.push_back( MakeIntComponent( "Shape", { { "kind", int32_t( ShapeKind::Box ) } } ) );
	chest.components.push_back( MakeIntComponent( "Body", { { "type", 0 } } ) );
	chest.components.push_back( MakeIntComponent( "Container", { { "slots", 6 } } ) );
	map.templates.push_back( chest );
	QuantizeLayout( map );
	SimConfig config = TestConfig();
	config.slots = 9;
	config.slotHand = 0;
	ItemShape tool;		 // a tool, in slot 1 if it is free
	tool.slot = 1;
	tool.usedEvent = 0;
	ItemShape ball;		 // a stack of 5, used once a press, its own key the action of bit 0
	ball.kind = 1;
	ball.half = { 0.1f, 0.1f, 0.1f };
	ball.center = { 0.0f, 0.0f, 0.0f };
	ball.stack = 5;
	ball.use = kItemUsePress;
	ball.useAction = 0;
	ball.usedEvent = 1;
	ItemShape jet = tool; // used while held, its own key bit 1, and its slot's key
	jet.slot = 4;
	jet.use = kItemUseHold;
	jet.useAction = 1;
	jet.slotKeyUses = 1;
	jet.usedEvent = 2;
	ItemShape shield = tool; // the left hand while carried
	shield.slot = 0;
	shield.hand = kItemHandLeft;
	shield.usedEvent = 0xFFFF;
	const uint16_t kinds = 4;

	test::ScenarioOptions options;
	options.ticks = 900;
	options.seed = 8765;
	options.initialPlayers = 4;
	options.maxPlayers = 6;
	std::vector<InputFrame> frames = test::MakeScenario( options );
	uint64_t rng = 0x5107;
	uint8_t seq[kMaxPlayers] = {};
	const uint32_t kChest = 0xFFFFFF00u; // a chest by its place among the three, until it exists
	for ( InputFrame& f : frames )
	{
		if ( f.tick == 5 )
		{
			// Three chests ahead of the spawn, filled by commands (a mod's), as the chest mod does.
			for ( int i = 0; i < 3; ++i )
			{
				SimCommand c;
				c.type = CommandType::SpawnProp;
				c.value = 0;
				c.a = { -2.0f + 2.0f * float( i ), 0.5f, 3.0f };
				f.commands.push_back( c );
			}
		}
		for ( int p = 0; p < kMaxPlayers; ++p )
		{
			PlayerInput& in = f.inputs[p];
			uint64_t r = NextRandom( rng );
			// Given something now and then (into a free slot, onto a stack); a chest too.
			if ( ( r & 31 ) == 0 )
			{
				SimCommand c;
				c.type = CommandType::SpawnItem;
				c.target = SlotTarget( PlayerSlot( p ) );
				c.index = uint16_t( ( r >> 8 ) % kinds );
				c.mode = kNoSocket;
				c.value = 1;
				const uint64_t where = r >> 5 & 7;
				if ( where == 0 )
				{
					c.target = 0; // lying about
					c.a = { test::ScenarioFloat( r >> 12, -3.0f, 6.0f ), 1.0f, test::ScenarioFloat( r >> 20, 1.0f, 4.0f ) };
				}
				else if ( where == 1 && f.tick > 10 )
				{
					c.target = kChest + uint32_t( ( r >> 12 ) % 3 ); // into a chest (its NetId is known once it is made)
				}
				f.commands.push_back( c );
			}
			// A slot's key, now and then held for a few ticks.
			in.slotKeys = ( r >> 28 & 15 ) < 2 ? uint16_t( 1u << ( ( r >> 32 ) % 10 ) ) : uint16_t( 0 );
			// An intent: select, move, split, drop, open, close; a slot of the open chest at times.
			if ( ( r >> 36 & 15 ) == 0 )
			{
				uint64_t s = NextRandom( rng );
				auto slot = [&]( uint64_t bits ) {
					uint8_t at = uint8_t( bits % 9 );
					return ( bits >> 4 & 3 ) == 0 ? uint8_t( kContainerSlotBit | ( at % 6 ) ) : at;
				};
				in.intent = uint8_t( 1 + s % uint64_t( kLastSlotIntent ) );
				in.intentA = slot( s >> 8 );
				in.intentB = slot( s >> 16 );
				in.intentSeq = ++seq[p];
			}
			else
			{
				in.intentSeq = seq[p];
			}
		}
		if ( f.tick == 600 )
		{
			// The middle chest is destroyed: what it held falls out.
			SimCommand c;
			c.type = CommandType::Destroy;
			c.target = kChest + 1;
			f.commands.push_back( c );
		}
	}
	Simulation sim( config, map );
	sim.SetItemShapes( { tool, ball, jet, shield } );
	std::vector<uint32_t> chests;
	for ( InputFrame& frame : frames )
	{
		if ( frame.tick == 6 )
		{
			for ( const Simulation::EntityRef& e : sim.Entities() )
			{
				if ( flecs::entity( sim.World(), e.entity ).has<Container>() )
				{
					chests.push_back( e.netId );
				}
			}
			if ( chests.size() != 3 )
			{
				std::printf( "the slots reference has %zu chests, not 3\n", chests.size() );
				std::abort();
			}
		}
		for ( SimCommand& c : frame.commands )
		{
			if ( c.target >= kChest && c.target < kChest + 3 )
			{
				c.target = chests[c.target - kChest];
			}
		}
		sim.Step( frame );
		hashes.push_back( sim.ComputeHash() );
	}
}

// The fourth reference run: a level of every piece shape (hulls built by Box3D from the map's points,
// pieces rolled), and boxes and balls dropped onto it. Every build must build the same hulls and
// land the same things on them. The points are written out, not computed, so no libm is involved.
// Two breakable walls: one broken whole, one holed three times (its top falls).
void AppendShapesReference( std::vector<uint64_t>& hashes )
{
	LevelLayout map;
	map.name = "shapes_reference";
	map.spawnCenter = { 0.0f, 1.5f, 20.0f };
	map.spawnRadius = 2.0f;
	map.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { 30.0f, 0.5f, 30.0f }, 0.0f, 0.0f } );
	// A pyramid, an eight-sided drum, a wedge and a lumpy rock.
	map.hulls.push_back( { { -1.5f, 0.0f, -1.5f }, { 1.5f, 0.0f, -1.5f }, { 1.5f, 0.0f, 1.5f }, { -1.5f, 0.0f, 1.5f }, { 0.0f, 2.0f, 0.0f } } );
	map.hulls.push_back( {} );
	const float drum[8][2] = { { 1.0f, 0.0f }, { 0.7071f, 0.7071f }, { 0.0f, 1.0f }, { -0.7071f, 0.7071f },
							   { -1.0f, 0.0f }, { -0.7071f, -0.7071f }, { 0.0f, -1.0f }, { 0.7071f, -0.7071f } };
	for ( const auto& rim : drum )
	{
		map.hulls.back().push_back( { rim[0], -0.75f, rim[1] } );
		map.hulls.back().push_back( { rim[0], 0.75f, rim[1] } );
	}
	map.hulls.push_back( { { -2.0f, 0.0f, -1.0f }, { 2.0f, 0.0f, -1.0f }, { -2.0f, 0.0f, 1.0f }, { 2.0f, 0.0f, 1.0f }, { 2.0f, 1.2f, -1.0f },
						   { 2.0f, 1.2f, 1.0f } } );
	map.hulls.push_back( { { 0.9f, 0.1f, 0.2f }, { -0.8f, 0.3f, 0.6f }, { 0.1f, 1.1f, -0.2f }, { 0.3f, -0.6f, -0.9f }, { -0.5f, -0.4f, -0.7f },
						   { 0.6f, 0.7f, 0.8f }, { -0.2f, 0.9f, 0.7f }, { 0.4f, -0.2f, 1.0f }, { -1.0f, 0.2f, -0.3f } } );
	map.statics.push_back( { { -6.0f, 0.0f, 0.0f }, { 1.5f, 2.0f, 1.5f }, 0.0f, 0.3f, 0.0f, ShapeKind::Hull, 0 } );
	map.statics.push_back( { { 0.0f, 0.6f, 0.0f }, { 1.0f, 0.75f, 1.0f }, 0.0f, 0.0f, 1.5707964f, ShapeKind::Hull, 1 } ); // lying
	map.statics.push_back( { { 6.0f, 0.0f, 0.0f }, { 2.0f, 1.2f, 1.0f }, 0.0f, -0.7f, 0.0f, ShapeKind::Hull, 2 } );
	map.statics.push_back( { { 0.0f, 0.8f, -6.0f }, { 1.0f, 1.1f, 1.0f }, 0.4f, 1.1f, -0.6f, ShapeKind::Hull, 3 } );
	map.statics.push_back( { { 0.0f, 0.5f, 6.0f }, { 2.0f, 0.1f, 0.6f }, 0.2f, 0.5f, 0.35f, ShapeKind::Box } );
	map.statics.push_back( { { -6.0f, 0.6f, 6.0f }, { 0.6f, 1.0f, 0.0f }, 0.0f, 0.0f, 1.2f, ShapeKind::Capsule } );
	map.statics.push_back( { { 6.0f, 0.0f, 6.0f }, { 1.2f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Sphere } );
	// Boxes and balls above each piece, a little off its middle so they roll and tumble.
	for ( int row = 0; row < 3; ++row )
	{
		for ( int column = 0; column < 3; ++column )
		{
			float x = -6.0f + 6.0f * float( column ) + 0.3f;
			float z = -6.0f + 6.0f * float( row ) + 0.2f;
			map.props.push_back( { ShapeKind::Box, { x, 4.0f, z }, { 0.3f, 0.3f, 0.3f } } );
			map.props.push_back( { ShapeKind::Sphere, { x - 0.4f, 6.0f, z + 0.3f }, { 0.35f, 0.0f, 0.0f } } );
		}
	}
	// Bodies of several shapes, a hull among them, turned and dropped among the pieces.
	map.shapeSets.push_back( { { { -1.0f, 0.0f, 0.0f }, { 0.4f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Sphere },
							   { { 1.0f, 0.0f, 0.0f }, { 0.4f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, ShapeKind::Sphere },
							   { { 0.0f, 0.0f, 0.0f }, { 0.1f, 0.8f, 0.0f }, 0.0f, 0.0f, 1.5707964f, ShapeKind::Capsule } } );
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.1f, 0.3f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box },
							   { { 0.9f, 0.5f, 0.0f }, { 0.1f, 0.6f, 0.3f }, 0.2f, 0.0f, 0.0f, ShapeKind::Box },
							   { { -0.6f, 0.4f, 0.0f }, { 1.0f, 1.1f, 1.0f }, 0.0f, 0.5f, 0.3f, ShapeKind::Hull, 3 } } );
	EntityTemplate dumbbell;
	dumbbell.name = "dumbbell";
	dumbbell.components.push_back( MakeComponent( "Material", { { "mass", 12.0f } } ) );
	dumbbell.shapes = 0;
	EntityTemplate ell;
	ell.name = "ell";
	ell.shapes = 1;
	// A breakable wall of six bricks (rubble), broken by a blast a second in.
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 1.5f, 1.0f, 0.25f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );
	map.shapeSets.push_back( { { { 0.0f, 0.0f, 0.0f }, { 0.49f, 0.32f, 0.24f }, 0.0f, 0.0f, 0.0f, ShapeKind::Box } } );
	EntityTemplate wall;
	wall.name = "wall";
	wall.components.push_back( MakeIntComponent( "Body", { { "type", 0 } } ) );
	wall.shapes = uint32_t( map.shapeSets.size() - 2 );
	for ( int i = 0; i < 6; ++i )
	{
		wall.pieces.push_back( { 3, { -1.0f + float( i % 3 ), i < 3 ? -0.66f : 0.0f, 0.0f }, 0.0f, 0.0f, 0.05f * float( i ) } );
	}
	EntityTemplate brick;
	brick.name = "brick";
	brick.shapes = uint32_t( map.shapeSets.size() - 1 );
	AuthoredComponent rubble;
	rubble.id = Fnv32( "Debris" );
	brick.components.push_back( rubble );
	// Another of 3 x 3 bricks, held at its foot, that blasts hole: its middle, its foot's two ends
	// (then its top falls).
	EntityTemplate holed = wall;
	holed.name = "holed";
	holed.pieces.clear();
	for ( int i = 0; i < 9; ++i )
	{
		TemplatePiece piece = { 3, { -1.0f + float( i % 3 ), -0.66f + 0.66f * float( i / 3 ), 0.0f } };
		piece.held = i < 3;
		holed.pieces.push_back( piece );
		if ( i % 3 < 2 )
		{
			holed.links.push_back( { uint16_t( i ), uint16_t( i + 1 ) } );
		}
		if ( i < 6 )
		{
			holed.links.push_back( { uint16_t( i ), uint16_t( i + 3 ) } );
		}
	}
	map.templates = { dumbbell, ell, wall, brick, holed };
	map.instances.push_back( { 2, { -6.0f, 1.0f, -6.0f }, 0.0f, 0.4f } );
	map.instances.push_back( { 4, { 10.0f, 0.99f, 9.0f }, 0.0f, 0.3f } );
	map.instances.push_back( { 0, { -5.0f, 5.0f, 0.5f }, 0.3f, 0.2f, 0.1f } );
	map.instances.push_back( { 1, { 0.5f, 6.0f, -5.5f }, -0.2f, 0.9f, 0.4f } );
	map.instances.push_back( { 0, { 5.5f, 7.0f, 5.0f }, 0.0f, 1.2f, -0.3f } );
	map.instances.push_back( { 1, { 0.2f, 8.0f, 0.3f }, 0.5f, 0.0f, 0.0f } );
	// Level surfaces: a trough of triangles and a bumpy patch of terrain (one cell a hole), turned,
	// with balls rolling on them.
	LevelMesh trough;
	trough.vertices = { { -3.0f, 1.5f, -2.0f }, { 0.0f, 0.0f, -2.0f }, { 3.0f, 1.5f, -2.0f },
						{ -3.0f, 1.5f, 2.0f },	{ 0.0f, 0.2f, 2.0f },  { 3.0f, 1.5f, 2.0f } };
	trough.indices = { 0, 4, 1, 0, 3, 4, 1, 5, 2, 1, 4, 5 };
	map.meshes.push_back( trough );
	map.statics.push_back( { { 14.0f, 0.0f, 0.0f }, { 3.0f, 1.5f, 2.0f }, 0.0f, 0.3f, 0.05f, ShapeKind::Mesh, 0 } );
	LevelHeightField bumps;
	bumps.countX = 7;
	bumps.countZ = 6;
	bumps.scale = { 1.5f, 0.75f, 1.25f };
	const float rows[6][7] = { { 0, 1, 2, 1, 0, 1, 0 }, { 1, 2, 3, 2, 1, 0, 1 }, { 0, 1, 2, 0.5f, 0, 1, 2 },
							   { 2, 1, 0, 0, 1, 2, 1 }, { 1, 0, 1, 2, 3, 2, 0 }, { 0, 1, 0, 1, 2, 1, 0 } };
	for ( const auto& row : rows )
	{
		bumps.heights.insert( bumps.heights.end(), std::begin( row ), std::end( row ) );
	}
	bumps.holes.assign( 30, 0 );
	bumps.holes[2 * 6 + 3] = 1;
	map.heightFields.push_back( bumps );
	map.statics.push_back( { { -18.0f, 0.0f, -4.0f }, { 9.0f, 2.25f, 6.25f }, 0.0f, 0.2f, 0.0f, ShapeKind::HeightField, 0 } );
	for ( int i = 0; i < 4; ++i )
	{
		map.props.push_back( { ShapeKind::Sphere, { 13.0f + 0.6f * float( i ), 4.0f + float( i ), -1.0f + 0.5f * float( i ) }, { 0.3f, 0.0f, 0.0f } } );
		map.props.push_back( { ShapeKind::Box, { -15.0f + 1.3f * float( i ), 5.0f + float( i ), -1.0f + 0.7f * float( i ) }, { 0.3f, 0.3f, 0.3f } } );
	}
	QuantizeLayout( map );
	Simulation sim( TestConfig(), map );
	// Items of several parts (a hammer: a turned handle, a head, a wedge hull), dropped among them.
	ItemShape hammer;
	hammer.half = { 0.24f, 0.06f, 0.68f };
	hammer.center = {};
	hammer.mass = 2.0f;
	ItemPart handle;
	handle.kind = uint8_t( ShapeKind::Capsule );
	handle.half = { 0.03f, 0.3f, 0.0f };
	handle.center = { 0.0f, 0.0f, -0.3f };
	handle.turn[0] = 0.70711f;
	handle.turn[3] = 0.70711f;
	ItemPart head;
	head.kind = uint8_t( ShapeKind::Box );
	head.half = { 0.12f, 0.05f, 0.05f };
	head.center = { 0.0f, 0.0f, -0.62f };
	ItemPart wedge;
	wedge.kind = uint8_t( ShapeKind::Hull );
	wedge.half = { 0.06f, 0.05f, 0.05f };
	wedge.center = { 0.18f, 0.0f, -0.62f };
	wedge.points = { { 0.0f, -0.05f, -0.05f }, { 0.0f, 0.05f, -0.05f }, { 0.0f, 0.0f, 0.05f }, { 0.06f, 0.0f, 0.0f } };
	hammer.parts = { handle, head, wedge };
	sim.SetItemShapes( { hammer } );
	hashes.push_back( sim.ComputeHash() );
	InputFrame frame;
	for ( uint32_t tick = 0; tick < 360; ++tick )
	{
		frame.tick = sim.Tick();
		frame.commands.clear();
		if ( tick == 60 )
		{
			for ( const Simulation::EntityRef& r : sim.Entities() )
			{
				const TemplateRef* ref = flecs::entity( sim.World(), r.entity ).try_get<TemplateRef>();
				if ( ref != nullptr && ref->index == 2 )
				{
					SimCommand c;
					c.type = CommandType::Break;
					c.target = r.netId;
					c.a = { -5.5f, 1.2f, -4.0f };
					c.b = { 9.0f, 0.0f, 0.0f };
					frame.commands.push_back( c );
				}
			}
		}
		// The holes: a blast at the middle, then at the foot's ends (whatever of it stands nearest).
		const b3Vec3 holes[3] = { { 10.0f, 0.99f, 9.6f }, { 9.04f, 0.33f, 9.9f }, { 10.95f, 0.33f, 9.3f } };
		for ( int k = 0; k < 3; ++k )
		{
			if ( tick != 120u + 60u * uint32_t( k ) )
			{
				continue;
			}
			for ( const Simulation::EntityRef& r : sim.Entities() )
			{
				flecs::entity e( sim.World(), r.entity );
				const TemplateRef* ref = e.try_get<TemplateRef>();
				if ( ( ref != nullptr && ref->index == 4 ) || e.has<WallPiece>() )
				{
					SimCommand c;
					c.type = CommandType::Break;
					c.target = r.netId;
					c.a = { holes[k].x, holes[k].y, holes[k].z };
					c.b = { 5.0f, 0.75f, 0.0f };
					frame.commands.push_back( c );
					break;
				}
			}
		}
		if ( tick < 3 )
		{
			SimCommand c;
			c.type = CommandType::SpawnItem;
			c.target = 0;
			c.index = 0;
			c.mode = kNoSocket;
			c.value = 1;
			c.a = { -5.5f + 5.0f * float( tick ), 4.0f + float( tick ), 0.3f * float( tick ) };
			frame.commands.push_back( c );
		}
		sim.Step( frame );
		hashes.push_back( sim.ComputeHash() );
	}
	// (The holed wall: of its nine, the one brick between the two blasted at its foot stands.)
	int standing = 0;
	for ( const Simulation::EntityRef& r : sim.Entities() )
	{
		standing += flecs::entity( sim.World(), r.entity ).has<WallPiece>() ? 1 : 0;
	}
	CHECK( standing == 1 );
}

int DumpHashes( const char* path )
{
	auto frames = test::MakeScenario( {} );
	auto hashes = RunReference( frames, TestConfig() );
	AppendMotionReference( hashes );
	AppendSlotsReference( hashes );
	AppendShapesReference( hashes );
	FILE* f = std::fopen( path, "w" );
	if ( f == nullptr )
	{
		std::printf( "cannot open %s\n", path );
		return 1;
	}
	for ( size_t i = 0; i < hashes.size(); ++i )
	{
		std::fprintf( f, "%zu %016" PRIx64 "\n", i, hashes[i] );
	}
	std::fclose( f );
	std::printf( "wrote %zu hashes, final %016" PRIx64 "\n", hashes.size(), hashes.back() );
	return 0;
}

int CompareHashes( const char* path )
{
	auto frames = test::MakeScenario( {} );
	auto hashes = RunReference( frames, TestConfig() );
	AppendMotionReference( hashes );
	AppendSlotsReference( hashes );
	AppendShapesReference( hashes );
	FILE* f = std::fopen( path, "r" );
	if ( f == nullptr )
	{
		std::printf( "cannot open %s\n", path );
		return 1;
	}
	size_t index;
	uint64_t hash;
	size_t count = 0;
	while ( std::fscanf( f, "%zu %" SCNx64, &index, &hash ) == 2 )
	{
		if ( index >= hashes.size() || hashes[index] != hash )
		{
			std::printf( "MISMATCH at tick %zu\n", index );
			std::fclose( f );
			return 1;
		}
		++count;
	}
	std::fclose( f );
	if ( count != hashes.size() )
	{
		std::printf( "MISMATCH: expected %zu hashes, file has %zu\n", hashes.size(), count );
		return 1;
	}
	std::printf( "all %zu tick hashes match\n", count );
	return 0;
}

// Portable snapshot across processes/compilers: save at tick 300 in one build, load in another,
// and check the continued run against the loading build's own reference.
constexpr uint32_t kPortableTick = 300;

int SavePortableFile( const char* path )
{
	auto frames = test::MakeScenario( {} );
	Simulation sim( TestConfig() );
	for ( uint32_t t = 0; t < kPortableTick; ++t )
	{
		sim.Step( frames[t] );
	}
	std::vector<uint8_t> image;
	sim.SavePortable( image );
	FILE* f = std::fopen( path, "wb" );
	if ( f == nullptr || std::fwrite( image.data(), 1, image.size(), f ) != image.size() )
	{
		std::printf( "cannot write %s\n", path );
		return 1;
	}
	std::fclose( f );
	std::printf( "wrote %zu bytes\n", image.size() );
	return 0;
}

int LoadPortableFile( const char* path )
{
	FILE* f = std::fopen( path, "rb" );
	if ( f == nullptr )
	{
		std::printf( "cannot open %s\n", path );
		return 1;
	}
	std::vector<uint8_t> image;
	uint8_t buf[65536];
	size_t n;
	while ( ( n = std::fread( buf, 1, sizeof( buf ), f ) ) > 0 )
	{
		image.insert( image.end(), buf, buf + n );
	}
	std::fclose( f );

	auto frames = test::MakeScenario( {} );
	auto reference = RunReference( frames, TestConfig() );
	Simulation sim( TestConfig() );
	if ( sim.LoadPortable( image ) == false )
	{
		std::printf( "LoadPortable failed\n" );
		return 1;
	}
	for ( uint32_t t = kPortableTick; t < frames.size(); ++t )
	{
		if ( sim.ComputeHash() != reference[t] )
		{
			std::printf( "MISMATCH at tick %u\n", t );
			return 1;
		}
		sim.Step( frames[t] );
	}
	if ( sim.ComputeHash() != reference.back() )
	{
		std::printf( "MISMATCH at final tick\n" );
		return 1;
	}
	std::printf( "portable snapshot continued identically for %zu ticks\n", frames.size() - kPortableTick );
	return 0;
}

// --- The viewer protocol as bytes (present/view_codec.h, view_file.h) ----------------------------

// The frames of a scripted session, the way a source would publish them: a ViewFrame per tick.
std::vector<present::ViewFrame> ScenarioViewFrames( uint32_t ticks )
{
	test::ScenarioOptions options;
	options.ticks = ticks;
	std::vector<InputFrame> inputs = test::MakeScenario( options );
	SimConfig config;
	config.physicsArenaMB = 64;
	Simulation sim( config );

	ModSchema schema;
	schema.mods = { "pistol", "deathmatch" };
	schema.events = { "pistol.fired", "combat.killed" };
	schema.characters.front().name = "mannequin";

	std::vector<present::ViewFrame> frames;
	present::ViewFrame f;
	for ( const InputFrame& in : inputs )
	{
		sim.Step( in );
		f.serial += 1;
		f.state = "playing";
		f.rate = 1.0f;
		f.alphaAtPublish = float( in.tick % 7 ) / 7.0f;
		f.localPressed = uint16_t( in.tick % 5 == 0 ? 2 : 0 );
		f.mapHash = 0x1234;
		f.mapName = "sandbox";
		f.templateNames = { "ball" };
		f.templateVisuals = { "prop_bouncy" };
		f.schema = schema;
		f.schemaGeneration = 1;
		// A name changes halfway, the way a late joiner's would.
		f.names[0] = "Ada";
		f.names[3] = in.tick < ticks / 2 ? "" : "Grace";
		f.namesGeneration = in.tick < ticks / 2 ? 1 : 2;
		f.stats.clear();
		f.stats.push_back( { "tick", int64_t( in.tick ) } );
		f.stats.push_back( { "clock_error", 0.25 * double( in.tick % 3 ) } );
		f.stats.push_back( { "fp_environment_ok", true } );
		f.stats.push_back( { "fingerprint", std::string( "9c11ed5d70f50c6" ) } );
		present::CaptureFrame( sim, f.frame );
		f.frame.resetGeneration = 1;
		f.frame.rolledBack = in.tick % 11 == 0;
		f.frame.localNetId = sim.Globals().playerNetIds[0];
		f.frame.hasInputs = true;
		f.frame.inputs = in.inputs;
		f.hasWorld = true;
		frames.push_back( f );
	}
	return frames;
}

// A frame standing alone as bytes: two frames are the same exactly when these are.
std::vector<uint8_t> WholeBytes( const present::ViewFrame& frame )
{
	std::vector<uint8_t> bytes;
	present::EncodeView( frame, nullptr, 0.0, bytes );
	return bytes;
}

void TestViewCodec()
{
	std::vector<present::ViewFrame> frames = ScenarioViewFrames( 600 );

	// Every frame survives the trip, whole (every 100th) or as a delta against the one before, and
	// decodes to the same bytes it was.
	present::ViewFrame decoded[2];
	std::vector<uint8_t> packet;
	size_t deltaBytes = 0, deltaMax = 0, deltaCount = 0, wholeBytes = 0, wholeCount = 0;
	size_t most = 0;
	bool allEqual = true;
	bool allDecoded = true;
	for ( size_t i = 0; i < frames.size(); ++i )
	{
		bool whole = i % 100 == 0;
		const present::ViewFrame* encodeBase = whole ? nullptr : &frames[i - 1];
		const present::ViewFrame* decodeBase = whole ? nullptr : &decoded[( i + 1 ) % 2];
		present::EncodeView( frames[i], encodeBase, 0.0, packet );
		CHECK( present::ViewPacketBase( packet.data(), packet.size() ) == ( whole ? 0 : frames[i - 1].serial ) );
		present::ViewFrame& out = decoded[i % 2];
		allDecoded &= present::DecodeView( packet.data(), packet.size(), decodeBase, out );
		allEqual &= WholeBytes( out ) == WholeBytes( frames[i] );
		most = std::max( most, frames[i].frame.entities.size() );
		( whole ? wholeBytes : deltaBytes ) += packet.size();
		( whole ? wholeCount : deltaCount ) += 1;
		deltaMax = whole ? deltaMax : std::max( deltaMax, packet.size() );
	}
	CHECK( allDecoded );
	CHECK( allEqual );
	std::printf( "    up to %zu entities: a whole frame is %zu bytes, a delta %zu on average (%zu at most)\n", most,
				 wholeBytes / wholeCount, deltaBytes / deltaCount, deltaMax );
	CHECK( deltaBytes / deltaCount < wholeBytes / wholeCount );

	// A tick in which nothing moved costs next to nothing, however big the world.
	present::ViewFrame still = frames.back();
	still.serial += 1;
	still.frame.tick += 1;
	present::EncodeView( still, &frames.back(), 0.0, packet );
	std::printf( "    a tick in which nothing changed: %zu bytes\n", packet.size() );
	CHECK( packet.size() < 128 );

	// Typed text and the confirmed tick (a live connection's): in a whole packet, the lines again
	// only when more came; without either, the packet is as it was (a recording's).
	{
		present::ViewFrame plain = frames.back();
		std::vector<uint8_t> before;
		present::EncodeView( plain, nullptr, 0.0, before );
		present::ViewFrame live = plain;
		live.confirmedTick = live.frame.tick - 3;
		live.textCount = 2;
		live.texts = { { "chat", 1, "hello" }, { "chat", kNoSlot, "Slow down" } };
		std::vector<uint8_t> bytes;
		present::EncodeView( live, nullptr, 0.0, bytes );
		present::ViewFrame got;
		CHECK( present::DecodeView( bytes.data(), bytes.size(), nullptr, got ) );
		CHECK( got.confirmedTick == live.confirmedTick && got.textCount == 2 && got.texts == live.texts );
		present::ViewFrame next = live;
		next.serial += 1;
		next.confirmedTick += 1;
		present::EncodeView( next, &live, 0.0, bytes );
		present::ViewFrame same = got;
		CHECK( present::DecodeView( bytes.data(), bytes.size(), &got, same ) );
		CHECK( same.texts == live.texts && same.textCount == 2 && same.confirmedTick == next.confirmedTick );
		std::vector<uint8_t> again;
		present::EncodeView( plain, nullptr, 0.0, again );
		CHECK( again == before );
		present::ViewFrame back;
		CHECK( present::DecodeView( again.data(), again.size(), nullptr, back ) && back.confirmedTick == 0 && back.texts.empty() );
	}

	// The local player's private fields: in a whole packet, in a delta when they changed, and
	// otherwise the base's, at no cost.
	{
		present::ViewFrame first = frames.back();
		first.privates.values[3] = 41;
		first.privates.values[30] = -5;
		std::vector<uint8_t> bytes;
		present::EncodeView( first, nullptr, 0.0, bytes );
		present::ViewFrame got;
		CHECK( present::DecodeView( bytes.data(), bytes.size(), nullptr, got ) );
		CHECK( std::memcmp( &got.privates, &first.privates, sizeof( Blackboard ) ) == 0 );
		present::ViewFrame second = first;
		second.serial += 1;
		present::EncodeView( second, &first, 0.0, bytes );
		size_t unchanged = bytes.size();
		present::ViewFrame same = got;
		CHECK( present::DecodeView( bytes.data(), bytes.size(), &got, same ) );
		CHECK( same.privates.values[3] == 41 && same.privates.values[30] == -5 );
		second.privates.values[3] = 0;
		second.privates.values[7] = 12;
		present::EncodeView( second, &first, 0.0, bytes );
		present::ViewFrame changed = got;
		CHECK( present::DecodeView( bytes.data(), bytes.size(), &got, changed ) );
		CHECK( changed.privates.values[3] == 0 && changed.privates.values[7] == 12 && changed.privates.values[30] == -5 );
		std::printf( "    private fields: %zu bytes more when two of them changed\n", bytes.size() - unchanged );
		CHECK( bytes.size() - unchanged < 16 );
	}

	// Compact packets (what a small view file holds): the sender keeps exact frames, the receiver only
	// what it decoded, and the two must still agree on every grid point after 600 deltas in a row.
	// Agreement is: what came through the chain of deltas is bit for bit what one whole compact
	// packet of the sender's frame decodes to.
	auto seenCompact = []( const present::ViewFrame& frame ) {
		std::vector<uint8_t> bytes;
		present::EncodeView( frame, nullptr, 0.0, bytes, present::ViewPrecision::Compact );
		present::ViewFrame seen;
		present::DecodeView( bytes.data(), bytes.size(), nullptr, seen );
		return seen;
	};
	present::ViewFrame received[2];
	size_t compactBytes = 0, compactMax = 0, compactWholeBytes = 0;
	bool compactDecoded = true;
	bool compactAgrees = true;
	float worstPosition = 0.0f;
	float worstTurn = 0.0f;
	for ( size_t i = 0; i < frames.size(); ++i )
	{
		present::EncodeView( frames[i], i == 0 ? nullptr : &frames[i - 1], 0.0, packet, present::ViewPrecision::Compact );
		present::ViewFrame& out = received[i % 2];
		compactDecoded &= present::DecodeView( packet.data(), packet.size(), i == 0 ? nullptr : &received[( i + 1 ) % 2], out );
		compactAgrees &= WholeBytes( out ) == WholeBytes( seenCompact( frames[i] ) );
		( i == 0 ? compactWholeBytes : compactBytes ) += packet.size();
		compactMax = i == 0 ? compactMax : std::max( compactMax, packet.size() );
		// How far the picture is from the truth: half a grid step, a fraction of a degree.
		for ( size_t e = 0; compactDecoded && e < frames[i].frame.entities.size(); ++e )
		{
			const present::FrameEntity& truth = frames[i].frame.entities[e];
			const present::FrameEntity& seen = out.frame.entities[e];
			if ( truth.holder != 0 )
			{
				continue; // drawn in its holder's hand: it has no place of its own
			}
			worstPosition = std::max( worstPosition, b3Length( b3Sub( truth.transform.position, seen.transform.position ) ) );
			float dot = std::fabs( truth.transform.rotation.v.x * seen.transform.rotation.v.x + truth.transform.rotation.v.y * seen.transform.rotation.v.y +
								   truth.transform.rotation.v.z * seen.transform.rotation.v.z + truth.transform.rotation.s * seen.transform.rotation.s );
			worstTurn = std::max( worstTurn, 2.0f * std::acos( std::min( dot, 1.0f ) ) );
		}
	}
	CHECK( compactDecoded );
	CHECK( compactAgrees );
	std::printf( "    compact: a whole frame is %zu bytes, a delta %zu on average (%zu at most); off by at most %.2f mm and %.2f degrees\n",
				 compactWholeBytes, compactBytes / ( frames.size() - 1 ), compactMax, worstPosition * 1000.0f, worstTurn * 57.29578f );
	CHECK( compactBytes / ( frames.size() - 1 ) < deltaBytes / deltaCount / 2 );
	CHECK( worstPosition < 0.002f && worstTurn < 0.01f );
	const present::ViewFrame& compactLast = received[( frames.size() - 1 ) % 2];
	// (Inputs are not part of a compact packet: nobody draws them.)
	CHECK( compactLast.stats == frames.back().stats && compactLast.frame.hasInputs == false );
	CHECK( compactLast.frame.modEventCount == frames.back().frame.modEventCount );
	// Damaged compact deltas, like damaged exact packets, fail or decode; none crash.
	{
		std::vector<uint8_t> delta;
		present::EncodeView( frames[300], &frames[299], 0.0, delta, present::ViewPrecision::Compact );
		present::ViewFrame base = seenCompact( frames[299] );
		base.serial = frames[299].serial;
		present::ViewFrame scratchFrame;
		uint64_t state = 7;
		for ( int i = 0; i < 3000; ++i )
		{
			std::vector<uint8_t> damaged = delta;
			for ( int k = 0, flips = 1 + int( NextRandom( state ) % 4 ); k < flips; ++k )
			{
				damaged[size_t( NextRandom( state ) % damaged.size() )] ^= uint8_t( 1u << ( NextRandom( state ) % 8 ) );
			}
			present::DecodeView( damaged.data(), damaged.size(), &base, scratchFrame );
		}
	}

	// A settled world costs nothing more than an exact one's.
	present::ViewFrame stillCompact = compactLast;
	stillCompact.serial += 1;
	stillCompact.frame.tick += 1;
	present::EncodeView( stillCompact, &compactLast, 0.0, packet, present::ViewPrecision::Compact );
	CHECK( packet.size() < 128 );

	// What is not in the world travels too.
	const present::ViewFrame& last = frames.back();
	const present::ViewFrame& got = decoded[( frames.size() - 1 ) % 2];
	CHECK( got.serial == last.serial && got.state == "playing" && got.rate == 1.0f );
	CHECK( got.alphaAtPublish == last.alphaAtPublish && got.localPressed == last.localPressed );
	CHECK( got.stats == last.stats );
	CHECK( got.mapHash == 0x1234 && got.mapName == "sandbox" && got.templateVisuals == last.templateVisuals );
	CHECK( got.schema.characters.front().name == "mannequin" && got.schema.events == last.schema.events );
	CHECK( got.names[0] == "Ada" && got.names[3] == "Grace" && got.namesGeneration == 2 );
	CHECK( got.frame.tick == last.frame.tick && got.frame.localNetId == last.frame.localNetId );
	CHECK( got.frame.entities.size() == last.frame.entities.size() && got.frame.ragdolls.size() == last.frame.ragdolls.size() );
	CHECK( got.frame.inputs == last.frame.inputs );

	// The age travels, not the sender's clock.
	present::EncodeView( last, nullptr, 0.5, packet );
	present::ViewFrame aged;
	CHECK( present::DecodeView( packet.data(), packet.size(), nullptr, aged ) );
	double age = present::ViewClock() - aged.publishedAt;
	CHECK( age > 0.49 && age < 1.0 );

	// A frame with no world yet (still connecting), and a rejected one.
	present::ViewFrame waiting;
	waiting.serial = 7;
	waiting.state = "rejected";
	waiting.stats.push_back( { "reject_reason", std::string( "the server is full" ) } );
	present::EncodeView( waiting, nullptr, 0.0, packet );
	present::ViewFrame waitingGot;
	CHECK( present::DecodeView( packet.data(), packet.size(), nullptr, waitingGot ) );
	CHECK( waitingGot.hasWorld == false && waitingGot.state == "rejected" && waitingGot.stats == waiting.stats );

	// A delta needs its base, and that base.
	present::EncodeView( frames[10], &frames[9], 0.0, packet );
	present::ViewFrame scratch;
	CHECK( present::DecodeView( packet.data(), packet.size(), nullptr, scratch ) == false );
	CHECK( present::DecodeView( packet.data(), packet.size(), &frames[8], scratch ) == false );
	CHECK( present::DecodeView( packet.data(), packet.size(), &frames[9], scratch ) );

	// Bytes that are not a packet are refused, never trusted: every truncation fails, and damaged
	// packets either fail or decode to something (no crash, no endless loop).
	bool truncationsFail = true;
	for ( size_t size = 0; size < packet.size(); size += 1 + size / 64 )
	{
		truncationsFail &= present::DecodeView( packet.data(), size, &frames[9], scratch ) == false;
	}
	CHECK( truncationsFail );
	uint64_t rng = 99;
	int survived = 0;
	std::vector<uint8_t> whole = WholeBytes( frames[300] );
	for ( int i = 0; i < 3000; ++i )
	{
		std::vector<uint8_t> damaged = whole;
		int flips = 1 + int( NextRandom( rng ) % 4 );
		for ( int k = 0; k < flips; ++k )
		{
			damaged[size_t( NextRandom( rng ) % damaged.size() )] ^= uint8_t( 1u << ( NextRandom( rng ) % 8 ) );
		}
		survived += present::DecodeView( damaged.data(), damaged.size(), nullptr, scratch ) ? 1 : 0;
	}
	std::printf( "    3000 damaged packets: %d still decoded, none crashed\n", survived );
}

// The world of a frame, without what a file source says for itself (its own serial, state, stats,
// timing, who is local).
std::vector<uint8_t> WorldBytes( present::ViewFrame frame )
{
	frame.serial = 0;
	frame.state.clear();
	frame.stats.clear();
	frame.alphaAtPublish = 0.0f;
	frame.rate = 0.0f;
	frame.localPressed = 0;
	frame.frame.localNetId = 0;
	frame.frame.resetGeneration = 0;
	frame.frame.rolledBack = false;
	return WholeBytes( frame );
}

void TestViewFile()
{
	std::vector<present::ViewFrame> frames = ScenarioViewFrames( 700 );
	for ( present::ViewFrame& f : frames )
	{
		f.frame.localNetId = 0; // as a server records it: nobody is local
	}
	std::filesystem::path path = std::filesystem::temp_directory_path() / "cinderbox_view_test.cbv";
	{
		present::ViewFileWriter writer;
		CHECK( writer.Open( path.string() ) );
		for ( const present::ViewFrame& f : frames )
		{
			writer.Add( f );
		}
	}
	auto stat = []( const present::ViewFrame& f, const char* name ) {
		for ( const present::ViewStat& s : f.stats )
		{
			if ( s.name == name )
			{
				return s.value;
			}
		}
		return decltype( present::ViewStat::value )( int64_t( -999 ) );
	};
	auto frameOf = []( const present::ViewFrame& f ) { return size_t( f.frame.tick - 1 ); }; // the scenario's tick t is frame t - 1

	present::ViewFileSource source( path.string() );
	CHECK( source.Length() == frames.size() );
	CHECK( source.TakesInput() == false );
	source.Control( "pause", 1.0 );
	present::ViewFrame got;
	CHECK( source.Take( got ) );
	CHECK( got.state == "playing" && got.hasWorld && got.rate == 0.0f );
	CHECK( frameOf( got ) == 0 && WorldBytes( got ) == WorldBytes( frames[0] ) );
	CHECK( std::get<bool>( stat( got, "replay_paused" ) ) );
	CHECK( std::fabs( std::get<double>( stat( got, "replay_length_seconds" ) ) - 700.0 / 60.0 ) < 1e-3 );
	// Nobody was local in the file: the first player there is gets followed.
	CHECK( got.frame.localNetId != 0 );
	CHECK( std::get<int64_t>( stat( got, "replay_follow" ) ) == 0 );
	CHECK( source.Take( got ) == false ); // paused: nothing new

	// Seeking lands on the frame asked for, across keys (every 300th) and back; each is a jump.
	uint64_t generation = got.frame.resetGeneration;
	for ( double seconds : { 5.0, 10.5, 0.0, 9.0, 4.99 } )
	{
		source.Control( "seek", seconds );
		CHECK( source.Take( got ) );
		size_t expected = size_t( seconds * 60.0 + 0.5 );
		CHECK( frameOf( got ) == expected );
		CHECK( WorldBytes( got ) == WorldBytes( frames[expected] ) );
		CHECK( got.frame.resetGeneration != generation );
		generation = got.frame.resetGeneration;
	}
	size_t at = frameOf( got );
	source.Control( "step", 1.0 );
	CHECK( source.Take( got ) && frameOf( got ) == at + 1 && WorldBytes( got ) == WorldBytes( frames[at + 1] ) );
	source.Control( "step", -2.0 );
	CHECK( source.Take( got ) && frameOf( got ) == at - 1 && WorldBytes( got ) == WorldBytes( frames[at - 1] ) );
	source.Control( "skip", 1.0 );
	CHECK( source.Take( got ) && frameOf( got ) == at - 1 + 60 );

	// Following: the next player, a slot, nobody.
	uint32_t first = got.frame.localNetId;
	int64_t firstSlot = std::get<int64_t>( stat( got, "replay_follow" ) );
	source.Control( "follow_next", 1.0 );
	CHECK( source.Take( got ) && got.frame.localNetId != 0 && got.frame.localNetId != first );
	source.Control( "follow", -1.0 );
	CHECK( source.Take( got ) && got.frame.localNetId == 0 );
	source.Control( "follow", double( firstSlot ) );
	CHECK( source.Take( got ) && got.frame.localNetId == first );

	// From the start to the end at 16x: frames come in order, the followed player's presses are
	// reported, and the end is said once.
	source.Control( "seek", 0.0 );
	source.Control( "speed", 16.0 );
	source.Control( "pause", 0.0 );
	bool inOrder = true;
	bool ended = false;
	uint16_t pressed = 0;
	size_t previous = 0;
	auto start = std::chrono::steady_clock::now();
	while ( ended == false && std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count() < 10.0 )
	{
		if ( source.Take( got ) )
		{
			inOrder &= frameOf( got ) >= previous;
			previous = frameOf( got );
			pressed |= got.localPressed;
			ended = std::get<bool>( stat( got, "replay_ended" ) );
		}
	}
	CHECK( ended && inOrder );
	CHECK( frameOf( got ) == frames.size() - 1 && got.rate == 0.0f );
	CHECK( WorldBytes( got ) == WorldBytes( frames.back() ) );
	CHECK( pressed != 0 );

	// A small file: every third tick, compact. A third of the frames, each lasting
	// three ticks; seeking still lands where it is asked; the world is the true one on the grid.
	std::filesystem::path thin = std::filesystem::temp_directory_path() / "cinderbox_view_test_thin.cbv";
	{
		present::ViewFileWriter writer;
		CHECK( writer.Open( thin.string(), 3, present::ViewPrecision::Compact ) );
		for ( const present::ViewFrame& f : frames )
		{
			writer.Add( f );
		}
	}
	{
		present::ViewFileSource thinned( thin.string() );
		CHECK( thinned.Length() == ( frames.size() + 2 ) / 3 );
		thinned.Control( "pause", 1.0 );
		CHECK( thinned.Take( got ) );
		CHECK( got.stride == 3 && frameOf( got ) == 0 );
		CHECK( std::fabs( std::get<double>( stat( got, "replay_length_seconds" ) ) - double( thinned.Length() ) * 3.0 / 60.0 ) < 1e-3 );
		thinned.Control( "seek", 6.0 );
		CHECK( thinned.Take( got ) && frameOf( got ) == 360 );
		// (The world as one whole compact packet of the true frame decodes; the stride is the file's.)
		auto seen = []( const present::ViewFrame& truth ) {
			std::vector<uint8_t> bytes;
			present::EncodeView( truth, nullptr, 0.0, bytes, present::ViewPrecision::Compact );
			present::ViewFrame frame;
			present::DecodeView( bytes.data(), bytes.size(), nullptr, frame );
			frame.stride = 3;
			return WorldBytes( frame );
		};
		CHECK( WorldBytes( got ) == seen( frames[360] ) );
		thinned.Control( "step", 1.0 );
		CHECK( thinned.Take( got ) && frameOf( got ) == 363 && WorldBytes( got ) == seen( frames[363] ) );
		std::printf( "    every third tick, compact: %zu frames in %llu bytes (%.0f per frame)\n", thinned.Length(),
					 (unsigned long long)std::filesystem::file_size( thin ), double( std::filesystem::file_size( thin ) ) / double( thinned.Length() ) );
	}
	{
		std::error_code ignored;
		std::filesystem::remove( thin, ignored );
	}

	// A file cut short loses only its last frame; a file that is not one is rejected with a reason.
	std::filesystem::path cut = std::filesystem::temp_directory_path() / "cinderbox_view_test_cut.cbv";
	{
		std::error_code ignored;
		std::filesystem::copy_file( path, cut, std::filesystem::copy_options::overwrite_existing, ignored );
		std::filesystem::resize_file( cut, std::filesystem::file_size( cut ) - 10, ignored );
	}
	present::ViewFileSource shorter( cut.string() );
	CHECK( shorter.Length() == frames.size() - 1 );
	present::ViewFileSource missing( ( std::filesystem::temp_directory_path() / "cinderbox_no_such_file.cbv" ).string() );
	CHECK( missing.Take( got ) && got.state == "rejected" && got.hasWorld == false );
	CHECK( std::get<std::string>( stat( got, "reject_reason" ) ).empty() == false );
	CHECK( missing.Take( got ) == false );

	std::printf( "    %zu frames in %llu bytes (%.0f per frame)\n", frames.size(), (unsigned long long)std::filesystem::file_size( path ),
				 double( std::filesystem::file_size( path ) ) / double( frames.size() ) );
	std::error_code ignored;
	std::filesystem::remove( path, ignored );
	std::filesystem::remove( cut, ignored );
}

} // namespace

#if defined( _WIN32 )
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// A crash says where: the return addresses from this executable's start, for llvm-symbolizer
// (--relative-address) with its .pdb.
static LONG WINAPI SayWhereItCrashed( EXCEPTION_POINTERS* info )
{
	void* frames[48];
	const USHORT count = CaptureStackBackTrace( 0, 48, frames, nullptr );
	const uintptr_t base = reinterpret_cast<uintptr_t>( GetModuleHandleA( nullptr ) );
	std::fprintf( stderr, "crashed: exception 0x%08lx at 0x%llx; the stack, from the executable's start:\n",
				  info->ExceptionRecord->ExceptionCode,
				  static_cast<unsigned long long>( reinterpret_cast<uintptr_t>( info->ExceptionRecord->ExceptionAddress ) - base ) );
	for ( USHORT i = 0; i < count; ++i )
	{
		std::fprintf( stderr, "  0x%llx\n", static_cast<unsigned long long>( reinterpret_cast<uintptr_t>( frames[i] ) - base ) );
	}
	std::fflush( stderr );
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main( int argc, char** argv )
{
#if defined( _WIN32 )
	SetUnhandledExceptionFilter( SayWhereItCrashed );
#endif
	// Every index answer checked against a scan of every entity (simulation.h).
	Simulation::s_checkIndexes = true;
	if ( argc == 2 && std::strcmp( argv[1], "--anim-hash" ) == 0 )
	{
		std::printf( "%016" PRIx64 "\n", AnimPoseHash( *anim::AnimSet::CreateProcedural() ) );
		return 0;
	}
	if ( argc == 2 && std::strcmp( argv[1], "--anim-hash-parts" ) == 0 )
	{
		auto set = anim::AnimSet::CreateProcedural();
		std::printf( "layers %016" PRIx64 "  +legs %016" PRIx64 "  +aim %016" PRIx64 "\n", AnimPoseHash( *set, false, false ),
					 AnimPoseHash( *set, true, false ), AnimPoseHash( *set, false, true ) );
		return 0;
	}
	if ( argc == 3 && std::strcmp( argv[1], "--save-portable" ) == 0 )
	{
		return SavePortableFile( argv[2] );
	}
	if ( argc == 3 && std::strcmp( argv[1], "--load-portable" ) == 0 )
	{
		return LoadPortableFile( argv[2] );
	}
	if ( argc == 3 && std::strcmp( argv[1], "--dump" ) == 0 )
	{
		return DumpHashes( argv[2] );
	}
	if ( argc == 3 && std::strcmp( argv[1], "--compare" ) == 0 )
	{
		return CompareHashes( argv[2] );
	}

	struct Test
	{
		const char* name;
		std::function<void()> fn;
	};
	const Test tests[] = {
		{ "map_format", TestMapFormat },
		{ "map_shapes", TestMapShapes },
		{ "map_bodies", TestMapBodies },
		{ "map_surfaces", TestMapSurfaces },
		{ "rolling", TestRolling },
		{ "breakable", TestBreakable },
		{ "holes", TestHoles },
		{ "item_breaks", TestItemBreaks },
		{ "glue", TestGlue },
		{ "pushers", TestPushers },
		{ "physics_without_characters", TestPhysicsWithoutCharacters },
		{ "slopes", TestSlopes },
		{ "templates", TestTemplates },
		{ "sim_events", TestSimEvents },
		{ "repeatability", TestRepeatability },
		{ "snapshot_roundtrip", TestSnapshotRoundTrip },
		{ "portable_snapshot", TestPortableSnapshot },
		{ "portable_bytes", TestPortableBytes },
		{ "rollback", TestRollback },
		{ "hybrid_model", TestHybridModel },
		{ "rollback_reset", TestRollbackReset },
		{ "gameplay_sanity", TestGameplaySanity },
		{ "commands", TestCommands },
		{ "move_params", TestMoveParams },
		{ "motions", TestMotions },
		{ "motions_while", TestMotionsWhile },
		{ "motion_steps", TestMotionSteps },
		{ "motion_order", TestMotionOrder },
		{ "motion_regular", TestMotionRegular },
		{ "motion_amounts", TestMotionAmounts },
		{ "motion_inspector", TestMotionInspector },
		{ "characters", TestCharacters },
		{ "viewer_characters", TestViewerCharacters },
		{ "mirror_netid_reused", TestMirrorNetIdReused },
		{ "first_person_camera", TestFirstPersonCamera },
		{ "any_skeleton", TestAnySkeleton },
		{ "possession", TestPossession },
		{ "possess_prop", TestPossessProp },
		{ "mod_driven", TestModDriven },
		{ "driven_body_kept", TestDrivenBodyKept },
		{ "prop_walls", TestPropWalls },
		{ "prop_leaves_walls", TestPropLeavesWalls },
		{ "motion_finds", TestMotionFinds },
		{ "motion_casts", TestMotionCasts },
		{ "motion_curves", TestMotionCurves },
		{ "motion_markers", TestMotionMarkers },
		{ "item_blast", TestItemBlast },
		{ "motion_repeats", TestMotionRepeats },
		{ "item_motions", TestItemMotions },
		{ "motion_conditions", TestMotionConditions },
		{ "links", TestLinks },
		{ "probe_holds", TestProbeHolds },
		{ "rope_alone", TestRopeAlone },
		{ "ragdoll", TestRagdoll },
		{ "anim_controller", TestAnimController },
		{ "anim_graph", TestAnimGraph },
		{ "state_speed", TestStateSpeed },
		{ "one_shot", TestOneShot },
		{ "nested_machine", TestNestedMachine },
		{ "held_items", TestHeldItems },
		{ "slots", TestSlots },
		{ "slot_keys", TestSlotKeys },
		{ "two_of_a_kind", TestTwoOfAKind },
		{ "item_fields", TestItemFields },
		{ "left_hand", TestLeftHand },
		{ "thrown_from_hand", TestThrownFromHand },
		{ "fast_launch", TestFastLaunch },
		{ "stacks", TestStacks },
		{ "containers", TestContainers },
		{ "item_use", TestItemUse },
		{ "event_news", TestEventNews },
		{ "world_items", TestWorldItems },
		{ "stowed_items", TestStowedItems },
		{ "attack_resolve", TestAttackResolve },
		{ "anim_blend2d", TestAnimBlend2D },
		{ "pose_tools", TestPoseTools },
		{ "fields", TestFields },
		{ "expr", TestExpr },
		{ "camera_collision", TestCameraCollision },
		{ "view_codec", TestViewCodec },
		{ "view_file", TestViewFile },
		{ "hitboxes", TestHitboxes },
		{ "placeholder_graph", TestPlaceholderGraph },
		{ "mannequin_character", TestMannequinCharacter },
		{ "retarget", TestRetarget },
		{ "layer_swap", TestLayerSwap },
		{ "overlay_grip", TestOverlayGrip },
		{ "held_arms", TestHeldArms },
		{ "anim_lead", TestAnimLead },
		{ "anim_catch_up", TestAnimCatchUp },
		{ "lead_reads_packs", TestLeadReadsPacks },
		{ "ual_mannequin", TestUalMannequin },
		{ "anim_pipeline", TestAnimPipeline },
		{ "physics_threads", TestPhysicsThreads },
		{ "loops", TestLoops },
		{ "stress", TestStress },
	};

	const char* filter = argc > 1 ? argv[1] : nullptr;
	int run = 0;
	for ( const Test& t : tests )
	{
		if ( filter != nullptr && std::strcmp( filter, t.name ) != 0 )
		{
			continue;
		}
		++run;
		int before = g_failures;
		auto start = Clock::now();
		std::printf( "[ RUN  ] %s\n", t.name );
		std::fflush( stdout );
		t.fn();
		std::printf( "[ %s ] %s (%.0f ms)\n", g_failures == before ? " OK " : "FAIL", t.name, MsSince( start ) );
		std::fflush( stdout );
	}

	if ( run == 0 )
	{
		std::printf( "no test named %s\n", filter );
		return 1;
	}
	return g_failures == 0 ? 0 : 1;
}
