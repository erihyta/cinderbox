#pragma once

// A sheet as a resource (M284; docs/step6-plan.md): a .cbsheet file, plain text, read and written by
// the extension's own loader and saver (as .gd and .gdshader are by Godot's), set on the node that
// uses it (CbRules.sheet) as a resource, not a path in a string: Godot knows the dependency, a sheet
// moved or renamed in the editor keeps working, and a pack carries it with the scene. What a sheet
// says is the sheet compiler's (src/sheet); this is its Godot side. The editor's Sheet screen is
// cinderbox_sheet.h.

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>

namespace cb::gd
{

class CbSheet : public godot::Resource
{
	GDCLASS( CbSheet, godot::Resource )

public:
	void set_text( const godot::String& text );
	godot::String get_text() const
	{
		return m_text;
	}

protected:
	static void _bind_methods();

private:
	godot::String m_text;
};

class CbSheetLoader : public godot::ResourceFormatLoader
{
	GDCLASS( CbSheetLoader, godot::ResourceFormatLoader )

public:
	godot::PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type( const godot::StringName& type ) const override;
	godot::String _get_resource_type( const godot::String& path ) const override;
	// No id of its own (-1): a scene names a sheet by its path, as a mod's pack has no ids to find
	// one by (an id in an exported scene is a warning in the game); so Godot writes no .uid file.
	int64_t _get_resource_uid( const godot::String& ) const override
	{
		return -1;
	}
	godot::Variant _load( const godot::String& path, const godot::String& original_path, bool use_sub_threads, int32_t cache_mode ) const override;

protected:
	static void _bind_methods()
	{
	}
};

class CbSheetSaver : public godot::ResourceFormatSaver
{
	GDCLASS( CbSheetSaver, godot::ResourceFormatSaver )

public:
	godot::Error _save( const godot::Ref<godot::Resource>& resource, const godot::String& path, uint32_t flags ) override;
	bool _recognize( const godot::Ref<godot::Resource>& resource ) const override;
	godot::PackedStringArray _get_recognized_extensions( const godot::Ref<godot::Resource>& resource ) const override;

protected:
	static void _bind_methods()
	{
	}
};

// The loader and the saver, added to Godot's once the extension's classes are (register_types.cpp).
void AddSheetFormats();
void RemoveSheetFormats();

} // namespace cb::gd
