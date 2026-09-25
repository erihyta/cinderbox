#include "mod_api.h"

#include "anim_graph.h"

#include "detmath.h"
#include "hit_test.h"
#include "util.h"

#include "box3d/box3d.h"

#include <algorithm>
#include <cstdlib>

namespace cb::mods
{

namespace
{

Float3 ToFloat3( b3Vec3 v )
{
	return { v.x, v.y, v.z };
}

uint16_t YawIndex( float yaw )
{
	return detmath::RadiansToYaw( yaw );
}

} // namespace

// --- Declarations ----------------------------------------------------------------------------------

void Declarations::BeginMod( const std::string& name )
{
	m_mod = name;
	m_schema.mods.push_back( name );
}

FieldHandle Declarations::Field( const std::string& name, BoardType type, BoardScope scope )
{
	if ( name.empty() || name.size() > kMaxSchemaName )
	{
		m_errors.push_back( m_mod + ": bad field name \"" + name + "\"" );
		return {};
	}
	if ( const BoardField* existing = m_schema.FindField( name ) )
	{
		if ( existing->type != type || existing->scope != scope )
		{
			m_errors.push_back( m_mod + ": field \"" + name + "\" was already declared with another type or scope" );
			return {};
		}
		return { existing->slot, existing->scope, existing->type };
	}

	int& used = scope == BoardScope::Entity ? m_entitySlots : m_globalSlots;
	if ( used >= kBoardSlots )
	{
		m_errors.push_back( m_mod + ": no board slot left for \"" + name + "\"" );
		return {};
	}
	BoardField field;
	field.name = name;
	field.type = type;
	field.scope = scope;
	field.slot = uint8_t( used++ );
	m_schema.fields.push_back( field );
	return { field.slot, scope, type };
}

EventHandle Declarations::Event( const std::string& name )
{
	int existing = m_schema.FindEvent( name );
	if ( existing >= 0 )
	{
		return { existing };
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.events.size() >= 65535 )
	{
		m_errors.push_back( m_mod + ": bad event \"" + name + "\"" );
		return {};
	}
	m_schema.events.push_back( name );
	return { int( m_schema.events.size() - 1 ) };
}

LayerHandle Declarations::Layer( const std::string& name )
{
	int existing = m_schema.FindLayer( name );
	if ( existing >= 0 )
	{
		return { existing };
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.layers.size() >= size_t( kMaxAnimLayers ) )
	{
		m_errors.push_back( m_mod + ": bad or one too many animation layers (\"" + name + "\")" );
		return {};
	}
	m_schema.layers.push_back( name );
	return { int( m_schema.layers.size() - 1 ) };
}

StanceHandle Declarations::Stance( const std::string& name )
{
	int existing = m_schema.FindStance( name );
	if ( existing >= 0 )
	{
		return { existing };
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.stances.size() >= size_t( kMaxStances ) )
	{
		m_errors.push_back( m_mod + ": bad stance \"" + name + "\"" );
		return {};
	}
	m_schema.stances.push_back( name );
	return { int( m_schema.stances.size() - 1 ) };
}

ActionHandle Declarations::Action( const std::string& name, const std::string& key )
{
	if ( const ModAction* existing = m_schema.FindAction( name ) )
	{
		return { uint16_t( 1u << existing->bit ) };
	}
	if ( name.empty() || name.size() > kMaxSchemaName || key.size() > kMaxSchemaName )
	{
		m_errors.push_back( m_mod + ": bad action \"" + name + "\"" );
		return {};
	}
	if ( m_schema.actions.size() >= size_t( kMaxActions ) )
	{
		m_errors.push_back( m_mod + ": no action bit left for \"" + name + "\"" );
		return {};
	}
	ModAction action;
	action.name = name;
	action.bit = uint8_t( m_schema.actions.size() );
	action.key = key;
	m_schema.actions.push_back( action );
	return { uint16_t( 1u << action.bit ) };
}

// --- Context ---------------------------------------------------------------------------------------

Context::Context( Simulation& sim, const ModSchema& schema, InputFrame& frame, const std::array<PlayerInput, kMaxPlayers>& previous,
				  flecs::world& world, uint64_t& rng )
	: m_sim( sim )
	, m_schema( schema )
	, m_frame( frame )
	, m_previous( previous )
	, m_world( world )
	, m_rng( rng )
{
}

bool Context::Joining( PlayerSlot slot ) const
{
	for ( const PlayerEvent& e : m_frame.events )
	{
		if ( e.slot == slot && e.type == PlayerEventType::Join )
		{
			return true;
		}
	}
	return false;
}

bool Context::Leaving( PlayerSlot slot ) const
{
	for ( const PlayerEvent& e : m_frame.events )
	{
		if ( e.slot == slot && e.type == PlayerEventType::Leave )
		{
			return true;
		}
	}
	return false;
}

int Context::SlotOf( uint32_t netId ) const
{
	if ( netId == 0 )
	{
		return -1;
	}
	for ( int slot = 0; slot < kMaxPlayers; ++slot )
	{
		if ( m_sim.PlayerNetId( PlayerSlot( slot ) ) == netId )
		{
			return slot;
		}
	}
	return -1;
}

bool Context::CastRay( b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit ) const
{
	if ( m_hits != nullptr )
	{
		return m_hits->CastRay( m_sim, origin, translation, ignoreNetId, hit );
	}
	return m_sim.CastRay( origin, translation, ignoreNetId, hit );
}

b3Vec3 Context::EyePosition( PlayerSlot slot ) const
{
	const Transform* t = m_sim.EntityTransform( m_sim.PlayerNetId( slot ) );
	if ( t == nullptr )
	{
		return {};
	}
	// Clients orbit their camera around this point (see game.gd), so the crosshair ray goes
	// through it along the camera's direction.
	return b3Add( t->position, b3Vec3{ 0.0f, 0.4f, 0.0f } );
}

b3Vec3 Context::AimDirection( PlayerSlot slot ) const
{
	const PlayerInput& in = m_frame.inputs[slot];
	float yaw = detmath::YawToRadians( in.cameraYaw );
	float pitch = float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f );
	b3CosSin p = detmath::CosSin( pitch );
	b3Vec3 forward = detmath::YawForward( yaw );
	return { forward.x * p.cosine, p.sine, forward.z * p.cosine };
}

int32_t Context::Get( uint32_t netId, FieldHandle field ) const
{
	if ( field.Valid() == false )
	{
		return 0;
	}
	if ( field.scope == BoardScope::Global )
	{
		return m_sim.GlobalBoardValue( field.slot );
	}
	return m_sim.BoardValue( netId, field.slot );
}

int32_t Context::GetGlobal( FieldHandle field ) const
{
	return field.Valid() ? m_sim.GlobalBoardValue( field.slot ) : 0;
}

bool Context::IsDynamic( uint32_t netId ) const
{
	flecs::entity e = m_sim.FindEntity( netId );
	if ( e.is_valid() == false )
	{
		return false;
	}
	if ( e.has<Ragdoll>() )
	{
		return true;
	}
	const PhysicsBody* pb = e.try_get<PhysicsBody>();
	return pb != nullptr && e.has<Character>() == false && e.has<StaticGeometry>() == false &&
		   b3Body_GetType( m_sim.BodyOf( *pb ) ) == b3_dynamicBody;
}

std::vector<uint32_t> Context::Props() const
{
	std::vector<uint32_t> out;
	for ( const Simulation::EntityRef& r : m_sim.Entities() )
	{
		if ( flecs::entity( m_sim.World(), r.entity ).has<Prop>() )
		{
			out.push_back( r.netId );
		}
	}
	return out;
}

std::vector<uint32_t> Context::SpawnedProps() const
{
	std::vector<uint32_t> out;
	for ( const Simulation::EntityRef& r : m_sim.Entities() )
	{
		const Prop* p = flecs::entity( m_sim.World(), r.entity ).try_get<Prop>();
		if ( p != nullptr && p->owner != 0 )
		{
			out.push_back( r.netId );
		}
	}
	return out;
}

std::vector<uint32_t> Context::Ragdolls() const
{
	std::vector<uint32_t> out;
	for ( const Simulation::EntityRef& r : m_sim.Entities() )
	{
		if ( flecs::entity( m_sim.World(), r.entity ).has<Ragdoll>() )
		{
			out.push_back( r.netId );
		}
	}
	return out;
}

bool Context::AnimationEmits( EventHandle event ) const
{
	return event.Valid() && m_sim.Graph() != nullptr && m_sim.Graph()->EmitsEvent( event.index );
}

std::vector<ModEventRecord> Context::RecentEvents() const
{
	std::vector<ModEventRecord> out;
	const SimGlobals& g = m_sim.Globals();
	uint32_t kept = std::min( g.modEventCount, kModEventHistory );
	for ( uint32_t i = 0; i < kept; ++i )
	{
		const ModEventRecord& e = g.modEvents[( g.modEventCount - kept + i ) % kModEventHistory];
		if ( e.tick + 1 == m_frame.tick )
		{
			out.push_back( e );
		}
	}
	return out;
}

double Context::Option( const std::string& name, double fallback ) const
{
	if ( m_options == nullptr )
	{
		return fallback;
	}
	auto it = m_options->find( name );
	if ( it == m_options->end() )
	{
		return fallback;
	}
	char* end = nullptr;
	double value = std::strtod( it->second.c_str(), &end );
	return end != it->second.c_str() ? value : fallback;
}

uint64_t Context::Random()
{
	return NextRandom( m_rng );
}

float Context::RandomRange( float lo, float hi )
{
	return cb::RandomRange( m_rng, lo, hi );
}

void Context::Add( const SimCommand& command )
{
	if ( m_frame.commands.size() < kMaxCommandsPerFrame )
	{
		m_frame.commands.push_back( command );
	}
}

void Context::Set( uint32_t target, FieldHandle field, int32_t value )
{
	if ( field.Valid() == false )
	{
		return;
	}
	SimCommand c;
	c.type = CommandType::SetField;
	c.target = field.scope == BoardScope::Global ? 0 : target;
	c.index = uint16_t( field.slot );
	c.value = value;
	Add( c );
}

void Context::Emit( EventHandle event, uint32_t a, uint32_t b, int32_t value, b3Vec3 point, b3Vec3 vector )
{
	if ( event.Valid() == false )
	{
		return;
	}
	SimCommand c;
	c.type = CommandType::Event;
	c.index = uint16_t( event.index );
	c.target = a;
	c.other = b;
	c.value = value;
	c.a = ToFloat3( point );
	c.b = ToFloat3( vector );
	Add( c );
}

void Context::SpawnProp( ShapeKind kind, b3Vec3 halfExtents, b3Vec3 position, float yaw, b3Vec3 velocity, uint32_t owner,
						 uint32_t lifetimeTicks )
{
	SimCommand c;
	c.type = CommandType::SpawnProp;
	c.mode = uint8_t( kind );
	c.index = YawIndex( yaw );
	c.target = owner;
	c.other = lifetimeTicks;
	c.value = -1;
	c.a = ToFloat3( position );
	c.b = ToFloat3( velocity );
	c.c = ToFloat3( halfExtents );
	Add( c );
}

void Context::SpawnTemplate( uint32_t templateIndex, b3Vec3 position, float yaw, b3Vec3 velocity, uint32_t owner,
							 uint32_t lifetimeTicks )
{
	SimCommand c;
	c.type = CommandType::SpawnProp;
	c.index = YawIndex( yaw );
	c.target = owner;
	c.other = lifetimeTicks;
	c.value = int32_t( templateIndex );
	c.a = ToFloat3( position );
	c.b = ToFloat3( velocity );
	Add( c );
}

void Context::Destroy( uint32_t target )
{
	SimCommand c;
	c.type = CommandType::Destroy;
	c.target = target;
	Add( c );
}

void Context::Push( uint32_t target, b3Vec3 point, b3Vec3 vector, ImpulseMode mode )
{
	SimCommand c;
	c.type = CommandType::Impulse;
	c.mode = mode;
	c.target = target;
	c.a = ToFloat3( point );
	c.b = ToFloat3( vector );
	Add( c );
}

void Context::Kill( uint32_t target, bool ragdoll, b3Vec3 hitPoint, b3Vec3 hitVelocity, uint32_t ragdollLifetimeTicks,
					uint32_t ragdollCap )
{
	SimCommand c;
	c.type = CommandType::Kill;
	c.mode = ragdoll ? 1 : 0;
	c.target = target;
	c.other = ragdollLifetimeTicks;
	c.value = int32_t( std::min<uint32_t>( ragdollCap, 1u << 30 ) );
	c.a = ToFloat3( hitPoint );
	c.b = ToFloat3( hitVelocity );
	Add( c );
}

void Context::Respawn( uint32_t target )
{
	SimCommand c;
	c.type = CommandType::Respawn;
	c.target = target;
	Add( c );
}

void Context::Freeze( uint32_t target, bool frozen )
{
	SimCommand c;
	c.type = CommandType::Freeze;
	c.mode = frozen ? 1 : 0;
	c.target = target;
	Add( c );
}

void Context::SetStance( uint32_t target, LayerHandle layer, StanceHandle stance )
{
	if ( layer.Valid() == false )
	{
		return;
	}
	SimCommand c;
	c.type = CommandType::Stance;
	c.index = uint16_t( layer.index );
	c.value = stance.Valid() ? stance.index + 1 : 0;
	c.target = target;
	Add( c );
}

void Context::FaceCamera( uint32_t target, bool faceCamera )
{
	SimCommand c;
	c.type = CommandType::Facing;
	c.mode = faceCamera ? 1 : 0;
	c.target = target;
	Add( c );
}

void Context::Aim( uint32_t target, bool aiming )
{
	SimCommand c;
	c.type = CommandType::Aim;
	c.mode = aiming ? 1 : 0;
	c.target = target;
	Add( c );
}

void Context::RespawnAt( uint32_t target, b3Vec3 position, float yaw )
{
	SimCommand c;
	c.type = CommandType::Respawn;
	c.mode = 1;
	c.index = YawIndex( yaw );
	c.target = target;
	c.a = ToFloat3( position );
	Add( c );
}

} // namespace cb::mods
