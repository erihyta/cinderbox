// Determinism and rollback tests.
//
//   cb_tests                    run all tests
//   cb_tests <name>             run one test
//   cb_tests --dump <file>      write per-tick hashes of the reference scenario (cross-build check)
//   cb_tests --compare <file>   compare against a dump from another build/platform
//   cb_tests --anim-hash         pose hash of the procedural rig (cross-build check)
//   cb_tests --save-portable <file> / --load-portable <file>   portable snapshot across builds

#include "anim_controller.h"
#include "map.h"
#include "pose.h"
#include "rollback.h"
#include "scenario.h"
#include "simulation.h"

#include <chrono>
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

	// The spawn button makes the map's template, not the built-in random prop.
	InputFrame frame;
	frame.events.push_back( { PlayerEventType::Join, 0 } );
	frame.tick = 0;
	sim.Step( frame );
	frame.events.clear();

	size_t before = CountFromTemplate( sim, 0 );
	for ( uint32_t t = 1; t < 30; ++t )
	{
		frame.tick = t;
		frame.inputs[0].buttons = ( t % 4 == 0 ) ? BtnSpawnProp : 0;
		sim.Step( frame );
	}
	size_t spawned = CountFromTemplate( sim, 0 );
	std::printf( "    spawned %zu entities from the map's template\n", spawned - before );
	CHECK( spawned > before );

	// The authored lifetime expires them; the level's own instance stays.
	frame.inputs[0].buttons = 0;
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

	// Spawn 15 props with edge-triggered presses: the per-player cap keeps 10.
	for ( int i = 0; i < 15; ++i )
	{
		f.inputs[0].buttons = BtnSpawnProp;
		step( 1 );
		f.inputs[0].buttons = 0;
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

	// Holding the button must not spawn more.
	f.inputs[0].buttons = BtnSpawnProp;
	step( 30 );
	uint32_t netIdsBefore = sim.Globals().nextNetId;
	step( 30 );
	CHECK( sim.Globals().nextNetId == netIdsBefore );

	// Props expire after their lifetime.
	f.inputs[0].buttons = 0;
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
		{ "rollback", TestRollback },
		{ "rollback_reset", TestRollbackReset },
		{ "gameplay_sanity", TestGameplaySanity },
		{ "anim_controller", TestAnimController },
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
