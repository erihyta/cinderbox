#include "map.h"

#include "physics_arena.h"
#include "util.h"

#include "box3d/collision.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace cb
{

namespace
{

const char kMagic[4] = { 'C', 'B', 'M', 'P' };

} // namespace

int32_t MapQuantize( float value, float scale )
{
	float limit = kMapPositionLimit * scale;
	float scaled = value * scale;
	if ( scaled > limit )
	{
		scaled = limit;
	}
	if ( scaled < -limit )
	{
		scaled = -limit;
	}
	// Both the scaling (a power of two) and roundf (ties away from zero, always exact) are
	// specified operations, so every build produces the same integer.
	return int32_t( std::roundf( scaled ) );
}

float MapDequantize( int32_t value, float scale )
{
	return float( value ) * ( 1.0f / scale );
}

namespace
{

void Snap( float& value, float scale )
{
	value = MapDequantize( MapQuantize( value, scale ), scale );
}

void SnapVec( b3Vec3& v, float scale )
{
	Snap( v.x, scale );
	Snap( v.y, scale );
	Snap( v.z, scale );
}

void AppendU32( std::vector<uint8_t>& out, uint32_t value )
{
	out.push_back( uint8_t( value ) );
	out.push_back( uint8_t( value >> 8 ) );
	out.push_back( uint8_t( value >> 16 ) );
	out.push_back( uint8_t( value >> 24 ) );
}

void AppendI32( std::vector<uint8_t>& out, int32_t value )
{
	AppendU32( out, uint32_t( value ) );
}

void AppendString( std::vector<uint8_t>& out, const std::string& text )
{
	std::string clipped = text.substr( 0, kMapNameLimit );
	AppendU32( out, uint32_t( clipped.size() ) );
	out.insert( out.end(), clipped.begin(), clipped.end() );
}

void AppendPos( std::vector<uint8_t>& out, const b3Vec3& v )
{
	AppendI32( out, MapQuantize( v.x, kMapPositionScale ) );
	AppendI32( out, MapQuantize( v.y, kMapPositionScale ) );
	AppendI32( out, MapQuantize( v.z, kMapPositionScale ) );
}

struct Reader
{
	const uint8_t* data;
	size_t size;
	size_t cursor = 0;
	bool ok = true;

	uint32_t U32()
	{
		if ( cursor + 4 > size )
		{
			ok = false;
			return 0;
		}
		uint32_t v = uint32_t( data[cursor] ) | ( uint32_t( data[cursor + 1] ) << 8 ) | ( uint32_t( data[cursor + 2] ) << 16 ) |
					 ( uint32_t( data[cursor + 3] ) << 24 );
		cursor += 4;
		return v;
	}

	int32_t I32()
	{
		return int32_t( U32() );
	}

	uint8_t U8()
	{
		if ( cursor + 1 > size )
		{
			ok = false;
			return 0;
		}
		return data[cursor++];
	}

	b3Vec3 Pos()
	{
		b3Vec3 v;
		v.x = MapDequantize( I32(), kMapPositionScale );
		v.y = MapDequantize( I32(), kMapPositionScale );
		v.z = MapDequantize( I32(), kMapPositionScale );
		return v;
	}

	std::string Text()
	{
		uint32_t length = U32();
		if ( ok == false || length > kMapNameLimit || cursor + length > size )
		{
			ok = false;
			return {};
		}
		std::string text( reinterpret_cast<const char*>( data + cursor ), length );
		cursor += length;
		return text;
	}
};

} // namespace

bool MapHullBuilds( const std::vector<b3Vec3>& points )
{
	static thread_local PhysicsArena arena( size_t( 4 ) << 20 );
	PhysicsArena::Scope scope( arena );
	b3HullData* hull = b3CreateHull( points.data(), int( points.size() ), B3_MAX_HULL_VERTICES );
	if ( hull == nullptr )
	{
		return false;
	}
	b3DestroyHull( hull );
	return true;
}

namespace
{

void AppendPiece( std::vector<uint8_t>& out, const LevelBox& box )
{
	out.push_back( uint8_t( box.kind ) );
	out.push_back( 0 );
	out.push_back( 0 );
	out.push_back( 0 );
	AppendPos( out, box.center );
	AppendPos( out, box.halfExtents );
	AppendI32( out, MapQuantize( box.pitch, kMapAngleScale ) );
	AppendI32( out, MapQuantize( box.yaw, kMapAngleScale ) );
	AppendI32( out, MapQuantize( box.roll, kMapAngleScale ) );
	AppendU32( out, box.hull );
}

// A piece as `version` wrote it, checked: what Box3D could not build is refused.
bool ReadPiece( Reader& rd, uint32_t version, const LevelLayout& layout, LevelBox& box, std::string& error, bool level = false )
{
	if ( version >= 4 )
	{
		uint8_t kind = rd.U8();
		rd.U8();
		rd.U8();
		rd.U8();
		const bool surface = kind == uint8_t( ShapeKind::Mesh ) || kind == uint8_t( ShapeKind::HeightField );
		if ( kind > uint8_t( ShapeKind::HeightField ) || ( kind == uint8_t( ShapeKind::Hull ) && version < 5 ) ||
			 ( surface && ( version < 7 || level == false ) ) )
		{
			error = surface ? "a triangle mesh or height field can only be level geometry" : "unknown static shape";
			return false;
		}
		box.kind = ShapeKind( kind );
	}
	box.center = rd.Pos();
	box.halfExtents = rd.Pos();
	box.pitch = MapDequantize( rd.I32(), kMapAngleScale );
	box.yaw = MapDequantize( rd.I32(), kMapAngleScale );
	box.roll = version >= 4 ? MapDequantize( rd.I32(), kMapAngleScale ) : 0.0f;
	box.hull = version >= 5 ? rd.U32() : 0;
	if ( ( box.kind == ShapeKind::Hull && box.hull >= layout.hulls.size() ) || ( box.kind == ShapeKind::Mesh && box.hull >= layout.meshes.size() ) ||
		 ( box.kind == ShapeKind::HeightField && box.hull >= layout.heightFields.size() ) )
	{
		error = "a static names a hull, mesh or height field that is not in the map";
		return false;
	}
	// A box or sphere of no size, a capsule of no radius.
	const b3Vec3 h = box.halfExtents;
	bool boxy = box.kind != ShapeKind::Sphere && box.kind != ShapeKind::Capsule; // (a hull's, a mesh's: its bounds)
	bool solid = boxy ? ( h.x > 0.0f && h.y > 0.0f && h.z > 0.0f ) : ( h.x > 0.0f && h.y >= 0.0f );
	if ( solid == false )
	{
		error = "a static's size is out of range";
		return false;
	}
	return true;
}

// Names reach clients as resource paths (res://maps/<name>.tscn, res://prefabs/<visual>.tscn) and
// they come from the server, so they may not contain anything that walks out of those folders.
bool SafeName( const std::string& name )
{
	for ( char c : name )
	{
		bool allowed = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
		if ( allowed == false )
		{
			return false;
		}
	}
	return true;
}

} // namespace

void QuantizeLayout( LevelLayout& layout )
{
	for ( LevelBox& box : layout.statics )
	{
		SnapVec( box.center, kMapPositionScale );
		SnapVec( box.halfExtents, kMapPositionScale );
		Snap( box.pitch, kMapAngleScale );
		Snap( box.yaw, kMapAngleScale );
		Snap( box.roll, kMapAngleScale );
	}
	for ( std::vector<LevelBox>& set : layout.shapeSets )
	{
		for ( LevelBox& box : set )
		{
			SnapVec( box.center, kMapPositionScale );
			SnapVec( box.halfExtents, kMapPositionScale );
			Snap( box.pitch, kMapAngleScale );
			Snap( box.yaw, kMapAngleScale );
			Snap( box.roll, kMapAngleScale );
		}
	}
	for ( LevelInstance& instance : layout.instances )
	{
		Snap( instance.roll, kMapAngleScale );
	}
	for ( EntityTemplate& t : layout.templates )
	{
		for ( TemplatePiece& piece : t.pieces )
		{
			SnapVec( piece.position, kMapPositionScale );
			Snap( piece.pitch, kMapAngleScale );
			Snap( piece.yaw, kMapAngleScale );
			Snap( piece.roll, kMapAngleScale );
		}
	}
	for ( std::vector<b3Vec3>& hull : layout.hulls )
	{
		for ( b3Vec3& point : hull )
		{
			SnapVec( point, kMapPositionScale );
		}
	}
	for ( LevelMesh& mesh : layout.meshes )
	{
		for ( b3Vec3& vertex : mesh.vertices )
		{
			SnapVec( vertex, kMapPositionScale );
		}
	}
	for ( LevelHeightField& field : layout.heightFields )
	{
		SnapVec( field.scale, kMapPositionScale );
		for ( float& height : field.heights )
		{
			Snap( height, kMapPositionScale );
		}
	}
	for ( LevelProp& prop : layout.props )
	{
		SnapVec( prop.position, kMapPositionScale );
		SnapVec( prop.halfExtents, kMapPositionScale );
		Snap( prop.mass, kMapBodyScale );
		Snap( prop.density, kMapBodyScale );
		Snap( prop.friction, kMapBodyScale );
		Snap( prop.restitution, kMapBodyScale );
		Snap( prop.rolling, kMapBodyScale );
		Snap( prop.drag, kMapBodyScale );
	}
	SnapVec( layout.spawnCenter, kMapPositionScale );
	Snap( layout.spawnRadius, kMapPositionScale );
}

void SerializeMap( const LevelLayout& layout, std::vector<uint8_t>& out )
{
	out.clear();
	out.insert( out.end(), kMagic, kMagic + 4 );
	AppendU32( out, kMapVersion );
	AppendU32( out, 0 ); // flags

	std::string name = layout.name.substr( 0, kMapNameLimit );
	AppendU32( out, uint32_t( name.size() ) );
	out.insert( out.end(), name.begin(), name.end() );

	AppendPos( out, layout.spawnCenter );
	AppendI32( out, MapQuantize( layout.spawnRadius, kMapPositionScale ) );

	AppendU32( out, uint32_t( layout.hulls.size() ) );
	for ( const std::vector<b3Vec3>& hull : layout.hulls )
	{
		AppendU32( out, uint32_t( hull.size() ) );
		for ( const b3Vec3& point : hull )
		{
			AppendPos( out, point );
		}
	}

	AppendU32( out, uint32_t( layout.meshes.size() ) );
	for ( const LevelMesh& mesh : layout.meshes )
	{
		AppendU32( out, uint32_t( mesh.vertices.size() ) );
		for ( const b3Vec3& vertex : mesh.vertices )
		{
			AppendPos( out, vertex );
		}
		AppendU32( out, uint32_t( mesh.indices.size() / 3 ) );
		for ( size_t i = 0; i + 2 < mesh.indices.size(); i += 3 )
		{
			AppendU32( out, mesh.indices[i] );
			AppendU32( out, mesh.indices[i + 1] );
			AppendU32( out, mesh.indices[i + 2] );
		}
	}
	AppendU32( out, uint32_t( layout.heightFields.size() ) );
	for ( const LevelHeightField& field : layout.heightFields )
	{
		AppendU32( out, field.countX );
		AppendU32( out, field.countZ );
		AppendPos( out, field.scale );
		for ( float height : field.heights )
		{
			AppendI32( out, MapQuantize( height, kMapPositionScale ) );
		}
		uint32_t holes = 0;
		for ( uint8_t hole : field.holes )
		{
			holes += hole != 0 ? 1 : 0;
		}
		AppendU32( out, holes );
		for ( size_t cell = 0; cell < field.holes.size(); ++cell )
		{
			if ( field.holes[cell] != 0 )
			{
				AppendU32( out, uint32_t( cell ) );
			}
		}
	}

	AppendU32( out, uint32_t( layout.statics.size() ) );
	for ( const LevelBox& box : layout.statics )
	{
		AppendPiece( out, box );
	}

	AppendU32( out, uint32_t( layout.shapeSets.size() ) );
	for ( const std::vector<LevelBox>& set : layout.shapeSets )
	{
		AppendU32( out, uint32_t( set.size() ) );
		for ( const LevelBox& box : set )
		{
			AppendPiece( out, box );
		}
	}

	AppendU32( out, uint32_t( layout.props.size() ) );
	for ( const LevelProp& prop : layout.props )
	{
		out.push_back( uint8_t( prop.kind ) );
		out.push_back( 0 );
		out.push_back( 0 );
		out.push_back( 0 );
		AppendPos( out, prop.position );
		AppendPos( out, prop.halfExtents );
		for ( float value : { prop.mass, prop.density, prop.friction, prop.restitution, prop.rolling, prop.drag } )
		{
			AppendI32( out, MapQuantize( value, kMapBodyScale ) );
		}
	}

	AppendU32( out, uint32_t( layout.templates.size() ) );
	for ( const EntityTemplate& t : layout.templates )
	{
		AppendString( out, t.name );
		AppendString( out, t.visual );
		AppendU32( out, uint32_t( t.components.size() ) );
		for ( const AuthoredComponent& component : t.components )
		{
			AppendU32( out, component.id );
			AppendU32( out, uint32_t( component.fields.size() ) );
			for ( const AuthoredField& field : component.fields )
			{
				AppendU32( out, field.id );
				AppendI32( out, field.raw[0] );
				AppendI32( out, field.raw[1] );
				AppendI32( out, field.raw[2] );
			}
		}
		AppendU32( out, t.shapes );
		AppendU32( out, t.look );
		AppendU32( out, uint32_t( t.pieces.size() ) );
		for ( const TemplatePiece& piece : t.pieces )
		{
			AppendU32( out, piece.templateIndex );
			AppendPos( out, piece.position );
			AppendI32( out, MapQuantize( piece.pitch, kMapAngleScale ) );
			AppendI32( out, MapQuantize( piece.yaw, kMapAngleScale ) );
			AppendI32( out, MapQuantize( piece.roll, kMapAngleScale ) );
			AppendU32( out, piece.held ? 1u : 0u );
		}
		AppendU32( out, uint32_t( t.links.size() ) );
		for ( const PieceLink& link : t.links )
		{
			AppendU32( out, link.a );
			AppendU32( out, link.b );
		}
	}

	AppendU32( out, uint32_t( layout.instances.size() ) );
	for ( const LevelInstance& instance : layout.instances )
	{
		AppendU32( out, instance.templateIndex );
		AppendPos( out, instance.position );
		AppendI32( out, MapQuantize( instance.pitch, kMapAngleScale ) );
		AppendI32( out, MapQuantize( instance.yaw, kMapAngleScale ) );
		AppendI32( out, MapQuantize( instance.roll, kMapAngleScale ) );
	}

	AppendU32( out, layout.spawnTemplate );
}

bool DeserializeMap( const uint8_t* data, size_t size, LevelLayout& out, std::string& error )
{
	if ( data == nullptr || size < 12 || std::memcmp( data, kMagic, 4 ) != 0 )
	{
		error = "not a map file";
		return false;
	}

	Reader rd{ data, size, 4 };
	uint32_t version = rd.U32();
	if ( version < kMapOldestVersion || version > kMapVersion )
	{
		error = "map version " + std::to_string( version ) + ", expected " + std::to_string( kMapOldestVersion ) + " to " +
				std::to_string( kMapVersion );
		return false;
	}
	rd.U32(); // flags

	LevelLayout layout;
	layout.name = rd.Text();
	if ( rd.ok == false || SafeName( layout.name ) == false )
	{
		error = "bad map name";
		return false;
	}

	layout.spawnCenter = rd.Pos();
	layout.spawnRadius = MapDequantize( rd.I32(), kMapPositionScale );

	if ( version >= 5 )
	{
		uint32_t hullCount = rd.U32();
		// The smallest a hull can be is a count and four points.
		if ( rd.ok == false || hullCount > ( size - rd.cursor ) / 52 )
		{
			error = "corrupt hull list";
			return false;
		}
		layout.hulls.reserve( hullCount );
		for ( uint32_t i = 0; i < hullCount; ++i )
		{
			uint32_t pointCount = rd.U32();
			if ( rd.ok == false || pointCount < 4 || pointCount > kMapHullPointLimit || pointCount > ( size - rd.cursor ) / 12 )
			{
				error = "a hull has too few or too many points";
				return false;
			}
			std::vector<b3Vec3> hull( pointCount );
			for ( b3Vec3& point : hull )
			{
				point = rd.Pos();
			}
			if ( MapHullBuilds( hull ) == false )
			{
				error = "a hull's points make no solid (they lie in a plane)";
				return false;
			}
			layout.hulls.push_back( std::move( hull ) );
		}
	}

	if ( version >= 7 )
	{
		uint32_t meshCount = rd.U32();
		if ( rd.ok == false || meshCount > ( size - rd.cursor ) / 8 )
		{
			error = "corrupt mesh list";
			return false;
		}
		for ( uint32_t i = 0; i < meshCount; ++i )
		{
			LevelMesh mesh;
			uint32_t vertexCount = rd.U32();
			if ( rd.ok == false || vertexCount < 3 || vertexCount > kMapMeshVertexLimit || vertexCount > ( size - rd.cursor ) / 12 )
			{
				error = "a mesh has too few or too many vertices";
				return false;
			}
			mesh.vertices.resize( vertexCount );
			for ( b3Vec3& vertex : mesh.vertices )
			{
				vertex = rd.Pos();
			}
			uint32_t triangleCount = rd.U32();
			if ( rd.ok == false || triangleCount < 1 || triangleCount > kMapMeshTriangleLimit || triangleCount > ( size - rd.cursor ) / 12 )
			{
				error = "a mesh has no triangles, or too many";
				return false;
			}
			mesh.indices.resize( size_t( triangleCount ) * 3 );
			for ( uint32_t& index : mesh.indices )
			{
				index = rd.U32();
				if ( index >= vertexCount )
				{
					error = "a mesh's triangle names a vertex it does not have";
					return false;
				}
			}
			layout.meshes.push_back( std::move( mesh ) );
		}
		uint32_t fieldCount = rd.U32();
		if ( rd.ok == false || fieldCount > ( size - rd.cursor ) / 24 )
		{
			error = "corrupt height field list";
			return false;
		}
		for ( uint32_t i = 0; i < fieldCount; ++i )
		{
			LevelHeightField field;
			field.countX = rd.U32();
			field.countZ = rd.U32();
			field.scale = rd.Pos();
			const uint64_t points = uint64_t( field.countX ) * field.countZ;
			if ( rd.ok == false || field.countX < 2 || field.countZ < 2 || field.countX > kMapHeightFieldSideLimit ||
				 field.countZ > kMapHeightFieldSideLimit || points > ( size - rd.cursor ) / 4 || field.scale.x <= 0.0f || field.scale.y <= 0.0f ||
				 field.scale.z <= 0.0f )
			{
				error = "a height field's size is out of range";
				return false;
			}
			field.heights.resize( size_t( points ) );
			for ( float& height : field.heights )
			{
				height = MapDequantize( rd.I32(), kMapPositionScale );
			}
			const uint32_t cells = ( field.countX - 1 ) * ( field.countZ - 1 );
			uint32_t holes = rd.U32();
			if ( rd.ok == false || holes > cells || holes > ( size - rd.cursor ) / 4 )
			{
				error = "a height field has more holes than cells";
				return false;
			}
			if ( holes > 0 )
			{
				field.holes.assign( cells, 0 );
			}
			for ( uint32_t h = 0; h < holes; ++h )
			{
				uint32_t cell = rd.U32();
				if ( cell >= cells )
				{
					error = "a height field's hole is not one of its cells";
					return false;
				}
				field.holes[cell] = 1;
			}
			layout.heightFields.push_back( std::move( field ) );
		}
	}

	uint32_t staticCount = rd.U32();
	// Each entry is 44 bytes (40 in version 4, 32 before), so a count larger than the remaining
	// bytes is corrupt input.
	const size_t staticBytes = version >= 5 ? 44 : version >= 4 ? 40 : 32;
	if ( rd.ok == false || staticCount > ( size - rd.cursor ) / staticBytes )
	{
		error = "corrupt static list";
		return false;
	}
	layout.statics.reserve( staticCount );
	for ( uint32_t i = 0; i < staticCount; ++i )
	{
		LevelBox box;
		if ( ReadPiece( rd, version, layout, box, error, true ) == false )
		{
			return false;
		}
		layout.statics.push_back( box );
	}

	if ( version >= 6 )
	{
		uint32_t setCount = rd.U32();
		// The smallest a set can be is a count and one piece.
		if ( rd.ok == false || setCount > ( size - rd.cursor ) / 48 )
		{
			error = "corrupt shape set list";
			return false;
		}
		layout.shapeSets.reserve( setCount );
		for ( uint32_t i = 0; i < setCount; ++i )
		{
			uint32_t pieceCount = rd.U32();
			if ( rd.ok == false || pieceCount < 1 || pieceCount > kMapSetPieceLimit || pieceCount > ( size - rd.cursor ) / 44 )
			{
				error = "a body has no shapes, or too many";
				return false;
			}
			std::vector<LevelBox> set( pieceCount );
			for ( LevelBox& box : set )
			{
				if ( ReadPiece( rd, version, layout, box, error ) == false )
				{
					return false;
				}
			}
			layout.shapeSets.push_back( std::move( set ) );
		}
	}

	uint32_t propCount = rd.U32();
	if ( rd.ok == false || propCount > ( size - rd.cursor ) / ( version >= 8 ? 52 : 44 ) )
	{
		error = "corrupt prop list";
		return false;
	}
	layout.props.reserve( propCount );
	for ( uint32_t i = 0; i < propCount; ++i )
	{
		LevelProp prop;
		uint8_t kind = rd.U8();
		rd.U8();
		rd.U8();
		rd.U8();
		if ( kind > uint8_t( ShapeKind::Capsule ) )
		{
			error = "unknown prop shape";
			return false;
		}
		prop.kind = ShapeKind( kind );
		prop.position = rd.Pos();
		prop.halfExtents = rd.Pos();
		prop.mass = MapDequantize( rd.I32(), kMapBodyScale );
		prop.density = MapDequantize( rd.I32(), kMapBodyScale );
		prop.friction = MapDequantize( rd.I32(), kMapBodyScale );
		prop.restitution = MapDequantize( rd.I32(), kMapBodyScale );
		if ( version >= 8 )
		{
			prop.rolling = MapDequantize( rd.I32(), kMapBodyScale );
			prop.drag = MapDequantize( rd.I32(), kMapBodyScale );
		}
		if ( prop.rolling < 0.0f || prop.rolling > 1.0f || prop.drag < 0.0f || prop.drag > 100.0f || prop.mass < 0.0f || prop.mass > 100000.0f || prop.density < 0.001f || prop.density > 100000.0f || prop.friction < 0.0f ||
			 prop.friction > 10.0f || prop.restitution < 0.0f || prop.restitution > 1.0f )
		{
			error = "a prop's body is out of range";
			return false;
		}
		layout.props.push_back( prop );
	}

	uint32_t templateCount = rd.U32();
	// The smallest a template can be is two empty strings and a component count.
	if ( rd.ok == false || templateCount > ( size - rd.cursor ) / 12 )
	{
		error = "corrupt template list";
		return false;
	}
	layout.templates.reserve( templateCount );
	for ( uint32_t i = 0; i < templateCount; ++i )
	{
		EntityTemplate t;
		t.name = rd.Text();
		t.visual = rd.Text();
		if ( rd.ok == false || SafeName( t.name ) == false || SafeName( t.visual ) == false )
		{
			error = "bad template name";
			return false;
		}

		uint32_t componentCount = rd.U32();
		if ( rd.ok == false || componentCount > ( size - rd.cursor ) / 8 )
		{
			error = "corrupt template components";
			return false;
		}
		for ( uint32_t c = 0; c < componentCount; ++c )
		{
			AuthoredComponent component;
			component.id = rd.U32();
			uint32_t fieldCount = rd.U32();
			if ( rd.ok == false || fieldCount > ( size - rd.cursor ) / 16 )
			{
				error = "corrupt template fields";
				return false;
			}
			for ( uint32_t f = 0; f < fieldCount; ++f )
			{
				AuthoredField field;
				field.id = rd.U32();
				field.raw[0] = rd.I32();
				field.raw[1] = rd.I32();
				field.raw[2] = rd.I32();
				component.fields.push_back( field );
			}
			t.components.push_back( std::move( component ) );
		}
		if ( version >= 6 )
		{
			t.shapes = rd.U32();
			t.look = rd.U32();
			if ( t.shapes != kNoShapes && t.shapes >= layout.shapeSets.size() )
			{
				error = "a template names shapes that are not in the map";
				return false;
			}
		}
		if ( version >= 9 )
		{
			uint32_t pieces = rd.U32();
			if ( rd.ok == false || pieces > kMapPieceLimit || pieces > ( size - rd.cursor ) / 28 )
			{
				error = "a template has too many pieces";
				return false;
			}
			for ( uint32_t p = 0; p < pieces; ++p )
			{
				TemplatePiece piece;
				piece.templateIndex = rd.U32();
				piece.position = rd.Pos();
				piece.pitch = MapDequantize( rd.I32(), kMapAngleScale );
				piece.yaw = MapDequantize( rd.I32(), kMapAngleScale );
				piece.roll = MapDequantize( rd.I32(), kMapAngleScale );
				piece.held = version >= 10 && rd.U32() != 0;
				t.pieces.push_back( piece );
			}
			if ( version >= 10 )
			{
				uint32_t links = rd.U32();
				if ( rd.ok == false || links > kMapPieceLinkLimit || links > ( size - rd.cursor ) / 8 )
				{
					error = "a template has too many links between its pieces";
					return false;
				}
				for ( uint32_t l = 0; l < links; ++l )
				{
					uint32_t a = rd.U32();
					uint32_t b = rd.U32();
					if ( rd.ok == false || a >= b || b >= pieces )
					{
						error = "a link between pieces that a template does not have";
						return false;
					}
					t.links.push_back( { uint16_t( a ), uint16_t( b ) } );
				}
			}
		}
		// Values this build does not understand are dropped, and the rest is clamped to its range,
		// so a map can never push the simulation outside what the registry allows.
		SanitizeTemplate( t );
		layout.templates.push_back( std::move( t ) );
	}

	// A piece is a template of the map, and never a breakable itself (no breaking twice).
	for ( const EntityTemplate& t : layout.templates )
	{
		for ( const TemplatePiece& piece : t.pieces )
		{
			if ( piece.templateIndex >= layout.templates.size() || layout.templates[piece.templateIndex].pieces.empty() == false )
			{
				error = "a breakable's piece is not a template of the map, or breaks itself";
				return false;
			}
		}
	}

	uint32_t instanceCount = rd.U32();
	if ( rd.ok == false || instanceCount > ( size - rd.cursor ) / ( version >= 6 ? 28 : 24 ) )
	{
		error = "corrupt instance list";
		return false;
	}
	layout.instances.reserve( instanceCount );
	for ( uint32_t i = 0; i < instanceCount; ++i )
	{
		LevelInstance instance;
		instance.templateIndex = rd.U32();
		instance.position = rd.Pos();
		instance.pitch = MapDequantize( rd.I32(), kMapAngleScale );
		instance.yaw = MapDequantize( rd.I32(), kMapAngleScale );
		instance.roll = version >= 6 ? MapDequantize( rd.I32(), kMapAngleScale ) : 0.0f;
		if ( instance.templateIndex >= layout.templates.size() )
		{
			error = "instance refers to a template that is not in the map";
			return false;
		}
		layout.instances.push_back( instance );
	}

	layout.spawnTemplate = rd.U32();
	if ( layout.spawnTemplate != kNoTemplate && layout.spawnTemplate >= layout.templates.size() )
	{
		error = "spawn template is not in the map";
		return false;
	}

	if ( rd.ok == false )
	{
		error = "truncated map";
		return false;
	}
	out = std::move( layout );
	return true;
}

uint64_t MapHash( const uint8_t* data, size_t size )
{
	return HashBytes( kHashSeed, data, size );
}

bool LoadMapFile( const std::string& path, LevelLayout& out, std::vector<uint8_t>& bytes, std::string& error )
{
	FILE* file = std::fopen( path.c_str(), "rb" );
	if ( file == nullptr )
	{
		error = "cannot open " + path;
		return false;
	}
	bytes.clear();
	uint8_t buffer[4096];
	size_t read = 0;
	while ( ( read = std::fread( buffer, 1, sizeof( buffer ), file ) ) > 0 )
	{
		bytes.insert( bytes.end(), buffer, buffer + read );
	}
	std::fclose( file );
	return DeserializeMap( bytes.data(), bytes.size(), out, error );
}

bool WriteMapFile( const std::string& path, const std::vector<uint8_t>& bytes, std::string& error )
{
	FILE* file = std::fopen( path.c_str(), "wb" );
	if ( file == nullptr )
	{
		error = "cannot write " + path;
		return false;
	}
	size_t written = std::fwrite( bytes.data(), 1, bytes.size(), file );
	std::fclose( file );
	if ( written != bytes.size() )
	{
		error = "short write to " + path;
		return false;
	}
	return true;
}

} // namespace cb
