#pragma once

// Access to Box3D internals that are not part of its public API. Implemented in C against the
// pinned Box3D sources, because the internal headers are C-only.

#include "box3d/id.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The b3World struct lives in Box3D's static world table, outside of any allocator.
size_t cbx_WorldStructSize( void );
void* cbx_WorldStructPtr( b3WorldId worldId );

// Portable physics state (no pointers), built on Box3D's replay snapshot serializer. Used to send
// the world to a joining client. Requires the same Box3D build settings and struct layout on both
// ends; the image carries a layout hash and loading fails on mismatch.
typedef void cbx_WriteFcn( void* context, const void* data, size_t size );
void cbx_SerializeWorld( b3WorldId worldId, cbx_WriteFcn* write, void* context );
// Where the sections of the last world serialized began, from the start of its snapshot (after
// the size cbx_SerializeWorld writes first): up to `capacity` of them; how many there are.
int cbx_SnapSections( const char** names, int* starts, int capacity );
// The sizes of Box3D's structs a snapshot holds, as this build lays them out ("b3Body 240 ...").
void cbx_StructSizes( char* out, int capacity );
// What a byte of a snapshot's contacts section is (`snap` the snapshot, `section` where the contacts
// begin in it, `at` the byte): which contact, live or free, its flags, and the field (of the
// contact, or of a manifold's point: which, and whether the manifold uses it).
void cbx_DescribeContactByte( const uint8_t* snap, int size, int section, int at, char* out, int capacity );

// Overwrites an existing world in place. Returns false on a corrupt or incompatible image.
// `keep` gives lasting memory for what the world keeps pointing at (a triangle mesh's, a height
// field's data): it outlives the call, and the world's own arena never has it.
typedef void* cbx_KeepFcn( void* context, size_t size );
bool cbx_DeserializeWorld( b3WorldId worldId, const uint8_t* data, size_t size, cbx_KeepFcn* keep, void* keepContext );

#ifdef __cplusplus
}
#endif
