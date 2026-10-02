#include "packet_handoff.h"

#include "view_codec.h"

#include <cstring>

using namespace godot;

namespace cb::gd
{

void PacketHandoff::Reset()
{
	m_frames[0] = present::ViewFrame();
	m_frames[1] = present::ViewFrame();
	m_current = 0;
	m_haveBase = false;
}

PackedByteArray PacketHandoff::Take( present::ViewSource& source, bool whole )
{
	PackedByteArray out;
	// A source hands a frame over once. A viewer that asks for a whole one (it is new, or it lost
	// its place) gets the newest frame again, standing alone.
	present::ViewFrame& next = m_frames[1 - m_current];
	next.serial = m_frames[m_current].serial;
	if ( source.Take( next ) )
	{
		const present::ViewFrame* base = m_haveBase && whole == false ? &m_frames[m_current] : nullptr;
		present::EncodeView( next, base, present::ViewClock() - next.publishedAt, m_bytes );
		m_current = 1 - m_current;
		m_haveBase = true;
	}
	else if ( whole && m_haveBase )
	{
		const present::ViewFrame& again = m_frames[m_current];
		present::EncodeView( again, nullptr, present::ViewClock() - again.publishedAt, m_bytes );
	}
	else
	{
		return out;
	}
	out.resize( int64_t( m_bytes.size() ) );
	std::memcpy( out.ptrw(), m_bytes.data(), m_bytes.size() );
	return out;
}

} // namespace cb::gd
