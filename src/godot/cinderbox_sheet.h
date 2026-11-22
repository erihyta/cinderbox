#pragma once

// A sheet (a .cbsheet file, CbSheet: cue_sheet.h) in the editor (M284): it opens in the Sheet
// screen, its words coloured, the names the server's mods declare offered while typing, mistakes
// marked on their lines (the highlighter, cinderbox_rules_editor.h), saved with the scene (Ctrl+S).

#include "cue_sheet.h"

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <vector>

namespace cb::gd
{

// The Sheet screen of the editor: the sheets opened (double-clicked in the file system, or a node's
// sheet in the inspector), each in a tab of its own, saved when the scene is.
class CbSheetEditorPlugin : public godot::EditorPlugin
{
	GDCLASS( CbSheetEditorPlugin, godot::EditorPlugin )

public:
	void _enter_tree() override;
	void _exit_tree() override;
	bool _has_main_screen() const override
	{
		return true;
	}
	godot::String _get_plugin_name() const override
	{
		return "Sheet";
	}
	godot::Ref<godot::Texture2D> _get_plugin_icon() const override;
	void _make_visible( bool visible ) override;
	bool _handles( godot::Object* object ) const override;
	void _edit( godot::Object* object ) override;
	void _save_external_data() override;
	godot::String _get_unsaved_status( const godot::String& for_scene ) const override;

	void on_text_changed( godot::Object* edit );
	void on_tab_close( int64_t tab );
	void on_tab_changed( int64_t tab );
	void on_format();

protected:
	static void _bind_methods();

private:
	struct Open
	{
		godot::Ref<CbSheet> sheet;
		godot::CodeEdit* edit = nullptr;
		bool dirty = false;
	};
	void Title( size_t tab );
	// (M291) What the open sheet's mistakes are, as it is typed: how many, and the first.
	void Status();
	godot::VBoxContainer* m_screen = nullptr;
	godot::Label* m_status = nullptr;
	godot::TabContainer* m_tabs = nullptr;
	std::vector<Open> m_open;
};

} // namespace cb::gd
