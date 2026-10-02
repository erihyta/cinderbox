#include "stream_source.h"

#include "protocol.h"
#include "transport.h"
#include "view_codec.h"

#include <chrono>
#include <deque>
#include <thread>
#include <vector>

namespace cb
{

using namespace net;

namespace
{
constexpr size_t kKept = 16; // decoded frames kept as possible bases (the server keeps as many)
}

StreamSource::StreamSource( const StreamOptions& options )
	: m_options( options )
{
	StartThread();
}

StreamSource::~StreamSource()
{
	StopThread();
}

void StreamSource::SetInput( const PlayerInput& input )
{
	std::lock_guard<std::mutex> lock( m_inputMutex );
	m_input = input;
	m_latchedButtons |= uint8_t( input.buttons & BtnJump );
	m_latchedActions |= input.actions;
}

void StreamSource::Run()
{
	Transport transport;
	std::vector<NetEvent> events;
	std::vector<uint8_t> buffer;
	std::deque<present::ViewFrame> kept; // newest last
	present::ViewFrame scratch;

	enum class State
	{
		Connecting,
		Joining,
		Playing,
		Reconnecting,
		Rejected,
	} state = State::Connecting;
	std::string rejectReason;
	uint64_t token = 0;
	bool connectPending = false;
	double nextConnect = 0.0;
	double nextInput = 0.0;
	uint64_t ack = 0;
	uint64_t connectFailures = 0;
	uint64_t frames = 0;
	uint64_t dropped = 0;
	uint64_t bytesAtSecond = 0;
	double secondStarted = present::ViewClock();
	double kbitDown = 0.0;
	bool changed = true; // something the viewer should hear about

	auto stateName = [&]() {
		switch ( state )
		{
			case State::Connecting:
				return "connecting";
			case State::Joining:
				return "joining";
			case State::Playing:
				return "playing";
			case State::Reconnecting:
				return "reconnecting";
			case State::Rejected:
				return "rejected";
		}
		return "connecting";
	};
	auto lost = [&]( double now ) {
		connectPending = false;
		if ( state == State::Rejected )
		{
			return;
		}
		state = token != 0 ? State::Reconnecting : State::Connecting;
		nextConnect = now + m_options.reconnectIntervalSeconds;
		// Whatever the server kept for the old connection is gone: start from a whole frame.
		ack = 0;
		kept.clear();
		changed = true;
	};

	while ( Stopping() == false )
	{
		double now = present::ViewClock();
		bool gotFrame = false;

		events.clear();
		transport.Poll( events );
		for ( const NetEvent& ev : events )
		{
			if ( ev.type == NetEvent::Type::Connected )
			{
				connectPending = false;
				MsgHello hello;
				hello.reconnectToken = token;
				hello.name = m_options.playerName;
				hello.stream = true;
				Encode( hello, buffer );
				transport.Send( ev.peer, ChannelReliable, buffer, true );
				state = State::Joining;
				changed = true;
			}
			else if ( ev.type == NetEvent::Type::Disconnected )
			{
				lost( now );
			}
			else if ( ev.type == NetEvent::Type::Received )
			{
				ByteReader r( ev.data.data(), ev.data.size() );
				auto type = ReadType( r );
				if ( !type )
				{
					continue;
				}
				if ( *type == MsgType::Reject )
				{
					MsgReject msg;
					if ( Decode( r, msg ) )
					{
						rejectReason = msg.reason;
						state = State::Rejected;
						changed = true;
					}
				}
				else if ( *type == MsgType::StreamWelcome )
				{
					MsgStreamWelcome msg;
					if ( Decode( r, msg ) )
					{
						token = msg.reconnectToken;
						ack = 0;
						kept.clear();
					}
				}
				else if ( *type == MsgType::View && ev.data.size() > 1 && state != State::Rejected )
				{
					const uint8_t* packet = ev.data.data() + 1;
					size_t size = ev.data.size() - 1;
					// The base the server chose is one this said it has; an old packet (late, or
					// duplicated) is not worth decoding.
					uint64_t baseSerial = present::ViewPacketBase( packet, size );
					const present::ViewFrame* base = nullptr;
					for ( const present::ViewFrame& frame : kept )
					{
						base = frame.serial == baseSerial ? &frame : base;
					}
					if ( ( baseSerial != 0 && base == nullptr ) || present::DecodeView( packet, size, base, scratch ) == false )
					{
						dropped += 1;
						ack = baseSerial != 0 && base == nullptr ? 0 : ack;
						continue;
					}
					if ( kept.empty() == false && scratch.serial <= kept.back().serial )
					{
						continue;
					}
					kept.push_back( scratch );
					while ( kept.size() > kKept )
					{
						kept.pop_front();
					}
					ack = scratch.serial;
					frames += 1;
					gotFrame = true;
					if ( state != State::Playing )
					{
						state = State::Playing;
					}
				}
			}
		}

		if ( ( state == State::Connecting || state == State::Reconnecting ) && connectPending == false && now >= nextConnect )
		{
			if ( transport.Connect( m_options.host, m_options.port ) )
			{
				connectPending = true;
			}
			else
			{
				// Only an address that names nothing counts: a busy peer is tried again.
				connectFailures += Transport::Resolves( m_options.host ) ? 0 : 1;
				nextConnect = now + m_options.reconnectIntervalSeconds;
				changed = true;
			}
		}

		// The input goes up many times a second, with the newest frame this has.
		if ( ( state == State::Playing || state == State::Joining ) && token != 0 && now >= nextInput )
		{
			nextInput = now + 1.0 / double( std::max<uint32_t>( m_options.inputRate, 1 ) );
			MsgStreamInput msg;
			{
				std::lock_guard<std::mutex> lock( m_inputMutex );
				msg.input = m_input;
				msg.input.buttons |= m_latchedButtons;
				msg.input.actions |= m_latchedActions;
				m_latchedButtons = 0;
				m_latchedActions = 0;
			}
			msg.ackSerial = ack;
			Encode( msg, buffer );
			transport.Send( transport.ServerPeer(), ChannelInput, buffer, false );
		}
		transport.Flush();

		if ( now - secondStarted >= 1.0 )
		{
			kbitDown = double( transport.BytesReceived() - bytesAtSecond ) * 8.0 / 1000.0 / ( now - secondStarted );
			bytesAtSecond = transport.BytesReceived();
			secondStarted = now;
		}

		if ( gotFrame || changed )
		{
			changed = false;
			present::ViewFrame& f = Building();
			if ( kept.empty() == false )
			{
				f = kept.back();
			}
			else
			{
				f.hasWorld = false;
			}
			f.state = stateName();
			// A frame is drawn from the one before to this one, starting when it arrives; while the
			// connection is down nothing moves on.
			f.alphaAtPublish = gotFrame ? 0.0f : 1.0f;
			f.rate = state == State::Playing ? 1.0f : 0.0f;
			f.stats.clear();
			f.stats.push_back( { "reject_reason", rejectReason } );
			f.stats.push_back( { "tick", int64_t( f.hasWorld ? f.frame.tick : 0 ) } );
			f.stats.push_back( { "rtt_ms", int64_t( transport.Stats( transport.ServerPeer() ).roundTripMs ) } );
			f.stats.push_back( { "connect_failures", int64_t( connectFailures ) } );
			f.stats.push_back( { "frames", int64_t( frames ) } );
			f.stats.push_back( { "frames_dropped", int64_t( dropped ) } );
			f.stats.push_back( { "kbit_down", kbitDown } );
			f.stats.push_back( { "kbit_down_total", double( transport.BytesReceived() ) * 8.0 / 1000.0 } );
			Publish( false );
		}

		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
}

} // namespace cb
