#pragma once

// Info buttons for the editor: a small (?) that says one line on hover and a few more on click.
// Used by the reaction inspector and the Cue Preview panel. Editor only.

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/editor_inspector_plugin.hpp>
#include <godot_cpp/classes/h_box_container.hpp>

namespace cb::gd
{

class CbInfoButton : public godot::Button
{
	GDCLASS( CbInfoButton, godot::Button )

public:
	// `brief`: one line (the tooltip). `detail`: shown on click; [b]bold[/b] works.
	static CbInfoButton* Make( const godot::String& brief, const godot::String& detail );
	void _pressed() override;

protected:
	static void _bind_methods()
	{
	}

private:
	godot::String m_detail;
};

// A row: the (?) button and its brief line, dimmed. What the inspector puts at the top of a group.
godot::HBoxContainer* InfoRow( const godot::String& brief, const godot::String& detail );

// Adds an info row to the top of every CbReaction group in the inspector, and one about the node.
class CbReactionInspector : public godot::EditorInspectorPlugin
{
	GDCLASS( CbReactionInspector, godot::EditorInspectorPlugin )

public:
	bool _can_handle( godot::Object* object ) const override;
	void _parse_begin( godot::Object* object ) override;
	void _parse_group( godot::Object* object, const godot::String& group ) override;

protected:
	static void _bind_methods()
	{
	}
};

} // namespace cb::gd
