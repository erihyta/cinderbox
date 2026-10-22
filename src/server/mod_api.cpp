#include "mod_api.h"

#include "anim_graph.h"

#include "detmath.h"
#include "map.h"
#include "protocol.h"
#include "reflect.h"
#include "hit_test.h"
#include "mover.h"
#include "ragdoll.h"
#include "util.h"

#include "box3d/box3d.h"

#include <algorithm>
#include <cctype>
#include <bit>
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

	int& used = scope == BoardScope::Entity ? m_entitySlots : scope == BoardScope::Global ? m_globalSlots : m_privateSlots;
	if ( used >= kFieldLimit )
	{
		m_errors.push_back( m_mod + ": no field number left for \"" + name + "\"" );
		return {};
	}
	BoardField field;
	field.name = name;
	field.type = type;
	field.scope = scope;
	field.slot = uint16_t( used++ );
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

IntentHandle Declarations::Intent( const std::string& name )
{
	int existing = m_schema.FindIntent( name );
	if ( existing >= 0 )
	{
		return { existing };
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.intents.size() >= size_t( 255 - kFirstModIntent ) )
	{
		m_errors.push_back( m_mod + ": bad intent \"" + name + "\"" );
		return {};
	}
	m_schema.intents.push_back( name );
	return { int( m_schema.intents.size() - 1 ) };
}

void Declarations::TextChannel( const std::string& name )
{
	bool good = name.empty() == false && name.size() <= net::kMaxTextChannel;
	for ( char c : name )
	{
		good = good && ( std::isalnum( uint8_t( c ) ) || c == '.' || c == '_' || c == '-' );
	}
	if ( good == false )
	{
		m_errors.push_back( m_mod + ": bad text channel \"" + name + "\"" );
		return;
	}
	if ( std::find( m_textChannels.begin(), m_textChannels.end(), name ) == m_textChannels.end() )
	{
		m_textChannels.push_back( name );
	}
}

LayerHandle Declarations::Layer( const std::string& name )
{
	int existing = m_schema.FindLayer( name );
	if ( existing >= 0 )
	{
		return { existing };
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.layers.size() >= size_t( kLayerLimit ) )
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

ItemKindHandle Declarations::ItemKind( const std::string& name )
{
	int existing = m_schema.FindItemKind( name );
	if ( existing >= 0 )
	{
		std::vector<std::string>& mods = m_itemMods[size_t( existing )];
		if ( std::find( mods.begin(), mods.end(), m_mod ) == mods.end() )
		{
			mods.push_back( m_mod );
		}
		return { existing };
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.itemKinds.size() >= 255 )
	{
		m_errors.push_back( m_mod + ": bad item kind \"" + name + "\"" );
		return {};
	}
	m_schema.itemKinds.push_back( name );
	m_schema.itemShapes.push_back( ItemShape{} );
	m_itemMods.push_back( { m_mod } );
	return { int( m_schema.itemKinds.size() - 1 ) };
}

std::vector<Declarations::OwnItem> Declarations::ItemsOfLook()
{
	std::vector<OwnItem> out;
	if ( !m_lookItems )
	{
		return out;
	}
	for ( LookItem& look : m_lookItems( m_mod ) )
	{
		OwnItem own;
		own.kind = ItemKind( look.kind );
		if ( own.kind.Valid() == false )
		{
			continue;
		}
		if ( look.pack.empty() == false )
		{
			ItemLayers( own.kind, AnimPack( look.pack ) );
		}
		if ( look.holster.empty() == false )
		{
			ItemProperty( own.kind, "holster", Socket( look.holster ) );
		}
		auto property = look.properties.find( "use" );
		int use = property != look.properties.end() ? int( property->second ) : 0;
		if ( use == kItemUsePress || use == kItemUseHold )
		{
			// Used by its slot's key: what the simulation records then, for its motions and looks.
			Event( look.kind + ".used" );
		}
		// Its own fields: each copy has them, starting as the scene says.
		for ( const LookItem::Field& field : look.fields )
		{
			BoardType type = field.type == "float" ? BoardType::Float : field.type == "bool" ? BoardType::Bool : BoardType::Int;
			FieldHandle handle = Field( field.name, type, BoardScope::Entity );
			if ( handle.Valid() == false )
			{
				continue;
			}
			int32_t raw = type == BoardType::Float	 ? BoardFromFloat( field.value )
						  : type == BoardType::Bool ? ( field.value != 0.0f ? 1 : 0 )
													 : int32_t( std::lround( field.value ) );
			m_itemFieldDefaults[own.kind.index].push_back( { uint16_t( handle.slot ), raw } );
		}
		if ( look.useKey.empty() == false )
		{
			// Its own key: uses the first one carried, wherever it is.
			ItemUseKey( own.kind, look.useKey );
		}
		if ( look.motions )
		{
			// Its own motions: each asks that its player has the item (the bake wrote that).
			Motions( look.kind );
		}
		// (Its numbers reach every mod as the kind's properties when its body is read.)
		own.look = std::move( look );
		out.push_back( std::move( own ) );
	}
	return out;
}

ItemKindHandle Declarations::ItemKind( const std::string& name, const ItemShape& shape )
{
	ItemKindHandle handle = ItemKind( name );
	if ( handle.Valid() == false )
	{
		return handle;
	}
	size_t index = size_t( handle.index );
	if ( m_shapeDeclared.size() <= index )
	{
		m_shapeDeclared.resize( index + 1, false );
	}
	if ( m_shapeDeclared[index] == false )
	{
		m_schema.itemShapes[index] = shape;
		m_shapeDeclared[index] = true;
	}
	return handle;
}

void Declarations::ItemLayers( ItemKindHandle kind, AnimPackHandle pack )
{
	if ( kind.Valid() == false || pack.Valid() == false )
	{
		m_errors.push_back( m_mod + ": item layers need an item kind and an animation pack" );
		return;
	}
	m_itemLayers.emplace( kind.index, pack.index ); // the first one stays
}

void Declarations::ItemUseKey( ItemKindHandle kind, const std::string& key )
{
	if ( kind.Valid() == false || key.empty() || key.find( ' ' ) != std::string::npos )
	{
		m_errors.push_back( m_mod + ": bad use key \"" + key + "\"" );
		return;
	}
	ActionHandle action = Action( m_schema.itemKinds[size_t( kind.index )] + ".use", key );
	if ( action.mask != 0 && m_itemUseActions.count( kind.index ) == 0 )
	{
		m_itemUseActions[kind.index] = uint8_t( std::countr_zero( action.mask ) ); // the first one stays
	}
}

void Declarations::ContainerTemplate( const std::string& name, const std::string& visual, b3Vec3 size, int slots )
{
	// (A map's names: letters, digits, _ and -.)
	auto safe = []( const std::string& text ) {
		return text.empty() == false && std::all_of( text.begin(), text.end(), []( char c ) {
				   return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
			   } );
	};
	if ( safe( name ) == false || safe( visual ) == false )
	{
		m_errors.push_back( m_mod + ": a template's name and look are letters, digits, _ and - (\"" + name + "\", \"" + visual + "\")" );
		return;
	}
	auto raw = []( const char* field, int32_t value ) {
		AuthoredField f;
		f.id = Fnv32( field );
		f.raw[0] = value;
		return f;
	};
	EntityTemplate t;
	t.name = name;
	t.visual = visual;
	AuthoredComponent shape;
	shape.id = Fnv32( "Shape" );
	shape.fields.push_back( raw( "kind", int32_t( ShapeKind::Box ) ) );
	AuthoredField extent;
	extent.id = Fnv32( "size" );
	extent.raw[0] = MapQuantize( size.x, kMapPositionScale );
	extent.raw[1] = MapQuantize( size.y, kMapPositionScale );
	extent.raw[2] = MapQuantize( size.z, kMapPositionScale );
	shape.fields.push_back( extent );
	AuthoredComponent body;
	body.id = Fnv32( "Body" );
	body.fields.push_back( raw( "type", 0 ) ); // static
	AuthoredComponent container;
	container.id = Fnv32( "Container" );
	container.fields.push_back( raw( "slots", int32_t( std::clamp( slots, 1, kMaxSlots ) ) ) );
	t.components = { shape, body, container };
	SanitizeTemplate( t );
	m_templates.push_back( t );
}

double Declarations::Option( const std::string& name, double fallback ) const
{
	auto it = m_options != nullptr ? m_options->find( name ) : decltype( m_options->end() )();
	if ( m_options == nullptr || it == m_options->end() )
	{
		return fallback;
	}
	char* end = nullptr;
	double value = std::strtod( it->second.c_str(), &end );
	return end != it->second.c_str() ? value : fallback;
}

void Declarations::Slots( int count )
{
	if ( count < 0 || count > kMaxSlots )
	{
		m_errors.push_back( m_mod + ": " + std::to_string( count ) + " slots (a player has at most " + std::to_string( kMaxSlots ) + ")" );
		return;
	}
	m_slots = std::max( m_slots, count );
}

void Declarations::ItemProperty( ItemKindHandle kind, const std::string& name, float value )
{
	if ( kind.Valid() == false || name.empty() )
	{
		m_errors.push_back( m_mod + ": bad item property \"" + name + "\"" );
		return;
	}
	m_itemProperties.emplace( std::make_pair( kind.index, name ), value ); // the first one stays
}

void Declarations::ItemProperty( ItemKindHandle kind, const std::string& name, SocketHandle socket )
{
	// Stored as a number: socket + 1, so 0 (and "not declared") reads as none.
	ItemProperty( kind, name, float( socket.Valid() ? socket.index + 1 : 0 ) );
}

AnimPackHandle Declarations::AnimPack( const std::string& name )
{
	for ( size_t i = 0; i < m_schema.animPacks.size(); ++i )
	{
		if ( m_schema.animPacks[i].name == name )
		{
			return { int( i ) };
		}
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.animPacks.size() >= 254 )
	{
		m_errors.push_back( m_mod + ": bad animation pack \"" + name + "\"" );
		return {};
	}
	m_schema.animPacks.push_back( { m_mod, name, "" } );
	return { int( m_schema.animPacks.size() - 1 ) };
}

CharacterHandle Declarations::Character( const std::string& name )
{
	for ( size_t i = 0; i < m_schema.characters.size(); ++i )
	{
		if ( m_schema.characters[i].name == name )
		{
			return { int( i ) };
		}
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.characters.size() >= kMaxCharacters )
	{
		m_errors.push_back( m_mod + ": bad character \"" + name + "\"" );
		return {};
	}
	CharacterInfo character;
	character.name = name;
	character.mod = m_mod;
	m_schema.characters.push_back( character );
	return { int( m_schema.characters.size() - 1 ) };
}

MotionsHandle Declarations::Motions( const std::string& name )
{
	for ( size_t i = 0; i < m_schema.motionSets.size(); ++i )
	{
		if ( m_schema.motionSets[i].name == name )
		{
			return { int( i ) };
		}
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.motionSets.size() >= 254 )
	{
		m_errors.push_back( m_mod + ": bad motion set \"" + name + "\"" );
		return {};
	}
	m_schema.motionSets.push_back( { m_mod, name, "" } );
	return { int( m_schema.motionSets.size() - 1 ) };
}

SocketHandle Declarations::Socket( const std::string& name )
{
	int existing = m_schema.FindSocket( name );
	if ( existing >= 0 )
	{
		return { existing };
	}
	if ( name.empty() || name.size() > kMaxSchemaName || m_schema.sockets.size() >= 255 )
	{
		m_errors.push_back( m_mod + ": bad socket \"" + name + "\"" );
		return {};
	}
	m_schema.sockets.push_back( name );
	return { int( m_schema.sockets.size() - 1 ) };
}

ActionHandle Declarations::Action( const std::string& name, const std::string& key )
{
	if ( const ModAction* existing = m_schema.FindAction( name ) )
	{
		return { ActionBits( 1 ) << existing->bit };
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
	return { ActionBits( 1 ) << action.bit };
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
	// Clients orbit their third-person camera around this point (see game.gd).
	return b3Add( t->position, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } );
}

b3Vec3 Context::HeadPosition( PlayerSlot slot ) const
{
	b3Vec3 head;
	if ( m_hits == nullptr || m_hits->JointPosition( m_sim, slot, "Head", head ) == false )
	{
		return EyePosition( slot );
	}
	// A little ahead of the face and above the joint, as the first-person camera sits.
	b3Vec3 forward = AimDirection( slot );
	const PlayerInput& in = m_frame.inputs[slot];
	b3Vec3 flat = detmath::YawForward( detmath::YawToRadians( in.cameraYaw ) );
	b3CosSin p = detmath::CosSin( float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f ) );
	b3Vec3 up = { -flat.x * p.sine, p.cosine, -flat.z * p.sine };
	return b3Add( head, b3Add( b3MulSV( kEyeAhead, forward ), b3MulSV( kEyeUp, up ) ) );
}

b3Vec3 Context::ViewPosition( PlayerSlot slot ) const
{
	const PlayerInput& in = m_frame.inputs[slot];
	switch ( ViewMode( in.view ) )
	{
		case ViewMode::FirstPerson:
		{
			const Transform* t = m_sim.EntityTransform( m_sim.PlayerNetId( slot ) );
			if ( t == nullptr || m_hits == nullptr )
			{
				return EyePosition( slot );
			}
			const uint16_t character = m_sim.CharacterOf( m_sim.FindEntity( m_sim.PlayerNetId( slot ) ) );
			const Simulation::CharacterBody& body = m_sim.Body( character );
			b3Vec3 feet = b3Sub( t->position, b3Vec3{ 0.0f, mover::FeetBelowCenter( body.radius, body.halfHeight ), 0.0f } );
			return b3Add( b3Add( feet, b3Vec3{ 0.0f, m_hits->EyeHeight( character ), 0.0f } ), b3MulSV( kEyeAhead, AimDirection( slot ) ) );
		}
		case ViewMode::ShoulderRight:
		case ViewMode::ShoulderLeft:
		{
			b3Vec3 right = detmath::YawRight( detmath::YawToRadians( in.cameraYaw ) );
			float side = ViewMode( in.view ) == ViewMode::ShoulderRight ? kShoulderOffset : -kShoulderOffset;
			return b3MulAdd( EyePosition( slot ), side, right );
		}
		case ViewMode::ThirdPerson:
		default:
			return EyePosition( slot );
	}
}

bool Context::CastAim( PlayerSlot slot, float range, RayHit& hit, b3Vec3& origin, b3Vec3& direction ) const
{
	uint32_t self = m_sim.PlayerNetId( slot );
	b3Vec3 look = AimDirection( slot );
	origin = HeadPosition( slot );
	direction = look;
	{
		// What is under the crosshair: the first thing along the camera's line, from where that
		// line passes the player (nothing between the camera and there is in the way).
		b3Vec3 view = ViewPosition( slot );
		RayHit seen;
		b3Vec3 target = CastRay( view, b3MulSV( range, look ), self, seen ) ? seen.point : b3MulAdd( view, range, look );
		// From the eye to it. Something right in front of the face gives no sensible direction:
		// the shot then goes where the player looks.
		b3Vec3 to = b3Sub( target, origin );
		float distance = b3Length( to );
		if ( distance > 0.5f && b3Dot( to, look ) > 0.5f * distance )
		{
			direction = b3MulSV( 1.0f / distance, to );
		}
	}
	return CastRay( origin, b3MulSV( range, direction ), self, hit );
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
	if ( field.scope == BoardScope::Private )
	{
		int slot = SlotOfTarget( netId );
		return m_privates != nullptr && slot >= 0 ? ( *m_privates )[size_t( slot )].values[field.slot] : 0;
	}
	return m_sim.BoardValue( netId, field.slot );
}

int32_t Context::GetGlobal( FieldHandle field ) const
{
	return field.Valid() ? m_sim.GlobalBoardValue( field.slot ) : 0;
}

bool Context::IsBreakable( uint32_t netId ) const
{
	flecs::entity e = m_sim.FindEntity( netId );
	if ( e.is_valid() == false )
	{
		return false;
	}
	if ( e.has<WallPiece>() )
	{
		return true;
	}
	const TemplateRef* ref = e.try_get<TemplateRef>();
	return ref != nullptr && ref->index < m_sim.Map().templates.size() && m_sim.Map().templates[ref->index].pieces.empty() == false;
}

b3Vec3 Context::EntityVelocity( uint32_t netId ) const
{
	flecs::entity e = m_sim.FindEntity( netId );
	if ( e.is_valid() == false )
	{
		return { 0.0f, 0.0f, 0.0f };
	}
	const PhysicsBody* pb = e.try_get<PhysicsBody>();
	if ( pb != nullptr && e.has<Character>() == false )
	{
		return b3Body_GetLinearVelocity( m_sim.BodyOf( *pb ) );
	}
	const Velocity* v = e.try_get<Velocity>();
	return v != nullptr ? v->linear : b3Vec3{ 0.0f, 0.0f, 0.0f };
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
	if ( event.Valid() == false )
	{
		return false;
	}
	if ( m_sim.Graph() != nullptr && m_sim.Graph()->EmitsEvent( event.index ) )
	{
		return true;
	}
	// A pack's clips too: an item that brings its own swing brings the swing's marker.
	for ( const auto& pack : m_sim.Packs() )
	{
		if ( pack != nullptr && pack->EmitsEvent( event.index ) )
		{
			return true;
		}
	}
	return false;
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

float Context::ItemProperty( ItemKindHandle kind, const std::string& name, float fallback ) const
{
	if ( m_itemProperties == nullptr )
	{
		return fallback;
	}
	auto it = m_itemProperties->find( std::make_pair( kind.index, name ) );
	return it != m_itemProperties->end() ? it->second : fallback;
}

SocketHandle Context::ItemSocket( ItemKindHandle kind, const std::string& name ) const
{
	int socket = int( ItemProperty( kind, name, 0.0f ) ) - 1;
	return socket >= 0 && socket < int( m_schema.sockets.size() ) ? SocketHandle{ socket } : SocketHandle{};
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
	if ( field.scope == BoardScope::Private )
	{
		// Not a command: nothing of it reaches the simulation or anyone but its owner.
		int slot = SlotOfTarget( target );
		if ( m_privates != nullptr && slot >= 0 && ( *m_privates )[size_t( slot )].values[field.slot] != value )
		{
			( *m_privates )[size_t( slot )].values[field.slot] = value;
			( *m_privatesChanged )[size_t( slot )] = true;
		}
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

void Context::Break( uint32_t target, b3Vec3 blast, float speed, float seconds, float radius )
{
	SimCommand c;
	c.type = CommandType::Break;
	c.target = target;
	c.a = { blast.x, blast.y, blast.z };
	c.b = { std::max( speed, 0.0f ), std::max( radius, 0.0f ), 0.0f };
	c.other = uint32_t( std::max( seconds, 0.0f ) * float( m_sim.Config().tickRate ) );
	Add( c );
}

std::vector<uint32_t> Context::BreakablesNear( b3Vec3 point, float radius ) const
{
	return m_sim.BreakablesNear( point, radius );
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

void Context::SetMove( uint32_t target, MoveParam param, float value )
{
	SimCommand c;
	c.type = CommandType::SetMove;
	c.mode = 1;
	c.index = uint16_t( param );
	c.target = target;
	c.a = { value, 0.0f, 0.0f };
	Add( c );
}

void Context::ResetMove( uint32_t target, MoveParam param )
{
	SimCommand c;
	c.type = CommandType::SetMove;
	c.mode = 0;
	c.index = uint16_t( param );
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

void Context::SpawnItem( uint32_t holder, ItemKindHandle kind, SocketHandle socket )
{
	if ( kind.Valid() == false || socket.Valid() == false )
	{
		return;
	}
	SimCommand c;
	c.type = CommandType::SpawnItem;
	c.target = holder;
	c.index = uint16_t( kind.index );
	c.mode = uint8_t( socket.index );
	Add( c );
}

void Context::GiveItem( uint32_t holder, ItemKindHandle kind, SocketHandle holster )
{
	if ( kind.Valid() == false )
	{
		return;
	}
	SimCommand c;
	c.type = CommandType::SpawnItem;
	c.target = holder;
	c.index = uint16_t( kind.index );
	c.mode = holster.Valid() ? uint8_t( holster.index ) : kNoSocket;
	c.value = 1;
	Add( c );
}

void Context::StowItem( uint32_t item, SocketHandle holster )
{
	SimCommand c;
	c.type = CommandType::MoveItem;
	c.target = item;
	c.mode = holster.Valid() ? uint8_t( holster.index ) : kNoSocket;
	c.value = 1;
	Add( c );
}

void Context::HoldItem( uint32_t item, SocketHandle socket )
{
	if ( socket.Valid() == false )
	{
		return;
	}
	SimCommand c;
	c.type = CommandType::MoveItem;
	c.target = item;
	c.mode = uint8_t( socket.index );
	c.value = 0;
	Add( c );
}

// What the simulation's slots do with this tick's input (Simulation::ItemUses): the same copies.
uint32_t Context::UsedItem( PlayerSlot slot, ItemKindHandle kind, bool held ) const
{
	if ( kind.Valid() == false || slot >= kMaxPlayers )
	{
		return 0;
	}
	std::vector<Simulation::ItemUse> uses;
	m_sim.ItemUses( m_sim.PlayerNetId( slot ), m_frame.inputs[slot], m_previous[slot].buttons, m_previous[slot].actions, uses );
	for ( const Simulation::ItemUse& use : uses )
	{
		const cb::HeldItem* item = m_sim.FindEntity( use.item ).try_get<cb::HeldItem>();
		if ( item != nullptr && int( item->kind ) == kind.index && ( held ? use.inUse : use.used ) )
		{
			return use.item;
		}
	}
	return 0;
}

bool Context::Using( PlayerSlot slot, ItemKindHandle kind ) const
{
	return UsedItem( slot, kind, true ) != 0;
}

bool Context::Used( PlayerSlot slot, ItemKindHandle kind ) const
{
	return UsedItem( slot, kind, false ) != 0;
}

int Context::SlotCount() const
{
	return int( m_sim.Config().slots );
}

int Context::SelectedSlot( PlayerSlot player ) const
{
	uint8_t selected = m_sim.SelectedSlot( m_sim.PlayerNetId( player ) );
	return selected == kNoSlot ? -1 : int( selected );
}

uint32_t Context::SlotItem( PlayerSlot player, int slot ) const
{
	return slot >= 0 && slot < kMaxSlots ? m_sim.SlotItem( m_sim.PlayerNetId( player ), uint8_t( slot ) ) : 0;
}

const std::vector<TypedText>& Context::Typed() const
{
	static const std::vector<TypedText> none;
	return m_typed != nullptr ? *m_typed : none;
}

void Context::Say( const std::string& channel, const std::string& text, PlayerSlot from, PlayerSlot to )
{
	std::string words = net::SanitizeText( text, net::kMaxText );
	if ( m_said == nullptr || words.empty() || channel.empty() || channel.size() > net::kMaxTextChannel )
	{
		return;
	}
	m_said->push_back( { channel, std::move( words ), from < kMaxPlayers ? from : kNoSlot, to < kMaxPlayers ? to : kNoSlot } );
}

std::vector<IntentSent> Context::Intents( IntentHandle intent ) const
{
	std::vector<IntentSent> out;
	if ( intent.Valid() == false )
	{
		return out;
	}
	const uint8_t code = uint8_t( kFirstModIntent + intent.index );
	for ( int i = 0; i < kMaxPlayers; ++i )
	{
		const PlayerInput& in = m_frame.inputs[i];
		// (New on this tick: its count moved. A late or guessed input repeats the count.)
		if ( m_sim.PlayerNetId( PlayerSlot( i ) ) != 0 && in.intent == code && in.intentSeq != m_previous[i].intentSeq )
		{
			out.push_back( { PlayerSlot( i ), in.intentA, in.intentB } );
		}
	}
	return out;
}

std::vector<uint32_t> Context::Containers() const
{
	std::vector<uint32_t> out;
	for ( const Simulation::EntityRef& r : m_sim.Entities() )
	{
		if ( flecs::entity( m_sim.World(), r.entity ).has<Container>() )
		{
			out.push_back( r.netId );
		}
	}
	return out;
}

int Context::ContainerSlots( uint32_t container ) const
{
	flecs::entity e = m_sim.FindEntity( container );
	return e.is_valid() && e.has<Container>() ? int( e.get<Container>().slots ) : 0;
}

uint32_t Context::ContainerItem( uint32_t container, int slot ) const
{
	return slot >= 0 && slot < ContainerSlots( container ) ? m_sim.SlotItem( container, uint8_t( slot ) ) : 0;
}

void Context::PutInContainer( uint32_t container, ItemKindHandle kind )
{
	if ( kind.Valid() == false || container == 0 )
	{
		return;
	}
	SimCommand c;
	c.type = CommandType::SpawnItem;
	c.target = container;
	c.index = uint16_t( kind.index );
	c.mode = kNoSocket;
	c.value = 1;
	Add( c );
}

int Context::FindTemplate( const std::string& name ) const
{
	const auto& templates = m_sim.Map().templates;
	for ( size_t i = 0; i < templates.size(); ++i )
	{
		if ( templates[i].name == name )
		{
			return int( i );
		}
	}
	return -1;
}

std::vector<CarriedItem> Context::CarriedItems( PlayerSlot slot ) const
{
	std::vector<CarriedItem> out;
	uint32_t holder = m_sim.PlayerNetId( slot );
	if ( holder == 0 )
	{
		return out;
	}
	for ( const Simulation::EntityRef& r : m_sim.Entities() )
	{
		const cb::HeldItem* item = flecs::entity( m_sim.World(), r.entity ).try_get<cb::HeldItem>();
		if ( item != nullptr && item->holder == holder )
		{
			SocketHandle socket = item->socket < m_schema.sockets.size() ? SocketHandle{ int( item->socket ) } : SocketHandle{};
			out.push_back( { r.netId, ItemKindHandle{ int( item->kind ) }, item->stowed != 0, socket, item->slot == kNoSlot ? -1 : int( item->slot ),
							 int( StackOf( *item ) ) } );
		}
	}
	return out;
}

int Context::LayerIndex( const std::string& layer ) const
{
	const AnimGraph* graph = m_sim.Graph();
	for ( size_t l = 0; graph != nullptr && l < graph->layers.size(); ++l )
	{
		if ( graph->layers[l].name == layer )
		{
			return int( l );
		}
	}
	return -1;
}

int Context::SlotOfTarget( uint32_t target ) const
{
	if ( target & kSlotTargetBit )
	{
		uint32_t slot = target & ~kSlotTargetBit;
		return slot < uint32_t( kMaxPlayers ) ? int( slot ) : -1;
	}
	return SlotOf( target );
}

void Context::SetCharacter( uint32_t target, CharacterHandle character )
{
	SimCommand c;
	c.type = CommandType::SetCharacter;
	c.target = target;
	c.index = uint16_t( character.Valid() ? character.index : 0 );
	Add( c );
}

void Context::SwapLayer( uint32_t target, AnimPackHandle pack, const std::string& layer )
{
	int l = LayerIndex( layer );
	if ( l < 0 || pack.Valid() == false )
	{
		return;
	}
	int slot = SlotOfTarget( target );
	if ( m_layerWishes != nullptr && slot >= 0 )
	{
		m_layerWishes->mod[slot][l] = uint8_t( pack.index + 1 ); // ResolveLayers sends it
		return;
	}
	SimCommand c;
	c.type = CommandType::SwapLayer;
	c.target = target;
	c.index = uint16_t( l );
	c.value = pack.index + 1;
	Add( c );
}

void Context::RestoreLayer( uint32_t target, const std::string& layer )
{
	int l = LayerIndex( layer );
	if ( l < 0 )
	{
		return;
	}
	int slot = SlotOfTarget( target );
	if ( m_layerWishes != nullptr && slot >= 0 )
	{
		m_layerWishes->mod[slot][l] = 0;
		return;
	}
	SimCommand c;
	c.type = CommandType::SwapLayer;
	c.target = target;
	c.index = uint16_t( l );
	c.value = 0;
	Add( c );
}

void Context::ResolveLayers()
{
	const AnimGraph* graph = m_sim.Graph();
	if ( m_layerWishes == nullptr || graph == nullptr )
	{
		return;
	}
	const auto& packs = m_sim.Packs();
	// The character's layers and the overlays after them, as far as a player's state has room.
	const size_t allLayers = LayerSlots( *graph );
	for ( int slot = 0; slot < kMaxPlayers; ++slot )
	{
		if ( Joining( PlayerSlot( slot ) ) || Leaving( PlayerSlot( slot ) ) )
		{
			// A new player starts on its own layers.
			for ( uint8_t& wish : m_layerWishes->mod[slot] )
			{
				wish = 0;
			}
		}
		uint32_t netId = m_sim.PlayerNetId( PlayerSlot( slot ) );
		AnimStateCopy anim = netId != 0 ? m_sim.EntityAnimState( netId ) : AnimStateCopy();
		if ( anim == nullptr )
		{
			continue;
		}
		const size_t layers = std::min( allLayers, anim->graph.size() );
		// What its held items bring: the first socket's item that has that layer wins.
		const size_t ownLayers = graph->layers.size();
		std::vector<uint8_t> fromItems( std::max( layers, ownLayers ) );
		for ( size_t socket = 0; m_itemLayers != nullptr && socket < m_schema.sockets.size(); ++socket )
		{
			uint32_t item = m_sim.HeldItemOf( netId, uint32_t( socket ) );
			auto brings = item != 0 ? m_itemLayers->find( ItemKindOf( item ).index ) : m_itemLayers->end();
			if ( brings == m_itemLayers->end() || size_t( brings->second ) >= packs.size() || packs[size_t( brings->second )] == nullptr )
			{
				continue;
			}
			for ( const AnimGraphLayer& packLayer : packs[size_t( brings->second )]->layers )
			{
				int l = LayerIndex( packLayer.name );
				if ( l >= 0 && fromItems[l] == 0 )
				{
					fromItems[l] = uint8_t( brings->second + 1 );
				}
			}
		}
		// Overlays: every item it carries, in the hand or put away, whose pack has layers the
		// character does not have (a dash, hanging on a line), a slot for each, in NetId order.
		size_t overlay = ownLayers;
		for ( uint32_t item : m_sim.CarriedItemsOf( netId ) )
		{
			auto brings = m_itemLayers != nullptr ? m_itemLayers->find( ItemKindOf( item ).index ) : m_itemLayers->end();
			if ( m_itemLayers == nullptr || brings == m_itemLayers->end() || size_t( brings->second ) >= packs.size() ||
				 packs[size_t( brings->second )] == nullptr || OverlayLayerOf( *graph, *packs[size_t( brings->second )] ) < 0 )
			{
				continue;
			}
			uint8_t source = uint8_t( brings->second + 1 );
			bool already = false;
			for ( size_t l = ownLayers; l < overlay; ++l )
			{
				already |= fromItems[l] == source;
			}
			for ( int n = already ? 0 : OverlayCount( *graph, *packs[size_t( brings->second )] ); n > 0 && overlay < layers; --n )
			{
				fromItems[overlay++] = source;
			}
		}
		// A mod's wish first, then the item's; a command only where what plays would change.
		for ( size_t l = 0; l < layers; ++l )
		{
			uint8_t wish = l < m_layerWishes->mod[slot].size() ? m_layerWishes->mod[slot][l] : 0;
			uint8_t wanted = wish != 0 ? wish : fromItems[l];
			if ( anim->graph[l].source != wanted )
			{
				SimCommand c;
				c.type = CommandType::SwapLayer;
				c.target = SlotTarget( PlayerSlot( slot ) );
				c.index = uint16_t( l );
				c.value = wanted;
				Add( c );
			}
		}
	}
}

uint32_t Context::HeldItem( PlayerSlot slot, SocketHandle socket ) const
{
	uint32_t holder = m_sim.PlayerNetId( slot );
	return holder != 0 && socket.Valid() ? m_sim.HeldItemOf( holder, uint32_t( socket.index ) ) : 0;
}

namespace
{

const cb::HeldItem* ItemOf( Simulation& sim, uint32_t netId )
{
	flecs::entity e = sim.FindEntity( netId );
	return e.is_valid() ? e.try_get<cb::HeldItem>() : nullptr;
}

// A command's rotation: a unit quaternion's x, y, z with w >= 0 (the simulation rebuilds w).
Float3 CommandRotation( b3Quat q )
{
	float length = std::sqrt( q.v.x * q.v.x + q.v.y * q.v.y + q.v.z * q.v.z + q.s * q.s );
	float sign = q.s < 0.0f ? -1.0f : 1.0f;
	if ( ( length > 0.0f ) == false )
	{
		return {};
	}
	return { sign * q.v.x / length, sign * q.v.y / length, sign * q.v.z / length };
}

} // namespace

ItemKindHandle Context::ItemKindOf( uint32_t netId ) const
{
	const cb::HeldItem* item = ItemOf( m_sim, netId );
	return item != nullptr ? ItemKindHandle{ int( item->kind ) } : ItemKindHandle{};
}

uint32_t Context::ItemHolder( uint32_t netId ) const
{
	const cb::HeldItem* item = ItemOf( m_sim, netId );
	return item != nullptr ? item->holder : 0;
}

uint32_t Context::ThrownBy( uint32_t item ) const
{
	return m_sim.LaunchedBy( item );
}

std::vector<Context::ItemHit> Context::Hits( ItemKindHandle kind ) const
{
	std::vector<ItemHit> out;
	const SimGlobals& globals = m_sim.Globals();
	if ( kind.Valid() == false || m_sim.Tick() == 0 )
	{
		return out;
	}
	// The ring's newest, as far back as the tick that just ran.
	uint32_t kept = std::min( globals.impactCount, kImpactHistory );
	for ( uint32_t back = 0; back < kept; ++back )
	{
		const ImpactRecord& impact = globals.impacts[( globals.impactCount - 1 - back ) % kImpactHistory];
		if ( impact.tick + 1 != m_sim.Tick() )
		{
			break;
		}
		for ( int side = 0; side < 2; ++side )
		{
			uint32_t item = side == 0 ? impact.netIdA : impact.netIdB;
			uint32_t other = side == 0 ? impact.netIdB : impact.netIdA;
			if ( ItemKindOf( item ).index == kind.index && ItemHolder( item ) == 0 )
			{
				out.push_back( { item, other, impact.speed, impact.point } );
			}
		}
	}
	std::reverse( out.begin(), out.end() );
	return out;
}

std::vector<uint32_t> Context::Items() const
{
	std::vector<uint32_t> out;
	for ( const Simulation::EntityRef& r : m_sim.Entities() )
	{
		if ( flecs::entity( m_sim.World(), r.entity ).has<cb::HeldItem>() )
		{
			out.push_back( r.netId );
		}
	}
	return out;
}

std::vector<NearBody> Context::BodiesNear( b3Vec3 point, float radius ) const
{
	std::vector<NearBody> out;
	for ( const Simulation::EntityRef& r : m_sim.Entities() )
	{
		const Transform* at = m_sim.EntityTransform( r.netId );
		if ( at == nullptr || IsDynamic( r.netId ) == false )
		{
			continue;
		}
		float distance = b3Length( b3Sub( at->position, point ) );
		if ( distance <= radius )
		{
			flecs::entity e( m_sim.World(), r.entity );
			const Shape* shape = e.try_get<Shape>();
			float size = shape == nullptr ? 0.25f : shape->kind == ShapeKind::Sphere ? shape->halfExtents.x : b3Length( shape->halfExtents );
			const Ragdoll* ragdoll = e.try_get<Ragdoll>();
			out.push_back( { r.netId, at->position, distance, size, ragdoll != nullptr ? std::max<uint32_t>( ragdoll->spawnTick, 1 ) : 0u } );
		}
	}
	std::stable_sort( out.begin(), out.end(), []( const NearBody& a, const NearBody& b ) { return a.distance < b.distance; } );
	return out;
}

std::vector<WorldItem> Context::ItemsNear( b3Vec3 point, float radius ) const
{
	std::vector<WorldItem> out;
	for ( const Simulation::EntityRef& r : m_sim.Entities() )
	{
		flecs::entity e( m_sim.World(), r.entity );
		const cb::HeldItem* item = e.try_get<cb::HeldItem>();
		if ( item == nullptr || item->holder != 0 || e.has<Transform>() == false )
		{
			continue;
		}
		b3Vec3 at = e.get<Transform>().position;
		float distance = b3Length( b3Sub( at, point ) );
		if ( distance <= radius )
		{
			out.push_back( { r.netId, ItemKindHandle{ int( item->kind ) }, at, distance } );
		}
	}
	std::stable_sort( out.begin(), out.end(), []( const WorldItem& a, const WorldItem& b ) { return a.distance < b.distance; } );
	return out;
}

void Context::SpawnWorldItem( ItemKindHandle kind, b3Vec3 grip, b3Quat rotation, b3Vec3 velocity )
{
	if ( kind.Valid() == false )
	{
		return;
	}
	SimCommand c;
	c.type = CommandType::SpawnItem;
	c.target = 0;
	c.index = uint16_t( kind.index );
	c.a = { grip.x, grip.y, grip.z };
	c.b = { velocity.x, velocity.y, velocity.z };
	c.c = CommandRotation( rotation );
	Add( c );
}

void Context::DropItem( uint32_t item, b3Vec3 grip, b3Quat rotation, b3Vec3 velocity )
{
	SimCommand c;
	c.type = CommandType::DropItem;
	c.target = item;
	c.a = { grip.x, grip.y, grip.z };
	c.b = { velocity.x, velocity.y, velocity.z };
	c.c = CommandRotation( rotation );
	Add( c );
}

void Context::PickUpItem( uint32_t holder, uint32_t item, SocketHandle socket )
{
	if ( socket.Valid() == false )
	{
		return;
	}
	SimCommand c;
	c.type = CommandType::PickUpItem;
	c.target = holder;
	c.other = item;
	c.mode = uint8_t( socket.index );
	Add( c );
}

void Context::PickUpStowed( uint32_t holder, uint32_t item, SocketHandle holster )
{
	SimCommand c;
	c.type = CommandType::PickUpItem;
	c.target = holder;
	c.other = item;
	c.mode = holster.Valid() ? uint8_t( holster.index ) : kNoSocket;
	c.value = 1;
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
