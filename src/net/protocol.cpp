#include "protocol.h"

#include <algorithm>

namespace cb::net
{

namespace
{

constexpr uint32_t kMaxBlobBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxReasonLength = 256;
constexpr uint8_t kMaxInputsPerPacket = 32;

// Per-player field mask inside a frame.
enum InputField : uint8_t
{
	FieldMoveRight = 1 << 0,
	FieldMoveForward = 1 << 1,
	FieldYaw = 1 << 2,		// absolute, u16
	FieldYawDelta = 1 << 3, // relative, i8
	FieldButtons = 1 << 4,
	FieldReserved = 1 << 5,
	FieldAll = ( 1 << 6 ) - 1,
};

void Begin( std::vector<uint8_t>& out, MsgType type )
{
	out.clear();
	out.push_back( uint8_t( type ) );
}

void WriteConfig( ByteWriter& w, const SimConfig& c )
{
	w.Write( c.tickRate );
	w.Write( c.subSteps );
	w.Write( c.seed );
	w.Write( c.propLifetimeSeconds );
	w.Write( c.propsPerPlayer );
	w.Write( c.propsGlobal );
	w.Write( c.killY );
	w.Write( c.physicsArenaMB );
}

bool ReadConfig( ByteReader& r, SimConfig& c )
{
	c.tickRate = r.Read<uint32_t>();
	c.subSteps = r.Read<uint32_t>();
	c.seed = r.Read<uint64_t>();
	c.propLifetimeSeconds = r.Read<uint32_t>();
	c.propsPerPlayer = r.Read<uint32_t>();
	c.propsGlobal = r.Read<uint32_t>();
	c.killY = r.Read<float>();
	c.physicsArenaMB = r.Read<uint32_t>();
	return r.Ok() && c.tickRate >= 10 && c.tickRate <= 240 && c.subSteps >= 1 && c.subSteps <= 16 && c.physicsArenaMB >= 8 &&
		   c.physicsArenaMB <= 4096;
}

bool ReadBlobChecked( ByteReader& r, std::vector<uint8_t>& out )
{
	uint32_t size = r.Read<uint32_t>();
	if ( size > kMaxBlobBytes )
	{
		return false;
	}
	const uint8_t* p = r.Take( size );
	if ( p == nullptr )
	{
		return false;
	}
	out.assign( p, p + size );
	return true;
}

void WriteInputs( ByteWriter& w, const InputArray& inputs )
{
	w.WriteBytes( inputs.data(), sizeof( PlayerInput ) * inputs.size() );
}

bool ReadInputs( ByteReader& r, InputArray& inputs )
{
	const uint8_t* p = r.Take( sizeof( PlayerInput ) * inputs.size() );
	if ( p == nullptr )
	{
		return false;
	}
	std::memcpy( inputs.data(), p, sizeof( PlayerInput ) * inputs.size() );
	return true;
}

} // namespace

std::optional<MsgType> ReadType( ByteReader& r )
{
	uint8_t t = r.Read<uint8_t>();
	if ( r.Ok() == false || t < uint8_t( MsgType::Hello ) || t > uint8_t( MsgType::FrameBatch ) )
	{
		return std::nullopt;
	}
	return MsgType( t );
}

void Encode( const MsgHello& m, std::vector<uint8_t>& out )
{
	Begin( out, MsgType::Hello );
	ByteWriter w( out );
	w.Write( m.version );
	w.Write( m.fingerprint );
	w.Write( m.reconnectToken );
}

bool Decode( ByteReader& r, MsgHello& m )
{
	m.version = r.Read<uint32_t>();
	m.fingerprint = r.Read<uint64_t>();
	m.reconnectToken = r.Read<uint64_t>();
	return r.Ok();
}

void Encode( const MsgWelcome& m, std::vector<uint8_t>& out )
{
	Begin( out, MsgType::Welcome );
	ByteWriter w( out );
	w.Write( m.version );
	w.Write( m.fingerprint );
	WriteConfig( w, m.config );
	w.Write( m.slot );
	w.Write( m.reconnectToken );
	w.Write( m.snapshotTick );
	WriteInputs( w, m.baseInputs );
	w.Write( m.mapHash );
	w.WriteBlob( m.map );
	w.WriteBlob( m.image );
}

bool Decode( ByteReader& r, MsgWelcome& m )
{
	m.version = r.Read<uint32_t>();
	m.fingerprint = r.Read<uint64_t>();
	if ( ReadConfig( r, m.config ) == false )
	{
		return false;
	}
	m.slot = r.Read<PlayerSlot>();
	m.reconnectToken = r.Read<uint64_t>();
	m.snapshotTick = r.Read<uint32_t>();
	if ( ReadInputs( r, m.baseInputs ) == false )
	{
		return false;
	}
	m.mapHash = r.Read<uint64_t>();
	return ReadBlobChecked( r, m.map ) && ReadBlobChecked( r, m.image ) && m.slot < kMaxPlayers;
}

void Encode( const MsgReject& m, std::vector<uint8_t>& out )
{
	Begin( out, MsgType::Reject );
	ByteWriter w( out );
	size_t n = std::min( m.reason.size(), kMaxReasonLength );
	w.Write( uint16_t( n ) );
	w.WriteBytes( m.reason.data(), n );
}

bool Decode( ByteReader& r, MsgReject& m )
{
	uint16_t n = r.Read<uint16_t>();
	if ( n > kMaxReasonLength )
	{
		return false;
	}
	const uint8_t* p = r.Take( n );
	if ( p == nullptr )
	{
		return false;
	}
	m.reason.assign( reinterpret_cast<const char*>( p ), n );
	return true;
}

void Encode( const MsgChecksum& m, std::vector<uint8_t>& out )
{
	Begin( out, MsgType::Checksum );
	ByteWriter w( out );
	w.Write( m.tick );
	w.Write( m.hash );
}

bool Decode( ByteReader& r, MsgChecksum& m )
{
	m.tick = r.Read<uint32_t>();
	m.hash = r.Read<uint64_t>();
	return r.Ok();
}

void Encode( const MsgResyncRequest& m, std::vector<uint8_t>& out )
{
	Begin( out, MsgType::ResyncRequest );
	ByteWriter w( out );
	w.Write( m.tick );
}

bool Decode( ByteReader& r, MsgResyncRequest& m )
{
	m.tick = r.Read<uint32_t>();
	return r.Ok();
}

void Encode( const MsgInput& m, std::vector<uint8_t>& out )
{
	Begin( out, MsgType::Input );
	ByteWriter w( out );
	size_t n = std::min<size_t>( m.inputs.size(), kMaxInputsPerPacket );
	w.Write( m.newestTick );
	w.Write( m.ackTick );
	w.Write( uint8_t( n ) );
	w.WriteBytes( m.inputs.data() + ( m.inputs.size() - n ), n * sizeof( PlayerInput ) );
}

bool Decode( ByteReader& r, MsgInput& m )
{
	m.newestTick = r.Read<uint32_t>();
	m.ackTick = r.Read<uint32_t>();
	uint8_t n = r.Read<uint8_t>();
	if ( r.Ok() == false || n > kMaxInputsPerPacket || n > m.newestTick + 1 )
	{
		return false;
	}
	const uint8_t* p = r.Take( n * sizeof( PlayerInput ) );
	if ( p == nullptr )
	{
		return false;
	}
	m.inputs.resize( n );
	std::memcpy( m.inputs.data(), p, n * sizeof( PlayerInput ) );
	return true;
}

// Frame layout: type, tick u32, eventCount u8, events (type u8, slot u8)*, changedMask u64,
// then one PlayerInput per set bit in slot order.
void FrameCodec::Encode( const InputFrame& frame, std::vector<uint8_t>& out )
{
	Begin( out, MsgType::Frame );
	ByteWriter w( out );
	EncodeBody( frame, w );
}

bool FrameCodec::Decode( ByteReader& r, InputFrame& frame )
{
	return DecodeBody( r, frame );
}

void FrameCodec::EncodeBody( const InputFrame& frame, ByteWriter& w )
{
	w.Write( frame.tick );
	w.Write( uint8_t( frame.events.size() ) );
	for ( const PlayerEvent& e : frame.events )
	{
		w.Write( uint8_t( e.type ) );
		w.Write( e.slot );
	}

	uint64_t changed = 0;
	for ( int i = 0; i < kMaxPlayers; ++i )
	{
		if ( !( frame.inputs[i] == m_previous[i] ) )
		{
			changed |= uint64_t( 1 ) << i;
		}
	}
	w.Write( changed );
	for ( int i = 0; i < kMaxPlayers; ++i )
	{
		if ( changed & ( uint64_t( 1 ) << i ) )
		{
			// Only the fields that changed; small camera turns as a one-byte delta.
			const PlayerInput& now = frame.inputs[i];
			const PlayerInput& before = m_previous[i];
			int yawDelta = int16_t( uint16_t( now.cameraYaw - before.cameraYaw ) );
			uint8_t fields = 0;
			fields |= now.moveRight != before.moveRight ? FieldMoveRight : 0;
			fields |= now.moveForward != before.moveForward ? FieldMoveForward : 0;
			if ( now.cameraYaw != before.cameraYaw )
			{
				fields |= ( yawDelta >= -128 && yawDelta <= 127 ) ? FieldYawDelta : FieldYaw;
			}
			fields |= now.buttons != before.buttons ? FieldButtons : 0;
			fields |= now.reserved != before.reserved ? FieldReserved : 0;

			w.Write( fields );
			if ( fields & FieldMoveRight )
				w.Write( now.moveRight );
			if ( fields & FieldMoveForward )
				w.Write( now.moveForward );
			if ( fields & FieldYaw )
				w.Write( now.cameraYaw );
			if ( fields & FieldYawDelta )
				w.Write( int8_t( yawDelta ) );
			if ( fields & FieldButtons )
				w.Write( now.buttons );
			if ( fields & FieldReserved )
				w.Write( now.reserved );
		}
	}
	m_previous = frame.inputs;
}

bool FrameCodec::DecodeBody( ByteReader& r, InputFrame& frame )
{
	frame.tick = r.Read<uint32_t>();
	uint8_t eventCount = r.Read<uint8_t>();
	frame.events.clear();
	for ( uint8_t i = 0; i < eventCount && r.Ok(); ++i )
	{
		uint8_t type = r.Read<uint8_t>();
		PlayerSlot slot = r.Read<PlayerSlot>();
		if ( ( type != uint8_t( PlayerEventType::Join ) && type != uint8_t( PlayerEventType::Leave ) ) || slot >= kMaxPlayers )
		{
			return false;
		}
		frame.events.push_back( { PlayerEventType( type ), slot } );
	}

	uint64_t changed = r.Read<uint64_t>();
	frame.inputs = m_previous;
	for ( int i = 0; i < kMaxPlayers && r.Ok(); ++i )
	{
		if ( ( changed & ( uint64_t( 1 ) << i ) ) == 0 )
		{
			continue;
		}
		PlayerInput& in = frame.inputs[i];
		uint8_t fields = r.Read<uint8_t>();
		if ( fields == 0 || ( fields & ~FieldAll ) != 0 || ( ( fields & FieldYaw ) && ( fields & FieldYawDelta ) ) )
		{
			return false;
		}
		if ( fields & FieldMoveRight )
			in.moveRight = r.Read<int8_t>();
		if ( fields & FieldMoveForward )
			in.moveForward = r.Read<int8_t>();
		if ( fields & FieldYaw )
			in.cameraYaw = r.Read<uint16_t>();
		if ( fields & FieldYawDelta )
			in.cameraYaw = uint16_t( in.cameraYaw + uint16_t( int16_t( r.Read<int8_t>() ) ) );
		if ( fields & FieldButtons )
			in.buttons = r.Read<uint8_t>();
		if ( fields & FieldReserved )
			in.reserved = r.Read<uint8_t>();
	}
	if ( r.Ok() == false )
	{
		return false;
	}
	m_previous = frame.inputs;
	return true;
}

void EncodeFrameBatch( const InputArray& base, const InputFrame* const* frames, size_t count, std::vector<uint8_t>& out )
{
	Begin( out, MsgType::FrameBatch );
	ByteWriter w( out );
	count = std::min( count, kMaxBatchFrames );
	w.Write( count > 0 ? frames[0]->tick : uint32_t( 0 ) );
	w.Write( uint8_t( count ) );
	FrameCodec codec;
	codec.Reset( base );
	for ( size_t i = 0; i < count; ++i )
	{
		codec.EncodeBody( *frames[i], w );
	}
}

bool ReadFrameBatchHeader( ByteReader& r, uint32_t& firstTick, uint32_t& count )
{
	firstTick = r.Read<uint32_t>();
	count = r.Read<uint8_t>();
	return r.Ok() && count > 0 && count <= kMaxBatchFrames;
}

bool ReadFrameBatchBody( ByteReader& r, const InputArray& base, uint32_t firstTick, uint32_t count,
						 std::vector<InputFrame>& out )
{
	FrameCodec codec;
	codec.Reset( base );
	out.resize( count );
	for ( uint32_t i = 0; i < count; ++i )
	{
		if ( codec.DecodeBody( r, out[i] ) == false || out[i].tick != firstTick + i )
		{
			return false;
		}
	}
	return true;
}

} // namespace cb::net
