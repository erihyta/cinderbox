#include "map.h"

#include "util.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace cb
{

namespace
{

const char kMagic[4] = { 'C', 'B', 'M', 'P' };

int32_t Quantize( float value, float scale )
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

float Dequantize( int32_t value, float scale )
{
	return float( value ) * ( 1.0f / scale );
}

void Snap( float& value, float scale )
{
	value = Dequantize( Quantize( value, scale ), scale );
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

void AppendPos( std::vector<uint8_t>& out, const b3Vec3& v )
{
	AppendI32( out, Quantize( v.x, kMapPositionScale ) );
	AppendI32( out, Quantize( v.y, kMapPositionScale ) );
	AppendI32( out, Quantize( v.z, kMapPositionScale ) );
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
		v.x = Dequantize( I32(), kMapPositionScale );
		v.y = Dequantize( I32(), kMapPositionScale );
		v.z = Dequantize( I32(), kMapPositionScale );
		return v;
	}
};

} // namespace

void QuantizeLayout( LevelLayout& layout )
{
	for ( LevelBox& box : layout.statics )
	{
		SnapVec( box.center, kMapPositionScale );
		SnapVec( box.halfExtents, kMapPositionScale );
		Snap( box.pitch, kMapAngleScale );
		Snap( box.yaw, kMapAngleScale );
	}
	for ( LevelProp& prop : layout.props )
	{
		SnapVec( prop.position, kMapPositionScale );
		SnapVec( prop.halfExtents, kMapPositionScale );
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
	AppendI32( out, Quantize( layout.spawnRadius, kMapPositionScale ) );

	AppendU32( out, uint32_t( layout.statics.size() ) );
	for ( const LevelBox& box : layout.statics )
	{
		AppendPos( out, box.center );
		AppendPos( out, box.halfExtents );
		AppendI32( out, Quantize( box.pitch, kMapAngleScale ) );
		AppendI32( out, Quantize( box.yaw, kMapAngleScale ) );
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
	}
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
	if ( version != kMapVersion )
	{
		error = "map version " + std::to_string( version ) + ", expected " + std::to_string( kMapVersion );
		return false;
	}
	rd.U32(); // flags

	LevelLayout layout;
	uint32_t nameLength = rd.U32();
	if ( rd.ok == false || nameLength > kMapNameLimit || rd.cursor + nameLength > size )
	{
		error = "bad map name";
		return false;
	}
	layout.name.assign( reinterpret_cast<const char*>( data + rd.cursor ), nameLength );
	rd.cursor += nameLength;
	// Clients turn the name into a resource path, and the map comes from the server, so keep it to
	// characters that cannot walk out of the maps folder.
	for ( char c : layout.name )
	{
		bool allowed = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
		if ( allowed == false )
		{
			error = "map name has characters outside a-z, A-Z, 0-9, _ and -";
			return false;
		}
	}

	layout.spawnCenter = rd.Pos();
	layout.spawnRadius = Dequantize( rd.I32(), kMapPositionScale );

	uint32_t staticCount = rd.U32();
	// Each entry is 32 bytes, so a count larger than the remaining bytes is corrupt input.
	if ( rd.ok == false || staticCount > ( size - rd.cursor ) / 32 )
	{
		error = "corrupt static list";
		return false;
	}
	layout.statics.reserve( staticCount );
	for ( uint32_t i = 0; i < staticCount; ++i )
	{
		LevelBox box;
		box.center = rd.Pos();
		box.halfExtents = rd.Pos();
		box.pitch = Dequantize( rd.I32(), kMapAngleScale );
		box.yaw = Dequantize( rd.I32(), kMapAngleScale );
		layout.statics.push_back( box );
	}

	uint32_t propCount = rd.U32();
	if ( rd.ok == false || propCount > ( size - rd.cursor ) / 28 )
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
		layout.props.push_back( prop );
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
