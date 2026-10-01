#pragma once

// The peer extension: the sources that simulate, for a Godot viewer in another library.
//
// A CinderboxPeer plays on a server (connection, prediction, rollback) or re-simulates a recording,
// on a thread of its own, and hands the newest frame over as bytes (present/view_codec.h). It is
// what CinderboxClient.set_source() takes (see godot/object_source.h for the methods a source
// has); nothing but bytes crosses between the two libraries.
//
// This library holds the simulation and the networking. The viewer library holds neither.

#include "view.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>
#include <vector>

namespace cb::gd
{

class CinderboxPeer : public godot::RefCounted
{
	GDCLASS( CinderboxPeer, godot::RefCounted )

public:
	CinderboxPeer();
	~CinderboxPeer() override;

	// One source at a time; starting one stops the one before.
	// Plays on the server at host:port, as player_name.
	void connect_to_server();
	// Plays a recording (cb_server --record). A file system path (globalize res:// and user://).
	void open_replay( const godot::String& path );
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
	void set_rollback_min( int v )
	{
		m_rollbackMin = v;
	}
	int get_rollback_min() const
	{
		return m_rollbackMin;
	}
	void set_rollback_max( int v )
	{
		m_rollbackMax = v;
	}
	int get_rollback_max() const
	{
		return m_rollbackMax;
	}

protected:
	static void _bind_methods();

private:
	void Start( std::unique_ptr<present::ViewSource> source );

	godot::String m_host = "127.0.0.1";
	int m_port = 7777;
	godot::String m_playerName;
	int m_rollbackMin = 8;
	int m_rollbackMax = 20;

	std::unique_ptr<present::ViewSource> m_source;
	// The frame last handed over (the next delta's base) and the one being taken.
	present::ViewFrame m_frames[2];
	int m_current = 0;
	bool m_haveBase = false;
	std::vector<uint8_t> m_bytes;
};

} // namespace cb::gd
