#include "cinderbox_hud.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>

#include "cinderbox_client.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_map.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>

using namespace godot;

namespace cb::gd
{

namespace
{

bool InGame()
{
	return Engine::get_singleton()->is_editor_hint() == false;
}

double Now()
{
	return double( Time::get_singleton()->get_ticks_msec() ) / 1000.0;
}

// On a CbList's row: the entity it is for, and its place in the list. (Names made on first use: a
// StringName cannot be made while the library loads.)
const StringName& SubjectMeta()
{
	static const StringName name = "cb_subject";
	return name;
}

const StringName& RankMeta()
{
	static const StringName name = "cb_rank";
	return name;
}

} // namespace

CinderboxClient* FindClient( Node* from, ObjectID& cache )
{
	if ( auto* client = Object::cast_to<CinderboxClient>( ObjectDB::get_instance( cache ) ) )
	{
		return client;
	}
	if ( from->is_inside_tree() == false )
	{
		return nullptr;
	}
	auto* client = Object::cast_to<CinderboxClient>( from->get_tree()->get_first_node_in_group( "cinderbox_client" ) );
	cache = client != nullptr ? client->get_instance_id() : ObjectID();
	return client;
}

int64_t SubjectOf( const Node* node, const CinderboxClient* client, int* rank )
{
	for ( const Node* at = node; at != nullptr; at = at->get_parent() )
	{
		if ( at->has_meta( SubjectMeta() ) )
		{
			if ( rank != nullptr )
			{
				*rank = int( at->get_meta( RankMeta(), 0 ) );
			}
			return int64_t( at->get_meta( SubjectMeta() ) );
		}
	}
	if ( rank != nullptr )
	{
		*rank = 0;
	}
	return client != nullptr ? client->get_local_net_id() : 0;
}

// --- CbPromptLabel ---------------------------------------------------------------------------------

CbPromptLabel::CbPromptLabel()
{
	// Readable from anywhere: facing the camera, the same size on screen, never hidden by the thing
	// it is about.
	set_billboard_mode( BaseMaterial3D::BILLBOARD_ENABLED );
	set_draw_flag( FLAG_FIXED_SIZE, true );
	set_draw_flag( FLAG_DISABLE_DEPTH_TEST, true );
	set_pixel_size( 0.0012f );
	set_font_size( 26 );
	set_outline_size( 10 );
}

void CbPromptLabel::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_text_format", "value" ), &CbPromptLabel::set_text_format );
	ClassDB::bind_method( D_METHOD( "get_text_format" ), &CbPromptLabel::get_text_format );
	ClassDB::bind_method( D_METHOD( "set_height", "value" ), &CbPromptLabel::set_height );
	ClassDB::bind_method( D_METHOD( "get_height" ), &CbPromptLabel::get_height );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "text_format", PROPERTY_HINT_MULTILINE_TEXT ), "set_text_format", "get_text_format" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "0,3,0.01,suffix:m" ), "set_height", "get_height" );
	ClassDB::bind_method( D_METHOD( "set_progress_field", "value" ), &CbPromptLabel::set_progress_field );
	ClassDB::bind_method( D_METHOD( "get_progress_field" ), &CbPromptLabel::get_progress_field );
	ClassDB::bind_method( D_METHOD( "set_bar_color", "value" ), &CbPromptLabel::set_bar_color );
	ClassDB::bind_method( D_METHOD( "get_bar_color" ), &CbPromptLabel::get_bar_color );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "progress_field", PROPERTY_HINT_PLACEHOLDER_TEXT, "pickup.progress" ),
				  "set_progress_field", "get_progress_field" );
	ClassDB::bind_method( D_METHOD( "set_since_field", "value" ), &CbPromptLabel::set_since_field );
	ClassDB::bind_method( D_METHOD( "get_since_field" ), &CbPromptLabel::get_since_field );
	ClassDB::bind_method( D_METHOD( "set_duration_field", "value" ), &CbPromptLabel::set_duration_field );
	ClassDB::bind_method( D_METHOD( "get_duration_field" ), &CbPromptLabel::get_duration_field );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "since_field", PROPERTY_HINT_PLACEHOLDER_TEXT, "pickup.since" ), "set_since_field",
				  "get_since_field" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "duration_field", PROPERTY_HINT_PLACEHOLDER_TEXT, "pickup.hold" ), "set_duration_field",
				  "get_duration_field" );
	ADD_PROPERTY( PropertyInfo( Variant::COLOR, "bar_color" ), "set_bar_color", "get_bar_color" );
}

namespace
{

// The bar is drawn like the text: facing the camera, the same size at any distance, over everything.
// Sizes are in the label's units (what a metre away looks like).
constexpr float kBarWidth = 0.11f;
constexpr float kBarHeight = 0.009f;
constexpr float kBarDrop = -0.03f; // under the text

Ref<StandardMaterial3D> BarMaterial( const Color& color, int priority )
{
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_shading_mode( BaseMaterial3D::SHADING_MODE_UNSHADED );
	material->set_transparency( BaseMaterial3D::TRANSPARENCY_ALPHA );
	material->set_billboard_mode( BaseMaterial3D::BILLBOARD_ENABLED );
	material->set_flag( BaseMaterial3D::FLAG_BILLBOARD_KEEP_SCALE, true );
	material->set_flag( BaseMaterial3D::FLAG_FIXED_SIZE, true );
	material->set_flag( BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true );
	material->set_albedo( color );
	material->set_render_priority( priority );
	return material;
}

} // namespace

void CbPromptLabel::ShowBar( float progress )
{
	if ( m_barBack == nullptr )
	{
		Ref<QuadMesh> back;
		back.instantiate();
		back->set_size( Vector2( kBarWidth + 0.006f, kBarHeight + 0.006f ) );
		back->set_center_offset( Vector3( 0, kBarDrop, 0 ) );
		m_barBack = memnew( MeshInstance3D );
		m_barBack->set_mesh( back );
		m_barBack->set_material_override( BarMaterial( Color( 0, 0, 0, 0.55f ), 1 ) );
		add_child( m_barBack );
		m_fillMesh.instantiate();
		m_barFill = memnew( MeshInstance3D );
		m_barFill->set_mesh( m_fillMesh );
		m_barFill->set_material_override( BarMaterial( m_barColor, 2 ) );
		add_child( m_barFill );
	}
	bool show = progress > 0.0f;
	m_barBack->set_visible( show );
	m_barFill->set_visible( show );
	if ( show )
	{
		// From the left edge: wider, and moved half its width to the right.
		float width = kBarWidth * std::min( progress, 1.0f );
		m_fillMesh->set_size( Vector2( width, kBarHeight ) );
		m_fillMesh->set_center_offset( Vector3( -0.5f * kBarWidth + 0.5f * width, kBarDrop, 0 ) );
	}
}

void CbPromptLabel::_ready()
{
	set_process( InGame() );
}

void CbPromptLabel::_process( double )
{
	if ( is_queued_for_deletion() )
	{
		return; // leaving: its reaction has moved on to another target this frame
	}
	// Upright above whatever it is about, however that lies (a bat on its side).
	if ( auto* parent = Object::cast_to<Node3D>( get_parent() ) )
	{
		set_global_transform( Transform3D( Basis(), parent->get_global_position() + Vector3( 0, float( m_height ), 0 ) ) );
	}
	CinderboxClient* client = FindClient( this, m_client );
	String text = client != nullptr && client->has_local_player() ? client->format_local_fields( m_format ).strip_edges() : String();
	set_visible( text.is_empty() == false );
	set_text( text );
	if ( m_sinceField.is_empty() == false && m_durationField.is_empty() == false && client != nullptr )
	{
		Variant since = client->get_local_field( m_sinceField );
		Variant duration = client->get_local_field( m_durationField );
		double began = since.get_type() == Variant::NIL ? 0.0 : double( since );
		double ticks = duration.get_type() == Variant::NIL ? 0.0 : double( duration ) * client->get_tick_rate();
		ShowBar( began > 0.0 && ticks > 0.0 ? float( std::clamp( ( client->get_tick_time() - began ) / ticks, 0.001, 1.0 ) ) : 0.0f );
	}
	else if ( m_progressField.is_empty() == false && client != nullptr )
	{
		Variant value = client->get_local_field( m_progressField );
		ShowBar( value.get_type() == Variant::NIL ? 0.0f : float( double( value ) ) );
	}
}

// --- CbFieldLabel ----------------------------------------------------------------------------------

void CbFieldLabel::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_text_format", "value" ), &CbFieldLabel::set_text_format );
	ClassDB::bind_method( D_METHOD( "get_text_format" ), &CbFieldLabel::get_text_format );
	ClassDB::bind_method( D_METHOD( "set_conditions", "value" ), &CbFieldLabel::set_conditions );
	ClassDB::bind_method( D_METHOD( "get_conditions" ), &CbFieldLabel::get_conditions );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "text_format", PROPERTY_HINT_MULTILINE_TEXT ), "set_text_format", "get_text_format" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "conditions" ), "set_conditions", "get_conditions" );
	ClassDB::bind_method( D_METHOD( "set_choice_field", "value" ), &CbFieldLabel::set_choice_field );
	ClassDB::bind_method( D_METHOD( "get_choice_field" ), &CbFieldLabel::get_choice_field );
	ClassDB::bind_method( D_METHOD( "set_choices", "value" ), &CbFieldLabel::set_choices );
	ClassDB::bind_method( D_METHOD( "get_choices" ), &CbFieldLabel::get_choices );
	ADD_GROUP( "Choices", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "choice_field", PROPERTY_HINT_PLACEHOLDER_TEXT, "deathmatch.ending" ), "set_choice_field",
				  "get_choice_field" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "choices" ), "set_choices", "get_choices" );
}

void CbFieldLabel::_ready()
{
	set_process( InGame() );
}

void CbFieldLabel::_process( double )
{
	CinderboxClient* client = FindClient( this, m_client );
	int rank = 0;
	int64_t subject = SubjectOf( this, client, &rank );
	bool show = client != nullptr && client->has_local_player() && client->check_conditions( subject, m_conditions );
	String format = m_format;
	if ( show && m_choiceField.is_empty() == false )
	{
		// The line the server's number picks; none (a mod that is not running, an empty line, a
		// number past the last) and there is nothing to say.
		Variant picked = client->get_field( subject, m_choiceField );
		if ( picked.get_type() == Variant::NIL )
		{
			picked = client->evaluate( subject, m_choiceField );
		}
		int64_t line = picked.get_type() == Variant::NIL ? -1 : int64_t( double( picked ) );
		String choice = line >= 0 && line < m_choices.size() ? m_choices[line] : String();
		show = choice.is_empty() == false;
		format = m_format.is_empty() ? choice : m_format.replace( "{choice}", choice );
	}
	set_visible( show );
	if ( show && format.is_empty() == false )
	{
		set_text( client->format_fields( subject, format.replace( "{rank}", String::num_int64( rank ) ) ) );
	}
}

// --- CbFieldBinding --------------------------------------------------------------------------------

void CbFieldBinding::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_field", "value" ), &CbFieldBinding::set_field );
	ClassDB::bind_method( D_METHOD( "get_field" ), &CbFieldBinding::get_field );
	ClassDB::bind_method( D_METHOD( "set_target", "value" ), &CbFieldBinding::set_target );
	ClassDB::bind_method( D_METHOD( "get_target" ), &CbFieldBinding::get_target );
	ClassDB::bind_method( D_METHOD( "set_property", "value" ), &CbFieldBinding::set_property );
	ClassDB::bind_method( D_METHOD( "get_property" ), &CbFieldBinding::get_property );
	ClassDB::bind_method( D_METHOD( "set_multiply", "value" ), &CbFieldBinding::set_multiply );
	ClassDB::bind_method( D_METHOD( "get_multiply" ), &CbFieldBinding::get_multiply );
	ClassDB::bind_method( D_METHOD( "set_add", "value" ), &CbFieldBinding::set_add );
	ClassDB::bind_method( D_METHOD( "get_add" ), &CbFieldBinding::get_add );
	ClassDB::bind_method( D_METHOD( "set_conditions", "value" ), &CbFieldBinding::set_conditions );
	ClassDB::bind_method( D_METHOD( "get_conditions" ), &CbFieldBinding::get_conditions );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "field" ), "set_field", "get_field" );
	ADD_PROPERTY( PropertyInfo( Variant::NODE_PATH, "target" ), "set_target", "get_target" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "property" ), "set_property", "get_property" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "multiply" ), "set_multiply", "get_multiply" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "add" ), "set_add", "get_add" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "conditions" ), "set_conditions", "get_conditions" );
}

void CbFieldBinding::_ready()
{
	set_process( InGame() );
}

void CbFieldBinding::_process( double )
{
	CinderboxClient* client = FindClient( this, m_client );
	Node* target = get_node_or_null( m_target );
	if ( client == nullptr || target == nullptr )
	{
		return;
	}
	int64_t subject = SubjectOf( this, client );
	if ( m_conditions.is_empty() == false )
	{
		bool holds = client->has_local_player() && client->check_conditions( subject, m_conditions );
		Variant visible = target->get( "visible" );
		if ( visible.get_type() == Variant::BOOL && bool( visible ) != holds )
		{
			target->set( "visible", holds );
		}
		if ( holds == false )
		{
			return;
		}
	}
	if ( m_field.is_empty() || m_property.is_empty() )
	{
		return;
	}
	// A field's name, or an expression over fields ("combat.health * 100 / combat.max_health").
	Variant value = client->get_field( subject, m_field );
	if ( value.get_type() == Variant::NIL )
	{
		value = client->evaluate( subject, m_field );
	}
	if ( value.get_type() == Variant::NIL )
	{
		return; // the server does not run the mod that declares it
	}
	Variant current = target->get( m_property );
	if ( current.get_type() == Variant::BOOL )
	{
		target->set( m_property, double( value ) * m_multiply + m_add != 0.0 );
	}
	else if ( current.get_type() == Variant::INT )
	{
		target->set( m_property, int64_t( double( value ) * m_multiply + m_add ) );
	}
	else
	{
		target->set( m_property, double( value ) * m_multiply + m_add );
	}
}

// --- CbEventFeed -----------------------------------------------------------------------------------

void CbEventFeed::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_event", "value" ), &CbEventFeed::set_event );
	ClassDB::bind_method( D_METHOD( "get_event" ), &CbEventFeed::get_event );
	ClassDB::bind_method( D_METHOD( "set_text_format", "value" ), &CbEventFeed::set_text_format );
	ClassDB::bind_method( D_METHOD( "get_text_format" ), &CbEventFeed::get_text_format );
	ClassDB::bind_method( D_METHOD( "set_nobody_text", "value" ), &CbEventFeed::set_nobody_text );
	ClassDB::bind_method( D_METHOD( "get_nobody_text" ), &CbEventFeed::get_nobody_text );
	ClassDB::bind_method( D_METHOD( "set_max_lines", "value" ), &CbEventFeed::set_max_lines );
	ClassDB::bind_method( D_METHOD( "get_max_lines" ), &CbEventFeed::get_max_lines );
	ClassDB::bind_method( D_METHOD( "set_line_seconds", "value" ), &CbEventFeed::set_line_seconds );
	ClassDB::bind_method( D_METHOD( "get_line_seconds" ), &CbEventFeed::get_line_seconds );
	ClassDB::bind_method( D_METHOD( "set_label_settings", "value" ), &CbEventFeed::set_label_settings );
	ClassDB::bind_method( D_METHOD( "get_label_settings" ), &CbEventFeed::get_label_settings );
	ClassDB::bind_method( D_METHOD( "on_mod_event", "name", "a", "b", "value", "position", "vector" ), &CbEventFeed::on_mod_event );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "event" ), "set_event", "get_event" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "text_format" ), "set_text_format", "get_text_format" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "nobody_text" ), "set_nobody_text", "get_nobody_text" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "max_lines", PROPERTY_HINT_RANGE, "1,20" ), "set_max_lines", "get_max_lines" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "line_seconds", PROPERTY_HINT_RANGE, "0.5,30,0.1" ), "set_line_seconds",
				  "get_line_seconds" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "label_settings", PROPERTY_HINT_RESOURCE_TYPE, "LabelSettings" ),
				  "set_label_settings", "get_label_settings" );
}

void CbEventFeed::_ready()
{
	set_process( InGame() );
}

void CbEventFeed::_process( double )
{
	if ( m_connected == false )
	{
		if ( CinderboxClient* client = FindClient( this, m_client ) )
		{
			client->connect( "mod_event", Callable( this, "on_mod_event" ) );
			m_connected = true;
		}
	}
	// Lines leave oldest first once their time is up.
	double now = Now();
	while ( m_expires.empty() == false && m_expires.front() <= now && get_child_count() > 0 )
	{
		m_expires.erase( m_expires.begin() );
		Node* oldest = get_child( 0 );
		remove_child( oldest );
		oldest->queue_free();
	}
}

void CbEventFeed::on_mod_event( const String& name, int64_t a, int64_t b, int64_t value, const Vector3&, const Vector3& )
{
	if ( name != m_event )
	{
		return;
	}
	CinderboxClient* client = FindClient( this, m_client );
	if ( client == nullptr )
	{
		return;
	}
	auto who = [&]( int64_t id ) {
		String n = id != 0 ? client->get_player_name( id ) : String();
		return n.is_empty() ? m_nobody : n;
	};
	Label* line = memnew( Label );
	line->set_text( m_format.replace( "{a}", who( a ) ).replace( "{b}", who( b ) ).replace( "{value}", String::num_int64( value ) ) );
	if ( m_labelSettings.is_valid() )
	{
		line->set_label_settings( m_labelSettings );
	}
	line->set_horizontal_alignment( HORIZONTAL_ALIGNMENT_RIGHT );
	add_child( line );
	m_expires.push_back( Now() + double( m_lineSeconds ) );
	while ( get_child_count() > m_maxLines )
	{
		Node* oldest = get_child( 0 );
		remove_child( oldest );
		oldest->queue_free();
		m_expires.erase( m_expires.begin() );
	}
}

// --- CbList ----------------------------------------------------------------------------------------

CbList::CbList()
{
	set_vertical( true );
}

void CbList::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_of", "value" ), &CbList::set_of );
	ClassDB::bind_method( D_METHOD( "get_of" ), &CbList::get_of );
	ClassDB::bind_method( D_METHOD( "set_item_kind", "value" ), &CbList::set_item_kind );
	ClassDB::bind_method( D_METHOD( "get_item_kind" ), &CbList::get_item_kind );
	ClassDB::bind_method( D_METHOD( "set_where", "value" ), &CbList::set_where );
	ClassDB::bind_method( D_METHOD( "get_where" ), &CbList::get_where );
	ClassDB::bind_method( D_METHOD( "set_sort_by", "value" ), &CbList::set_sort_by );
	ClassDB::bind_method( D_METHOD( "get_sort_by" ), &CbList::get_sort_by );
	ClassDB::bind_method( D_METHOD( "set_descending", "value" ), &CbList::set_descending );
	ClassDB::bind_method( D_METHOD( "get_descending" ), &CbList::get_descending );
	ClassDB::bind_method( D_METHOD( "set_max_rows", "value" ), &CbList::set_max_rows );
	ClassDB::bind_method( D_METHOD( "get_max_rows" ), &CbList::get_max_rows );
	ClassDB::bind_method( D_METHOD( "set_conditions", "value" ), &CbList::set_conditions );
	ClassDB::bind_method( D_METHOD( "get_conditions" ), &CbList::get_conditions );
	ClassDB::bind_method( D_METHOD( "get_entries" ), &CbList::get_entries );
	ADD_GROUP( "Rows", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "of", PROPERTY_HINT_ENUM, "Players,Items,Items its subject holds" ), "set_of", "get_of" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "item_kind", PROPERTY_HINT_PLACEHOLDER_TEXT, "pistol.gun (empty: any)" ), "set_item_kind",
				  "get_item_kind" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "where" ), "set_where", "get_where" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "sort_by", PROPERTY_HINT_PLACEHOLDER_TEXT, "deathmatch.score" ), "set_sort_by",
				  "get_sort_by" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "descending" ), "set_descending", "get_descending" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "max_rows", PROPERTY_HINT_RANGE, "0,64,1,or_greater" ), "set_max_rows", "get_max_rows" );
	ADD_GROUP( "Shown", "" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "conditions" ), "set_conditions", "get_conditions" );

	BIND_ENUM_CONSTANT( OF_PLAYERS );
	BIND_ENUM_CONSTANT( OF_ITEMS );
	BIND_ENUM_CONSTANT( OF_HELD_ITEMS );
}

Control* CbList::Template() const
{
	for ( int i = 0; i < get_child_count(); ++i )
	{
		if ( auto* control = Object::cast_to<Control>( get_child( i ) ); control != nullptr && control->has_meta( SubjectMeta() ) == false )
		{
			return control;
		}
	}
	return nullptr;
}

PackedStringArray CbList::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	if ( Template() == nullptr )
	{
		warnings.push_back( "Give it a child (any Control): that is one row. In the game it is copied for every entry, and the "
							"CbFieldLabel and CbFieldBinding nodes in it read that entry's fields." );
	}
	return warnings;
}

void CbList::_ready()
{
	set_process( InGame() );
	if ( InGame() )
	{
		// The row as designed is the pattern: hidden, and its own nodes do not run.
		if ( Control* pattern = Template() )
		{
			pattern->set_visible( false );
			pattern->set_process_mode( PROCESS_MODE_DISABLED );
		}
	}
}

PackedInt64Array CbList::get_entries() const
{
	PackedInt64Array out;
	for ( int64_t id : m_entries )
	{
		out.push_back( id );
	}
	return out;
}

void CbList::_process( double )
{
	CinderboxClient* client = FindClient( this, m_client );
	Control* pattern = Template();
	int64_t subject = SubjectOf( get_parent(), client );
	bool show = client != nullptr && pattern != nullptr && client->has_local_player() && client->check_conditions( subject, m_conditions );
	set_visible( show );
	m_entries.clear();
	if ( show == false )
	{
		return;
	}

	PackedInt64Array found;
	switch ( m_of )
	{
		case OF_ITEMS:
			found = client->get_items( m_itemKind, 0 );
			break;
		case OF_HELD_ITEMS:
			found = subject != 0 ? client->get_items( m_itemKind, subject ) : PackedInt64Array();
			break;
		case OF_PLAYERS:
		default:
			found = client->get_players();
			break;
	}
	struct Entry
	{
		int64_t id;
		double key;
	};
	std::vector<Entry> order;
	for ( int64_t i = 0; i < found.size(); ++i )
	{
		if ( m_where.is_empty() || client->check_conditions( found[i], m_where ) )
		{
			Variant key = m_sortBy.is_empty() ? Variant() : client->evaluate( found[i], m_sortBy );
			order.push_back( { found[i], key.get_type() == Variant::NIL ? 0.0 : double( key ) } );
		}
	}
	if ( m_sortBy.is_empty() == false )
	{
		// Ties keep the world's order, so rows do not swap places from frame to frame.
		std::stable_sort( order.begin(), order.end(), [&]( const Entry& x, const Entry& y ) {
			return m_descending ? x.key > y.key : x.key < y.key;
		} );
	}
	if ( m_maxRows > 0 && int( order.size() ) > m_maxRows )
	{
		order.resize( size_t( m_maxRows ) );
	}

	// A row per entry, in order; rows are kept and handed to whoever has that place now.
	for ( size_t i = 0; i < order.size(); ++i )
	{
		Control* row = i < m_rows.size() ? Object::cast_to<Control>( ObjectDB::get_instance( m_rows[i] ) ) : nullptr;
		if ( row == nullptr )
		{
			row = Object::cast_to<Control>( pattern->duplicate() );
			if ( row == nullptr )
			{
				return;
			}
			row->set_process_mode( PROCESS_MODE_INHERIT );
			row->set_meta( SubjectMeta(), order[i].id );
			row->set_meta( RankMeta(), int( i ) + 1 );
			add_child( row );
			if ( i < m_rows.size() )
			{
				m_rows[i] = ObjectID( row->get_instance_id() );
			}
			else
			{
				m_rows.push_back( ObjectID( row->get_instance_id() ) );
			}
		}
		row->set_meta( SubjectMeta(), order[i].id );
		row->set_meta( RankMeta(), int( i ) + 1 );
		row->set_visible( true );
		m_entries.push_back( order[i].id );
	}
	for ( size_t i = order.size(); i < m_rows.size(); ++i )
	{
		if ( auto* row = Object::cast_to<Control>( ObjectDB::get_instance( m_rows[i] ) ) )
		{
			row->set_visible( false );
		}
	}
}

// --- CbShowKey -------------------------------------------------------------------------------------

void CbShowKey::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_action", "value" ), &CbShowKey::set_action );
	ClassDB::bind_method( D_METHOD( "get_action" ), &CbShowKey::get_action );
	ClassDB::bind_method( D_METHOD( "set_key", "value" ), &CbShowKey::set_key );
	ClassDB::bind_method( D_METHOD( "get_key" ), &CbShowKey::get_key );
	ClassDB::bind_method( D_METHOD( "set_mode", "value" ), &CbShowKey::set_mode );
	ClassDB::bind_method( D_METHOD( "get_mode" ), &CbShowKey::get_mode );
	ClassDB::bind_method( D_METHOD( "set_target", "value" ), &CbShowKey::set_target );
	ClassDB::bind_method( D_METHOD( "get_target" ), &CbShowKey::get_target );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "action", PROPERTY_HINT_PLACEHOLDER_TEXT, "scores" ), "set_action", "get_action" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "key", PROPERTY_HINT_PLACEHOLDER_TEXT, "Tab" ), "set_key", "get_key" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "mode", PROPERTY_HINT_ENUM, "While held,Each press switches it" ), "set_mode", "get_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::NODE_PATH, "target" ), "set_target", "get_target" );
	ClassDB::bind_method( D_METHOD( "set_conditions", "value" ), &CbShowKey::set_conditions );
	ClassDB::bind_method( D_METHOD( "get_conditions" ), &CbShowKey::get_conditions );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "conditions" ), "set_conditions", "get_conditions" );

	BIND_ENUM_CONSTANT( MODE_HOLD );
	BIND_ENUM_CONSTANT( MODE_TOGGLE );
}

PackedStringArray CbShowKey::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	String action = m_action.strip_edges();
	if ( action.is_empty() || action.contains( " " ) )
	{
		warnings.push_back( "Name the key's action in one word (scores): \"{key:scores}\" then shows the key it is bound to." );
	}
	if ( OS::get_singleton()->find_keycode_from_string( m_key.strip_edges() ) == KEY_NONE )
	{
		warnings.push_back( "\"" + m_key + "\" is not a key's name. Write it as Godot does: Tab, M, F1, Space." );
	}
	return warnings;
}

void CbShowKey::_ready()
{
	set_process( InGame() );
	if ( InGame() == false )
	{
		return;
	}
	if ( Node* target = get_node_or_null( m_target ) )
	{
		target->set( "visible", false );
	}
}

void CbShowKey::_process( double )
{
	Node* target = get_node_or_null( m_target );
	StringName action = "cb_" + m_action.strip_edges();
	InputMap* map = InputMap::get_singleton();
	if ( target == nullptr || m_action.strip_edges().is_empty() )
	{
		return;
	}
	if ( map->has_action( action ) == false )
	{
		// The action is the game's from now on: whoever names it first gives it its default key.
		Key key = OS::get_singleton()->find_keycode_from_string( m_key.strip_edges() );
		if ( key == KEY_NONE )
		{
			return;
		}
		Ref<InputEventKey> event;
		event.instantiate();
		event->set_physical_keycode( key );
		map->add_action( action );
		map->action_add_event( action, event );
	}
	Input* input = Input::get_singleton();
	if ( m_mode == MODE_TOGGLE )
	{
		m_on = input->is_action_just_pressed( action ) ? m_on == false : m_on;
	}
	else
	{
		m_on = input->is_action_pressed( action );
	}
	bool show = m_on;
	if ( show && m_conditions.is_empty() == false )
	{
		CinderboxClient* client = FindClient( this, m_client );
		show = client != nullptr && client->check_conditions( SubjectOf( this, client ), m_conditions );
	}
	Variant visible = target->get( "visible" );
	if ( visible.get_type() == Variant::BOOL && bool( visible ) != show )
	{
		target->set( "visible", show );
	}
}

} // namespace cb::gd
