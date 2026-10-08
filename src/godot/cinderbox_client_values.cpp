#include "cinderbox_client_parts.h"

namespace cb::gd
{

// --- Mod data: fields and conditions ------------------------------------------------

const Blackboard* CinderboxClient::BoardOf( uint32_t netId ) const
{
	if ( !m_mirror )
	{
		return nullptr;
	}
	flecs::entity ve = m_mirror->VisualOf( netId );
	if ( ve.is_valid() == false )
	{
		return nullptr;
	}
	const present::Visual& v = ve.get<present::Visual>();
	return v.hasBoard ? &v.board : nullptr;
}

std::vector<std::string> CinderboxClient::Conditions( const PackedStringArray& conditions ) const
{
	std::vector<std::string> out;
	out.reserve( size_t( conditions.size() ) );
	for ( int64_t i = 0; i < conditions.size(); ++i )
	{
		out.push_back( ToStd( conditions[i] ) );
	}
	return out;
}

Ref<PackedScene> CinderboxClient::LoadScene( const String& path )
{
	std::string key = "scene:" + ToStd( path );
	auto it = m_prefabs.find( key );
	if ( it != m_prefabs.end() )
	{
		return it->second;
	}
	Ref<PackedScene> scene;
	if ( ResourceLoader::get_singleton()->exists( path ) )
	{
		scene = ResourceLoader::get_singleton()->load( path, "PackedScene" );
	}
	if ( scene.is_null() )
	{
		UtilityFunctions::push_warning( "Cinderbox: missing scene ", path );
	}
	m_prefabs[key] = scene;
	return scene;
}

Array CinderboxClient::get_actions() const
{
	Array out;
	for ( const ModAction& a : m_frame.schema.actions )
	{
		Dictionary d;
		d["name"] = String( a.name.c_str() );
		d["bit"] = int64_t( a.bit );
		d["key"] = String( a.key.c_str() );
		out.push_back( d );
	}
	return out;
}

PackedStringArray CinderboxClient::get_mod_names() const
{
	PackedStringArray out;
	for ( const std::string& name : m_frame.schema.mods )
	{
		out.push_back( String( name.c_str() ) );
	}
	return out;
}

Variant CinderboxClient::get_field( int64_t net_id, const String& name ) const
{
	const BoardValues* globals = m_mirror ? &m_mirror->GlobalBoard() : nullptr;
	present::FieldValue value =
		present::ReadField( m_frame.schema, ToStd( name ), BoardOf( uint32_t( net_id ) ), globals, PrivatesOf( uint32_t( net_id ) ) );
	if ( value.declared == false )
	{
		return Variant();
	}
	switch ( value.type )
	{
		case BoardType::Float:
			return value.AsFloat();
		case BoardType::Bool:
			return value.AsBool();
		case BoardType::Int:
		default:
			return int64_t( value.raw );
	}
}

Variant CinderboxClient::get_local_field( const String& name ) const
{
	return get_field( get_local_net_id(), name );
}

void CinderboxClient::RefreshHeldKinds()
{
	m_heldKinds.clear();
	if ( !m_mirror )
	{
		return;
	}
	m_mirror->ForEach( [&]( uint64_t, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		// In use only: a holstered pistol does not show its HUD.
		if ( v.kind == present::VisualKind::Item && v.holder != 0 && v.stowed == false )
		{
			m_heldKinds[v.holder].push_back( v.itemKind );
		}
	} );
}

bool CinderboxClient::Holds( uint32_t netId, uint16_t kind ) const
{
	auto it = m_heldKinds.find( netId );
	return it != m_heldKinds.end() && std::find( it->second.begin(), it->second.end(), kind ) != it->second.end();
}

bool CinderboxClient::check_conditions( int64_t net_id, const PackedStringArray& conditions ) const
{
	return CheckWith( net_id, conditions, Dictionary() );
}

// A name of the asker's own, or one of the viewer's own ("ui."): true with its value.
bool CinderboxClient::OwnName( const std::string& name, const Dictionary& extra, float& value ) const
{
	String key = String::utf8( name.c_str() );
	if ( extra.has( key ) )
	{
		value = float( double( extra[key] ) );
		return true;
	}
	if ( name.rfind( "ui.", 0 ) == 0 )
	{
		auto found = m_localValues.find( name );
		value = found != m_localValues.end() ? float( found->second ) : 0.0f;
		return true;
	}
	return false;
}

bool CinderboxClient::CheckWith( int64_t net_id, const PackedStringArray& conditions, const Dictionary& extra ) const
{
	const BoardValues* globals = m_mirror ? &m_mirror->GlobalBoard() : nullptr;
	// Item kinds are names too: "pistol.gun" holds while the player holds one.
	present::ExtraFields held = [&]( const std::string& name, float& value ) {
		if ( OwnName( name, extra, value ) )
		{
			return true;
		}
		// Whose it is: a row of a CbList asks whether it is the viewer's own.
		if ( name == "is_local" )
		{
			value = net_id != 0 && net_id == get_local_net_id() ? 1.0f : 0.0f;
			return true;
		}
		int kind = m_frame.schema.FindItemKind( name );
		if ( kind < 0 )
		{
			return false;
		}
		value = Holds( uint32_t( net_id ), uint16_t( kind ) ) ? 1.0f : 0.0f;
		return true;
	};
	for ( const std::string& condition : Conditions( conditions ) )
	{
		if ( present::CheckCondition( m_frame.schema, condition, BoardOf( uint32_t( net_id ) ), globals, &held, PrivatesOf( uint32_t( net_id ) ) ) ==
			 false )
		{
			return false;
		}
	}
	return true;
}

bool CinderboxClient::check_local_conditions( const PackedStringArray& conditions ) const
{
	return check_conditions( get_local_net_id(), conditions );
}

// The value of an expression over an entity's fields ("combat.health * 100 / combat.max_health").
// Null when it does not parse, or when it reads nothing this server declares (so a HUD node for a
// mod that is not running leaves its target alone).
Variant CinderboxClient::evaluate( int64_t net_id, const String& expression ) const
{
	return EvaluateWith( net_id, expression, Dictionary() );
}

Variant CinderboxClient::EvaluateWith( int64_t net_id, const String& expression, const Dictionary& extra ) const
{
	const BoardValues* globals = m_mirror ? &m_mirror->GlobalBoard() : nullptr;
	bool known = false;
	present::ExtraFields names = [&]( const std::string& name, float& value ) {
		if ( OwnName( name, extra, value ) )
		{
			known = true;
			return true;
		}
		if ( name == "is_local" )
		{
			known = true;
			value = net_id != 0 && net_id == get_local_net_id() ? 1.0f : 0.0f;
			return true;
		}
		int kind = m_frame.schema.FindItemKind( name );
		known |= kind >= 0 || m_frame.schema.FindField( name ) != nullptr;
		if ( kind < 0 )
		{
			return false;
		}
		value = Holds( uint32_t( net_id ), uint16_t( kind ) ) ? 1.0f : 0.0f;
		return true;
	};
	float value = 0.0f;
	if ( present::EvaluateFields( m_frame.schema, ToStd( expression ), BoardOf( uint32_t( net_id ) ), globals, value, &names,
								  PrivatesOf( uint32_t( net_id ) ) ) == false ||
		 known == false )
	{
		return Variant();
	}
	return double( value );
}

Variant CinderboxClient::evaluate_local( const String& expression ) const
{
	return evaluate( get_local_net_id(), expression );
}

String CinderboxClient::format_local_fields( const String& format ) const
{
	return format_fields( get_local_net_id(), format );
}

// "{name:deathmatch.winner}": the field holds a player's NetId; show that player's name.
String CinderboxClient::ResolveNameFields( int64_t net_id, const String& format ) const
{
	String out = format;
	int64_t at = out.find( "{name:" );
	while ( at >= 0 )
	{
		int64_t close = out.find( "}", at );
		if ( close < 0 )
		{
			break;
		}
		String field = out.substr( at + 6, close - at - 6 );
		Variant id = get_field( net_id, field );
		String name = id.get_type() == Variant::NIL || int64_t( id ) == 0 ? String() : get_player_name( int64_t( id ) );
		out = out.substr( 0, at ) + name.replace( "{", "(" ) + out.substr( close + 1 );
		at = out.find( "{name:", at + name.length() );
	}
	return out;
}

int64_t CinderboxClient::get_local_net_id() const
{
	return m_haveFrame ? int64_t( m_frame.frame.localNetId ) : 0;
}

bool CinderboxClient::is_local_player_dead() const
{
	if ( !m_mirror )
	{
		return false;
	}
	flecs::entity ve = m_mirror->VisualOf( uint32_t( get_local_net_id() ) );
	return ve.is_valid() && ve.get<present::Visual>().dead;
}

double CinderboxClient::get_tick_time() const
{
	// Drawn `alpha` of the way from the frame before (stride ticks back) to this one.
	return m_haveFrame ? double( m_frame.frame.tick ) + 1.0 + ( double( m_alpha ) - 1.0 ) * double( m_frame.stride ) : 0.0;
}

double CinderboxClient::get_tick_rate() const
{
	return m_haveFrame && m_frame.frame.tickSeconds > 0.0f ? 1.0 / double( m_frame.frame.tickSeconds ) : 60.0;
}

Vector3 CinderboxClient::get_camera_target() const
{
	if ( !m_mirror )
	{
		return Vector3( 0, 1, 0 );
	}
	if ( is_local_player_dead() )
	{
		// Watch the body fall.
		flecs::entity body = m_mirror->RagdollOf( uint32_t( get_local_net_id() ) );
		if ( body.is_valid() )
		{
			if ( const present::RagdollAnim* ra = body.try_get<present::RagdollAnim>() )
			{
				return ToGodot( ra->current[0].position ) + Vector3( 0, 0.3f, 0 );
			}
		}
	}
	present::RenderPose pose;
	if ( m_mirror->LocalPlayer( pose ) )
	{
		// The point the server takes for a third-person camera's pivot (Context::EyePosition).
		return ToGodot( pose.position ) + Vector3( 0, kViewPivotHeight, 0 );
	}
	return Vector3( 0, 1, 0 );
}

Vector3 CinderboxClient::get_view_position( int64_t view, const Basis& camera ) const
{
	Vector3 pivot = get_camera_target();
	switch ( ViewMode( view >= 0 && view < int64_t( kViewModes ) ? view : 0 ) )
	{
		case ViewMode::FirstPerson:
		{
			present::RenderPose pose;
			if ( m_mirror && m_animSet && is_local_player_dead() == false && m_mirror->LocalPlayer( pose ) )
			{
				// The character's eye height above its feet, as Context::ViewPosition places it: on
				// the mover, so no animation moves the camera.
				Vector3 feet = ToGodot( pose.position ) - Vector3( 0, ragdoll::kFeetBelowCenter, 0 );
				return feet + Vector3( 0, anim::EyeHeight( *m_animSet ), 0 ) - camera.get_column( 2 ) * kEyeAhead;
			}
			return pivot;
		}
		case ViewMode::ShoulderRight:
			return pivot + camera.get_column( 0 ) * kShoulderOffset;
		case ViewMode::ShoulderLeft:
			return pivot - camera.get_column( 0 ) * kShoulderOffset;
		case ViewMode::ThirdPerson:
		default:
			return pivot;
	}
}

double CinderboxClient::get_camera_distance( const Vector3& target, const Vector3& direction, double max_distance, double radius ) const
{
	Vector3 d = direction.normalized();
	if ( m_haveFrame == false || m_frame.hasWorld == false || d.is_zero_approx() )
	{
		return max_distance;
	}
	return present::CameraFreeDistance( m_frame.frame, { target.x, target.y, target.z }, { d.x, d.y, d.z }, float( max_distance ),
										float( radius ) );
}

Node3D* CinderboxClient::get_entity_node( int64_t net_id ) const
{
	if ( !m_mirror )
	{
		return nullptr;
	}
	flecs::entity ve = m_mirror->VisualOf( uint32_t( net_id ) );
	return ve.is_valid() ? get_visual_node( int64_t( ve.id() ) ) : nullptr;
}

Vector3 CinderboxClient::get_bone_position( int64_t net_id, const String& bone ) const
{
	Node3D* node = get_entity_node( net_id );
	if ( node == nullptr )
	{
		return Vector3();
	}
	if ( CinderboxSkeleton* skeleton = FindSkeleton( node ) )
	{
		Transform3D joint;
		if ( bone.is_empty() == false && skeleton->JointTransform( bone, joint ) )
		{
			return ( skeleton->get_global_transform() * joint ).origin;
		}
	}
	return node->get_global_position();
}

int CinderboxClient::SlotOfNetId( uint32_t netId ) const
{
	if ( !m_mirror )
	{
		return -1;
	}
	flecs::entity ve = m_mirror->VisualOf( netId );
	if ( ve.is_valid() == false )
	{
		return -1;
	}
	const present::Visual& v = ve.get<present::Visual>();
	bool person = v.kind == present::VisualKind::Player || v.kind == present::VisualKind::Ragdoll;
	return person ? int( v.slot ) : -1;
}

PackedInt64Array CinderboxClient::get_players() const
{
	PackedInt64Array out;
	if ( !m_mirror )
	{
		return out;
	}
	m_mirror->ForEach( [&]( uint64_t, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		if ( v.kind == present::VisualKind::Player )
		{
			out.push_back( int64_t( v.netId ) );
		}
	} );
	return out;
}

String CinderboxClient::get_player_name( int64_t net_id ) const
{
	int slot = SlotOfNetId( uint32_t( net_id ) );
	if ( slot < 0 )
	{
		return String();
	}
	const std::string& name = m_frame.names[size_t( slot )];
	return name.empty() ? String( "Player " ) + String::num_int64( slot + 1 ) : String::utf8( name.c_str() );
}

PackedInt64Array CinderboxClient::get_items( const String& kind, int64_t holder ) const
{
	PackedInt64Array out;
	int wanted = kind.is_empty() ? -1 : m_frame.schema.FindItemKind( ToStd( kind ) );
	if ( !m_mirror || ( kind.is_empty() == false && wanted < 0 ) )
	{
		return out;
	}
	m_mirror->ForEach( [&]( uint64_t, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		if ( v.kind == present::VisualKind::Item && ( wanted < 0 || int( v.itemKind ) == wanted ) &&
			 ( holder == 0 || int64_t( v.holder ) == holder ) )
		{
			out.push_back( int64_t( v.netId ) );
		}
	} );
	return out;
}

String CinderboxClient::get_entity_name( int64_t net_id ) const
{
	flecs::entity ve = m_mirror ? m_mirror->VisualOf( uint32_t( net_id ) ) : flecs::entity();
	if ( ve.is_valid() )
	{
		const present::Visual& v = ve.get<present::Visual>();
		if ( v.kind == present::VisualKind::Item && v.itemKind < m_frame.schema.itemKinds.size() )
		{
			const std::string& kind = m_frame.schema.itemKinds[v.itemKind];
			auto found = m_itemNames.find( kind );
			return found != m_itemNames.end() && found->second.is_empty() == false ? found->second : String::utf8( kind.c_str() );
		}
	}
	return get_player_name( net_id );
}

String CinderboxClient::ResolveKeysAndLooks( int64_t net_id, const String& format ) const
{
	String out = format;
	// {key:action}: what the player pressed to do it, as bound now (rebinding shows).
	for ( int64_t at = out.find( "{key:" ); at >= 0; at = out.find( "{key:", at + 1 ) )
	{
		int64_t close = out.find( "}", at );
		if ( close < 0 )
		{
			break;
		}
		String action = "cb_" + out.substr( at + 5, close - at - 5 );
		String key = "?";
		InputMap* map = InputMap::get_singleton();
		if ( map->has_action( action ) )
		{
			TypedArray<InputEvent> events = map->action_get_events( action );
			Ref<InputEvent> first = events.size() > 0 ? Ref<InputEvent>( events[0] ) : Ref<InputEvent>();
			if ( Ref<InputEventKey> k = first; k.is_valid() )
			{
				Key code = k->get_physical_keycode() != KEY_NONE ? k->get_physical_keycode() : k->get_keycode();
				key = OS::get_singleton()->get_keycode_string( code );
			}
			else if ( Ref<InputEventMouseButton> m = first; m.is_valid() )
			{
				MouseButton button = m->get_button_index();
				key = button == MOUSE_BUTTON_LEFT ? "LMB" : button == MOUSE_BUTTON_RIGHT ? "RMB" : button == MOUSE_BUTTON_MIDDLE ? "MMB" : "Mouse";
			}
			else if ( first.is_valid() )
			{
				key = first->as_text();
			}
		}
		out = out.substr( 0, at ) + key + out.substr( close + 1 );
	}
	// {look:field}: the field holds an entity's NetId; what is it called?
	for ( int64_t at = out.find( "{look:" ); at >= 0; at = out.find( "{look:", at + 1 ) )
	{
		int64_t close = out.find( "}", at );
		if ( close < 0 )
		{
			break;
		}
		Variant id = get_field( net_id, out.substr( at + 6, close - at - 6 ) );
		String name;
		flecs::entity ve = m_mirror && id.get_type() != Variant::NIL ? m_mirror->VisualOf( uint32_t( int64_t( id ) ) ) : flecs::entity();
		if ( ve.is_valid() )
		{
			const present::Visual& target = ve.get<present::Visual>();
			if ( target.kind == present::VisualKind::Item && target.itemKind < m_frame.schema.itemKinds.size() )
			{
				const std::string& kind = m_frame.schema.itemKinds[target.itemKind];
				auto found = m_itemNames.find( kind );
				name = found != m_itemNames.end() && found->second.is_empty() == false ? found->second : String::utf8( kind.c_str() );
			}
			else if ( target.kind == present::VisualKind::Player )
			{
				name = get_player_name( int64_t( id ) );
			}
		}
		out = out.substr( 0, at ) + name.replace( "{", "(" ) + out.substr( close + 1 );
	}
	return out;
}

String CinderboxClient::format_fields( int64_t net_id, const String& format ) const
{
	return FormatWith( net_id, format, Dictionary() );
}

String CinderboxClient::FormatWith( int64_t net_id, const String& format, const Dictionary& extra ) const
{
	// The asker's own names and the viewer's own values, first: "{slot.number}", "{ui.picked}".
	String own = format;
	Array keys = extra.keys();
	for ( int64_t i = 0; i < keys.size(); ++i )
	{
		if ( extra[keys[i]].get_type() == Variant::STRING )
		{
			// Words of the asker's own (a name in a row of events): as they are, and never read again
			// as a field's name.
			own = own.replace( "{" + String( keys[i] ) + "}", String( extra[keys[i]] ).replace( "{", "(" ) );
			continue;
		}
		double v = double( extra[keys[i]] );
		own = own.replace( "{" + String( keys[i] ) + "}", v == std::floor( v ) ? String::num_int64( int64_t( v ) ) : String::num( v, 1 ) );
	}
	for ( const auto& [name, v] : m_localValues )
	{
		own = own.replace( "{" + String::utf8( name.c_str() ) + "}", v == std::floor( v ) ? String::num_int64( int64_t( v ) ) : String::num( v, 1 ) );
	}
	String withName = ResolveNameFields(
		net_id, ResolveKeysAndLooks( net_id, own ).replace( "{name}", get_entity_name( net_id ).replace( "{", "(" ) ) );
	const BoardValues* globals = m_mirror ? &m_mirror->GlobalBoard() : nullptr;
	std::string text =
		present::FormatFields( m_frame.schema, ToStd( withName ), BoardOf( uint32_t( net_id ) ), globals, PrivatesOf( uint32_t( net_id ) ) );
	return String::utf8( text.c_str() );
}

Array CinderboxClient::get_required_items() const
{
	Array out;
	for ( const ModItem& item : m_frame.schema.items )
	{
		Dictionary d;
		d["mod"] = String( item.mod.c_str() );
		d["sha256"] = String( item.sha256.c_str() );
		out.push_back( d );
	}
	return out;
}

String CinderboxClient::get_kind( int64_t net_id ) const
{
	if ( !m_mirror )
	{
		return String();
	}
	flecs::entity ve = m_mirror->VisualOf( uint32_t( net_id ) );
	return ve.is_valid() ? String( KindName( ve.get<present::Visual>().kind ) ) : String();
}

String CinderboxClient::get_entity_template_name( int64_t net_id ) const
{
	if ( !m_mirror )
	{
		return String();
	}
	flecs::entity ve = m_mirror->VisualOf( uint32_t( net_id ) );
	return ve.is_valid() ? TemplateName( ve.get<present::Visual>().templateIndex ) : String();
}

} // namespace cb::gd
