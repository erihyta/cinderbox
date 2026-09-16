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

// Overwrites an existing world in place. Returns false on a corrupt or incompatible image.
bool cbx_DeserializeWorld( b3WorldId worldId, const uint8_t* data, size_t size );

#ifdef __cplusplus
}
#endif
