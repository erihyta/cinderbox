#include "cue_names.h"

#include "expr.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace godot;

namespace cb::gd::names
{

namespace
{

const char* const kFile = "res://cinderbox_names.cfg";
// Cues the engine itself sends: events no mod declares.
const char* const kBuiltInCues[] = { "spawned", "destroying", "jumped", "landed", "footstep", "impact" };

std::string Std( const String& s )
{
	CharString utf8 = s.utf8();
	return std::string( utf8.get_data(), size_t( utf8.length() ) );
}

String Gd( const std::string& s )
{
	return String::utf8( s.c_str(), int64_t( s.size() ) );
}

struct Book
{
	bool known = false;
	uint64_t modified = 0;
	uint64_t checkedAt = 0;
	std::map<std::string, std::set<std::string>> byKind; // "field" -> names
	std::set<std::string> declared;						  // fields, events and item kinds: what a condition may name
};

// The file, read again when it changed (looked at no more than once a second).
const Book& TheBook()
{
	static Book book;
	uint64_t now = Time::get_singleton()->get_ticks_msec();
	if ( book.checkedAt != 0 && now - book.checkedAt < 1000 )
	{
		return book;
	}
	book.checkedAt = now == 0 ? 1 : now;
	if ( FileAccess::file_exists( kFile ) == false )
	{
		book = Book{};
		book.checkedAt = now == 0 ? 1 : now;
		return book;
	}
	uint64_t modified = FileAccess::get_modified_time( kFile );
	if ( book.known && modified == book.modified )
	{
		return book;
	}
	Book fresh;
	fresh.checkedAt = book.checkedAt;
	fresh.modified = modified;
	Ref<FileAccess> file = FileAccess::open( kFile, FileAccess::READ );
	if ( file.is_null() )
	{
		book = fresh;
		return book;
	}
	PackedStringArray lines = file->get_as_text().split( "\n" );
	for ( int64_t i = 0; i < lines.size(); ++i )
	{
		PackedStringArray words = lines[i].strip_edges().split( "\t" );
		if ( i == 0 )
		{
			fresh.known = words.size() >= 1 && words[0] == "cinderbox_names";
			continue;
		}
		if ( fresh.known && words.size() >= 2 )
		{
			fresh.byKind[Std( words[0] )].insert( Std( words[1] ) );
		}
	}
	for ( const char* cue : kBuiltInCues )
	{
		fresh.byKind["event"].insert( cue );
	}
	for ( const char* kind : { "field", "event", "item" } )
	{
		const std::set<std::string>& names = fresh.byKind[kind];
		fresh.declared.insert( names.begin(), names.end() );
	}
	book = fresh;
	return book;
}

bool Has( const Book& book, const char* kind, const std::string& name )
{
	auto it = book.byKind.find( kind );
	return it != book.byKind.end() && it->second.count( name ) != 0;
}

size_t Distance( const std::string& a, const std::string& b )
{
	std::vector<size_t> row( b.size() + 1 );
	for ( size_t j = 0; j <= b.size(); ++j )
	{
		row[j] = j;
	}
	for ( size_t i = 1; i <= a.size(); ++i )
	{
		size_t diagonal = row[0];
		row[0] = i;
		for ( size_t j = 1; j <= b.size(); ++j )
		{
			size_t above = row[j];
			row[j] = std::min( { row[j] + 1, row[j - 1] + 1, diagonal + ( a[i - 1] == b[j - 1] ? 0 : 1 ) } );
			diagonal = above;
		}
	}
	return row[b.size()];
}

// The closest name there is, when one is close enough to be what was meant.
std::string Closest( const std::string& name, const std::set<std::string>& among )
{
	std::string best;
	size_t bestDistance = std::max<size_t>( 2, name.size() / 3 ) + 1;
	for ( const std::string& candidate : among )
	{
		size_t d = Distance( name, candidate );
		if ( d < bestDistance )
		{
			bestDistance = d;
			best = candidate;
		}
	}
	return best;
}

String Meant( const std::string& name, const std::set<std::string>& among )
{
	std::string closest = Closest( name, among );
	return closest.empty() ? String() : " (did you mean " + Gd( closest ) + "?)";
}

// One name as a property holds it ("melee.hit" as a reaction's event).
void CheckName( const Book& book, const char* kind, const String& value, const String& property, PackedStringArray& out )
{
	std::string name = Std( value.strip_edges() );
	if ( name.empty() || Has( book, kind, name ) )
	{
		return;
	}
	auto it = book.byKind.find( kind );
	static const std::set<std::string> kNone;
	const std::set<std::string>& among = it != book.byKind.end() ? it->second : kNone;
	const char* what = std::string( kind ) == "item" ? "item kind" : kind;
	out.push_back( property + String( ": no mod declares the " ) + what + " \"" + Gd( name ) + "\"" + Meant( name, among ) + "." );
}

// A name as an expression reads it: a mod's field, event or item kind; a key ("held.dash"); or one
// of the reader's own, which is not a mod's to declare.
void CheckRead( const Book& book, const std::string& written, const String& property, PackedStringArray& out )
{
	std::string path, name;
	expr::SplitName( written, path, name );
	for ( const char* key : { "held.", "pressed." } )
	{
		if ( name.rfind( key, 0 ) == 0 )
		{
			std::string action = name.substr( std::string( key ).size() );
			if ( action != "jump" && action != "sprint" && Has( book, "action", action ) == false )
			{
				out.push_back( property + String( ": no mod declares the action \"" ) + Gd( action ) + "\"" + Meant( action, book.byKind.count( "action" ) ? book.byKind.at( "action" ) : std::set<std::string>{} ) + "." );
			}
			return;
		}
	}
	size_t dot = name.find( '.' );
	if ( dot == std::string::npos || book.declared.count( name ) != 0 )
	{
		return; // one of the reader's own (speed, is_local, a stance), or declared
	}
	std::string prefix = name.substr( 0, dot );
	for ( const char* own : { "ui", "event", "slot", "motion" } )
	{
		if ( prefix == own )
		{
			return;
		}
	}
	if ( Has( book, "mod", prefix ) )
	{
		out.push_back( property + String( ": no mod declares \"" ) + Gd( name ) + "\"" + Meant( name, book.declared ) + "." );
	}
	else
	{
		out.push_back( property + String( ": \"" ) + Gd( name ) + "\" is of no mod this server has (\"" + Gd( prefix ) + "\")" + Meant( name, book.declared ) + "." );
	}
}

void CheckExpression( const Book& book, const String& text, const String& property, PackedStringArray& out )
{
	expr::Program program;
	std::string error;
	if ( text.strip_edges().is_empty() || expr::Compile( Std( text ), program, error ) == false )
	{
		return; // what is not an expression is said by whoever reads it
	}
	std::set<std::string> seen;
	for ( const std::string& name : program.names )
	{
		if ( seen.insert( name ).second )
		{
			CheckRead( book, name, property, out );
		}
	}
}

// "{pistol.ammo} / 12", "[{key:pickup}] Pick up {look:pickup.target}": what is between braces.
void CheckFormat( const Book& book, const String& format, const String& property, PackedStringArray& out )
{
	int64_t at = 0;
	while ( true )
	{
		int64_t open = format.find( "{", at );
		int64_t close = open >= 0 ? format.find( "}", open ) : -1;
		if ( open < 0 || close < 0 )
		{
			return;
		}
		String inside = format.substr( open + 1, close - open - 1 ).strip_edges();
		at = close + 1;
		if ( inside.begins_with( "key:" ) )
		{
			continue; // a server's action or a key of the look's own (CbKey): either is fine
		}
		if ( inside.begins_with( "name:" ) || inside.begins_with( "look:" ) )
		{
			inside = inside.substr( 5 );
		}
		if ( inside == "name" || inside == "rank" || inside == "choice" || inside == "a" || inside == "b" )
		{
			continue;
		}
		CheckExpression( book, inside, property, out );
	}
}

// What a property holds.
enum class Holds
{
	Event,
	Action,
	Item,
	Expression,
	Expressions, // an array of them
	Format,
	Sets,	 // an array of "ui.x = expression"
	Changes, // an array of "field op value"
};

struct Entry
{
	const char* cls;
	const char* property;
	Holds holds;
};

// Which properties of which nodes hold names. By class name, so this library needs none of them.
const Entry kTable[] = {
	{ "CbReaction", "event", Holds::Event },
	{ "CbReaction", "conditions", Holds::Expressions },
	{ "CbReaction", "value_expression", Holds::Expression },
	{ "CbReaction", "volume_expression", Holds::Expression },
	{ "CbPrediction", "action", Holds::Action },
	{ "CbPrediction", "cue", Holds::Event },
	{ "CbPrediction", "conditions", Holds::Expressions },
	{ "CbPrediction", "changes", Holds::Changes },
	{ "CbMotion", "action", Holds::Action },
	{ "CbMotion", "event", Holds::Event },
	{ "CbMotion", "emits", Holds::Event },
	{ "CbMotion", "conditions", Holds::Expressions },
	{ "CbMotion", "until", Holds::Expressions },
	{ "CbMotion", "changes", Holds::Changes },
	{ "CbLaunch", "item_kind", Holds::Item },
	{ "CbFieldLabel", "text_format", Holds::Format },
	{ "CbFieldLabel", "conditions", Holds::Expressions },
	{ "CbFieldLabel", "choice_field", Holds::Expression },
	{ "CbFieldBinding", "field", Holds::Expression },
	{ "CbFieldBinding", "text_format", Holds::Format },
	{ "CbFieldBinding", "conditions", Holds::Expressions },
	{ "CbList", "event", Holds::Event },
	{ "CbList", "item_kind", Holds::Item },
	{ "CbList", "where", Holds::Expressions },
	{ "CbList", "sort_by", Holds::Expression },
	{ "CbList", "conditions", Holds::Expressions },
	{ "CbKey", "conditions", Holds::Expressions },
	{ "CbClick", "conditions", Holds::Expressions },
	{ "CbClick", "sets", Holds::Sets },
	{ "CbPromptLabel", "text_format", Holds::Format },
	{ "CbPromptLabel", "progress_field", Holds::Expression },
	{ "CbPromptLabel", "since_field", Holds::Expression },
	{ "CbPromptLabel", "duration_field", Holds::Expression },
};

const char* KindOf( Holds holds )
{
	return holds == Holds::Event ? "event" : holds == Holds::Action ? "action" : holds == Holds::Item ? "item" : nullptr;
}

} // namespace

bool Known()
{
	return TheBook().known;
}

void Hint( const Object* object, PropertyInfo& property )
{
	const Book& book = TheBook();
	if ( book.known == false || property.type != Variant::STRING )
	{
		return;
	}
	for ( const Entry& entry : kTable )
	{
		const char* kind = KindOf( entry.holds );
		if ( kind == nullptr || property.name != StringName( entry.property ) || object->is_class( entry.cls ) == false )
		{
			continue;
		}
		auto it = book.byKind.find( kind );
		if ( it == book.byKind.end() || it->second.empty() )
		{
			return;
		}
		String names;
		for ( const std::string& name : it->second )
		{
			names += ( names.is_empty() ? "" : "," ) + Gd( name );
		}
		// (A reaction on a motion's "jump" or an action of the engine's is typed as before: the
		// list is what to choose from, not all that may be written.)
		property.hint = PROPERTY_HINT_ENUM_SUGGESTION;
		property.hint_string = names;
		return;
	}
}

PackedStringArray Problems( const Node* node )
{
	PackedStringArray out;
	const Book& book = TheBook();
	if ( book.known == false || node == nullptr )
	{
		return out;
	}
	for ( const Entry& entry : kTable )
	{
		if ( node->is_class( entry.cls ) == false )
		{
			continue;
		}
		Variant value = node->get( StringName( entry.property ) );
		String property = entry.property;
		switch ( entry.holds )
		{
			case Holds::Event:
			case Holds::Item:
				CheckName( book, KindOf( entry.holds ), String( value ), property, out );
				break;
			case Holds::Action:
			{
				String action = String( value ).strip_edges();
				if ( action != "jump" && action != "sprint" && action != "use" )
				{
					CheckName( book, "action", action, property, out );
				}
				break;
			}
			case Holds::Expression:
				CheckExpression( book, String( value ), property, out );
				break;
			case Holds::Format:
				CheckFormat( book, String( value ), property, out );
				break;
			case Holds::Expressions:
			case Holds::Sets:
			case Holds::Changes:
			{
				PackedStringArray lines = value;
				for ( int64_t i = 0; i < lines.size(); ++i )
				{
					String line = lines[i];
					if ( entry.holds == Holds::Sets )
					{
						// "ui.x = expression": the right side is read.
						int64_t equals = line.find( "=" );
						line = equals >= 0 ? line.substr( equals + 1 ) : String();
					}
					else if ( entry.holds == Holds::Changes )
					{
						// "field op value": the field is written, so it has to be one.
						String field = line.strip_edges().get_slice( " ", 0 );
						for ( const char* op : { "+=", "-=", "=" } )
						{
							field = field.get_slice( op, 0 ).strip_edges();
						}
						std::string name = Std( field );
						if ( name.empty() == false && Has( book, "field", name ) == false )
						{
							out.push_back( property + String( ": no mod declares the field \"" ) + field + "\"" +
										   Meant( name, book.byKind.count( "field" ) ? book.byKind.at( "field" ) : std::set<std::string>{} ) + "." );
						}
						continue;
					}
					CheckExpression( book, line, property, out );
				}
				break;
			}
		}
	}
	return out;
}

PackedStringArray ProblemsUnder( const Node* root )
{
	PackedStringArray out;
	if ( root == nullptr )
	{
		return out;
	}
	std::vector<const Node*> open{ root };
	while ( open.empty() == false )
	{
		const Node* node = open.back();
		open.pop_back();
		PackedStringArray own = Problems( node );
		String where = node == root ? String( root->get_name() ) : String( root->get_name() ) + "/" + String( root->get_path_to( const_cast<Node*>( node ) ) );
		for ( int64_t i = 0; i < own.size(); ++i )
		{
			out.push_back( where + ": " + own[i] );
		}
		for ( int i = node->get_child_count() - 1; i >= 0; --i )
		{
			open.push_back( node->get_child( i ) );
		}
	}
	return out;
}

} // namespace cb::gd::names

namespace cb::gd
{

void CbNames::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "is_known" ), &CbNames::is_known );
	ClassDB::bind_method( D_METHOD( "get_names", "kind" ), &CbNames::get_names );
	ClassDB::bind_method( D_METHOD( "check_scene", "root" ), &CbNames::check_scene );
}

bool CbNames::is_known() const
{
	return names::Known();
}

PackedStringArray CbNames::get_names( const String& kind ) const
{
	PackedStringArray out;
	if ( names::Known() == false )
	{
		return out;
	}
	// (Through the hint's own path would need a node; the book is small, so it is read here.)
	Ref<FileAccess> file = FileAccess::open( "res://cinderbox_names.cfg", FileAccess::READ );
	if ( file.is_null() )
	{
		return out;
	}
	PackedStringArray lines = file->get_as_text().split( "\n" );
	for ( int64_t i = 1; i < lines.size(); ++i )
	{
		PackedStringArray words = lines[i].strip_edges().split( "\t" );
		if ( words.size() >= 2 && words[0] == kind )
		{
			out.push_back( words[1] );
		}
	}
	return out;
}

PackedStringArray CbNames::check_scene( Node* root ) const
{
	return names::ProblemsUnder( root );
}

} // namespace cb::gd
