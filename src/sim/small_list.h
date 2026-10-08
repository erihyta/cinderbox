#pragma once

// A list of plain values that is as long as it needs to be: the first N live in the object, more
// than that on the heap. For state whose size a server's mods decide (how many fields an entity
// has, how many layers a character's animation, how many motions a player): the usual sizes cost
// no allocation, and no size is a wall.
//
// Reading past the end gives a default value and writing past the end makes the list longer, so a
// list that was never told its size behaves like one of any size that is all defaults. Two lists
// are equal when they hold the same values, whatever their lengths (trailing defaults do not
// count).

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace cb
{

template <typename T, size_t N>
class SmallList
{
	static_assert( std::is_trivially_copyable_v<T>, "SmallList holds plain values" );

public:
	SmallList() = default;

	SmallList( const SmallList& other )
	{
		Assign( other.data(), other.m_size );
	}

	SmallList( SmallList&& other ) noexcept
	{
		Take( other );
	}

	SmallList& operator=( const SmallList& other )
	{
		if ( this != &other )
		{
			Assign( other.data(), other.m_size );
		}
		return *this;
	}

	SmallList& operator=( SmallList&& other ) noexcept
	{
		if ( this != &other )
		{
			delete[] m_heap;
			m_heap = nullptr;
			m_capacity = N;
			Take( other );
		}
		return *this;
	}

	~SmallList()
	{
		delete[] m_heap;
	}

	size_t size() const
	{
		return m_size;
	}

	bool empty() const
	{
		return m_size == 0;
	}

	const T* data() const
	{
		return m_heap != nullptr ? m_heap : m_inline;
	}

	T* data()
	{
		return m_heap != nullptr ? m_heap : m_inline;
	}

	const T* begin() const
	{
		return data();
	}

	const T* end() const
	{
		return data() + m_size;
	}

	T* begin()
	{
		return data();
	}

	T* end()
	{
		return data() + m_size;
	}

	// Past the end: a default value.
	T operator[]( size_t i ) const
	{
		return i < m_size ? data()[i] : T{};
	}

	// Past the end: the list grows to hold it.
	T& operator[]( size_t i )
	{
		if ( i >= m_size )
		{
			resize( i + 1 );
		}
		return data()[i];
	}

	// Null past the end (a reader that wants a reference without growing the list).
	const T* at( size_t i ) const
	{
		return i < m_size ? data() + i : nullptr;
	}

	void resize( size_t n )
	{
		if ( n > m_capacity )
		{
			size_t capacity = m_capacity * 2 > n ? m_capacity * 2 : n;
			T* grown = new T[capacity];
			std::memcpy( static_cast<void*>( grown ), data(), m_size * sizeof( T ) );
			delete[] m_heap;
			m_heap = grown;
			m_capacity = capacity;
		}
		for ( size_t i = m_size; i < n; ++i )
		{
			data()[i] = T{};
		}
		m_size = n;
	}

	void clear()
	{
		m_size = 0;
	}

	void assign( const T* values, size_t n )
	{
		Assign( values, n );
	}

	// Copies the first n values out, defaults where the list is shorter.
	void copy_to( T* out, size_t n ) const
	{
		size_t have = m_size < n ? m_size : n;
		std::memcpy( static_cast<void*>( out ), data(), have * sizeof( T ) );
		for ( size_t i = have; i < n; ++i )
		{
			out[i] = T{};
		}
	}

	bool operator==( const SmallList& other ) const
	{
		size_t common = m_size < other.m_size ? m_size : other.m_size;
		if ( std::memcmp( data(), other.data(), common * sizeof( T ) ) != 0 )
		{
			return false;
		}
		const SmallList& longer = m_size > other.m_size ? *this : other;
		const T none{};
		for ( size_t i = common; i < longer.m_size; ++i )
		{
			if ( std::memcmp( longer.data() + i, &none, sizeof( T ) ) != 0 )
			{
				return false;
			}
		}
		return true;
	}

	bool operator!=( const SmallList& other ) const
	{
		return !( *this == other );
	}

private:
	void Assign( const T* values, size_t n )
	{
		if ( n > m_capacity )
		{
			delete[] m_heap;
			m_heap = new T[n];
			m_capacity = n;
		}
		std::memcpy( static_cast<void*>( data() ), values, n * sizeof( T ) );
		m_size = n;
	}

	void Take( SmallList& other )
	{
		if ( other.m_heap != nullptr )
		{
			m_heap = other.m_heap;
			m_capacity = other.m_capacity;
			other.m_heap = nullptr;
			other.m_capacity = N;
		}
		else
		{
			std::memcpy( static_cast<void*>( m_inline ), other.m_inline, other.m_size * sizeof( T ) );
		}
		m_size = other.m_size;
		other.m_size = 0;
	}

	size_t m_size = 0;
	size_t m_capacity = N;
	T* m_heap = nullptr;
	T m_inline[N] = {};
};

} // namespace cb
