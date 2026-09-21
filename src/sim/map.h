#pragma once

// Map files (.cbmap): the collision and spawn data of a level, baked from an authored scene.
//
// Maps are authored in the Godot editor and baked offline (tools/bake_map.ps1). The server never
// reads a Godot scene: it loads the baked file, and sends it to every client on join, so both
// sides build the level from exactly the same bytes.
//
// Everything is stored as little-endian fixed-point integers, because a map has to produce
// bit-identical floats on every platform and toolchain:
//   positions and extents: 1/1024 m     angles: 1/4096 rad
// Both grids are powers of two, so quantizing (v * grid, rounded) and dequantizing (i / grid) are
// exact operations, not approximations that a different libm could round differently.
//
// Layout:
//   "CBMP", u32 version, u32 flags, u32 nameLength, name bytes (no terminator),
//   i32 spawnCenter[3], i32 spawnRadius,
//   u32 staticCount,  per static: i32 center[3], i32 halfExtents[3], i32 pitch, i32 yaw
//   u32 propCount,    per prop:   u8 kind, u8 pad[3], i32 position[3], i32 halfExtents[3]
//   u32 templateCount, per template: name, visual, u32 componentCount,
//                        per component: u32 componentId, u32 fieldCount,
//                          per field: u32 fieldId, i32 value[3]
//   u32 instanceCount, per instance: u32 templateIndex, i32 position[3], i32 pitch, i32 yaw
//   u32 spawnTemplate (kNoTemplate for none)
//
// Component and field ids are name hashes (see reflect.h), so the registry can grow and be
// reordered without invalidating baked maps: unknown entries are dropped when the map is read.
//
// Names (of the map, of templates, and of the prefab a template draws as) are the only text in the
// file and the only parts the simulation ignores: they tell a client what to draw. Everything else
// is collision and initial state the server and clients must agree on.
//
// The order of the entries is the order in which entities are created, which decides flecs ids,
// Box3D body order and therefore every state hash. It is part of the format: never reorder a
// baked map without rebaking every client and the reference hashes.

#include "level.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cb
{

inline constexpr uint32_t kMapVersion = 2;

// Fixed-point grids. Powers of two, see above.
inline constexpr float kMapPositionScale = 1024.0f;
inline constexpr float kMapAngleScale = 4096.0f;

inline constexpr uint32_t kMapNameLimit = 64;

// Largest magnitude a quantized value can hold (int -> float stays exact below 2^24).
inline constexpr float kMapPositionLimit = 16000.0f;

// The grid conversions. Both are exact: the scales are powers of two, roundf is a specified exact
// operation, and int -> float is exact below 2^24. Authored component values use them too.
int32_t MapQuantize( float value, float scale );
float MapDequantize( int32_t value, float scale );

// Rounds every value in the layout onto the storage grid, so that serializing and reading it back
// yields exactly the same floats. Call this on any layout built in code.
void QuantizeLayout( LevelLayout& layout );

// `layout` must already be quantized; values off the grid are rounded (and large ones clamped).
void SerializeMap( const LevelLayout& layout, std::vector<uint8_t>& out );

// Returns false and sets `error` when the data is not a readable map.
bool DeserializeMap( const uint8_t* data, size_t size, LevelLayout& out, std::string& error );

// Identity of a map, for the join handshake and logs.
uint64_t MapHash( const uint8_t* data, size_t size );

// Reads a .cbmap file, keeping the raw bytes (the server forwards them to clients unchanged).
bool LoadMapFile( const std::string& path, LevelLayout& out, std::vector<uint8_t>& bytes, std::string& error );

bool WriteMapFile( const std::string& path, const std::vector<uint8_t>& bytes, std::string& error );

} // namespace cb
