#include "cinderbox_sheet.h"

#include "cinderbox_rules_editor.h"

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
			if ( open.dirty == false )
			{
				open.dirty = true;
				Title( i );
			}
			return;
		}
	}
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
