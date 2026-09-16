#pragma once

// Thin ENet wrapper. ENet (and windows.h) stay inside transport.cpp so they never collide with
// raylib in the client.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cb::net
{

using PeerId = uint32_t;

struct NetEvent
{
	enum class Type
	{
		Connected,
		Disconnected,
		Received,
	};

	Type type = Type::Received;
	PeerId peer = 0;
	uint8_t channel = 0;
	std::vector<uint8_t> data;
};

struct PeerStats
{
	uint32_t roundTripMs = 0;
	uint32_t roundTripVarianceMs = 0;
	uint32_t packetLossPerMille = 0;
};

// One ENet host. Acts as a server (Listen) or a client (Connect to a single server).
class Transport
{
public:
	Transport();
	~Transport();
	Transport( const Transport& ) = delete;
	Transport& operator=( const Transport& ) = delete;

	bool Listen( uint16_t port, uint32_t maxPeers );
	// Starts connecting; the result arrives as a Connected or Disconnected event.
	bool Connect( const std::string& host, uint16_t port );

	// Collects all pending events without blocking.
	void Poll( std::vector<NetEvent>& events );

	void Send( PeerId peer, uint8_t channel, const std::vector<uint8_t>& data, bool reliable );
	void Flush();

	// Graceful disconnect (the peer gets a Disconnected event).
	void Disconnect( PeerId peer );
	// Drops the peer immediately without notifying it. Used to simulate a lost connection.
	void DropHard( PeerId peer );

	PeerStats Stats( PeerId peer ) const;

	// The server peer when used as a client (0 if none).
	PeerId ServerPeer() const
	{
		return m_serverPeer;
	}

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
	PeerId m_serverPeer = 0;
};

} // namespace cb::net
