#include "box3d_shim.h"

#include "core.h"
#include "physics_world.h"
#include "recording.h"
#include "recording_replay.h"
#include "world_snapshot.h"
#include "body.h"
#include "constraint_graph.h"
#include "contact.h"
#include "island.h"
#include "joint.h"
#include "shape.h"
#include "solver_set.h"

#include <stdio.h>

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

extern int b3SnapSectionCount;
extern const char* b3SnapSectionNames[];
extern int b3SnapSectionStarts[];

void cbx_StructSizes( char* out, int capacity )
{
	snprintf( out, (size_t)capacity,
			  "b3Body %d, b3BodySim %d, b3BodyState %d, b3Shape %d, b3Contact %d, b3Manifold %d, b3Joint %d, b3JointSim %d, "
			  "b3Island %d, b3IslandSim %d, b3TreeNode %d, b3GraphColor %d",
			  (int)sizeof( b3Body ), (int)sizeof( b3BodySim ), (int)sizeof( b3BodyState ), (int)sizeof( b3Shape ),
			  (int)sizeof( b3Contact ), (int)sizeof( b3Manifold ), (int)sizeof( b3Joint ), (int)sizeof( b3JointSim ),
			  (int)sizeof( b3Island ), (int)sizeof( b3IslandSim ), (int)sizeof( b3TreeNode ), (int)sizeof( b3GraphColor ) );
}

void cbx_DescribeContactByte( const uint8_t* snap, int size, int section, int at, char* out, int capacity )
{
	int cursor = section;
	int count = 0;
	if ( cursor + 4 > size )
	{
		snprintf( out, (size_t)capacity, "no contacts section" );
		return;
	}
	memcpy( &count, snap + cursor, 4 );
	cursor += 4;
	for ( int i = 0; i < count; ++i )
	{
		if ( cursor + (int)sizeof( b3Contact ) + 4 > size )
		{
			break;
		}
		b3Contact c;
		memcpy( &c, snap + cursor, sizeof( b3Contact ) );
		const bool live = c.contactId == i;
		const int start = cursor;
		cursor += (int)sizeof( b3Contact );
		if ( at >= start && at < cursor )
		{
			const int o = at - start;
			snprintf( out, (size_t)capacity,
					  "contact %d (%s, flags %u, manifolds %d), its byte %d (flags at %d, manifoldCount at %d, cached at %d, "
					  "friction at %d, the cache at %d, restitution at %d)",
					  i, live ? "live" : "free", c.flags, c.manifoldCount, o, (int)offsetof( b3Contact, flags ),
					  (int)offsetof( b3Contact, manifoldCount ), (int)offsetof( b3Contact, cachedRotationA ),
					  (int)offsetof( b3Contact, friction ), (int)offsetof( b3Contact, convexContact ),
					  (int)offsetof( b3Contact, restitution ) );
			return;
		}
		int manifoldCount = 0;
		memcpy( &manifoldCount, snap + cursor, 4 );
		cursor += 4;
		if ( live == false )
		{
			continue;
		}
		for ( int m = 0; m < manifoldCount && cursor + (int)sizeof( b3Manifold ) <= size; ++m )
		{
			if ( at >= cursor && at < cursor + (int)sizeof( b3Manifold ) )
			{
				b3Manifold manifold;
				memcpy( &manifold, snap + cursor, sizeof( b3Manifold ) );
				const int o = at - cursor;
				const int point = o / (int)sizeof( b3ManifoldPoint );
				snprintf( out, (size_t)capacity,
						  "contact %d (live, flags %u), manifold %d of %d (points used %d), its byte %d: point %d's byte %d "
						  "(normalImpulse at %d, normalVelocity at %d, featureId at %d; points end at %d)",
						  i, c.flags, m, manifoldCount, manifold.pointCount, o, point, o - point * (int)sizeof( b3ManifoldPoint ),
						  (int)offsetof( b3ManifoldPoint, normalImpulse ), (int)offsetof( b3ManifoldPoint, normalVelocity ),
						  (int)offsetof( b3ManifoldPoint, featureId ), (int)offsetof( b3Manifold, normal ) );
				return;
			}
			cursor += (int)sizeof( b3Manifold );
		}
		if ( c.flags & b3_simMeshContact )
		{
			int caches = 0;
			memcpy( &caches, snap + cursor, 4 );
			cursor += 4;
			if ( at >= cursor && at < cursor + caches * (int)sizeof( b3TriangleCache ) )
			{
				snprintf( out, (size_t)capacity, "contact %d (live, mesh), triangle cache %d, its byte %d", i,
						  ( at - cursor ) / (int)sizeof( b3TriangleCache ), ( at - cursor ) % (int)sizeof( b3TriangleCache ) );
				return;
			}
			cursor += caches * (int)sizeof( b3TriangleCache );
		}
	}
	snprintf( out, (size_t)capacity, "past the contacts read (%d of them)", count );
}

int cbx_SnapSections( const char** names, int* starts, int capacity )
{
	for ( int i = 0; i < b3SnapSectionCount && i < capacity; ++i )
	{
		names[i] = b3SnapSectionNames[i];
		starts[i] = b3SnapSectionStarts[i];
	}
	return b3SnapSectionCount;
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

bool cbx_DeserializeWorld( b3WorldId worldId, const uint8_t* data, size_t size, cbx_KeepFcn* keep, void* keepContext )
{
	Cursor c = { data, size, 0, true };

	uint32_t snapSize = ReadU32( &c );
	const uint8_t* snap = Take( &c, snapSize );
	uint32_t slotCount = ReadU32( &c );
	if ( c.ok == false )
	{
		return false;
	}

	// Slots borrow the input bytes. Hulls are cloned into the world; a mesh or a height field is used
	// where it is, so it gets lasting memory of its own (`keep`). This project makes no compounds.
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
		const bool kept = kind == (uint32_t)b3_geometryMesh || kind == (uint32_t)b3_geometryHeightField;
		if ( ( kind != (uint32_t)b3_geometryHull && kept == false ) || bytes == NULL )
		{
			c.ok = false;
			break;
		}
		if ( kept )
		{
			uint8_t* copy = keep != NULL ? (uint8_t*)keep( keepContext, byteCount ) : NULL;
			if ( copy == NULL )
			{
				c.ok = false;
				break;
			}
			memcpy( copy, bytes, byteCount );
			bytes = copy;
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
