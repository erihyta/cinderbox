#pragma once

#include <cstddef>
#include <cstdint>

namespace cb
{

// FNV-1a 64-bit. Used for state hashes; stable across platforms.
inline constexpr uint64_t kHashSeed = 0xcbf29ce484222325ull;

inline uint64_t HashBytes( uint64_t hash, const void* data, size_t size )
{
	const auto* p = static_cast<const uint8_t*>( data );
	for ( size_t i = 0; i < size; ++i )
	{
		hash ^= p[i];
		hash *= 0x100000001b3ull;
	}
	return hash;
}

// SplitMix64: tiny, fully specified, state is a single uint64 stored in the simulation.
inline uint64_t NextRandom( uint64_t& state )
{
	uint64_t z = ( state += 0x9e3779b97f4a7c15ull );
	z = ( z ^ ( z >> 30 ) ) * 0xbf58476d1ce4e5b9ull;
	z = ( z ^ ( z >> 27 ) ) * 0x94d049bb133111ebull;
	return z ^ ( z >> 31 );
}

// Uniform float in [0, 1). Uses the top 24 bits so the int -> float conversion is exact.
inline float RandomUnit( uint64_t& state )
{
	return float( NextRandom( state ) >> 40 ) * ( 1.0f / 16777216.0f );
}

inline float RandomRange( uint64_t& state, float lo, float hi )
{
	return lo + ( hi - lo ) * RandomUnit( state );
}

} // namespace cb
