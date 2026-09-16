#include "netsim.h"

#include "transport.h"
#include "util.h"

#include <enet/enet.h>

#include <algorithm>
#include <cstdio>
#include <queue>
#include <vector>

namespace cb::net
{

namespace
{

constexpr double kLinkIdleSeconds = 30.0;
constexpr size_t kMaxDatagram = 4096;

struct Pending
{
	double due;
	uint64_t order; // FIFO among equal due times
	ENetSocket socket;
	ENetAddress to;
	std::vector<uint8_t> data;
};

struct PendingLater
{
	bool operator()( const Pending& a, const Pending& b ) const
	{
		return a.due != b.due ? a.due > b.due : a.order > b.order;
	}
};

bool SameAddress( const ENetAddress& a, const ENetAddress& b )
{
	return a.host == b.host && a.port == b.port;
}

ENetSocket MakeSocket( uint16_t port )
{
	ENetSocket s = enet_socket_create( ENET_SOCKET_TYPE_DATAGRAM );
	if ( s == ENET_SOCKET_NULL )
	{
		return s;
	}
	ENetAddress bindAddress{};
	bindAddress.host = ENET_HOST_ANY;
	bindAddress.port = port;
	if ( enet_socket_bind( s, &bindAddress ) < 0 )
	{
		enet_socket_destroy( s );
		return ENET_SOCKET_NULL;
	}
	enet_socket_set_option( s, ENET_SOCKOPT_NONBLOCK, 1 );
	enet_socket_set_option( s, ENET_SOCKOPT_RCVBUF, 4 * 1024 * 1024 );
	enet_socket_set_option( s, ENET_SOCKOPT_SNDBUF, 4 * 1024 * 1024 );
	return s;
}

} // namespace

struct NetSimProxy::Impl
{
	NetSimConfig config;
	ENetSocket listen = ENET_SOCKET_NULL;
	ENetAddress target{};
	uint64_t rng = 1;
	uint64_t order = 0;

	struct Link
	{
		ENetAddress client;
		ENetSocket upstream;
		double lastActivity;
		double lastDueToServer; // keeps order when reordering is disabled
		double lastDueToClient;
	};
	std::vector<Link> links;
	std::priority_queue<Pending, std::vector<Pending>, PendingLater> queue;
	Stats stats;

	void Schedule( double now, ENetSocket socket, const ENetAddress& to, const uint8_t* data, size_t size, double& lastDue )
	{
		if ( RandomUnit( rng ) * 100.0f < config.lossPercent )
		{
			stats.dropped += 1;
			return;
		}

		int copies = RandomUnit( rng ) * 100.0f < config.duplicatePercent ? 2 : 1;
		stats.duplicated += uint64_t( copies - 1 );
		for ( int c = 0; c < copies; ++c )
		{
			double delay = double( config.latencyMs ) / 1000.0 + double( RandomUnit( rng ) ) * double( config.jitterMs ) / 1000.0;
			double due = now + delay;
			if ( config.allowReorder == false )
			{
				due = std::max( due, lastDue );
			}
			lastDue = due;
			queue.push( { due, order++, socket, to, std::vector<uint8_t>( data, data + size ) } );
		}
	}

	void Receive( double now )
	{
		uint8_t buffer[kMaxDatagram];
		ENetBuffer buf{};
		buf.data = buffer;
		buf.dataLength = sizeof( buffer );

		// Client -> server
		for ( ;; )
		{
			ENetAddress from{};
			int n = enet_socket_receive( listen, &from, &buf, 1 );
			if ( n <= 0 )
			{
				break;
			}

			Link* link = nullptr;
			for ( Link& l : links )
			{
				if ( SameAddress( l.client, from ) )
				{
					link = &l;
					break;
				}
			}
			if ( link == nullptr )
			{
				ENetSocket upstream = MakeSocket( 0 );
				if ( upstream == ENET_SOCKET_NULL )
				{
					continue;
				}
				links.push_back( { from, upstream, now, 0.0, 0.0 } );
				link = &links.back();
			}
			link->lastActivity = now;
			stats.forwarded += 1;
			Schedule( now, link->upstream, target, buffer, size_t( n ), link->lastDueToServer );
		}

		// Server -> client
		for ( Link& l : links )
		{
			for ( ;; )
			{
				ENetAddress from{};
				int n = enet_socket_receive( l.upstream, &from, &buf, 1 );
				if ( n <= 0 )
				{
					break;
				}
				l.lastActivity = now;
				stats.forwarded += 1;
				Schedule( now, listen, l.client, buffer, size_t( n ), l.lastDueToClient );
			}
		}
	}

	void Flush( double now )
	{
		while ( queue.empty() == false && queue.top().due <= now )
		{
			const Pending& p = queue.top();
			ENetBuffer buf{};
			buf.data = const_cast<void*>( static_cast<const void*>( p.data.data() ) );
			buf.dataLength = p.data.size();
			enet_socket_send( p.socket, &p.to, &buf, 1 );
			queue.pop();
		}
	}

	void DropIdleLinks( double now )
	{
		for ( size_t i = 0; i < links.size(); )
		{
			if ( now - links[i].lastActivity > kLinkIdleSeconds )
			{
				// Queued datagrams may still reference the socket; drop them with it.
				ENetSocket dead = links[i].upstream;
				std::priority_queue<Pending, std::vector<Pending>, PendingLater> kept;
				while ( queue.empty() == false )
				{
					if ( queue.top().socket != dead )
					{
						kept.push( queue.top() );
					}
					queue.pop();
				}
				queue.swap( kept );
				enet_socket_destroy( dead );
				links.erase( links.begin() + long( i ) );
			}
			else
			{
				++i;
			}
		}
	}
};

NetSimProxy::NetSimProxy()
	: m_impl( std::make_unique<Impl>() )
{
	EnsureNetworkInitialized();
}

NetSimProxy::~NetSimProxy()
{
	for ( auto& l : m_impl->links )
	{
		enet_socket_destroy( l.upstream );
	}
	if ( m_impl->listen != ENET_SOCKET_NULL )
	{
		enet_socket_destroy( m_impl->listen );
	}
}

bool NetSimProxy::Start( uint16_t listenPort, const std::string& targetHost, uint16_t targetPort, const NetSimConfig& config )
{
	m_impl->config = config;
	m_impl->rng = config.seed;
	if ( enet_address_set_host( &m_impl->target, targetHost.c_str() ) != 0 )
	{
		return false;
	}
	m_impl->target.port = targetPort;
	m_impl->listen = MakeSocket( listenPort );
	return m_impl->listen != ENET_SOCKET_NULL;
}

void NetSimProxy::SetConfig( const NetSimConfig& config )
{
	m_impl->config = config;
}

void NetSimProxy::Update( double now )
{
	m_impl->Receive( now );
	m_impl->Flush( now );
	m_impl->DropIdleLinks( now );
}

NetSimProxy::Stats NetSimProxy::GetStats() const
{
	Stats s = m_impl->stats;
	s.links = uint32_t( m_impl->links.size() );
	return s;
}

} // namespace cb::net
