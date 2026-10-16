#include "server_query.h"

#include "fingerprint.h"

namespace cb
{

using namespace net;

bool ServerQuery::Start( const std::string& host, uint16_t port, double now )
{
	m_done = false;
	m_error.clear();
	m_info = {};
	m_started = now;
	m_address = host + ":" + std::to_string( port );
	EnsureNetworkInitialized();
	m_transport = std::make_unique<Transport>();
	if ( Transport::Resolves( host ) == false || m_transport->Connect( host, port ) == false )
	{
		Finish( "The address \"" + host + "\" could not be found." );
		return false;
	}
	return true;
}

bool ServerQuery::Update( double now )
{
	if ( m_done || m_transport == nullptr )
	{
		return true;
	}
	std::vector<NetEvent> events;
	m_transport->Poll( events );
	for ( const NetEvent& ev : events )
	{
		if ( ev.type == NetEvent::Type::Connected )
		{
			std::vector<uint8_t> bytes;
			Encode( MsgInfoRequest{}, bytes );
			m_transport->Send( ev.peer, ChannelReliable, bytes, true );
			m_transport->Flush();
		}
		else if ( ev.type == NetEvent::Type::Disconnected )
		{
			Finish( "No answer from " + m_address + "." );
		}
		else
		{
			ByteReader r( ev.data.data(), ev.data.size() );
			auto type = ReadType( r );
			if ( type && *type == MsgType::Info )
			{
				MsgInfo info;
				bool read = Decode( r, info );
				m_info = info;
				Finish( read ? std::string() : info.version != kProtocolVersion
													? "This server runs another version of the game (protocol " + std::to_string( info.version ) +
														  ", this game " + std::to_string( kProtocolVersion ) + ")."
													: "The server's answer could not be read." );
			}
			else if ( type && *type == MsgType::Reject )
			{
				MsgReject reject;
				Decode( r, reject );
				Finish( "The server refused: " + reject.reason );
			}
		}
		if ( m_done )
		{
			break;
		}
	}
	if ( m_done == false && now - m_started > kTimeoutSeconds )
	{
		Finish( "No answer from " + m_address + ". Check the address and port, that the server is running, and that its port (UDP) is open." );
	}
	return m_done;
}

bool ServerQuery::Compatible() const
{
	return Ok() && m_info.version == kProtocolVersion && m_info.fingerprint == BuildFingerprint();
}

void ServerQuery::Finish( const std::string& error )
{
	m_done = true;
	m_error = error;
	if ( m_transport != nullptr )
	{
		if ( m_transport->ServerPeer() != 0 )
		{
			m_transport->Disconnect( m_transport->ServerPeer() );
			m_transport->Flush();
		}
		m_transport.reset();
	}
}

} // namespace cb
