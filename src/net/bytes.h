#pragma once

// Minimal binary serialization. All supported targets are little-endian, so PODs are written raw.

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace cb::net
{

class ByteWriter
{
public:
	explicit ByteWriter( std::vector<uint8_t>& out )
		: m_out( out )
	{
	}

	template <typename T>
	void Write( const T& value )
	{
		static_assert( std::is_trivially_copyable_v<T> );
		WriteBytes( &value, sizeof( T ) );
	}

	void WriteBytes( const void* data, size_t size )
	{
		const auto* p = static_cast<const uint8_t*>( data );
		m_out.insert( m_out.end(), p, p + size );
	}

	void WriteBlob( const std::vector<uint8_t>& blob )
	{
		Write( uint32_t( blob.size() ) );
		WriteBytes( blob.data(), blob.size() );
	}

private:
	std::vector<uint8_t>& m_out;
};

class ByteReader
{
public:
	ByteReader( const uint8_t* data, size_t size )
		: m_data( data )
		, m_size( size )
	{
	}

	template <typename T>
	T Read()
	{
		static_assert( std::is_trivially_copyable_v<T> );
		T value{};
		if ( const uint8_t* p = Take( sizeof( T ) ) )
		{
			std::memcpy( &value, p, sizeof( T ) );
		}
		return value;
	}

	const uint8_t* Take( size_t n )
	{
		if ( m_ok == false || m_cursor + n > m_size )
		{
			m_ok = false;
			return nullptr;
		}
		const uint8_t* p = m_data + m_cursor;
		m_cursor += n;
		return p;
	}

	void ReadBlob( std::vector<uint8_t>& out )
	{
		uint32_t size = Read<uint32_t>();
		const uint8_t* p = Take( size );
		if ( p != nullptr )
		{
			out.assign( p, p + size );
		}
	}

	bool Ok() const
	{
		return m_ok;
	}
	bool AtEnd() const
	{
		return m_cursor == m_size;
	}

private:
	const uint8_t* m_data;
	size_t m_size;
	size_t m_cursor = 0;
	bool m_ok = true;
};

} // namespace cb::net
