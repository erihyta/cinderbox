#include "cue_info.h"

#include "cue_director.h"
#include "cue_reaction.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/window.hpp>

using namespace godot;

namespace cb::gd
{

namespace
{

struct Info
{
	const char* key;
	const char* brief;
	const char* detail;
};

// The reaction inspector's texts: one per group, and one about the node ("").
const Info kReactionInfo[] = {
	{ "", "Reacts to the game with no code: on a cue, or while conditions hold, it changes how things look.",
	  "[b]Where[/b]: in any scene the game draws (a held item, a character, a prop) or in a vfx/reactions_*.tscn.\n"
	  "[b]Paths[/b] start from anchors: [b]^[/b] my entity · [b]^^[/b] my holder · [b]$at[/b] / [b]$other[/b] the cue's "
	  "entities · [b]$local[/b] the viewer · [b]$world[/b]. To type one into a path field, use the field's ⋮ menu, "
	  "Edit.\n"
	  "[b]Looks only[/b]: it never changes the game itself, and it cannot reach outside the World.\n"
	  "[b]Try it[/b] without a server: the Cue Preview panel at the bottom." },
	{ "When", "What it waits for, and whose state it reads.",
	  "[b]when[/b]: On a cue acts once per cue with this name. While stays on as long as the conditions hold, and "
	  "undoes what it did when they stop.\n"
	  "[b]event[/b]: melee.hit (a mod's) · footstep, jumped, landed, impact, spawned, destroying (the game's) · "
	  "pressed:fire (the viewer's key, before the server answers).\n"
	  "[b]event_side[/b]: A = the cue happened at the subject (melee.hit is at the attacker). B = the subject is the "
	  "other one (the victim).\n"
	  "[b]subject[/b]: ^ (default) my entity · ^^ my holder · $at or $other: any cue of this name.\n"
	  "[b]subject_kind / subject_template[/b]: only players, props, items... or one template (an item's is its kind: "
	  "melee.bat).\n"
	  "[b]conditions[/b]: all must hold. melee.hot · !combat.dead · pistol.ammo > 0 · ^^:combat.health < 30 · is_local "
	  "· event.value > 0" },
	{ "Timing", "Later, sometimes, and not too often (cue reactions).",
	  "[b]delay[/b]: act this many seconds after the cue.\n"
	  "[b]chance[/b]: 1 always, 0.3 about a third of the time.\n"
	  "[b]cooldown[/b]: at least this long between two firings (twenty props landing at once play one sound)." },
	{ "Animation", "Play an animation. Looks only: bodies are posed by the game.",
	  "[b]animation_player[/b]: a path to an AnimationPlayer (../AnimationPlayer).\n"
	  "[b]animation[/b]: played from the start.\n"
	  "[b]animation_off[/b]: played when a While ends; without one, the animation stops.\n"
	  "For an item's own tracks, lights, particles, materials. Bone tracks on a player's body are overwritten by the "
	  "game's pose." },
	{ "Property", "Change a property of a node, or call one of its methods.",
	  "[b]target[/b]: the node (../Barrel, ^^/Head).\n"
	  "[b]property[/b]: its name; sub-paths work: visible · surface_material_override/0:emission_energy_multiplier\n"
	  "[b]value[/b]: what it becomes. A While puts the old value back when it ends.\n"
	  "[b]blend_time[/b]: fade to it instead (numbers, vectors, colours).\n"
	  "[b]method + method_args[/b]: restart · play [\"slash\"] · set_visible [false]\n"
	  "Only listed methods are called (restart, play, stop, show, hide, set_visible, set_emitting, ...): use property for "
	  "anything else. Never a script or metadata." },
	{ "Scene", "Add a scene: particles, a decal, a light.",
	  "[b]scene[/b]: any .tscn.\n"
	  "[b]scene_parent[/b]: where it goes (default: next to this reaction; see Place).\n"
	  "[b]scene_lifetime[/b]: a cue's scene is freed after this many seconds; a While's when the While ends.\n"
	  "One-shot particles restart, so the burst always plays." },
	{ "Place", "Where the scene and the sound go.",
	  "[b]Under its parent[/b]: next to the reaction, moving with it.\n"
	  "[b]Cue point / Cue end[/b]: where it happened / where a shot ended; it stays there.\n"
	  "[b]Beam[/b]: a 1 m scene along -Z, stretched from place_node (or the point) to the end: tracers.\n"
	  "[b]At place_node[/b]: $at/RightHand, $other/Head.\n"
	  "[b]Follow the subject[/b]: rides along with it.\n"
	  "[b]offset[/b] is added to all of them." },
	{ "Sound", "Play a sound once, where Place puts it.",
	  "[b]volume_db, pitch_scale[/b]; [b]pitch_jitter[/b] adds a random ± to the pitch.\n"
	  "[b]bus[/b]: an audio bus. [b]max_distance[/b]: silent beyond it (0: the default falloff).\n"
	  "For variety, use an AudioStreamRandomizer as the sound." },
	{ "Screen", "Shake the camera or flash the screen: the viewer's own.",
	  "Every player's game runs every reaction, so add the condition [b]is_local[/b] (or subject $local) for what only "
	  "the affected player should feel.\n"
	  "[b]shake[/b]: strength, fading over shake_time.\n"
	  "[b]flash_color[/b]: its alpha is the strength, fading over flash_time." },
};

const Info* FindInfo( const String& key )
{
	for ( const Info& info : kReactionInfo )
	{
		if ( key == info.key )
		{
			return &info;
		}
	}
	return nullptr;
}

} // namespace

CbInfoButton* CbInfoButton::Make( const String& brief, const String& detail )
{
	auto* button = memnew( CbInfoButton );
	button->set_flat( true );
	button->set_tooltip_text( brief + String( "\n(click for more)" ) );
	button->m_detail = "[b]" + brief + "[/b]\n\n" + detail;
	EditorInterface* editor = EditorInterface::get_singleton();
	Ref<Theme> theme = editor != nullptr ? editor->get_editor_theme() : Ref<Theme>();
	if ( theme.is_valid() && theme->has_icon( "Help", "EditorIcons" ) )
	{
		button->set_button_icon( theme->get_icon( "Help", "EditorIcons" ) );
	}
	else
	{
		button->set_text( "?" );
	}
	return button;
}

void CbInfoButton::_pressed()
{
	auto* popup = memnew( PopupPanel );
	auto* text = memnew( RichTextLabel );
	text->set_use_bbcode( true );
	text->set_fit_content( true );
	text->set_custom_minimum_size( Vector2( 420, 0 ) );
	text->set_text( m_detail );
	popup->add_child( text );
	add_child( popup );
	popup->connect( "popup_hide", Callable( popup, "queue_free" ) );
	Vector2 at = get_screen_position() + Vector2( 0, get_size().y );
	popup->popup( Rect2i( Vector2i( int( at.x ), int( at.y ) ), Vector2i( 440, 0 ) ) );
}

HBoxContainer* InfoRow( const String& brief, const String& detail )
{
	auto* row = memnew( HBoxContainer );
	row->add_child( CbInfoButton::Make( brief, detail ) );
	auto* label = memnew( Label );
	label->set_text( brief );
	label->set_autowrap_mode( TextServer::AUTOWRAP_WORD_SMART );
	label->set_h_size_flags( Control::SIZE_EXPAND_FILL );
	label->set_modulate( Color( 1, 1, 1, 0.65 ) );
	row->add_child( label );
	return row;
}

bool CbReactionInspector::_can_handle( Object* object ) const
{
	return Object::cast_to<CbReaction>( object ) != nullptr;
}

void CbReactionInspector::_parse_begin( Object* )
{
	const Info* info = FindInfo( "" );
	add_custom_control( InfoRow( info->brief, info->detail ) );
}

void CbReactionInspector::_parse_group( Object*, const String& group )
{
	if ( const Info* info = FindInfo( group ) )
	{
		add_custom_control( InfoRow( info->brief, info->detail ) );
	}
}

} // namespace cb::gd
