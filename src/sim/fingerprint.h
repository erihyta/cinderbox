#pragma once

#include <cstdint>

namespace cb
{

// Hash of a short scripted run with a fixed config. Two builds that disagree on this cannot play
// together, so the server compares it at connect time. Computed once and cached.
uint64_t BuildFingerprint();

} // namespace cb
