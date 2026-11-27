#pragma once

// A sheet (.cbsheet) in Godot's script editor (M297.3; a text file it opens, as before M284): its
// words coloured, the names the server's mods declare offered while typing, and what cannot be read
// or names nobody declares marked on its line, with why when the caret is there. Registered with the
// script editor (CbAutoBakePlugin), which gives one to each sheet it opens; the highlighter takes care
// of the text box it is given.
//
// What it says is the names library's (cue_names.h: SheetProblems, Completions, WordKind), which
// tools and checks read too (CbNames); this is only the editor's side of it.

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_syntax_highlighter.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <map>
#include <set>
#include <string>

namespace cb::gd
{

class CbRulesHighlighter : public godot::EditorSyntaxHighlighter
{
	GDCLASS( CbRulesHighlighter, godot::EditorSyntaxHighlighter )

public:
	godot::String _get_name() const override;
	godot::PackedStringArray _get_supported_languages() const override;
	godot::Ref<godot::EditorSyntaxHighlighter> _create() const override;
	godot::Dictionary _get_line_syntax_highlighting( int32_t line ) const override;
	void _update_cache() override;

	void on_text_changed();
	void on_completion_requested();
	void on_caret_changed();

protected:
	static void _bind_methods();

private:
	void Check();
	godot::CodeEdit* m_edit = nullptr;
	std::map<int, godot::String> m_problems; // by line, from 0
	std::set<std::string> m_own;			 // the names the sheet declares itself
};

} // namespace cb::gd
