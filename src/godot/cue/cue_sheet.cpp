#include "cue_sheet.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

// --- The resource ----------------------------------------------------------------------------------

void CbSheet::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_text", "text" ), &CbSheet::set_text );
	ClassDB::bind_method( D_METHOD( "get_text" ), &CbSheet::get_text );
	// (Edited in the Sheet screen, not the inspector. A .cbsheet file is the text, whole (the saver's);
	// stored by Godot only for a sheet built into a scene, which CbRules warns about.)
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "text", PROPERTY_HINT_MULTILINE_TEXT, "", PROPERTY_USAGE_NO_EDITOR ), "set_text", "get_text" );
}

void CbSheet::set_text( const String& text )
{
	if ( text == m_text )
	{
		return;
	}
	m_text = text;
	emit_changed(); // (the nodes that use it build again)
}

// --- Its file -------------------------------------------------------------------------------------

PackedStringArray CbSheetLoader::_get_recognized_extensions() const
{
	return PackedStringArray( { "cbsheet" } );
}

bool CbSheetLoader::_handles_type( const StringName& type ) const
{
	return type == StringName( "CbSheet" ) || type == StringName( "Resource" );
}

String CbSheetLoader::_get_resource_type( const String& path ) const
{
	return path.get_extension().to_lower() == "cbsheet" ? String( "CbSheet" ) : String();
}

Variant CbSheetLoader::_load( const String& path, const String&, bool, int32_t ) const
{
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() )
	{
		return int64_t( ERR_FILE_CANT_OPEN );
	}
	Ref<CbSheet> sheet;
	sheet.instantiate();
	sheet->set_text( file->get_as_text() );
	return sheet;
}

Error CbSheetSaver::_save( const Ref<Resource>& resource, const String& path, uint32_t )
{
	Ref<CbSheet> sheet = resource;
	if ( sheet.is_null() )
	{
		return ERR_INVALID_PARAMETER;
	}
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::WRITE );
	if ( file.is_null() )
	{
		return ERR_FILE_CANT_WRITE;
	}
	file->store_string( sheet->get_text() );
	return OK;
}

bool CbSheetSaver::_recognize( const Ref<Resource>& resource ) const
{
	return Object::cast_to<CbSheet>( resource.ptr() ) != nullptr;
}

PackedStringArray CbSheetSaver::_get_recognized_extensions( const Ref<Resource>& resource ) const
{
	return _recognize( resource ) ? PackedStringArray( { "cbsheet" } ) : PackedStringArray();
}

namespace
{

Ref<CbSheetLoader>& TheLoader()
{
	static Ref<CbSheetLoader> loader;
	return loader;
}

Ref<CbSheetSaver>& TheSaver()
{
	static Ref<CbSheetSaver> saver;
	return saver;
}

} // namespace

void AddSheetFormats()
{
	TheLoader().instantiate();
	TheSaver().instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader( TheLoader() );
	ResourceSaver::get_singleton()->add_resource_format_saver( TheSaver() );
}

void RemoveSheetFormats()
{
	if ( TheLoader().is_valid() )
	{
		ResourceLoader::get_singleton()->remove_resource_format_loader( TheLoader() );
		TheLoader().unref();
	}
	if ( TheSaver().is_valid() )
	{
		ResourceSaver::get_singleton()->remove_resource_format_saver( TheSaver() );
		TheSaver().unref();
	}
}

} // namespace cb::gd
