#pragma once

// What the server's mods declared: the names behind board slots, mod event types and action bits.
//
// The simulation stores numbers (board slot 3, event type 1, action bit 0). The schema is how
// everyone else puts names on them: the server builds it from its mods at startup and sends it to
// every client on join, and presentation looks fields, events and actions up by name. A client
// therefore never needs to know what a pistol is to show "pistol.ammo" or play "pistol.fired".
//
// It is not simulation state and is never hashed: two servers with different mods can run the same
// build, and a client simply shows what its server declared.

#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cb
{

enum class BoardType : uint8_t
{
	Int = 0,
	Float = 1, // stored as the float's bits
	Bool = 2,
};

enum class BoardScope : uint8_t
{
	Entity = 0, // one value per entity (Blackboard component)
	Global = 1, // one value for the whole game (SimGlobals::board)
};

struct BoardField
{
	std::string name; // "pistol.ammo": mod name, a dot, then the field
	BoardType type = BoardType::Int;
	BoardScope scope = BoardScope::Entity;
	uint8_t slot = 0;

	bool operator==( const BoardField& ) const = default;
};

struct ModAction
{
	std::string name; // "fire"
	uint8_t bit = 0;  // bit in PlayerInput::actions
	// Suggested binding, for clients to use until the player changes it: a key name as Godot spells
	// it ("F", "R", "1", "Space"), or "MouseLeft" / "MouseRight" / "MouseMiddle".
	std::string key;

	bool operator==( const ModAction& ) const = default;
};

struct ModSchema
{
	std::vector<std::string> mods; // names of the mods the server runs, for display
	std::vector<BoardField> fields;
	std::vector<std::string> events; // index = ModEventRecord::type
	std::vector<ModAction> actions;

	bool operator==( const ModSchema& ) const = default;

	const BoardField* FindField( const std::string& name ) const;
	// -1 when not declared.
	int FindEvent( const std::string& name ) const;
	const ModAction* FindAction( const std::string& name ) const;
	// Bit mask of the named action, 0 when not declared.
	uint16_t ActionMask( const std::string& name ) const;
};

inline constexpr size_t kMaxSchemaName = 64;

void EncodeSchema( const ModSchema& schema, std::vector<uint8_t>& out );
// Rejects anything out of range (slots, bits, name lengths), so a client can trust what it read.
bool DecodeSchema( const uint8_t* data, size_t size, ModSchema& out );

// Board values are int32 on the wire and in the state; these are the float conversions.
inline int32_t BoardFromFloat( float value )
{
	int32_t bits;
	std::memcpy( &bits, &value, sizeof( bits ) );
	return bits;
}

inline float BoardToFloat( int32_t bits )
{
	float value;
	std::memcpy( &value, &bits, sizeof( value ) );
	return value;
}

} // namespace cb
