#pragma once

// SHA-256 (FIPS 180-4), for checking that a workshop item on disk is the one its manifest names.

#include <cstddef>
#include <cstdint>
#include <string>

namespace cb
{

// 64 lowercase hex digits.
std::string Sha256Hex( const void* data, size_t size );

} // namespace cb
