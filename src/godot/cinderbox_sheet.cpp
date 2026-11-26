#include "cinderbox_sheet.h"

#include "cinderbox_rules_editor.h"
#include "cue_names.h"
#include "sheet_v1.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/text_server.hpp>

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/tab_bar.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

// --- The Sheet screen ------------------------------------------------------------------------------

void CbSheetEditorPlugin::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "on_text_changed", "edit" ), &CbSheetEditorPlugin::on_text_changed );
}

namespace
{
std::string Utf8( const String& text )
{
	const CharString utf8 = text.utf8();
	return std::string( utf8.get_data(), size_t( utf8.length() ) );
}
} // namespace

void CbSheetEditorPlugin::_enter_tree()
{
	m_screen = memnew( VBoxContainer );
	m_screen->set_name( "SheetScreen" );
	m_screen->set_v_size_flags( Control::SIZE_EXPAND_FILL );
	m_screen->set_h_size_flags( Control::SIZE_EXPAND_FILL );
	m_tabs = memnew( TabContainer );
	m_tabs->set_v_size_flags( Control::SIZE_EXPAND_FILL );
	m_tabs->get_tab_bar()->set_tab_close_display_policy( TabBar::CLOSE_BUTTON_SHOW_ACTIVE_ONLY );
	m_tabs->get_tab_bar()->connect( "tab_close_pressed", callable_mp( this, &CbSheetEditorPlugin::on_tab_close ) );
	m_tabs->connect( "tab_changed", callable_mp( this, &CbSheetEditorPlugin::on_tab_changed ) );
	// (M291) A bar above: the formatter, and the open sheet's mistakes as it is typed.
	auto* bar = memnew( HBoxContainer );
	auto* format = memnew( Button );
	format->set_text( "Format" );
	format->set_tooltip_text( "Write the sheet in the one style (cb_sheet fmt): indented with tabs, no spaces at the ends of lines, at most one blank line in a row" );
	format->connect( "pressed", callable_mp( this, &CbSheetEditorPlugin::on_format ) );
	bar->add_child( format );
	m_status = memnew( Label );
	m_status->set_h_size_flags( Control::SIZE_EXPAND_FILL );
	m_status->set_text_overrun_behavior( TextServer::OVERRUN_TRIM_ELLIPSIS );
	bar->add_child( m_status );
	m_screen->add_child( bar );
	m_screen->add_child( m_tabs );
	EditorInterface::get_singleton()->get_editor_main_screen()->add_child( m_screen );
	m_screen->hide();
}

void CbSheetEditorPlugin::_exit_tree()
{
	m_open.clear();
	if ( m_screen != nullptr )
	{
		m_screen->queue_free();
		m_screen = nullptr;
		m_tabs = nullptr;
		m_status = nullptr;
	}
}

Ref<Texture2D> CbSheetEditorPlugin::_get_plugin_icon() const
{
	Ref<Theme> theme = EditorInterface::get_singleton()->get_editor_theme();
	return theme.is_valid() ? theme->get_icon( "TextFile", "EditorIcons" ) : Ref<Texture2D>();
}

void CbSheetEditorPlugin::_make_visible( bool visible )
{
	if ( m_screen != nullptr )
	{
		m_screen->set_visible( visible );
	}
}

bool CbSheetEditorPlugin::_handles( Object* object ) const
{
	return Object::cast_to<CbSheet>( object ) != nullptr;
}

void CbSheetEditorPlugin::_edit( Object* object )
{
	Ref<CbSheet> sheet( Object::cast_to<CbSheet>( object ) );
	if ( sheet.is_null() || m_tabs == nullptr )
	{
		return;
	}
	for ( size_t i = 0; i < m_open.size(); ++i )
	{
		if ( m_open[i].sheet == sheet )
		{
			m_tabs->set_current_tab( int32_t( i ) );
			EditorInterface::get_singleton()->set_main_screen_editor( "Sheet" );
			return;
		}
	}
	Open open;
	open.sheet = sheet;
	open.edit = memnew( CodeEdit );
	open.edit->set_v_size_flags( Control::SIZE_EXPAND_FILL );
	open.edit->set_text( sheet->get_text() );
	open.edit->set_draw_line_numbers( true );
	open.edit->set_indent_using_spaces( false );
	open.edit->set_auto_indent_enabled( true );
	open.edit->clear_undo_history();
	// Coloured, names offered, mistakes marked: the sheet's highlighter takes care of its text box.
	Ref<CbRulesHighlighter> colours;
	colours.instantiate();
	open.edit->set_syntax_highlighter( colours );
	const int64_t tab = int64_t( m_open.size() );
	open.edit->connect( "text_changed", Callable( this, "on_text_changed" ).bind( open.edit ) );
	m_tabs->add_child( open.edit );
	m_open.push_back( open );
	Title( m_open.size() - 1 );
	m_tabs->set_current_tab( int32_t( tab ) );
	EditorInterface::get_singleton()->set_main_screen_editor( "Sheet" );
	Status();
}

void CbSheetEditorPlugin::on_tab_close( int64_t tab )
{
	if ( tab < 0 || size_t( tab ) >= m_open.size() )
	{
		return;
	}
	// (Closed: what was typed stays in the sheet, saved with the scene.)
	Control* page = m_open[size_t( tab )].edit;
	m_open.erase( m_open.begin() + std::ptrdiff_t( tab ) );
	m_tabs->remove_child( page );
	page->queue_free();
}

void CbSheetEditorPlugin::on_text_changed( Object* edit )
{
	for ( size_t i = 0; i < m_open.size(); ++i )
	{
		Open& open = m_open[i];
		if ( open.edit == edit )
		{
			// What it says now is the sheet's at once: the scenes that use it show it.
			open.sheet->set_text( open.edit->get_text() );
			Status();
			if ( open.dirty == false )
			{
				open.dirty = true;
				Title( i );
			}
			return;
		}
	}
}

void CbSheetEditorPlugin::on_tab_changed( int64_t )
{
	Status();
}

void CbSheetEditorPlugin::Status()
{
	if ( m_status == nullptr || m_tabs == nullptr )
	{
		return;
	}
	const int64_t tab = m_tabs->get_current_tab();
	if ( tab < 0 || size_t( tab ) >= m_open.size() )
	{
		m_status->set_text( String() );
		return;
	}
	// Not version 1 (M292): the game reads it no more (M293: nothing converts version 0).
	const std::string old = sheet::NotVersion1( Utf8( m_open[size_t( tab )].edit->get_text() ) );
	if ( old.empty() == false )
	{
		m_status->set_text( String::utf8( old.c_str() ) );
		m_status->add_theme_color_override( "font_color", Color( 1.0f, 0.45f, 0.45f ) );
		return;
	}
	// The same mistakes its lines are marked with (the highlighter's): how many, and the first.
	const std::vector<std::pair<int, String>> problems = names::SheetProblems( m_open[size_t( tab )].edit->get_text() );
	if ( problems.empty() )
	{
		m_status->set_text( "No mistakes" );
		m_status->remove_theme_color_override( "font_color" );
		return;
	}
	m_status->set_text( String::num_int64( int64_t( problems.size() ) ) + ( problems.size() == 1 ? " mistake: line " : " mistakes; the first: line " ) +
						String::num_int64( problems.front().first ) + ": " + problems.front().second );
	m_status->add_theme_color_override( "font_color", Color( 1.0f, 0.45f, 0.45f ) );
}

void CbSheetEditorPlugin::on_format()
{
	const int64_t tab = m_tabs != nullptr ? m_tabs->get_current_tab() : -1;
	if ( tab < 0 || size_t( tab ) >= m_open.size() )
	{
		return;
	}
	CodeEdit* edit = m_open[size_t( tab )].edit;
	std::string formatted, why;
	if ( sheet::FormatVersion1( Utf8( edit->get_text() ), formatted, why ) == false )
	{
		m_status->set_text( "Not formatted: " + String::utf8( why.c_str() ) );
		m_status->add_theme_color_override( "font_color", Color( 1.0f, 0.45f, 0.45f ) );
		return;
	}
	Replace( edit, String::utf8( formatted.c_str() ) );
}

void CbSheetEditorPlugin::Replace( CodeEdit* edit, const String& text )
{
	if ( text == edit->get_text() )
	{
		return;
	}
	// As one change the undo takes back, the caret kept on its line.
	const int line = edit->get_caret_line();
	edit->begin_complex_operation();
	edit->select_all();
	edit->insert_text_at_caret( text );
	edit->end_complex_operation();
	edit->set_caret_line( std::min( line, edit->get_line_count() - 1 ) );
}

void CbSheetEditorPlugin::Title( size_t tab )
{
	const Open& open = m_open[tab];
	String name = open.sheet->get_path().get_file();
	m_tabs->set_tab_title( int32_t( tab ), ( name.is_empty() ? String( "(unsaved sheet)" ) : name ) + ( open.dirty ? "(*)" : "" ) );
}

void CbSheetEditorPlugin::_save_external_data()
{
	for ( size_t i = 0; i < m_open.size(); ++i )
	{
		Open& open = m_open[i];
		if ( open.dirty == false || open.sheet->get_path().is_empty() || open.sheet->get_path().contains( "::" ) )
		{
			continue; // (one saved inside a scene goes with the scene)
		}
		open.sheet->set_text( open.edit->get_text() );
		if ( ResourceSaver::get_singleton()->save( open.sheet, open.sheet->get_path() ) == OK )
		{
			open.dirty = false;
			Title( i );
		}
	}
}

String CbSheetEditorPlugin::_get_unsaved_status( const String& for_scene ) const
{
	if ( for_scene.is_empty() == false )
	{
		return String();
	}
	PackedStringArray unsaved;
	for ( const Open& open : m_open )
	{
		if ( open.dirty )
		{
			unsaved.push_back( open.sheet->get_path().get_file() );
		}
	}
	return unsaved.is_empty() ? String() : "Save the changes to these sheets?\n" + String( "\n" ).join( unsaved );
}

} // namespace cb::gd
