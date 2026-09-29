#pragma once

// Reading the server mods' board by name, for data-driven presentation.
//
// Bindings, labels and attachments are written against names ("pistol.ammo", "combat.dead"), never
// against what a pistol is. These helpers resolve a name through the schema the server sent and
// read the value from an entity's board (or the global one), so a condition such as
// "pistol.ammo > 0" means the same thing in every renderer.
//
// A condition is one of:
//     name                 true when the value is not zero
//     !name                true when it is zero
//     ?name                true when the server declared the field (its mod is running)
//     !?name               true when it did not
//     name <op> number     op is one of == != > >= < <= ; "true" and "false" count as 1 and 0
// A field the server did not declare reads as zero, so a binding for a mod that is not running
// simply never matches.

#include "components.h"
#include "mod_schema.h"

#include <string>
#include <vector>

namespace cb::present
{

struct FieldValue
{
	BoardType type = BoardType::Int;
	int32_t raw = 0;
	bool declared = false;

	float AsFloat() const
	{
		return type == BoardType::Float ? BoardToFloat( raw ) : float( raw );
	}
	bool AsBool() const
	{
		return type == BoardType::Float ? BoardToFloat( raw ) != 0.0f : raw != 0;
	}
};

// `board` may be null (the entity has published nothing), `globals` holds kBoardSlots values.
FieldValue ReadField( const ModSchema& schema, const std::string& name, const Blackboard* board, const int32_t* globals );

bool CheckCondition( const ModSchema& schema, const std::string& condition, const Blackboard* board, const int32_t* globals );
// True when every condition holds (and when there are none).
bool CheckConditions( const ModSchema& schema, const std::vector<std::string>& conditions, const Blackboard* board,
					  const int32_t* globals );

// Replaces every {name} in `format` with the field's value: integers as integers, floats with one
// decimal, booleans as "yes" / "no". "{{" is a literal brace.
std::string FormatFields( const ModSchema& schema, const std::string& format, const Blackboard* board, const int32_t* globals );

} // namespace cb::present
