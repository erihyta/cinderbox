#include "game_server.h"

#include "fingerprint.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <random>

namespace cb
{

using namespace net;

GameServer::GameServer() = default;
GameServer::~GameServer() = default;

bool GameServer::Start( const ServerOptions& options )
{
	m_options = options;
	m_sim = std::make_unique<Simulation>( options.config );
	m_history.assign( kFrameHistory, InputFrame{} );
	for ( InputFrame& f : m_history )
	{
		f.tick = UINT32_MAX;
	}

	if ( m_transport.Listen( options.port, options.maxClients ) == false )
	{
		Log( "failed to listen on port %u", options.port );
		return false;
	}

	std::random_device rd;
	m_tokenState = ( uint64_t( rd() ) << 32 ) ^ rd() ^ uint64_t( std::chrono::steady_clock::now().time_since_epoch().count() );

	if ( options.recordHashes )
	{
		m_hashes.push_back( m_sim->ComputeHash() );
	}

	if ( options.recordPath.empty() == false )
	{
		if ( m_replay.Open( options.recordPath, BuildFingerprint(), options.config ) == false )
		{
			Log( "cannot write replay %s", options.recordPath.c_str() );
			return false;
		}
		m_replay.AddChecksum( 0, m_sim->ComputeHash() );
		Log( "recording to %s", options.recordPath.c_str() );
	}

	Log( "listening on port %u, %u Hz, fingerprint %016llx", options.port, options.config.tickRate,
		 (unsigned long long)BuildFingerprint() );
	return true;
}

void GameServer::Log( const char* fmt, ... ) const
{
	if ( m_options.verbose == false )
	{
		return;
	}
	va_list args;
	va_start( args, fmt );
	std::printf( "[server %6u] ", m_sim ? m_sim->Tick() : 0 );
	std::vprintf( fmt, args );
	std::printf( "\n" );
	std::fflush( stdout );
	va_end( args );
}

int GameServer::ConnectedClients() const
{
	int n = 0;
	for ( const Client& c : m_clients )
	{
		n += ( c.used && c.connected ) ? 1 : 0;
	}
	return n;
}

bool GameServer::GetRecordedHash( uint32_t tick, uint64_t& hash ) const
{
	if ( tick >= m_hashes.size() )
	{
		return false;
	}
	hash = m_hashes[tick];
	return true;
}

GameServer::Client* GameServer::FindByPeer( PeerId peer )
{
	for ( Client& c : m_clients )
	{
		if ( c.used && c.connected && c.peer == peer )
		{
			return &c;
		}
	}
	return nullptr;
}

void GameServer::Reject( PeerId peer, const std::string& reason )
{
	Log( "rejecting peer %u: %s", peer, reason.c_str() );
	Encode( MsgReject{ reason }, m_buffer );
	m_transport.Send( peer, ChannelReliable, m_buffer, true );
	m_transport.Disconnect( peer );
}

double GameServer::TimeUntilNextTick( double now ) const
{
	if ( m_nextTickTime < 0.0 )
	{
		return 0.0;
	}
	return m_nextTickTime - now;
}

void GameServer::Update( double now )
{
	m_events.clear();
	m_transport.Poll( m_events );
	for ( const NetEvent& ev : m_events )
	{
		HandleEvent( ev, now );
	}

	// Players whose connection did not come back in time leave the world.
	for ( Client& c : m_clients )
	{
		if ( c.used && c.connected == false && now - c.disconnectedAt > m_options.reconnectGraceSeconds )
		{
			if ( c.inWorld )
			{
				m_pendingEvents.push_back( { PlayerEventType::Leave, c.slot } );
			}
			Log( "slot %u left (reconnect grace expired)", c.slot );
			c = Client{};
		}
	}

	const double dt = 1.0 / double( m_options.config.tickRate );
	if ( m_nextTickTime < 0.0 )
	{
		m_nextTickTime = now;
	}

	int ran = 0;
	while ( now >= m_nextTickTime && ran < 8 )
	{
		auto start = std::chrono::steady_clock::now();
		RunTick( now );
		double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
		m_stats.ticks += 1;
		m_stats.tickMsTotal += ms;
		m_stats.tickMsMax = std::max( m_stats.tickMsMax, ms );
		m_nextTickTime += dt;
		++ran;
	}
	if ( now - m_nextTickTime > 0.25 )
	{
		Log( "running %.0f ms behind, skipping ahead", ( now - m_nextTickTime ) * 1000.0 );
		m_nextTickTime = now;
	}

	m_transport.Flush();
	m_stats.bytesSent = m_transport.BytesSent();
	m_stats.bytesReceived = m_transport.BytesReceived();
}

void GameServer::HandleEvent( const NetEvent& ev, double now )
{
	switch ( ev.type )
	{
		case NetEvent::Type::Connected:
			Log( "peer %u connected", ev.peer );
			break;

		case NetEvent::Type::Disconnected:
			if ( Client* c = FindByPeer( ev.peer ) )
			{
				Log( "slot %u disconnected, holding for %.0f s", c->slot, m_options.reconnectGraceSeconds );
				c->connected = false;
				c->welcomed = false;
				c->peer = 0;
				c->disconnectedAt = now;
				c->lastInput = {};
			}
			break;

		case NetEvent::Type::Received:
		{
			ByteReader r( ev.data.data(), ev.data.size() );
			auto type = ReadType( r );
			if ( !type )
			{
				break;
			}

			Client* c = FindByPeer( ev.peer );
			if ( *type == MsgType::Hello )
			{
				MsgHello hello;
				if ( c == nullptr && Decode( r, hello ) )
				{
					HandleHello( ev.peer, hello, now );
				}
			}
			else if ( *type == MsgType::Input && c != nullptr && c->welcomed )
			{
				MsgInput msg;
				if ( Decode( r, msg ) )
				{
					HandleInput( *c, msg );
				}
			}
			else if ( *type == MsgType::ResyncRequest && c != nullptr && c->welcomed )
			{
				MsgResyncRequest msg;
				if ( Decode( r, msg ) )
				{
					m_stats.resyncRequests += 1;
					if ( now - c->lastResyncAt >= m_options.resyncCooldownSeconds )
					{
						Log( "slot %u reported a desync at tick %u, resending state", c->slot, msg.tick );
						c->needsSnapshot = true;
					}
				}
			}
			break;
		}
	}
}

void GameServer::HandleHello( PeerId peer, const MsgHello& hello, double now )
{
	if ( hello.version != kProtocolVersion )
	{
		Reject( peer, "protocol version mismatch" );
		return;
	}
	if ( hello.fingerprint != BuildFingerprint() )
	{
		Reject( peer, "simulation build mismatch (client and server were built differently)" );
		return;
	}

	Client* target = nullptr;
	if ( hello.reconnectToken != 0 )
	{
		for ( Client& c : m_clients )
		{
			if ( c.used && c.token == hello.reconnectToken )
			{
				target = &c;
				break;
			}
		}
		if ( target != nullptr )
		{
			if ( target->connected )
			{
				// The old connection is stale (the client noticed before we did).
				m_transport.DropHard( target->peer );
			}
			Log( "slot %u reconnected (peer %u)", target->slot, peer );
			m_stats.reconnects += 1;
		}
	}

	if ( target == nullptr )
	{
		for ( Client& c : m_clients )
		{
			if ( c.used == false )
			{
				target = &c;
				break;
			}
		}
		if ( target == nullptr )
		{
			Reject( peer, "server full" );
			return;
		}

		PlayerSlot slot = PlayerSlot( target - m_clients );
		*target = Client{};
		target->used = true;
		target->slot = slot;
		target->token = NextRandom( m_tokenState ) | 1;
		target->inWorld = true;
		m_pendingEvents.push_back( { PlayerEventType::Join, slot } );
		m_stats.joins += 1;
		Log( "peer %u joins as slot %u", peer, slot );
	}

	target->connected = true;
	target->peer = peer;
	target->welcomed = false;
	target->needsSnapshot = true;
	target->lastInput = {};
	target->lastResyncAt = now;
	for ( auto& s : target->inputs )
	{
		s.tick = UINT32_MAX;
	}
}

void GameServer::HandleInput( Client& client, const MsgInput& msg )
{
	uint32_t current = m_sim->Tick();
	// Acknowledgements only move forward (input packets can arrive out of order).
	client.ackTick = std::max( client.ackTick, std::min( msg.ackTick, current ) );
	uint32_t count = uint32_t( msg.inputs.size() );
	uint32_t first = msg.newestTick + 1 - count;
	for ( uint32_t i = 0; i < count; ++i )
	{
		uint32_t tick = first + i;
		if ( tick < current || tick >= current + kInputBuffer )
		{
			continue;
		}
		client.inputs[tick % kInputBuffer] = { tick, SanitizeInput( msg.inputs[i] ) };
	}
}

void GameServer::SendSnapshots()
{
	bool built = false;
	for ( Client& c : m_clients )
	{
		if ( c.used == false || c.connected == false || c.needsSnapshot == false )
		{
			continue;
		}

		if ( built == false )
		{
			m_sim->SavePortable( m_image );
			built = true;
		}

		MsgWelcome welcome;
		welcome.fingerprint = BuildFingerprint();
		welcome.config = m_options.config;
		welcome.slot = c.slot;
		welcome.reconnectToken = c.token;
		welcome.snapshotTick = m_sim->Tick();
		welcome.baseInputs = m_lastInputs;
		welcome.image = m_image;
		Encode( welcome, m_buffer );
		m_transport.Send( c.peer, ChannelReliable, m_buffer, true );

		c.needsSnapshot = false;
		c.welcomed = true;
		// The welcome carries the inputs of snapshotTick - 1, so frames start at snapshotTick.
		c.ackTick = m_sim->Tick();
		m_stats.snapshotsSent += 1;
	}
}

void GameServer::RunTick( double now )
{
	uint32_t tick = m_sim->Tick();

	InputFrame frame;
	frame.tick = tick;
	frame.events.swap( m_pendingEvents );

	for ( Client& c : m_clients )
	{
		if ( c.used == false || c.inWorld == false )
		{
			continue;
		}

		if ( c.connected && c.welcomed )
		{
			m_stats.inputTicks += 1;
			const Client::Slot& s = c.inputs[tick % kInputBuffer];
			if ( s.tick == tick )
			{
				c.lastInput = s.input;
			}
			else
			{
				m_stats.lateInputs += 1;
			}
		}
		else
		{
			c.lastInput = {};
		}
		frame.inputs[c.slot] = c.lastInput;
	}

	// Joining / reconnecting / desynced clients get the state before this tick.
	for ( Client& c : m_clients )
	{
		if ( c.used && c.connected && c.needsSnapshot )
		{
			c.lastResyncAt = now;
		}
	}
	SendSnapshots();

	m_sim->Step( frame );

	m_history[tick % kFrameHistory] = frame;
	m_lastInputs = frame.inputs;
	SendFrames( now );
	m_replay.AddFrame( frame );

	uint32_t stateTick = tick + 1;
	uint64_t hash = 0;
	bool haveHash = false;
	if ( m_options.recordHashes )
	{
		hash = m_sim->ComputeHash();
		haveHash = true;
		m_hashes.push_back( hash );
	}

	if ( m_replay.IsOpen() && m_options.replayChecksumInterval > 0 && stateTick % m_options.replayChecksumInterval == 0 )
	{
		if ( haveHash == false )
		{
			hash = m_sim->ComputeHash();
			haveHash = true;
		}
		m_replay.AddChecksum( stateTick, hash );
		if ( stateTick % ( 10 * m_options.replayChecksumInterval ) == 0 )
		{
			m_replay.Flush();
		}
	}

	if ( m_options.checksumInterval > 0 && stateTick % m_options.checksumInterval == 0 )
	{
		if ( haveHash == false )
		{
			hash = m_sim->ComputeHash();
		}
		Encode( MsgChecksum{ stateTick, hash }, m_buffer );
		for ( const Client& c : m_clients )
		{
			if ( c.used && c.connected && c.welcomed )
			{
				m_transport.Send( c.peer, ChannelReliable, m_buffer, true );
			}
		}
	}
}

const InputFrame* GameServer::HistoryFrame( uint32_t tick ) const
{
	const InputFrame& f = m_history[tick % kFrameHistory];
	return f.tick == tick ? &f : nullptr;
}

// Every client gets all frames from its acknowledgement up to the newest, unreliably, every tick.
// Clients that acknowledge the same tick share one encoded batch.
void GameServer::SendFrames( double now )
{
	uint32_t newest = m_sim->Tick() - 1;
	struct Encoded
	{
		uint32_t ack;
		size_t frames;
		std::vector<uint8_t> bytes;
	};
	std::vector<Encoded> cache;
	std::vector<const InputFrame*> frames;

	for ( Client& c : m_clients )
	{
		if ( c.used == false || c.connected == false || c.welcomed == false || c.ackTick > newest )
		{
			continue;
		}

		if ( newest - c.ackTick >= kFrameHistory - 1 )
		{
			// Too far behind to catch up with frames; send the state instead.
			if ( c.needsSnapshot == false )
			{
				Log( "slot %u is %u ticks behind, resending state", c.slot, newest - c.ackTick );
				m_stats.ackTooOld += 1;
				c.needsSnapshot = true;
				c.lastResyncAt = now;
			}
			continue;
		}

		Encoded* hit = nullptr;
		for ( Encoded& e : cache )
		{
			if ( e.ack == c.ackTick )
			{
				hit = &e;
				break;
			}
		}
		if ( hit == nullptr )
		{
			frames.clear();
			for ( uint32_t t = c.ackTick; t <= newest && frames.size() < kMaxBatchFrames; ++t )
			{
				frames.push_back( HistoryFrame( t ) );
			}
			InputArray base{};
			if ( c.ackTick > 0 )
			{
				base = HistoryFrame( c.ackTick - 1 ) ? HistoryFrame( c.ackTick - 1 )->inputs : m_lastInputs;
			}
			cache.push_back( { c.ackTick, frames.size(), {} } );
			hit = &cache.back();
			EncodeFrameBatch( base, frames.data(), frames.size(), hit->bytes );
		}
		m_transport.Send( c.peer, ChannelInput, hit->bytes, false );
		m_stats.batchesSent += 1;
		m_stats.framesSent += hit->frames;
	}
}

void GameServer::DropClientHard( PlayerSlot slot, double now )
{
	Client& c = m_clients[slot];
	if ( c.used && c.connected )
	{
		Log( "test: dropping slot %u without notice", slot );
		m_transport.DropHard( c.peer );
		c.connected = false;
		c.welcomed = false;
		c.peer = 0;
		c.disconnectedAt = now;
		c.lastInput = {};
	}
}

} // namespace cb
