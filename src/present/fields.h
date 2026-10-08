#pragma once

// Reading the server mods' board by name, for data-driven presentation.
//
// Bindings, labels and attachments are written against names ("pistol.ammo", "combat.dead"), never
// against what a pistol is. These helpers resolve a name through the schema the server sent and
// read the value from an entity's board (or the global one), so a condition such as
// "pistol.ammo > 0" means the same thing in every renderer.
//
// A condition is an expression of the one expression language (expr/expr.h): names, numbers,
// comparisons, arithmetic, and / or / not, "?name" for "the server declared it":
//     pistol.ammo > 0        !combat.dead        combat.health < combat.max_health / 2
//     ?deathmatch.score      pistol.gun and not pistol.reloading
// A field the server did not declare reads as zero, so a binding for a mod that is not running
// simply never matches. A condition that does not parse is false.
// A private field (BoardScope::Private) reads as its value for the viewer's own player and as zero
// for everyone else: nobody else's was sent.

#include "components.h"
#include "mod_schema.h"

#include <functional>
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

// `board` may be null (the entity has published nothing), and `globals` (the game's board) too.
// `privates`: the private fields of the entity when it is the viewer's own player (null for anyone
// else: a private field of someone else reads as 0, because the viewer was never sent it).
FieldValue ReadField( const ModSchema& schema, const std::string& name, const Blackboard* board, const BoardValues* globals,
					  const Blackboard* privates = nullptr );

// Names the caller knows that are not board fields ("event.value"): true with the value when known.
using ExtraFields = std::function<bool( const std::string& name, float& value )>;

bool CheckCondition( const ModSchema& schema, const std::string& condition, const Blackboard* board, const BoardValues* globals,
					 const ExtraFields* extra = nullptr, const Blackboard* privates = nullptr );
// The value of an expression over the same names ("combat.health * 100 / combat.max_health");
// false when it does not parse.
bool EvaluateFields( const ModSchema& schema, const std::string& expression, const Blackboard* board, const BoardValues* globals, float& out,
					 const ExtraFields* extra = nullptr, const Blackboard* privates = nullptr );
// True when every condition holds (and when there are none).
bool CheckConditions( const ModSchema& schema, const std::vector<std::string>& conditions, const Blackboard* board,
					  const BoardValues* globals, const Blackboard* privates = nullptr );

// Replaces every {name} in `format` with the field's value: integers as integers, floats with one
// decimal, booleans as "yes" / "no". {an expression} is replaced with its value. "{{" is a literal
// brace.
std::string FormatFields( const ModSchema& schema, const std::string& format, const Blackboard* board, const BoardValues* globals,
						  const Blackboard* privates = nullptr );

} // namespace cb::present
