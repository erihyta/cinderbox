#include "transport.h"

#include "protocol.h"

#include <enet/enet.h>

#include <cstdio>
#include <unordered_map>

namespace cb::net
{

namespace
{

// Detect a dead connection within 2-6 s instead of ENet's default ~30 s. Time already freezes on
// the client as soon as frames stop arriving (the prediction window fills), so this can be patient.
constexpr enet_uint32 kTimeoutLimit = 32;
constexpr enet_uint32 kTimeoutMinMs = 2000;
constexpr enet_uint32 kTimeoutMaxMs = 6000;

void ConfigurePeer( ENetPeer* peer )
{
	enet_peer_timeout( peer, kTimeoutLimit, kTimeoutMinMs, kTimeoutMaxMs );
}

// ENet's packet throttle drops unreliable packets whenever the round trip rises, and a large
// reliable transfer (a Welcome snapshot) is enough to make it drop every frame batch for about
// half a second. Batches repeat everything not yet acknowledged, so dropping them saves nothing
// and only stalls the client; with no deceleration the throttle never drops them.
// This sends a command to the other side, so it has to wait until the connection is up.
void DisableThrottleDrops( ENetPeer* peer )
{
	enet_peer_throttle_configure( peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_ACCELERATION, 0 );
}

struct EnetLibrary
{
	EnetLibrary()
	{
		if ( enet_initialize() != 0 )
		{
			std::fprintf( stderr, "enet_initialize failed\n" );
		}
	}
	~EnetLibrary()
	{
		enet_deinitialize();
	}
};

} // namespace

void EnsureNetworkInitialized()
{
	static EnetLibrary library;
}

struct Transport::Impl
{
	ENetHost* host = nullptr;
	PeerId nextId = 1;
	uint64_t bytesSent = 0;
	uint64_t bytesReceived = 0;

	// ENet's counters are 32-bit; move them into 64-bit totals before they can wrap.
	void AccumulateCounters()
	{
		if ( host != nullptr )
		{
			bytesSent += host->totalSentData;
			bytesReceived += host->totalReceivedData;
			host->totalSentData = 0;
			host->totalReceivedData = 0;
		}
	}
	// Lookup only, never iterated for anything order-dependent.
	std::unordered_map<PeerId, ENetPeer*> peers;

	PeerId Register( ENetPeer* peer )
	{
		PeerId id = nextId++;
		peer->data = reinterpret_cast<void*>( uintptr_t( id ) );
		peers[id] = peer;
		return id;
	}

	static PeerId IdOf( ENetPeer* peer )
	{
		return PeerId( uintptr_t( peer->data ) );
	}

	ENetPeer* Find( PeerId id ) const
	{
		auto it = peers.find( id );
		return it == peers.end() ? nullptr : it->second;
	}
};

Transport::Transport()
	: m_impl( std::make_unique<Impl>() )
{
	EnsureNetworkInitialized();
}

Transport::~Transport()
{
	if ( m_impl->host != nullptr )
	{
		for ( auto& [id, peer] : m_impl->peers )
		{
			enet_peer_disconnect_now( peer, 0 );
		}
		enet_host_flush( m_impl->host );
		enet_host_destroy( m_impl->host );
	}
}

bool Transport::Listen( uint16_t port, uint32_t maxPeers )
{
	ENetAddress address{};
	address.host = ENET_HOST_ANY;
	address.port = port;
	m_impl->host = enet_host_create( &address, maxPeers, ChannelCount, 0, 0 );
	return m_impl->host != nullptr;
}

bool Transport::Connect( const std::string& hostName, uint16_t port )
{
	if ( m_impl->host == nullptr )
	{
		m_impl->host = enet_host_create( nullptr, 1, ChannelCount, 0, 0 );
		if ( m_impl->host == nullptr )
		{
			return false;
		}
	}

	ENetAddress address{};
	if ( enet_address_set_host( &address, hostName.c_str() ) != 0 )
	{
		return false;
	}
	address.port = port;

	ENetPeer* peer = enet_host_connect( m_impl->host, &address, ChannelCount, 0 );
	if ( peer == nullptr )
	{
		return false;
	}
	ConfigurePeer( peer );
	m_serverPeer = m_impl->Register( peer );
	return true;
}

void Transport::Poll( std::vector<NetEvent>& events )
{
	if ( m_impl->host == nullptr )
	{
		return;
	}

	m_impl->AccumulateCounters();
	ENetEvent ev;
	while ( enet_host_service( m_impl->host, &ev, 0 ) > 0 )
	{
		switch ( ev.type )
		{
			case ENET_EVENT_TYPE_CONNECT:
			{
				PeerId id = Impl::IdOf( ev.peer );
				if ( id == 0 )
				{
					// Incoming connection on a listening host.
					id = m_impl->Register( ev.peer );
					ConfigurePeer( ev.peer );
				}
				DisableThrottleDrops( ev.peer );
				events.push_back( { NetEvent::Type::Connected, id, 0, {} } );
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT:
			{
				PeerId id = Impl::IdOf( ev.peer );
				if ( id != 0 )
				{
					m_impl->peers.erase( id );
					ev.peer->data = nullptr;
					if ( id == m_serverPeer )
					{
						m_serverPeer = 0;
					}
					events.push_back( { NetEvent::Type::Disconnected, id, 0, {} } );
				}
				break;
			}
			case ENET_EVENT_TYPE_RECEIVE:
			{
				NetEvent out{ NetEvent::Type::Received, Impl::IdOf( ev.peer ), ev.channelID, {} };
				out.data.assign( ev.packet->data, ev.packet->data + ev.packet->dataLength );
				enet_packet_destroy( ev.packet );
				events.push_back( std::move( out ) );
				break;
			}
			default:
				break;
		}
	}
}

void Transport::Send( PeerId peer, uint8_t channel, const std::vector<uint8_t>& data, bool reliable )
{
	ENetPeer* p = m_impl->Find( peer );
	if ( p == nullptr || p->state != ENET_PEER_STATE_CONNECTED )
	{
		return;
	}
	// Without UNRELIABLE_FRAGMENT, ENet sends large "unreliable" packets as reliable fragments,
	// which would bring back head-of-line blocking.
	enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
	ENetPacket* packet = enet_packet_create( data.data(), data.size(), flags );
	if ( enet_peer_send( p, channel, packet ) != 0 )
	{
		enet_packet_destroy( packet );
	}
}

void Transport::Flush()
{
	if ( m_impl->host != nullptr )
	{
		enet_host_flush( m_impl->host );
	}
}

void Transport::Disconnect( PeerId peer )
{
	if ( ENetPeer* p = m_impl->Find( peer ) )
	{
		enet_peer_disconnect_later( p, 0 );
	}
}

void Transport::DropHard( PeerId peer )
{
	if ( ENetPeer* p = m_impl->Find( peer ) )
	{
		p->data = nullptr;
		enet_peer_reset( p );
		m_impl->peers.erase( peer );
		if ( peer == m_serverPeer )
		{
			m_serverPeer = 0;
		}
	}
}

uint64_t Transport::BytesSent() const
{
	return m_impl->bytesSent + ( m_impl->host ? m_impl->host->totalSentData : 0 );
}

uint64_t Transport::BytesReceived() const
{
	return m_impl->bytesReceived + ( m_impl->host ? m_impl->host->totalReceivedData : 0 );
}

PeerStats Transport::Stats( PeerId peer ) const
{
	PeerStats s;
	if ( ENetPeer* p = m_impl->Find( peer ) )
	{
		s.roundTripMs = p->roundTripTime;
		s.roundTripVarianceMs = p->roundTripTimeVariance;
		s.packetLossPerMille = uint32_t( uint64_t( p->packetLoss ) * 1000 / ENET_PEER_PACKET_LOSS_SCALE );
	}
	return s;
}

} // namespace cb::net
