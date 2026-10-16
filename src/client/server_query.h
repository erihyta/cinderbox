#pragma once

// Asking a server what it is, before joining: its map, how many play, and the workshop items its
// mods need (net::MsgInfo). The asker is never a player: it connects, asks, takes the answer and
// leaves. A menu lists the items, fetches the missing ones from the workshop (never from the
// server), and only then joins.
//
//   ServerQuery q;
//   q.Start( "play.example.org", 7777, now );
//   while ( q.Update( now ) == false ) { ... }      // a few hundred milliseconds
//   q.Ok() ? q.Info() : q.Error()

#include "protocol.h"
#include "transport.h"

#include <memory>
#include <string>

namespace cb
{

class ServerQuery
{
public:
	// False (and Error() says why) when the address does not resolve.
	bool Start( const std::string& host, uint16_t port, double now );
	// Polls; true once there is an answer or none will come.
	bool Update( double now );

	bool Done() const
	{
		return m_done;
	}
	bool Ok() const
	{
		return m_done && m_error.empty();
	}
	const std::string& Error() const
	{
		return m_error;
	}
	const net::MsgInfo& Info() const
	{
		return m_info;
	}
	// Whether this game could join it: the same protocol and simulation build.
	bool Compatible() const;

	// How long to wait for an answer.
	static constexpr double kTimeoutSeconds = 5.0;

private:
	void Finish( const std::string& error );

	std::unique_ptr<net::Transport> m_transport;
	double m_started = 0.0;
	bool m_done = false;
	std::string m_error;
	net::MsgInfo m_info;
	std::string m_address;
};

} // namespace cb
