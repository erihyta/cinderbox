#pragma once

// The stream extension: a source for a Godot viewer that plays on a server without simulating
// (stream/stream_source.h). The server sends the frames to draw; this library holds the networking
// and the view codec, and no simulation. A game that ships only the viewer and this cannot predict
// and cannot be shown more than the server sends it.
//
// A CinderboxStream is what CinderboxClient.set_source() takes (godot/object_source.h), like the
// peer extension's CinderboxPeer.

#include "packet_handoff.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>

namespace cb::gd
{

class CinderboxStream : public godot::RefCounted
{
	GDCLASS( CinderboxStream, godot::RefCounted )

public:
	CinderboxStream();
	~CinderboxStream() override;

	// Plays on the server at host:port, as player_name.
	void connect_to_server();
	void stop();
	bool is_running() const;

	// The source's side of the viewer protocol.
	godot::PackedByteArray take( bool whole );
	bool takes_input() const;
	void set_input( const godot::PackedByteArray& input );
	void control( const godot::String& name, double value );

	void set_host( const godot::String& v )
	{
		m_host = v;
	}
	godot::String get_host() const
	{
		return m_host;
	}
	void set_port( int v )
	{
		m_port = v;
	}
	int get_port() const
	{
		return m_port;
	}
	void set_player_name( const godot::String& v )
	{
		m_playerName = v;
	}
	godot::String get_player_name() const
	{
		return m_playerName;
	}

protected:
	static void _bind_methods();

private:
	godot::String m_host = "127.0.0.1";
	int m_port = 7777;
	godot::String m_playerName;

	std::unique_ptr<present::ViewSource> m_source;
	PacketHandoff m_handoff;
};

} // namespace cb::gd
