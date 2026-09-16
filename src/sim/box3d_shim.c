#include "box3d_shim.h"

#include "core.h"
#include "physics_world.h"
#include "recording.h"
#include "recording_replay.h"
#include "world_snapshot.h"

#include "box3d/box3d.h"

#include <stdlib.h>
#include <string.h>

// Image layout: u32 snapshotSize, snapshot bytes, u32 geometryCount, then per geometry:
// u32 kind, u32 byteCount, bytes.

size_t cbx_WorldStructSize( void )
{
	return sizeof( b3World );
}

void* cbx_WorldStructPtr( b3WorldId worldId )
{
	return b3GetWorldFromId( worldId );
}

static void WriteU32( cbx_WriteFcn* write, void* context, uint32_t value )
{
	write( context, &value, sizeof( value ) );
}

void cbx_SerializeWorld( b3WorldId worldId, cbx_WriteFcn* write, void* context )
{
	b3World* world = b3GetWorldFromId( worldId );
	b3Recording* rec = b3CreateRecording( 0 );
	b3RecBuffer buf = { 0 };

	b3SerializeWorld( world, &buf, rec );

	WriteU32( write, context, (uint32_t)buf.size );
	write( context, buf.data, (size_t)buf.size );

	const b3GeometryRegistry* reg = &rec->registry;
	WriteU32( write, context, (uint32_t)reg->entries.count );
	for ( int i = 0; i < reg->entries.count; ++i )
	{
		const b3GeometryEntry* e = reg->entries.data + i;
		WriteU32( write, context, (uint32_t)e->kind );
		WriteU32( write, context, (uint32_t)e->byteCount );
		write( context, e->bytes, (size_t)e->byteCount );
	}

	b3RecBufFree( &buf );
	b3DestroyRecording( rec );
}

typedef struct Cursor
{
	const uint8_t* data;
	size_t size;
	size_t at;
	bool ok;
} Cursor;

static const uint8_t* Take( Cursor* c, size_t n )
{
	if ( c->ok == false || c->at + n > c->size )
	{
		c->ok = false;
		return NULL;
	}
	const uint8_t* p = c->data + c->at;
	c->at += n;
	return p;
}

static uint32_t ReadU32( Cursor* c )
{
	uint32_t v = 0;
	const uint8_t* p = Take( c, sizeof( v ) );
	if ( p != NULL )
	{
		memcpy( &v, p, sizeof( v ) );
	}
	return v;
}

bool cbx_DeserializeWorld( b3WorldId worldId, const uint8_t* data, size_t size )
{
	Cursor c = { data, size, 0, true };

	uint32_t snapSize = ReadU32( &c );
	const uint8_t* snap = Take( &c, snapSize );
	uint32_t slotCount = ReadU32( &c );
	if ( c.ok == false )
	{
		return false;
	}

	// Slots borrow the input bytes. Hulls are cloned into the world, and this project does not use
	// meshes, height fields or compounds (which would keep referencing the slot memory).
	b3RegistrySlot* slots = NULL;
	if ( slotCount > 0 )
	{
		slots = (b3RegistrySlot*)calloc( slotCount, sizeof( b3RegistrySlot ) );
		if ( slots == NULL )
		{
			return false;
		}
	}

	for ( uint32_t i = 0; i < slotCount && c.ok; ++i )
	{
		uint32_t kind = ReadU32( &c );
		uint32_t byteCount = ReadU32( &c );
		const uint8_t* bytes = Take( &c, byteCount );
		if ( kind != (uint32_t)b3_geometryHull )
		{
			c.ok = false;
			break;
		}
		slots[i].kind = (b3GeometryKind)kind;
		slots[i].byteCount = (int)byteCount;
		slots[i].bytes = (uint8_t*)bytes;
		slots[i].live = NULL;
	}

	bool ok = c.ok;
	if ( ok )
	{
		b3RecReader rdr;
		memset( &rdr, 0, sizeof( rdr ) );
		rdr.replayWorldId = worldId;
		rdr.ok = true;
		rdr.slots = slots;
		rdr.slotCount = (int)slotCount;
		ok = b3DeserializeIntoShell( snap, (int)snapSize, b3GetWorldFromId( worldId ), &rdr );
	}

	free( slots );
	return ok;
}
