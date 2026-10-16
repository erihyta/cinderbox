#include "cinderbox_rules_editor.h"

#include "cue_names.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/color.hpp>

using namespace godot;

namespace cb::gd
{

namespace
{

const Color kComment( 0.45f, 0.5f, 0.56f );
const Color kWord( 1.0f, 0.44f, 0.52f );
const Color kNumber( 0.63f, 1.0f, 0.88f );
const Color kText( 1.0f, 0.93f, 0.63f );
const Color kName( 0.34f, 0.7f, 1.0f );
const Color kUnknown( 1.0f, 0.35f, 0.35f );
const Color kPlain( 0.8f, 0.82f, 0.86f );
const Color kProblem( 1.0f, 0.25f, 0.25f, 0.16f );

// A character a word or a name of a sheet is made of ("brawl.cd_dash", "!combat.dead", "?weather").
bool WordChar( char32_t c )
{
	return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '.' || c == '!' || c == '?';
}

} // namespace

void CbRulesHighlighter::_bind_methods()
{
}

String CbRulesHighlighter::_get_name() const
{
	return "Cinderbox rules";
}

PackedStringArray CbRulesHighlighter::_get_supported_languages() const
{
	return PackedStringArray( { "rules" } );
}

Ref<EditorSyntaxHighlighter> CbRulesHighlighter::_create() const
{
	Ref<CbRulesHighlighter> made;
	made.instantiate();
	return made;
}

Dictionary CbRulesHighlighter::_get_line_syntax_highlighting( int32_t line ) const
{
	Dictionary out;
	TextEdit* edit = get_text_edit();
	if ( edit == nullptr )
	{
		return out;
	}
	const String text = edit->get_line( line );
	auto paint = [&]( int64_t at, const Color& color ) {
		Dictionary c;
		c["color"] = color;
		out[at] = c;
	};
	const int64_t n = text.length();
	int64_t i = 0;
	while ( i < n )
	{
		char32_t c = text[i];
		if ( c == '#' )
		{
			paint( i, kComment );
			break;
		}
		if ( c == '"' )
		{
			int64_t close = text.find( "\"", i + 1 );
			close = close < 0 ? n - 1 : close;
			paint( i, kText );
			i = close + 1;
			paint( i, kPlain );
			continue;
		}
		if ( WordChar( c ) )
		{
			int64_t end = i;
			while ( end < n && WordChar( text[end] ) )
			{
				++end;
			}
			String kind = names::WordKind( text.substr( i, end - i ) );
			paint( i, kind == "word" ? kWord : kind == "number" ? kNumber : kind == "name" ? kName : kind == "unknown" ? kUnknown : kPlain );
			i = end;
			paint( i, kPlain );
			continue;
		}
		++i;
	}
	return out;
}

void CbRulesHighlighter::_update_cache()
{
	// The text box this highlighter was given: taken care of once.
	auto* edit = Object::cast_to<CodeEdit>( get_text_edit() );
	if ( edit == nullptr || edit == m_edit )
	{
		return;
	}
	m_edit = edit;
	edit->set_code_completion_enabled( true );
	edit->connect( "text_changed", callable_mp( this, &CbRulesHighlighter::on_text_changed ) );
	edit->connect( "code_completion_requested", callable_mp( this, &CbRulesHighlighter::on_completion_requested ) );
	edit->connect( "caret_changed", callable_mp( this, &CbRulesHighlighter::on_caret_changed ) );
	Check();
}

void CbRulesHighlighter::Check()
{
	if ( m_edit == nullptr )
	{
		return;
	}
	m_problems.clear();
	for ( const auto& [line, why] : names::SheetProblems( m_edit->get_text() ) )
	{
		m_problems[line - 1] = m_problems.count( line - 1 ) ? m_problems[line - 1] + "\n" + why : why;
	}
	for ( int i = 0; i < m_edit->get_line_count(); ++i )
	{
		m_edit->set_line_background_color( i, m_problems.count( i ) ? kProblem : Color( 0, 0, 0, 0 ) );
	}
	on_caret_changed();
}

void CbRulesHighlighter::on_text_changed()
{
	Check();
	// Names are offered while a word is typed, as a script's are.
	int line = m_edit->get_caret_line();
	int column = m_edit->get_caret_column();
	String text = m_edit->get_line( line );
	if ( column > 0 && column <= text.length() && WordChar( text[column - 1] ) )
	{
		m_edit->request_code_completion();
	}
}

void CbRulesHighlighter::on_completion_requested()
{
	if ( m_edit == nullptr )
	{
		return;
	}
	String text = m_edit->get_line( m_edit->get_caret_line() );
	int64_t end = m_edit->get_caret_column();
	int64_t start = end;
	while ( start > 0 && WordChar( text[start - 1] ) )
	{
		--start;
	}
	String typed = text.substr( start, end - start );
	// The editor replaces only what follows the last dot: offer the rest of the name from there.
	int64_t dot = typed.rfind( "." );
	PackedStringArray options = names::Completions( typed );
	for ( int64_t i = 0; i < options.size(); ++i )
	{
		String rest = dot >= 0 ? options[i].substr( dot + 1 ) : options[i];
		m_edit->add_code_completion_option( CodeEdit::KIND_PLAIN_TEXT, rest, rest );
	}
	m_edit->update_code_completion_options( false );
}

void CbRulesHighlighter::on_caret_changed()
{
	if ( m_edit == nullptr )
	{
		return;
	}
	auto it = m_problems.find( m_edit->get_caret_line() );
	m_edit->set_code_hint( it != m_problems.end() ? it->second : String() );
}

} // namespace cb::gd
