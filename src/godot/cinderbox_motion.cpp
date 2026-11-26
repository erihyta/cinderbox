#include "cinderbox_motion.h"

#include "sheet_motions.h"
#include "sheet_parts.h"

#include <algorithm>

#include "cue/cue_info.h"

#include "types.h" // kMotionLimit

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

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

const Info kSetInfo = { "", "A mod's motions: the motion blocks of the rules sheets under it, which the server compiles; the parts of the scene they name are baked to motions/<set_name>.cfg.",
						"[b]set_name[/b]: what the server mod asks for: declare.Motions( \"dash.moves\" ).\n"
						"[b]Baking[/b]: saving the scene, the Bake button, and publishing the mod. A bake that changes "
						"nothing writes nothing.\n"
						"[b]As many motions as the mods have[/b]: a player keeps 16 bytes for each, all its mods' sets together." };

} // namespace

// --- CbMotionSet ------------------------------------------------------------------------------------

void CbMotionSet::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_set_name", "name" ), &CbMotionSet::set_set_name );
	ClassDB::bind_method( D_METHOD( "get_set_name" ), &CbMotionSet::get_set_name );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "set_name", PROPERTY_HINT_PLACEHOLDER_TEXT, "dash.moves" ), "set_set_name", "get_set_name" );
	ClassDB::bind_method( D_METHOD( "bake" ), &CbMotionSet::bake );
	ClassDB::bind_method( D_METHOD( "bake_to_project" ), &CbMotionSet::bake_to_project );
	ClassDB::bind_method( D_METHOD( "get_bake_button" ), &CbMotionSet::get_bake_button );
	ADD_PROPERTY( PropertyInfo( Variant::CALLABLE, "bake_button", PROPERTY_HINT_TOOL_BUTTON, "Bake motions,Save", PROPERTY_USAGE_EDITOR ), "",
				  "get_bake_button" );
}

Callable CbMotionSet::get_bake_button()
{
	return Callable( this, "bake_to_project" );
}

std::string CbMotionSet::Bake( String& error, std::string* parts ) const
{
	String name = m_name.strip_edges();
	if ( name.is_empty() || name.contains( "/" ) || name.contains( " " ) || name.contains( "\\" ) || MotionTextPlain( name ) == false )
	{
		error = "set_name must be one word (dash.moves): the server mod asks for the set by it";
		return std::string();
	}
	// The motion blocks of its sheets (a CbRules under it, holding a .cbsheet), as the server compiles
	// them; and what they name of the scene (M294: the mod carries that and its sheets).
	std::string text = "cinderbox_motions\t3\n";
	std::vector<std::string> names;
	sheet::OwnerParts recorded;
	if ( BakeSheetMotions( this, String(), 0, text, names, &recorded, error ) == false )
	{
		return std::string();
	}
	const int count = int( names.size() );
	if ( count > kMotionLimit )
	{
		error = vformat( "%d motions: a server runs at most %d, all its mods' sets together", count, kMotionLimit );
		return std::string();
	}
	// (No parts when a sheet with motions is kept inside the scene: the server reads sheets from files.)
	const bool files = std::all_of( recorded.sheets.begin(), recorded.sheets.end(), []( const sheet::OwnerParts::Sheet& s ) { return s.path.empty() == false; } );
	if ( parts != nullptr && files )
	{
		*parts = sheet::WriteParts( recorded, "The parts of the motion set " + std::string( name.utf8().get_data() ) +
												  "'s scene its motions name, and its sheets (M294): the server compiles the sheets. Edit the scene and bake again." );
	}
	return text;
}

PackedStringArray CbMotionSet::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	String error;
	if ( Bake( error ).empty() && error.is_empty() == false )
	{
		warnings.push_back( error );
	}
	return warnings;
}

Dictionary CbMotionSet::bake() const
{
	Dictionary result;
	String error;
	std::string parts;
	std::string text = Bake( error, &parts );
	// (How many: its lines that start one.)
	int motions = 0;
	for ( size_t at = text.find( "\nmotion\t" ); at != std::string::npos; at = text.find( "\nmotion\t", at + 1 ) )
	{
		motions += 1;
	}
	result["text"] = String::utf8( text.c_str() );
	result["parts"] = String::utf8( parts.c_str() );
	result["error"] = text.empty() ? error : String();
	result["motions"] = motions;
	return result;
}

void CbMotionSet::bake_to_project()
{
	Dictionary baked = bake();
	if ( String( baked["text"] ).is_empty() )
	{
		UtilityFunctions::push_error( "Motions bake failed: ", baked["error"] );
		return;
	}
	if ( String( baked["parts"] ).is_empty() )
	{
		UtilityFunctions::push_error( "Motions bake: a sheet with motions is kept inside the scene: save it as a .cbsheet file (the server compiles its motions from the file)" );
		return;
	}
	DirAccess::make_dir_recursive_absolute( "res://motions" );
	String path = "res://motions/" + m_name.strip_edges() + ".cfg";
	// (What the mod carries: its motions' parts, M294; the server compiles the motions from its sheets.)
	PackedByteArray bytes = String( baked["parts"] ).to_utf8_buffer();
	// A bake that changes nothing touches nothing: it runs on every save and every publish.
	if ( FileAccess::file_exists( path ) && FileAccess::get_file_as_bytes( path ) == bytes )
	{
		return;
	}
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::WRITE );
	if ( file.is_null() )
	{
		UtilityFunctions::push_error( "Motions bake: cannot write ", path );
		return;
	}
	file->store_buffer( bytes );
	UtilityFunctions::print( "Baked ", baked["motions"], " motions into ", path );
}

// --- The inspector's help -----------------------------------------------------------------------------

bool CbMotionInspector::_can_handle( Object* object ) const
{
	return Object::cast_to<CbMotionSet>( object ) != nullptr;
}

void CbMotionInspector::_parse_begin( Object* )
{
	add_custom_control( InfoRow( kSetInfo.brief, kSetInfo.detail ) );
}

} // namespace cb::gd
