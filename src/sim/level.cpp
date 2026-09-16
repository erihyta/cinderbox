#include "level.h"

#include "detmath.h"

namespace cb
{

namespace
{

LevelLayout BuildLayout()
{
	LevelLayout L;
	const float arenaHalf = 30.0f;
	const float wallHeight = 2.0f;
	const float wallThickness = 0.5f;

	// Ground slab (top surface at y = 0)
	L.statics.push_back( { { 0.0f, -0.5f, 0.0f }, { arenaHalf, 0.5f, arenaHalf }, 0.0f, 0.0f } );

	// Enclosing walls
	L.statics.push_back( { { 0.0f, wallHeight, arenaHalf + wallThickness }, { arenaHalf, wallHeight, wallThickness }, 0.0f, 0.0f } );
	L.statics.push_back( { { 0.0f, wallHeight, -arenaHalf - wallThickness }, { arenaHalf, wallHeight, wallThickness }, 0.0f, 0.0f } );
	L.statics.push_back( { { arenaHalf + wallThickness, wallHeight, 0.0f }, { wallThickness, wallHeight, arenaHalf }, 0.0f, 0.0f } );
	L.statics.push_back( { { -arenaHalf - wallThickness, wallHeight, 0.0f }, { wallThickness, wallHeight, arenaHalf }, 0.0f, 0.0f } );

	// Ramps of increasing steepness along +X
	const float slopes[] = { 0.25f, 0.45f, 0.8f };
	for ( int i = 0; i < 3; ++i )
	{
		float x = 10.0f + 5.0f * float( i );
		float pitch = slopes[i];
		b3CosSin cs = detmath::CosSin( pitch );
		float halfLen = 4.0f;
		// Place so the low edge touches the ground at z = 8
		float y = cs.sine * halfLen - 0.2f;
		float z = 8.0f + cs.cosine * halfLen;
		L.statics.push_back( { { x, y, z }, { 1.5f, 0.2f, halfLen }, -pitch, 0.0f } );
	}

	// Staircases with different step heights along -X
	const float stepHeights[] = { 0.15f, 0.3f, 0.5f };
	for ( int s = 0; s < 3; ++s )
	{
		float x = -10.0f - 5.0f * float( s );
		float h = stepHeights[s];
		for ( int i = 0; i < 6; ++i )
		{
			float top = h * float( i + 1 );
			L.statics.push_back( { { x, 0.5f * top, 8.0f + 0.8f * float( i ) }, { 1.5f, 0.5f * top, 0.4f }, 0.0f, 0.0f } );
		}
	}

	// Platforms at various heights
	L.statics.push_back( { { -12.0f, 1.0f, -12.0f }, { 3.0f, 0.25f, 3.0f }, 0.0f, 0.0f } );
	L.statics.push_back( { { -5.0f, 2.0f, -15.0f }, { 2.0f, 0.25f, 2.0f }, 0.0f, 0.0f } );
	L.statics.push_back( { { 2.0f, 3.0f, -18.0f }, { 2.0f, 0.25f, 2.0f }, 0.0f, 0.5f } );
	L.statics.push_back( { { 12.0f, 0.6f, -12.0f }, { 4.0f, 0.6f, 4.0f }, 0.0f, 0.0f } );

	// Box pyramid
	const float half = 0.4f;
	for ( int row = 0; row < 5; ++row )
	{
		for ( int i = 0; i < 5 - row; ++i )
		{
			float x = -2.0f + ( float( i ) + 0.5f * float( row ) ) * ( 2.0f * half + 0.02f );
			float y = half + float( row ) * ( 2.0f * half + 0.01f );
			L.props.push_back( { ShapeKind::Box, { x, y, -6.0f }, { half, half, half } } );
		}
	}

	// A few balls
	for ( int i = 0; i < 6; ++i )
	{
		L.props.push_back( { ShapeKind::Sphere, { 4.0f + 1.2f * float( i ), 0.5f, -3.0f }, { 0.5f, 0.0f, 0.0f } } );
	}

	L.spawnCenter = { 0.0f, 1.5f, 4.0f };
	L.spawnRadius = 4.0f;
	return L;
}

} // namespace

const LevelLayout& GetLevelLayout()
{
	static const LevelLayout layout = BuildLayout();
	return layout;
}

} // namespace cb
