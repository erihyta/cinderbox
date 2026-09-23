#pragma once

// HUD nodes that read the server mods' board, so a HUD is a scene and never a script.
//
// CbFieldLabel is a Label whose text is a format over the local player's fields
// ("AMMO {pistol.ammo}") and which shows only while its conditions hold ("combat.dead" for a death
// message, "loadout.slot == 2" for a crosshair). A mod's HUD can use it as it is: it carries no
// code, and the fields it names are whatever the server declared.

#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace cb::gd
{

class CinderboxClient;

class CbFieldLabel : public godot::Label
{
	GDCLASS( CbFieldLabel, godot::Label )

public:
	void _ready() override;
	void _process( double delta ) override;

	void set_text_format( const godot::String& value )
	{
		m_format = value;
	}
	godot::String get_text_format() const
	{
		return m_format;
	}
	void set_conditions( const godot::PackedStringArray& value )
	{
		m_conditions = value;
	}
	godot::PackedStringArray get_conditions() const
	{
		return m_conditions;
	}

protected:
	static void _bind_methods();

private:
	CinderboxClient* Client();

	// "{name}" is replaced by the field's value; empty leaves the label's own text alone.
	godot::String m_format;
	godot::PackedStringArray m_conditions;
	godot::ObjectID m_client;
};

} // namespace cb::gd
