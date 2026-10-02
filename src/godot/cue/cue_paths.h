#pragma once

// Cue paths and conditions: how reactions name nodes and test state. Plain Godot, no Cinderbox.
//
// A cue path is a Godot NodePath whose first name may be an anchor:
//     Barrel, ../Sparks      an ordinary path from the reaction
//     ^                      its entity: the nearest entity node at or above the reaction
//     ^^, ^^^                the entity above that one, and so on (a held item's holder)
//     $at, $other            the entities a cue names (who it is about, the other one)
//     $local                 the local player's entity
//     $world                 the director (the World node)
//     <anchor>@field         the entity whose id is in that entity's state field: "$local@pickup.target"
//                            (what the local player may pick up); "@field" is "^@field"
// followed by an ordinary path from there: "^^/RightHand/Item", "$other/Head". A path that finds
// nothing, or a node outside the director's tree, is nothing.
//
// An entity is any node the director was told about (add_entity): it carries its kind, its
// template and its state (a Dictionary of named values, "melee.hot" -> true) as metadata.
//
// A condition tests state by name:
//     name / !name           the value is not zero / is zero
//     ?name / !?name         the name is known / is not
//     name <op> number       op is one of == != > >= < <= ; true and false count as 1 and 0
// Plain names read the subject's state, then the world's. A cue path and a colon read another
// entity's: "^^:combat.dead", "$other:combat.health < 20", "$world:deathmatch.round".
// is_local, and event.<arg> ("event.value") are known too.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <functional>

namespace cb::cue
{

// What a reaction is resolved against: the cue that fired it (if any) and the well-known nodes.
struct Context
{
	godot::Node* director = nullptr; // $world, and the tree nothing may leave
	godot::Node* local = nullptr;
	bool event = false;
	// The cue is the viewer's own press, shown before the server answered (cue_prediction.h): it
	// names who it is at and nothing else. No point, no end, no value, no other entity.
	bool predicted = false;
	godot::Node* at = nullptr;
	godot::Node* other = nullptr;
	godot::Vector3 point;
	godot::Vector3 end;
	godot::Dictionary args;
};

inline constexpr const char* kEntityMeta = "cue_entity";
inline constexpr const char* kKindMeta = "cue_kind";
inline constexpr const char* kTemplateMeta = "cue_template";
inline constexpr const char* kStateMeta = "state";
inline constexpr const char* kIdsMeta = "cue_ids"; // on the director: { id: instance id }

bool IsEntity( const godot::Node* node );
// The entity at or above `node` (stopping at `stop`, which is not searched), or null.
godot::Node* EntityOf( godot::Node* node, const godot::Node* stop );

// False (with a reason) for a path that cannot mean anything.
bool CheckPath( const godot::NodePath& path, godot::String* error );
// True when the path starts from the cue's entities ($at, $other).
bool FromCue( const godot::NodePath& path );
godot::Node* Resolve( const godot::NodePath& path, godot::Node* origin, const Context& context );

struct Condition
{
	bool hasPath = false;
	godot::NodePath path;
	godot::String test; // the condition without its path: "!melee.hot"
};
bool ParseCondition( const godot::String& text, Condition& out, godot::String* error );
// `lookup` gives a name's value, false when the name is not known.
bool Test( const godot::String& test, const std::function<bool( const godot::String&, godot::Variant& )>& lookup );

// A name's value for `entity` (null: the world): is_local, event.<arg>, the entity's state, the
// world's state (kept on the director), or a name the director knows of (zero). False: unknown.
bool LookUp( const godot::String& name, godot::Node* entity, const Context& context, godot::Variant& out );
inline constexpr const char* kKnownMeta = "cue_known";

} // namespace cb::cue
