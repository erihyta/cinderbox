#pragma once

// A rules sheet (.rules) in the editor's script editor: its words coloured, the names the server's
// mods declare offered while typing, and what cannot be read or names nobody declares marked on its
// line, with why when the caret is there. The editor gives a .rules file this highlighter (its
// extension is its language), and the highlighter takes care of the text box it is given.
//
// What it says is the names library's (cue_names.h: SheetProblems, Completions, WordKind), which
// tools and checks read too (CbNames); this is only the editor's side of it.

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_syntax_highlighter.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <map>

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
};

} // namespace cb::gd
