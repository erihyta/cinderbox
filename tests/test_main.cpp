// Determinism and rollback tests.
//
//   cb_tests                    run all tests
//   cb_tests <name>             run one test
//   cb_tests --dump <file>      write per-tick hashes of the reference scenario (cross-build check)
//   cb_tests --compare <file>   compare against a dump from another build/platform
//   cb_tests --anim-hash         pose hash of the procedural rig (cross-build check)
//   cb_tests --save-portable <file> / --load-portable <file>   portable snapshot across builds

#include "anim_controller.h"
#include "anim_graph.h"
#include "map.h"
#include "pose.h"
#include "pose_tools.h"
#include "fields.h"
#include "hitboxes.h"
#include "detmath.h"
#include "ragdoll.h"
#include "rollback.h"
#include "scenario.h"
#include "simulation.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
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
	CHECK( std::fabs( detmath::WrapAngle( sim.PlayerCharacter( 0 )->facingYaw - detmath::YawToRadians( 16384 ) ) ) > 2.5f );
	CHECK( sim.EntityAnimState( p0 )->legsBackward == 0 );
	f.inputs[0].moveForward = 0;
	f.inputs[0].cameraYaw = 0;
	step( 30 );

	// Stances: a layer takes the stance, remembers the one it replaced, and restarts its clock.
	{
		SimCommand* c = command( CommandType::Stance, SlotTarget( 0 ) );
		c->index = 1;
		c->value = 3;
	}
	step( 5 );
	{
		const AnimState* a = sim.EntityAnimState( p0 );
		CHECK( a->stances[1] == 3 && a->previousStances[1] == 0 );
		CHECK( a->layerTime[1] > 0.0f && a->layerTime[1] < 0.1f );
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
	CHECK( sim.EntityAnimState( p0 )->stances[1] == 2 && sim.EntityAnimState( p0 )->previousStances[1] == 3 );

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
	std::printf( "    walking: groundSpeed %.3f phase %.3f\n", state().groundSpeed, state().locomotionPhase );
	CHECK( b3AbsFloat( state().groundSpeed - anim_tuning::kWalkSpeed ) < 0.1f );

	// Phase advances at the walk cycle rate.
	float before = state().locomotionPhase;
	step( 6 );
	float advanced = state().locomotionPhase - before;
	if ( advanced < 0.0f )
	{
		advanced += 1.0f;
	}
	float expected = 6.0f * sim.Config().TimeStep() / anim_tuning::kWalkCycleSeconds;
	CHECK( b3AbsFloat( advanced - expected ) < 0.01f );

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

// Directional clips in a 2D blend space, as Godot triangulates it (idle in the middle, four
// directions around), and the body-frame velocity that drives it.
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
	CHECK( std::fabs( layer( 0 ).blend - anim_tuning::kWalkSpeed ) < 0.1f );

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
uint64_t AnimPoseHash( const anim::AnimSet& set )
{
	anim::PoseEvaluator eval( set );
	uint64_t hash = kHashSeed;
	uint64_t rng = 99;
	for ( int i = 0; i < 400; ++i )
	{
		AnimState s;
		s.mode = AnimMode( NextRandom( rng ) % 4 );
		s.previousMode = AnimMode( NextRandom( rng ) % 4 );
		s.modeTime = RandomRange( rng, 0.0f, 3.0f );
		s.locomotionPhase = RandomUnit( rng );
		s.idleTime = RandomRange( rng, 0.0f, 60.0f );
		s.groundSpeed = RandomRange( rng, 0.0f, 8.0f );
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
	for ( int c = 0; c < anim::ClipCount; ++c )
	{
		CHECK( set->Get( anim::Clip( c ) ) != nullptr );
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

	// Its stance clips, as the editor baked them: the shipped mods' stances resolve without warnings.
	{
		std::string stanceWarnings;
		auto stances = anim::BuildStanceTable( *set, { "full", "upper" }, { "melee", "melee_swing", "pistol" }, stanceWarnings );
		CHECK( stanceWarnings.empty() );
		CHECK( stances->stances[0].clips[anim::ClipRun] != nullptr );
		CHECK( stances->stances[1].single != nullptr && stances->stances[2].single != nullptr );
		CHECK( set->Mask( "upper" ) == "Spine" );
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

// Layers and stances: an upper-body stance moves the arms and not the legs, a full-body one moves
// the legs too, a fresh stance is half faded in half way through its fade, and what a character
// lacks is reported and ignored.
void TestStances()
{
	auto set = anim::AnimSet::CreateProcedural();
	std::string warnings;
	auto table = anim::BuildStanceTable( *set, { "upper", "full", "arms" }, { "pistol", "melee", "sword" }, warnings );
	CHECK( table->masks.size() == 3 && table->stances.size() == 3 );
	CHECK( table->masks[0].empty() == false && table->masks[1].empty() == false );
	CHECK( table->masks[2].empty() ); // no "arms" mask on this character
	CHECK( warnings.find( "'arms'" ) != std::string::npos );
	CHECK( warnings.find( "'sword'" ) != std::string::npos );
	CHECK( table->stances[0].single != nullptr );				   // one pistol loop
	CHECK( table->stances[1].clips[anim::ClipWalk] != nullptr ); // melee has its own walk
	CHECK( table->stances[1].clips[anim::ClipFall] == nullptr ); // ...and falls like everyone else

	std::string ignored;
	std::vector<float> upper = anim::MaskWeights( *set, "Spine", ignored );
	CHECK( upper[size_t( present::FindJoint( *set, "Hips" ) )] == 0.0f );
	CHECK( upper[size_t( present::FindJoint( *set, "Head" ) )] == 1.0f );
	CHECK( upper[size_t( present::FindJoint( *set, "LeftUpperLeg" ) )] == 0.0f );

	auto at = []( const anim::PoseEvaluator& pose, const anim::AnimSet& s, const char* joint ) {
		float v[4];
		ozz::math::StorePtrU( pose.Models()[size_t( present::FindJoint( s, joint ) )].cols[3], v );
		return b3Vec3{ v[0], v[1], v[2] };
	};
	anim::PoseEvaluator base( *set );
	base.SetStances( table );
	base.Evaluate( AnimState{} );

	// Pistol on the upper layer, fully faded in.
	AnimState pistol;
	pistol.stances[0] = 1;
	pistol.layerTime[0] = 1.0f;
	anim::PoseEvaluator withPistol( *set );
	withPistol.SetStances( table );
	withPistol.Evaluate( pistol );
	CHECK( b3Distance( at( withPistol, *set, "RightHand" ), at( base, *set, "RightHand" ) ) > 0.2f );
	CHECK( b3Distance( at( withPistol, *set, "LeftFoot" ), at( base, *set, "LeftFoot" ) ) < 1e-4f );

	// Half way through the fade: about half way there.
	pistol.layerTime[0] = 0.5f * kStanceFadeSeconds;
	withPistol.Evaluate( pistol );
	float half = b3Distance( at( withPistol, *set, "RightHand" ), at( base, *set, "RightHand" ) );
	pistol.layerTime[0] = 1.0f;
	withPistol.Evaluate( pistol );
	float full = b3Distance( at( withPistol, *set, "RightHand" ), at( base, *set, "RightHand" ) );
	std::printf( "    pistol hand moved %.3f m, %.3f half way through the fade\n", full, half );
	CHECK( half > 0.2f * full && half < 0.8f * full );

	// Melee on the full layer: the legs change too.
	AnimState melee;
	melee.stances[1] = 2;
	melee.layerTime[1] = 1.0f;
	anim::PoseEvaluator withMelee( *set );
	withMelee.SetStances( table );
	withMelee.Evaluate( melee );
	CHECK( b3Distance( at( withMelee, *set, "LeftFoot" ), at( base, *set, "LeftFoot" ) ) > 0.02f );

	// A stance the character lacks, or a layer it cannot mask, changes nothing.
	AnimState missing;
	missing.stances[0] = 3;
	missing.stances[2] = 1;
	missing.layerTime[0] = missing.layerTime[2] = 1.0f;
	anim::PoseEvaluator withMissing( *set );
	withMissing.SetStances( table );
	withMissing.Evaluate( missing );
	CHECK( b3Distance( at( withMissing, *set, "RightHand" ), at( base, *set, "RightHand" ) ) < 1e-4f );

	// What plays alongside the pose, for companion tracks: the dominant base clip, and each layer's
	// own clip (a single loop by its layer clock, or its version of the dominant clip).
	{
		AnimState s;
		s.stances[0] = 1; // pistol on upper: a single loop
		s.layerTime[0] = 0.5f;
		s.stances[1] = 2; // melee on full: its own idle
		auto clips = anim::ActiveClips( s, *set, table.get() );
		CHECK( clips.size() == 3 );
		CHECK( clips[0].channel == 0 && clips[0].name == "idle" && clips[0].loops );
		CHECK( clips[1].channel == 1 && clips[1].name == "stance_pistol" && std::fabs( clips[1].time - 0.5f ) < 1e-5f );
		CHECK( clips[2].channel == 2 && clips[2].name == "stance_melee_idle" );
		s.stances[1] = 3; // "sword": no clips on this character, nothing of its own
		CHECK( anim::ActiveClips( s, *set, table.get() ).size() == 2 );
		CHECK( anim::ActiveClips( s, *set, nullptr ).size() == 1 );
	}

	// Without a table (a renderer that knows no schema) stances are ignored.
	anim::PoseEvaluator plain( *set );
	plain.Evaluate( pistol );
	CHECK( b3Distance( at( plain, *set, "RightHand" ), at( base, *set, "RightHand" ) ) < 1e-4f );
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
	schema.stances = { "melee", "melee_swing", "pistol" };
	schema.events = { "pistol.fired", "melee.strike" };
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
	CHECK( zoneAt( head.y + 0.08f ) == "head" );
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
	command( CommandType::Event, 0, 0, 0 ); // pistol.fired, by this player
	step( 1 );
	CHECK( state().graph[1].state == stateOf( 1, "Shoot" ) );
	step( 60 );
	CHECK( state().graph[1].state == stateOf( 1, "Pistol" ) );

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
	aiming.stances[1] = 3; // pistol on the upper layer
	aiming.layerTime[1] = 1.0f;
	std::string stanceWarnings;
	auto procedural = anim::AnimSet::CreateProcedural();
	anim::PoseEvaluator placeholder( *procedural );
	placeholder.SetStances( anim::BuildStanceTable( *procedural, { "full", "upper" }, { "melee", "melee_swing", "pistol" }, stanceWarnings ) );
	placeholder.Evaluate( aiming );
	CheckHeldItem( *procedural, placeholder );
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
	schema.stances = { "melee", "melee_swing", "pistol" };
	schema.events = { "pistol.fired", "melee.strike" };
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
	auto clips = anim::ActiveGraphClips( state, *graph );
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
			auto clips = anim::ActiveGraphClips( now, *graph );
			playing = clips.empty() ? "-" : clips[0].name;
		}
		std::printf( "    right %4d forward %4d: %-10s average hips %6.1f chest %6.1f head %6.1f\n", d[0], d[1], playing.c_str(), hipsYaw,
					 chest, head );
		CHECK( std::fabs( chest ) < 15.0f );
		CHECK( std::fabs( head ) < 15.0f );
	}
}

void TestAnimPipeline()
{
	auto procedural = anim::AnimSet::CreateProcedural();
	CHECK( procedural != nullptr );
	CHECK( procedural->Skeleton().num_joints() > 20 );
	for ( int c = 0; c < anim::ClipCount; ++c )
	{
		CHECK( procedural->Get( anim::Clip( c ) ) != nullptr );
	}

	// Feet on the ground, head up, in the rest-ish idle pose.
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
	uint64_t a = AnimPoseHash( *procedural );
	uint64_t b = AnimPoseHash( *loaded );
	std::printf( "    pose hash %016" PRIx64 " (procedural) %016" PRIx64 " (from files)\n", a, b );
	CHECK( a == b );
	std::filesystem::remove_all( dir );
}

int DumpHashes( const char* path )
{
	auto frames = test::MakeScenario( {} );
	auto hashes = RunReference( frames, TestConfig() );
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

} // namespace

int main( int argc, char** argv )
{
	if ( argc == 2 && std::strcmp( argv[1], "--anim-hash" ) == 0 )
	{
		std::printf( "%016" PRIx64 "\n", AnimPoseHash( *anim::AnimSet::CreateProcedural() ) );
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
		{ "ragdoll", TestRagdoll },
		{ "anim_controller", TestAnimController },
		{ "anim_graph", TestAnimGraph },
		{ "anim_blend2d", TestAnimBlend2D },
		{ "pose_tools", TestPoseTools },
		{ "fields", TestFields },
		{ "hitboxes", TestHitboxes },
		{ "stances", TestStances },
		{ "robot_character", TestRobotCharacter },
		{ "mannequin_character", TestMannequinCharacter },
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
