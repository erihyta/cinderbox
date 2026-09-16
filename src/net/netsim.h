#pragma once

// UDP relay that degrades traffic: latency, jitter, packet loss and duplication, each way.
// Clients connect to the proxy's port instead of the server; every client gets its own upstream
// socket, so the server still sees separate peers. ENet runs unmodified on both ends, so its RTT
// measurement and retransmissions behave exactly as on a real bad network.

#include <cstdint>
#include <memory>
#include <string>

namespace cb::net
{

struct NetSimConfig
{
	uint32_t latencyMs = 0;		 // added one-way delay
	uint32_t jitterMs = 0;		 // uniform extra delay in [0, jitter]
	float lossPercent = 0.0f;	 // dropped datagrams, per direction
	float duplicatePercent = 0.0f;
	bool allowReorder = true; // false: jitter never reorders datagrams of one flow
	uint64_t seed = 1;
};

class NetSimProxy
{
public:
	NetSimProxy();
	~NetSimProxy();
	NetSimProxy( const NetSimProxy& ) = delete;
	NetSimProxy& operator=( const NetSimProxy& ) = delete;

	bool Start( uint16_t listenPort, const std::string& targetHost, uint16_t targetPort, const NetSimConfig& config );

	// Relays everything that is due at `now` (seconds). Non-blocking; call often (every ~1 ms).
	void Update( double now );

	void SetConfig( const NetSimConfig& config );

	struct Stats
	{
		uint64_t forwarded = 0;
		uint64_t dropped = 0;
		uint64_t duplicated = 0;
		uint32_t links = 0;
	};
	Stats GetStats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace cb::net
