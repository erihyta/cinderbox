#pragma once

// What every source object does for a viewer in another library (object_source.h): take the newest
// frame from a view source and hand it over as a packet, a delta against the one handed over
// before. The peer extension uses it.

#include "view.h"

#include <godot_cpp/variant/packed_byte_array.hpp>

#include <vector>

namespace cb::gd
{

class PacketHandoff
{
public:
	// Forget what was handed over (a new source).
	void Reset();
	// The source's newest frame as a packet, or empty when it has none newer. `whole` asks for a
	// packet that stands alone: the newest frame again if there is no newer one.
	godot::PackedByteArray Take( present::ViewSource& source, bool whole );

private:
	// The frame last handed over (the next delta's base) and the one being taken.
	present::ViewFrame m_frames[2];
	int m_current = 0;
	bool m_haveBase = false;
	std::vector<uint8_t> m_bytes;
};

} // namespace cb::gd
