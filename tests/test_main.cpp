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
#include "anim_lead.h"
#include "map.h"
#include "pose.h"
#include "pose_tools.h"
#include "capture.h"
#include "camera.h"
#include "fields.h"
#include "hitboxes.h"
#include "detmath.h"
#include "expr.h"
#include "ragdoll.h"
#include "retarget.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "rollback.h"
#include "scenario.h"
#include "simulation.h"
#include "view_codec.h"
#include "view_file.h"

#include <algorithm>
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
			 x.yaw != y.yaw )
		{
			return false;
		}
	}
	for ( size_t i = 0; i < a.props.size(); ++i )
	{
		const LevelProp& x = a.props[i];
		const LevelProp& y = b.props[i];
		if ( x.kind != y.kind || sameVec( x.position, y.position ) == false || sameVec( x.halfExtents, y.halfExtents ) == false )
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
	QuantizeLayout( authored );
	std::vector<uint8_t> authoredBytes;
	SerializeMap( authored, authoredBytes );
	LevelLayout authoredBack;
	CHECK( DeserializeMap( authoredBytes.data(), authoredBytes.size(), authoredBack, error ) );
	CHECK( SameLayout( authored, authoredBack ) );

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
void RunRollbackScenario( uint32_t maxDelay, uint32_t window, uint64_t seed )
{
	test::ScenarioOptions opt;
	opt.seed = seed;
	auto frames = test::MakeScenario( opt );
	SimConfig config = TestConfig();
	auto reference = RunReference( frames, config );

	const PlayerSlot local = 0;
	RollbackSession session( config, local, window );
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
				break;
			}
			if ( h != reference[nextCheck] )
			{
				std::printf( "    confirmed state mismatch at tick %u\n", nextCheck );
				CHECK( false );
			}
			++checked;
		}
	}

	session.Reconcile();
	CHECK( session.CurrentTick() == total );
	CHECK( session.Sim().ComputeHash() == reference[total] );

	const auto& stats = session.GetStats();
	std::printf( "    delay<=%u window=%u: %u states verified, %" PRIu64 " rollbacks, %" PRIu64 " resimulated ticks, %" PRIu64
				 " stalls\n",
				 maxDelay, window, checked, stats.rollbacks, stats.resimulatedTicks, stats.stalls );
	CHECK( checked > total / 2 );
	CHECK( stats.rollbacks > 0 );
}

void TestRollback()
{
	RunRollbackScenario( 4, 8, 11 );
	RunRollbackScenario( 12, 8, 22 ); // delays beyond the window force stalls
	RunRollbackScenario( 2, 2, 33 );
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
	command( CommandType::SetField, SlotTarget( 0 ) )->index = kBoardSlots;
	command( CommandType::SetField, 123456 )->value = 1;
	command( CommandType::SetField, SlotTarget( 9 ) )->value = 1;
	step( 1 );
	CHECK( sim.BoardValue( p0, 0 ) == 42 );
	CHECK( sim.BoardValue( p1, 5 ) == -7 );
	CHECK( sim.GlobalBoardValue( 2 ) == 99 );

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
	const AnimState* look = sim.EntityAnimState( p0 );
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
	const AnimState* legs = sim.EntityAnimState( p0 );
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
		const AnimState* a = sim.EntityAnimState( p0 );
		CHECK( a->stances[1] == 3 && a->stances[0] == 0 );
	}
	{
		SimCommand* c = command( CommandType::Stance, SlotTarget( 0 ) );
		c->index = 1;
		c->value = 2;
	}
	{
		SimCommand* bad = command( CommandType::Stance, SlotTarget( 0 ) );
		bad->index = kMaxAnimLayers; // out of range: ignored
		bad->value = 1;
	}
	step( 1 );
	CHECK( sim.EntityAnimState( p0 )->stances[1] == 2 );

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
	const std::string text = "cinderbox_motions\t1\n"
							 "# a comment\n"
							 "motion\tDash\n"
							 "when\tpress\tdash\n"
							 "if\tdash.charges > 0\n"
							 "cooldown\t0.5\n"
							 "duration\t0.25\n"
							 "impulse\t12\tmove\thorizontal\n"
							 "param\tfriction\t0\n"
							 "change\tdash.charges\t-=\t1\n"
							 "change\tdash.fuel\t+=\t0.5\n"
							 "emit\tdash.started\n"
							 "motion\tDoubleJump\n"
							 "when\tpress\tjump\n"
							 "if\tnot grounded\n"
							 "uses\t1\tground\n"
							 "impulse\t6.5\tup\tvertical\n"
							 "emit\tdash.double_jump\n"
							 "motion\tBlink\n"
							 "when\tpress\tblink\n"
							 "uses\t2\t1\n"
							 "impulse\t5\tworld\tall\t0\t0\t1\n";
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
	CHECK( motions->list[0].changes.size() == 2 && motions->list[0].event == 0 && motions->list[1].event == 1 );

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
	CHECK( sim.FindEntity( p0 ).has<MotionState>() );

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
	CHECK( ev.type == 0 && ev.netIdA == p0 && ev.tick == pressTick && b3Length( ev.vector ) > 11.9f );
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
		return CompileMotionSet( "t", "cinderbox_motions\t1\n" + body, schema, out, error, warned ) ? int( out.size() ) : -1;
	};
	std::string error, warned;
	CHECK( compile( "motion\tA\nwhen\tpress\tdash\n", error, warned ) == 1 && warned.empty() );
	CHECK( compile( "when\tpress\tdash\n", error, warned ) == -1 && error.find( "line 2" ) != std::string::npos );
	CHECK( compile( "motion\tA\nimpulse\t5\tsideways\tnone\n", error, warned ) == -1 );
	CHECK( compile( "motion\tA\nimpulse\t5000\tup\tnone\n", error, warned ) == -1 );
	CHECK( compile( "motion\tA\nimpulse\t5\tworld\tnone\t0\t0\t0\n", error, warned ) == -1 );
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
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t1\nmotion\tA\nwhen\tpress\tfly\nchange\tround.time\t=\t1\nchange\tfly.fuel\t-=\t1\nemit\tfly.up\n",
							 schema, inert, error, warned ) );
	std::printf( "    %s\n", warned.c_str() );
	CHECK( inert.size() == 1 && inert[0].action == -1 && inert[0].changes.empty() && inert[0].event == -1 );
	CHECK( warned.find( "\"fly\"" ) != std::string::npos && warned.find( "round.time" ) != std::string::npos &&
		   warned.find( "fly.fuel" ) != std::string::npos && warned.find( "fly.up" ) != std::string::npos );

	// More motions than there are slots: the rest are left out, and said.
	ModSchema many = schema;
	std::string big = "cinderbox_motions\t1\n";
	for ( int i = 0; i < kMaxMotions + 3; ++i )
	{
		big += "motion\tM" + std::to_string( i ) + "\nwhen\tpress\tdash\n";
	}
	many.motionSets = { { "dash", "dash.many", big } };
	warned.clear();
	auto capped = CompileMotions( many, warned );
	CHECK( capped != nullptr && capped->list.size() == size_t( kMaxMotions ) && warned.find( "left out" ) != std::string::npos );
	// A server without motions has none, and its players carry nothing for them.
	ModSchema none;
	CHECK( CompileMotions( none, warned ) == nullptr );
	Simulation plain( TestConfig(), FlatMap() );
	InputFrame g;
	g.events.push_back( { PlayerEventType::Join, 0 } );
	plain.Step( g );
	CHECK( plain.FindEntity( plain.PlayerNetId( 0 ) ).has<MotionState>() == false );

	// The schema carries the text.
	std::vector<uint8_t> bytes;
	EncodeSchema( schema, bytes );
	ModSchema back;
	CHECK( DecodeSchema( bytes.data(), bytes.size(), back ) && back == schema );
}

// Motions that hold while conditions do (flight, a jetpack, a glide), and ones a mod event starts.
void TestMotionsWhile()
{
	ModSchema schema;
	schema.fields.push_back( { "flight.on", BoardType::Bool, BoardScope::Entity, 0 } );
	schema.fields.push_back( { "flight.fuel", BoardType::Float, BoardScope::Entity, 1 } );
	schema.fields.push_back( { "flight.count", BoardType::Int, BoardScope::Entity, 2 } );
	schema.events = { "flight.started", "flight.stopped", "flight.thrust", "stun.hit" };
	schema.actions.push_back( { "fly", 2, "T" } );
	const std::string text = "cinderbox_motions\t1\n"
							 "motion\tFlyOn\nwhen\tpress\tfly\nif\tnot flight.on\nchange\tflight.on\t=\t1\nemit\tflight.started\n"
							 "motion\tFlyOff\nwhen\tpress\tfly\nif\tflight.on\nchange\tflight.on\t=\t0\nemit\tflight.stopped\n"
							 "motion\tFlying\nwhen\twhile\nif\tflight.on\n"
							 "param\twalk_speed\t7\nparam\tair_control\t1\nparam\tgravity\t0\nparam\tair_friction\t3\nparam\tmove_frame\t1\n"
							 "motion\tThrust\nwhen\twhile\nif\theld.jump and not grounded and not flight.on and flight.fuel > 0\n"
							 "impulse\t32\tup\tnone\nchange\tflight.fuel\t-=\t30\nchange\tflight.count\t=\t7\nemit\tflight.thrust\n"
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
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t1\nmotion\tA\nwhen\twhile\nif\theld.warp\nchange\tflight.count\t+=\t1\n", schema, out, error,
							 warned ) );
	std::printf( "    %s\n", warned.c_str() );
	CHECK( out.size() == 1 && out[0].changes.empty() );
	CHECK( warned.find( "per second" ) != std::string::npos && warned.find( "held.warp" ) != std::string::npos );
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t1\nmotion\tA\nwhen\tevent\n", schema, out, error, warned ) == false );

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

// Tethers: a motion throws a line at what the player looks at; it flies, takes hold, pulls, and
// with a rope keeps the player within its length (a grappling hook).
void TestTethers()
{
	ModSchema schema;
	schema.events = { "grapple.fired" };
	schema.actions.push_back( { "grapple", 0, "X" } );
	schema.actions.push_back( { "leash", 1, "Z" } );
	const std::string text = "cinderbox_motions\t1\n"
							 "motion\tHook\nwhen\tpress\tgrapple\ncooldown\t0.2\ntether\t40\t30\t24\t3\trope\nuntil\tnot held.grapple\n"
							 "param\tfriction\t0\nparam\tair_control\t0.6\nemit\tgrapple.fired\n"
							 // No pull, no reel, at once: a rope and nothing else.
							 "motion\tLeash\nwhen\tpress\tleash\ntether\t40\t0\t0\t0\trope\nuntil\tnot held.leash\n";
	schema.motionSets.push_back( { "grapple", "grapple.moves", text } );
	std::string warnings;
	std::shared_ptr<const Motions> motions = CompileMotions( schema, warnings );
	CHECK( motions != nullptr && warnings.empty() && motions->list.size() == 2 && motions->list[0].tether && motions->list[0].tetherRope );
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
	auto tetherOf = [&]() -> const Tether* { return sim.FindEntity( p0 ).try_get<Tether>(); };
	auto position = [&] { return sim.EntityTransform( p0 )->position; };
	CHECK( tetherOf() == nullptr );

	// At the sky: the line finds nothing, and nothing happens (no event, no cooldown).
	f.inputs[0].cameraPitch = 14000;
	f.inputs[0].actions = grapple;
	step( 1 );
	CHECK( ( tetherOf() == nullptr || tetherOf()->on == 0 ) && sim.Globals().modEventCount == 0 );
	f.inputs[0].actions = 0;
	step( 1 );

	// At the wall, a little upward: it is thrown, flies (nothing pulls yet), then holds and pulls.
	f.inputs[0].cameraPitch = 500;
	f.inputs[0].actions = grapple;
	b3Vec3 from = position();
	step( 1 );
	const Tether* tether = tetherOf();
	CHECK( tether != nullptr && tether->on == 1 && tether->anchor == 0 && tether->motion == 0 );
	if ( tether == nullptr || tether->on == 0 )
	{
		return;
	}
	b3Vec3 anchor = tether->point;
	float reach = b3Distance( anchor, b3Add( from, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) );
	uint32_t flight = tether->holdTick - tether->startTick;
	std::printf( "    thrown %.2f m at the wall: %u ticks of flight at 30 m/s\n", reach, flight );
	CHECK( reach > 5.0f && reach < 40.0f && anchor.z > from.z + 5.0f );
	CHECK( flight == uint32_t( reach / 30.0f * 60.0f + 0.5f ) && std::fabs( tether->length - reach ) < 0.001f );
	// The event is at where it will hold.
	CHECK( sim.Globals().modEventCount == 1 && b3Distance( sim.Globals().modEvents[0].point, anchor ) < 0.001f );
	// While it flies its end is on the way, and the player has not moved.
	b3Vec3 end;
	bool holds = true;
	uint8_t which = 9;
	step( int( flight ) / 2 );
	CHECK( sim.EntityTether( p0, end, holds, which ) && holds == false && which == 0 );
	CHECK( end.z > from.z + 1.0f && end.z < anchor.z - 1.0f && b3Distance( position(), from ) < 0.01f );
	// Its parameters hold while it is out: it says so every tick, for that tick.
	CHECK( sim.FindEntity( p0 ).get<MotionState>().slots[0].untilTick == sim.Tick() );
	// Holding: the player is pulled along the line, and the rope is reeled in.
	step( int( flight ) / 2 + 2 );
	CHECK( sim.EntityTether( p0, end, holds, which ) && holds && b3Distance( end, anchor ) < 0.001f );
	step( 45 );
	float closer = b3Distance( anchor, b3Add( position(), b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) );
	std::printf( "    after 0.75 s of pull: %.2f m from the point (rope %.2f)\n", closer, tetherOf()->length );
	CHECK( closer < reach - 3.0f && tetherOf()->length < reach - 2.0f && tetherOf()->length > reach - 2.5f );
	// Letting the key go lets it go, and the parameters are the server's again.
	f.inputs[0].actions = 0;
	step( 1 );
	CHECK( tetherOf()->on == 0 && sim.EntityTether( p0, end, holds, which ) == false );
	step( 1 );
	CHECK( sim.FindEntity( p0 ).get<MotionState>().slots[0].untilTick < sim.Tick() );
	step( 180 );

	// A rope alone: the player cannot walk further from the point than the rope is long.
	f.inputs[0].cameraPitch = 500;
	f.inputs[0].actions = leash;
	step( 1 );
	CHECK( tetherOf()->on == 1 && tetherOf()->motion == 1 && tetherOf()->holdTick == tetherOf()->startTick );
	b3Vec3 post = tetherOf()->point;
	float rope = tetherOf()->length;
	f.inputs[0].moveForward = -127; // away from the wall
	float furthest = 0.0f;
	for ( int i = 0; i < 240; ++i )
	{
		step( 1 );
		furthest = std::max( furthest, b3Distance( post, b3Add( position(), b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } ) ) );
	}
	std::printf( "    on a %.2f m rope, walking away for 4 s: never past %.2f m\n", rope, furthest );
	CHECK( furthest < rope + 0.35f && tetherOf()->length == rope );
	f.inputs[0].moveForward = 0;
	f.inputs[0].actions = 0;
	step( 120 );
	CHECK( tetherOf()->on == 0 );

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
	step( 1 );
	CHECK( tetherOf()->on == 1 && tetherOf()->anchor == crate );
	step( 60 );
	float pulled = crateAt.z - sim.EntityTransform( crate )->position.z;
	std::printf( "    a crate on the hook came %.2f m closer in a second\n", pulled );
	CHECK( pulled > 0.3f && tetherOf()->on == 1 );
	{
		SimCommand c;
		c.type = CommandType::Destroy;
		c.target = crate;
		f.commands.push_back( c );
	}
	step( 2 );
	CHECK( tetherOf()->on == 0 );
	f.inputs[0].actions = 0;
	step( 60 );

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
	CHECK( tetherOf()->on == 1 && client.ComputeHash() == sim.ComputeHash() );
	sim.Load( before );
	CHECK( tetherOf()->on == 0 && sim.ComputeHash() == hashBefore );

	// A while motion cannot throw one; a tether line needs all its numbers.
	std::vector<Motion> out;
	std::string error, warned;
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t1\nmotion\tA\nwhen\twhile\ntether\t40\t30\t24\t3\trope\n", schema, out, error, warned ) == false );
	CHECK( error.find( "while" ) != std::string::npos );
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t1\nmotion\tA\nwhen\tpress\tgrapple\ntether\t40\t30\n", schema, out, error, warned ) == false );
	CHECK( CompileMotionSet( "t", "cinderbox_motions\t1\nmotion\tA\nwhen\tpress\tgrapple\ntether\t40\t30\t24\t3\tchain\n", schema, out, error, warned ) == false );
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
	int32_t globals[kBoardSlots] = {};
	globals[0] = BoardFromFloat( 12.5f );
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
	auto state = [&]() { return sim.FindEntity( sim.Globals().playerNetIds[0] ).get<AnimState>(); };

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
		std::string now = states[sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>().graph[0].state].name;
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
	AnimState a = sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>();
	std::printf( "    strafing right: move_right %.2f move_forward %.2f, blend (%.2f, %.2f)\n", a.moveRight, a.moveForward, a.graph[0].blend,
				 a.graph[0].blendY );
	CHECK( a.moveRight > 2.5f && std::fabs( a.moveForward ) < 0.3f );
	CHECK( a.graph[0].blend == a.moveRight && a.graph[0].blendY == a.moveForward );
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
	auto layer = [&]( int l ) { return sim.FindEntity( sim.Globals().playerNetIds[0] ).get<AnimState>().graph[l]; };
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

// The example character as the editor baked it (characters/robot): it loads like any item's files,
// every hitbox binds to its skeleton, and its taller head is where a ray finds the head zone. The
// built-in rig's head would be lower, so this also shows which character the hit test used.
void TestRobotCharacter()
{
	const std::string dir = std::string( CB_SOURCE_DIR ) + "/characters/robot/client/characters/robot";
	std::string error, warnings;
	auto set = anim::AnimSet::Load( dir, error, warnings );
	CHECK( set != nullptr );
	if ( set == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	CHECK( warnings.empty() );
	CHECK( set->Skeleton().num_joints() == 22 );
	CHECK( set->GraphText().empty() == false );
	for ( const char* clip : { "idle", "walk", "run", "jump_start", "fall", "land", "pistol_hold", "bat_idle", "bat_swing" } )
	{
		CHECK( set->NamedClip( clip ) != nullptr );
	}

	std::string text;
	CHECK( anim::DiskReader( dir )( "hitboxes.cfg", text ) );
	anim::HitboxSet hitboxes;
	CHECK( anim::ParseHitboxes( text, hitboxes, error ) );
	size_t count = hitboxes.boxes.size();
	anim::BindHitboxes( hitboxes, *set, warnings );
	CHECK( hitboxes.boxes.size() == count );
	CHECK( count == 11 );

	anim::PoseEvaluator pose( *set );
	pose.Evaluate( AnimState{} );
	auto zoneAt = [&]( float height ) -> std::string {
		anim::HitboxHit hit;
		b3Quat facing = { { 0.0f, 0.0f, 0.0f }, 1.0f };
		if ( anim::RayHitboxes( hitboxes, pose.Models(), {}, facing, { 0.0f, height, -3.0f }, { 0.0f, 0.0f, 6.0f }, 1.0f, hit ) == false )
		{
			return "";
		}
		return hit.box->zone;
	};
	CHECK( zoneAt( 1.80f ) == "head" ); // above the built-in rig's head
	CHECK( zoneAt( 1.25f ) == "torso" );
	CHECK( zoneAt( 2.05f ).empty() );

	// Its state machine, as the editor baked it from its AnimationTree: the shipped mods' stances
	// are states of its upper layer, which moves the arms and leaves the legs alone.
	{
		ModSchema schema;
		schema.layers = { "full", "upper" };
		schema.stances = { "melee", "melee_swing", "pistol", "rifle" };
		std::string graphWarnings;
		auto graph = CompileAnimGraph( set->GraphText(), schema, error, graphWarnings );
		CHECK( graph != nullptr && graphWarnings.empty() );
		if ( graph == nullptr )
		{
			std::printf( "    %s\n", error.c_str() );
			return;
		}
		CHECK( graph->layers.size() == 2 );
		anim::PoseEvaluator machine( *set );
		machine.SetGraph( graph, graphWarnings );
		CHECK( graphWarnings.empty() );
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
		auto at = [&]( const char* joint ) {
			float v[4];
			ozz::math::StorePtrU( machine.Models()[size_t( anim::FindJoint( *set, joint ) )].cols[3], v );
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
				f.commands.clear();
			}
		};
		auto state = [&]() { return sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>(); };
		auto stance = [&]( uint8_t layer, int32_t value ) {
			SimCommand c;
			c.type = CommandType::Stance;
			c.target = SlotTarget( 0 );
			c.index = layer;
			c.value = value;
			f.commands.push_back( c );
		};
		f.events.push_back( { PlayerEventType::Join, 0 } );
		step( 60 );
		CHECK( state().graph[0].state == stateOf( 0, "Locomotion" ) && state().graph[1].weight == 0.0f );
		machine.Evaluate( state() );
		b3Vec3 restHand = at( "RightHand" );
		b3Vec3 restFoot = at( "LeftFoot" );
		auto clips = anim::ActiveClips( state(), *graph );
		CHECK( clips.size() == 1 && clips[0].channel == 0 && clips[0].name == "idle" && clips[0].loops );

		stance( 1, 3 ); // upper layer, pistol
		step( 30 );
		CHECK( state().graph[1].state == stateOf( 1, "Pistol" ) && state().graph[1].weight == 1.0f );
		machine.Evaluate( state() );
		std::printf( "    pistol: the hand moved %.3f m, the foot %.4f m\n", b3Distance( at( "RightHand" ), restHand ),
					 b3Distance( at( "LeftFoot" ), restFoot ) );
		CHECK( b3Distance( at( "RightHand" ), restHand ) > 0.2f );
		CHECK( b3Distance( at( "LeftFoot" ), restFoot ) < 0.02f );
		clips = anim::ActiveClips( state(), *graph );
		CHECK( clips.size() == 2 && clips[1].channel == 1 && clips[1].name == "pistol_hold" );

		stance( 1, 0 );
		stance( 0, 1 ); // full layer, melee: the machine's upper layer holds the bat ready
		step( 30 );
		CHECK( state().graph[1].state == stateOf( 1, "Ready" ) );
		stance( 0, 2 ); // melee_swing
		step( 2 );
		CHECK( state().graph[1].state == stateOf( 1, "Swing" ) );
		clips = anim::ActiveClips( state(), *graph );
		CHECK( clips.size() == 2 && clips[1].name == "bat_swing" && clips[1].loops == false );
		stance( 0, 0 );
		step( 30 );
		CHECK( state().graph[1].state == stateOf( 1, "Rest" ) && state().graph[1].weight == 0.0f );
	}

	// Aiming straight ahead: the robot's own aim chain (the default, its right arm) points the hand
	// forward from the shoulder, an arm's length out, at shoulder height.
	AnimState aiming;
	aiming.aiming = 1;
	pose.Evaluate( aiming );
	auto at = [&]( const char* joint ) {
		float v[4];
		ozz::math::StorePtrU( pose.Models()[size_t( anim::FindJoint( *set, joint ) )].cols[3], v );
		return b3Vec3{ v[0], v[1], v[2] };
	};
	b3Vec3 shoulder = at( "RightUpperArm" );
	b3Vec3 hand = at( "RightHand" );
	std::printf( "    aiming: shoulder (%.2f %.2f %.2f) hand (%.2f %.2f %.2f)\n", shoulder.x, shoulder.y, shoulder.z, hand.x, hand.y,
				 hand.z );
	CHECK( hand.z - shoulder.z > 0.5f );
	CHECK( std::fabs( hand.y - shoulder.y ) < 0.05f );
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
	auto state = [&]() { return sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>(); };

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

	// The shipped mods' names, as a server would send them.
	ModSchema schema;
	schema.layers = { "full", "upper" };
	schema.stances = { "melee", "melee_swing", "pistol", "rifle" };
	schema.events = { "pistol.fired", "melee.strike", "rifle.fired" };
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	CHECK( graph != nullptr );
	if ( graph == nullptr )
	{
		std::printf( "    %s\n", error.c_str() );
		return;
	}
	CHECK( warnings.empty() );
	CHECK( graph->layers.size() == 2 && graph->EmitsEvent( 1 ) );
	anim::PoseEvaluator pose( *set );
	pose.SetGraph( graph, warnings );
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
	auto state = [&]() { return sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>(); };
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

	// The pistol: the upper layer draws it, aims (the aim chain), and plays a shot on pistol.fired.
	command( CommandType::Stance, 1, 3, 0 ); // upper layer, pistol
	command( CommandType::Aim, 0, 0, 1 );
	step( 30 );
	CHECK( state().graph[1].state == stateOf( 1, "Pistol" ) && state().graph[1].weight == 1.0f );
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
	command( CommandType::Event, 0, 0, 0 ); // pistol.fired, by this player
	step( 1 );
	CHECK( state().graph[1].state == stateOf( 1, "Shoot" ) );
	step( 60 );
	CHECK( state().graph[1].state == stateOf( 1, "Pistol" ) );
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
		CHECK( state().graph[1].state == stateOf( 1, "Shoot" ) && before > 0.15f );
		CHECK( state().graph[1].stateTime <= 1.5f * dt && state().graph[1].time <= 1.5f * dt );
		// It fades from where the last recoil was, not from the idle.
		CHECK( state().graph[1].previous == state().graph[1].state && state().graph[1].previousTime > 0.15f );
		// The viewer is told, so the keys inside the clip (a sound, a flash) are due again; only
		// for the moment of the crossfade.
		auto restarted = [&]() {
			bool any = false;
			for ( const anim::ActiveClip& clip : anim::ActiveClips( state(), *graph ) )
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
		CHECK( state().graph[1].state == stateOf( 1, "Shoot" ) );
		step( 15 );
		CHECK( state().graph[1].state == stateOf( 1, "Pistol" ) );
	}

	// The bat: the swing's marker emits melee.strike once, 0.4 s in.
	command( CommandType::Stance, 1, 0, 0 );
	command( CommandType::Aim, 0, 0, 0 );
	command( CommandType::Stance, 0, 1, 0 ); // full layer, melee
	step( 30 );
	CHECK( state().graph[1].state == stateOf( 1, "Ready" ) );
	auto strikes = [&]() {
		int n = 0;
		for ( uint32_t i = 0; i < std::min( sim.Globals().modEventCount, kModEventHistory ); ++i )
		{
			n += sim.Globals().modEvents[i].type == 1 ? 1 : 0;
		}
		return n;
	};
	command( CommandType::Stance, 0, 2, 0 ); // melee_swing
	step( 1 );
	CHECK( state().graph[1].state == stateOf( 1, "Swing" ) );
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

// The viewer's own player ahead of the server (present/anim_lead.h), on the mannequin's machine.
// A press 12 ticks before the server answers: the swing starts on the press, its clock runs on
// evenly through the moment the server's stance arrives, and the legs are left alone. A predicted
// event does the same for the pistol's recoil.
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
	schema.layers = { "full", "upper" };
	schema.stances = { "melee", "melee_swing", "pistol", "rifle" };
	schema.events = { "pistol.fired", "melee.strike", "rifle.fired" };
	auto graph = CompileAnimGraph( set->GraphText(), schema, error, warnings );
	CHECK( graph != nullptr );
	if ( graph == nullptr )
	{
		return;
	}
	auto stateOf = [&]( const char* name ) {
		const auto& states = graph->layers[1].states;
		for ( size_t i = 0; i < states.size(); ++i )
		{
			if ( states[i].name == name )
			{
				return int( i );
			}
		}
		return -1;
	};
	// What the machine reads above its base layer: the stances and the shot, not the strike marker.
	CHECK( graph->UpperLayersRead( AnimExpr::VarKind::Stance, 2 ) );
	CHECK( graph->UpperLayersRead( AnimExpr::VarKind::Event, 0 ) );
	CHECK( graph->UpperLayersRead( AnimExpr::VarKind::Event, 1 ) == false );

	Simulation sim( TestConfig(), FlatMap() );
	sim.SetAnimGraph( graph );
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
	auto state = [&]() { return sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>(); };
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
	command( CommandType::Stance, 0, 1 ); // full layer: melee, the bat is out
	step( 60 );
	CHECK( state().graph[1].state == stateOf( "Ready" ) );

	// No lead: nothing changes.
	{
		AnimState now = state();
		AnimState same = present::LeadAnimState( now, *graph, {}, lead );
		CHECK( std::memcmp( &same, &now, sizeof( AnimState ) ) == 0 );
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
			command( CommandType::Stance, 0, 2 ); // the server's answer: melee_swing
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
			input.layer = 0;
			input.stance = 2;
			lead.inputs.push_back( input );
		}
		AnimState shown = state();
		AnimState led = present::LeadAnimState( shown, *graph, {}, lead );
		// The legs are the server's.
		CHECK( std::memcmp( &led.graph[0], &shown.graph[0], sizeof( AnimGraphLayerState ) ) == 0 );
		if ( t < latency )
		{
			CHECK( shown.graph[1].state == stateOf( "Ready" ) ); // the server has not swung yet
		}
		if ( led.graph[1].state == stateOf( "Swing" ) )
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
	command( CommandType::Stance, 0, 0 );
	command( CommandType::Stance, 1, 3 ); // upper layer: pistol
	step( 60 );
	CHECK( state().graph[1].state == stateOf( "Pistol" ) );
	lead.inputs.clear();
	present::AnimLead::Input shot;
	shot.age = 4.0f * dt;
	shot.event = 0; // pistol.fired
	lead.inputs.push_back( shot );
	lead.seconds = 4.0f * dt;
	AnimState recoil = present::LeadAnimState( state(), *graph, {}, lead );
	CHECK( recoil.graph[1].state == stateOf( "Shoot" ) );
	CHECK( std::fabs( recoil.graph[1].time - 4.0f * dt ) < 0.5f * dt ); // as old as the press
	// An expired or refused prediction: no input, so the server's state as it is, only later.
	lead.inputs.clear();
	AnimState back = present::LeadAnimState( state(), *graph, {}, lead );
	CHECK( back.graph[1].state == stateOf( "Pistol" ) );
}

// The character built from the paid animation pack, where it was built (it is never in the
// repository, so CI and fresh clones skip this): strafing plays its sideways jog, hips straight.
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
	schema.events = { "pistol.fired", "melee.strike", "rifle.fired" };
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
	AnimState state = sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>();
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
			AnimState now = sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>();
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
	schema.stances = { "melee", "melee_swing", "pistol", "rifle" };
	schema.events = { "pistol.fired", "melee.strike", "rifle.fired" };
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
		pose.Evaluate( sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>() );
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
	AnimState a = sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>();
	CHECK( a.graph[0].source == 1 && a.graph[0].state == 0 );
	std::vector<anim::ActiveClip> clips = anim::ActiveClips( a, *graph, packs );
	CHECK( clips.empty() == false && clips[0].name == "Jump_Land" );
	float crouched = head();

	swap( 0, 0 );
	step( 30 );
	a = sim.FindEntity( sim.PlayerNetId( 0 ) ).get<AnimState>();
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
								   "cinderbox_motions\t1\n"
								   "motion\tDash\nwhen\tpress\ta\nif\tspeed > 0.5 or not grounded\ncooldown\t0.3\nduration\t0.2\n"
								   "impulse\t9\tlook\tnone\nparam\tfriction\t0\nparam\tgravity\t6\n"
								   "change\tm.count\t+=\t1\nchange\tm.fuel\t-=\t0.25\nemit\tm.dashed\n"
								   "motion\tHop\nwhen\tpress\tb\nuses\t2\tground\nduration\t0.5\nimpulse\t5\tmove\tvertical\nparam\tair_control\t1\n"
								   "motion\tBlink\nwhen\tpress\tjump\nif\tnot grounded and vertical_speed < 2\nuses\t1\t0.75\n"
								   "impulse\t7\tworld\tall\t0.3\t1\t-0.2\n"
								   "motion\tBurst\nwhen\tpress\tsprint\ncooldown\t1\nimpulse\t-4\tfacing\thorizontal\n"
								   "motion\tSoar\nwhen\twhile\nif\theld.b and m.fuel > -40\ncooldown\t0.5\n"
								   "impulse\t20\tlook\tnone\nparam\tmove_frame\t1\nparam\tair_friction\t2\nparam\tgravity\t3\n"
								   "change\tm.fuel\t-=\t3\nchange\tm.count\t=\t0\nemit\tm.dashed\n"
								   "motion\tStun\nwhen\tevent\tm.dashed\nduration\t0.25\nparam\tjump_speed\t9\n"
								   // A tether: the ray, the rope and the pull on whatever the look finds.
								   "motion\tHook\nwhen\tpress\ta\nif\tm.fuel < -1\ncooldown\t1.5\ntether\t30\t0\t18\t2\trope\n"
								   "until\tairborne_time > 1 or m.count > 6\nparam\tfriction\t0\n" } );
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

int DumpHashes( const char* path )
{
	auto frames = test::MakeScenario( {} );
	auto hashes = RunReference( frames, TestConfig() );
	AppendMotionReference( hashes );
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
	schema.character = "mannequin";

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
	CHECK( got.schema.character == "mannequin" && got.schema.events == last.schema.events );
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

int main( int argc, char** argv )
{
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
		{ "templates", TestTemplates },
		{ "sim_events", TestSimEvents },
		{ "repeatability", TestRepeatability },
		{ "snapshot_roundtrip", TestSnapshotRoundTrip },
		{ "portable_snapshot", TestPortableSnapshot },
		{ "portable_bytes", TestPortableBytes },
		{ "rollback", TestRollback },
		{ "rollback_reset", TestRollbackReset },
		{ "gameplay_sanity", TestGameplaySanity },
		{ "commands", TestCommands },
		{ "move_params", TestMoveParams },
		{ "motions", TestMotions },
		{ "motions_while", TestMotionsWhile },
		{ "tethers", TestTethers },
		{ "ragdoll", TestRagdoll },
		{ "anim_controller", TestAnimController },
		{ "anim_graph", TestAnimGraph },
		{ "held_items", TestHeldItems },
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
		{ "robot_character", TestRobotCharacter },
		{ "mannequin_character", TestMannequinCharacter },
		{ "retarget", TestRetarget },
		{ "layer_swap", TestLayerSwap },
		{ "anim_lead", TestAnimLead },
		{ "ual_mannequin", TestUalMannequin },
		{ "anim_pipeline", TestAnimPipeline },
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
