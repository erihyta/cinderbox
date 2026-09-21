#pragma once

// The authorable component registry: what a map author may attach to an entity, and what its
// fields mean.
//
// This is a schema, not storage. It exists so that one description drives three things:
//   - the Godot inspector (a CbComponent node builds its properties from this registry),
//   - the baker, which writes authored values into a .cbmap,
//   - the simulation, which applies them when it creates the entity.
// Adding a field here makes it appear in the editor with no Godot-side code, which is what keeps
// authoring drag-and-drop only.
//
// A schema entry is not always a flecs component. Some describe how the entity is created (Body,
// Material feed Box3D's body and shape definitions) and some do map to components (Velocity,
// Prop). What they have in common is that they are initial values, never runtime state.
//
// Values are stored quantized on the map's fixed-point grid, so an authored number is the same on
// every platform. A field the author did not set is absent, and the simulation keeps the engine
// default; that is how a map baked today still loads when a field is added tomorrow.

#include "box3d/math_functions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cb
{

// FNV-1a 32, used to identify components and fields by name in files. Names are the stable
// identity: entries may be reordered freely, but never renamed without breaking baked maps.
constexpr uint32_t Fnv32( const char* text )
{
	uint32_t hash = 0x811c9dc5u;
	for ( const char* p = text; *p != '\0'; ++p )
	{
		hash ^= uint32_t( uint8_t( *p ) );
		hash *= 0x01000193u;
	}
	return hash;
}

enum class FieldType : uint8_t
{
	Float = 0,
	Int = 1,
	Bool = 2,
	Enum = 3,
	Vec3 = 4,
};

struct FieldDef
{
	const char* name = "";
	FieldType type = FieldType::Float;
	// Shown in the editor and used when the simulation asks for a value that was not authored.
	float defaults[3] = { 0.0f, 0.0f, 0.0f };
	float min = -16000.0f;
	float max = 16000.0f;
	// "Box,Sphere,Capsule" for Enum fields, otherwise nullptr.
	const char* enumNames = nullptr;
	const char* doc = "";

	uint32_t Id() const
	{
		return Fnv32( name );
	}
};

struct ComponentDef
{
	const char* name = "";
	const char* doc = "";
	std::vector<FieldDef> fields;

	uint32_t Id() const
	{
		return Fnv32( name );
	}
	const FieldDef* Field( uint32_t fieldId ) const;
};

// Every component an author may attach, in the order the editor lists them.
const std::vector<ComponentDef>& AuthorableComponents();
const ComponentDef* FindComponent( uint32_t componentId );
const ComponentDef* FindComponent( const std::string& name );

// --- Authored values -----------------------------------------------------------------------------

// One field as it sits in a map file: quantized, and identified by name hash so that reordering or
// adding fields never invalidates a baked map.
struct AuthoredField
{
	uint32_t id = 0;
	int32_t raw[3] = { 0, 0, 0 };
};

struct AuthoredComponent
{
	uint32_t id = 0;
	std::vector<AuthoredField> fields;
};

// A named entity definition a map can place and the server can spawn at runtime.
struct EntityTemplate
{
	std::string name;
	// Prefab a client draws for it (presentation only; the simulation ignores it).
	std::string visual;
	std::vector<AuthoredComponent> components;

	const AuthoredComponent* Find( uint32_t componentId ) const;
};

// Readers used by the simulation. `fallback` is returned when the field was not authored, so
// callers can pass the engine's own default.
bool TemplateHas( const EntityTemplate& t, uint32_t componentId );
float TemplateFloat( const EntityTemplate& t, uint32_t componentId, uint32_t fieldId, float fallback );
int32_t TemplateInt( const EntityTemplate& t, uint32_t componentId, uint32_t fieldId, int32_t fallback );
bool TemplateBool( const EntityTemplate& t, uint32_t componentId, uint32_t fieldId, bool fallback );
b3Vec3 TemplateVec3( const EntityTemplate& t, uint32_t componentId, uint32_t fieldId, b3Vec3 fallback );

// Drops fields and components the registry does not know, and clamps values to their range. Run on
// anything that came from a file before the simulation sees it.
void SanitizeTemplate( EntityTemplate& t );

} // namespace cb
