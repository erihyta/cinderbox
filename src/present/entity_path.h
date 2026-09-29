#pragma once

// Entity paths: how presentation data names an entity relative to another, through the
// simulation's relations rather than a scene tree ("the item in my holder's left hand", "whoever
// this event hit").
//
//     self               the entity the data belongs to (the default: an empty path is self)
//     holder             the player holding this item
//     item:RightHand     the item in that socket
//     event.a, event.b   the entities an event names (who it is about, the other one)
//     local              the local player
//     world              no entity: the map, and the global board
//
// Steps chain with '/': "holder/item:LeftHand", "event.b/item:RightHand". A path that does not
// start with self, event.a, event.b, local or world starts at self ("holder" is "self/holder").
// "world" stands alone. A step that finds nothing (an empty hand, no event) makes the whole path
// find nothing.
//
// Conditions may read another entity's board with a path and a colon: "holder:melee.hot",
// "!event.b:combat.dead", "event.b:combat.health < 20", "world:deathmatch.round".

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cb::present
{

struct EntityPath
{
	enum class Step : uint8_t
	{
		Self,
		Holder,
		Item,
		EventA,
		EventB,
		Local,
		World,
	};
	struct Part
	{
		Step step = Step::Self;
		std::string socket; // Item
	};
	std::vector<Part> parts; // empty: self

	bool UsesEvent() const
	{
		return parts.empty() == false && ( parts[0].step == Step::EventA || parts[0].step == Step::EventB );
	}
	bool IsWorld() const
	{
		return parts.size() == 1 && parts[0].step == Step::World;
	}
};

// False (with a reason) for a path that cannot mean anything.
bool ParseEntityPath( const std::string& text, EntityPath& out, std::string* error = nullptr );

// What resolving needs to know about the world.
struct PathContext
{
	uint32_t self = 0;
	uint32_t eventA = 0; // 0 outside an event
	uint32_t eventB = 0;
	uint32_t local = 0;
	std::function<uint32_t( uint32_t netId )> holderOf;							  // 0: not a held item
	std::function<uint32_t( uint32_t holder, const std::string& socket )> itemIn; // 0: nothing there
};

struct PathTarget
{
	bool world = false;
	uint32_t netId = 0;

	bool Found() const
	{
		return world || netId != 0;
	}
};

PathTarget ResolveEntityPath( const EntityPath& path, const PathContext& context );

// A condition that may start with a path: "!holder:melee.hot" is the path "holder" and the plain
// condition "!melee.hot".
struct PathCondition
{
	EntityPath path;
	std::string condition;
};
bool ParsePathCondition( const std::string& text, PathCondition& out, std::string* error = nullptr );

} // namespace cb::present
