#include "view_file.h"

#include "view_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace cb::present
{

namespace
{
constexpr char kMagic[4] = { 'C', 'B', 'V', 'F' };
constexpr uint32_t kVersion = 2;
constexpr uint64_t kKeyInterval = 300;
constexpr size_t kHeaderSize = 8;
constexpr size_t kRecordHeaderSize = 5;
} // namespace

// --- Writer ----------------------------------------------------------------------------------------

ViewFileWriter::~ViewFileWriter()
{
	Close();
}

bool ViewFileWriter::Open( const std::string& path, uint32_t stride, ViewPrecision precision )
{
	Close();
	m_stride = std::max<uint32_t>( stride, 1 );
	m_precision = precision;
	m_given = 0;
	m_file = std::fopen( path.c_str(), "wb" );
	if ( m_file == nullptr )
	{
		return false;
	}
	std::fwrite( kMagic, 1, sizeof( kMagic ), m_file );
	std::fwrite( &kVersion, sizeof( kVersion ), 1, m_file );
	m_count = 0;
	return true;
}

void ViewFileWriter::Add( const ViewFrame& frame )
{
	if ( m_file == nullptr )
	{
		return;
	}
	// The frames in between are not kept; what happened in them is still in the next one's event
	// rings and counters.
	if ( m_given++ % m_stride != 0 )
	{
		return;
	}
	m_now = frame;
	m_now.serial = m_count + 1;
	m_now.stride = m_stride;
	bool key = m_count % kKeyInterval == 0;
	EncodeView( m_now, key ? nullptr : &m_last, 0.0, m_bytes, m_precision );
	uint32_t size = uint32_t( m_bytes.size() );
	uint8_t keyByte = key ? 1 : 0;
	std::fwrite( &size, sizeof( size ), 1, m_file );
	std::fwrite( &keyByte, 1, 1, m_file );
	std::fwrite( m_bytes.data(), 1, m_bytes.size(), m_file );
	std::swap( m_last, m_now );
	m_count += 1;
}

void ViewFileWriter::Flush()
{
	if ( m_file != nullptr )
	{
		std::fflush( m_file );
	}
}

void ViewFileWriter::Close()
{
	if ( m_file != nullptr )
	{
		std::fclose( m_file );
		m_file = nullptr;
	}
}

// --- Source ----------------------------------------------------------------------------------------

ViewFileSource::ViewFileSource( const std::string& path )
{
	FILE* file = std::fopen( path.c_str(), "rb" );
	if ( file == nullptr )
	{
		m_error = "cannot open " + path;
		return;
	}
	std::fseek( file, 0, SEEK_END );
	long size = std::ftell( file );
	std::fseek( file, 0, SEEK_SET );
	m_data.resize( size > 0 ? size_t( size ) : 0 );
	size_t read = m_data.empty() ? 0 : std::fread( m_data.data(), 1, m_data.size(), file );
	std::fclose( file );
	m_data.resize( read );

	uint32_t version = 0;
	if ( m_data.size() < kHeaderSize || std::memcmp( m_data.data(), kMagic, sizeof( kMagic ) ) != 0 )
	{
		m_error = path + " is not a view file";
		return;
	}
	std::memcpy( &version, m_data.data() + 4, sizeof( version ) );
	if ( version != kVersion )
	{
		m_error = path + " is a view file of version " + std::to_string( version ) + ", this build reads " + std::to_string( kVersion );
		return;
	}
	for ( size_t at = kHeaderSize; at + kRecordHeaderSize <= m_data.size(); )
	{
		Record record;
		std::memcpy( &record.size, m_data.data() + at, sizeof( record.size ) );
		record.key = m_data[at + 4] != 0;
		record.offset = at + kRecordHeaderSize;
		if ( record.offset + record.size > m_data.size() )
		{
			break; // truncated
		}
		m_records.push_back( record );
		at = record.offset + record.size;
	}
	if ( m_records.empty() || m_records[0].key == false )
	{
		m_records.clear();
		m_error = path + " holds no frames";
		return;
	}
	DecodeTo( 0, true );
}

bool ViewFileSource::DecodeTo( size_t target, bool fromKey )
{
	target = std::min( target, m_records.size() - 1 );
	size_t next = m_index + 1;
	if ( fromKey || m_index == SIZE_MAX || target < m_index )
	{
		next = target;
		while ( m_records[next].key == false )
		{
			next -= 1; // record 0 is a key
		}
		fromKey = true;
	}
	for ( ; next <= target; ++next )
	{
		const Record& record = m_records[next];
		if ( DecodeView( m_data.data() + record.offset, record.size, record.key ? nullptr : &m_current, m_scratch ) == false )
		{
			m_error = "the file is damaged at frame " + std::to_string( next );
			m_errorReported = false;
			return false;
		}
		std::swap( m_current, m_scratch );
		m_index = next;
		// What the followed player pressed on the way (not across a jump), from the recorded inputs.
		int slot = FollowedSlot();
		if ( fromKey == false && slot >= 0 && m_current.frame.hasInputs && m_scratch.frame.hasInputs )
		{
			m_pressed |= uint16_t( m_current.frame.inputs[slot].actions & ~m_scratch.frame.inputs[slot].actions );
		}
	}
	return true;
}

double ViewFileSource::FrameSeconds() const
{
	double tick = m_current.hasWorld && m_current.frame.tickSeconds > 0.0f ? double( m_current.frame.tickSeconds ) : 1.0 / 60.0;
	return tick * double( m_current.stride );
}

uint32_t ViewFileSource::PlayerInSlot( int slot ) const
{
	for ( const FrameEntity& e : m_current.frame.entities )
	{
		if ( e.kind == VisualKind::Player && int( e.slot ) == slot )
		{
			return e.netId;
		}
	}
	return 0;
}

int ViewFileSource::NextSlot( int from ) const
{
	int best = -1;
	int first = -1;
	for ( const FrameEntity& e : m_current.frame.entities )
	{
		if ( e.kind != VisualKind::Player )
		{
			continue;
		}
		int slot = int( e.slot );
		first = first < 0 || slot < first ? slot : first;
		if ( slot > from && ( best < 0 || slot < best ) )
		{
			best = slot;
		}
	}
	return best >= 0 ? best : first;
}

// The slot of the frame's local player as this source shows it, or -1.
int ViewFileSource::FollowedSlot() const
{
	if ( m_autoFollow == false )
	{
		return m_follow >= 0 && PlayerInSlot( m_follow ) != 0 ? m_follow : -1;
	}
	// Whoever the file was recorded as, else the first player there is.
	for ( const FrameEntity& e : m_current.frame.entities )
	{
		if ( m_current.frame.localNetId != 0 && e.netId == m_current.frame.localNetId && e.kind == VisualKind::Player )
		{
			return int( e.slot );
		}
	}
	if ( m_follow >= 0 && PlayerInSlot( m_follow ) != 0 )
	{
		return m_follow;
	}
	return NextSlot( -1 );
}

void ViewFileSource::Control( const std::string& name, double value )
{
	if ( m_records.empty() || std::isfinite( value ) == false )
	{
		return;
	}
	double last = double( m_records.size() - 1 );
	auto seekFrames = [&]( double to ) {
		m_position = std::clamp( std::floor( to ), 0.0, last );
		m_seek = true;
	};
	if ( name == "pause" )
	{
		m_paused = value != 0.0;
	}
	else if ( name == "speed" )
	{
		m_speed = std::clamp( value, 0.125, 16.0 );
	}
	else if ( name == "seek" )
	{
		seekFrames( value / FrameSeconds() + 0.5 );
	}
	else if ( name == "skip" )
	{
		seekFrames( m_position + value / FrameSeconds() + 0.5 );
	}
	else if ( name == "step" )
	{
		m_paused = true;
		seekFrames( std::floor( m_position ) + std::round( value ) );
	}
	else if ( name == "follow" )
	{
		m_autoFollow = false;
		m_follow = value >= 0.0 && value < double( kMaxPlayers ) ? int( value ) : -1;
	}
	else if ( name == "follow_next" )
	{
		m_follow = NextSlot( FollowedSlot() );
		m_autoFollow = false;
	}
	else
	{
		return;
	}
	m_dirty = true;
}

bool ViewFileSource::Take( ViewFrame& out )
{
	double now = ViewClock();
	if ( m_error.empty() == false )
	{
		if ( m_errorReported )
		{
			return false;
		}
		m_errorReported = true;
		out = ViewFrame();
		out.serial = ++m_serial;
		out.state = "rejected";
		out.stats.push_back( { "reject_reason", m_error } );
		out.publishedAt = now;
		return true;
	}

	// Play on from where the last frame was taken.
	double last = double( m_records.size() - 1 );
	if ( m_lastTake >= 0.0 && m_paused == false )
	{
		m_position += ( now - m_lastTake ) * m_speed / FrameSeconds();
	}
	m_lastTake = now;
	bool ended = m_position >= last + 1.0;
	m_position = std::min( m_position, last + 1.0 );
	size_t target = std::min( size_t( m_position ), m_records.size() - 1 );

	m_dirty |= ended != m_ended; // the end is news even though no frame follows it
	m_ended = ended;

	bool moved = target != m_index || m_seek;
	if ( moved )
	{
		if ( m_seek )
		{
			m_jumps += 1;
		}
		if ( DecodeTo( target, m_seek ) == false )
		{
			return Take( out ); // reports the damage
		}
		m_seek = false;
	}
	if ( moved == false && m_dirty == false )
	{
		return false;
	}
	m_dirty = false;

	int slot = FollowedSlot();
	out = m_current;
	out.serial = ++m_serial;
	out.state = "playing";
	out.publishedAt = now;
	out.alphaAtPublish = float( std::clamp( m_position - double( target ), 0.0, 1.0 ) );
	out.rate = m_paused || ended ? 0.0f : float( m_speed );
	out.localPressed = m_pressed;
	m_pressed = 0;
	out.frame.localNetId = slot >= 0 ? PlayerInSlot( slot ) : 0;
	out.frame.resetGeneration += m_jumps;
	out.frame.rolledBack = false;

	double seconds = FrameSeconds();
	out.stats.clear();
	out.stats.push_back( { "reject_reason", std::string() } );
	out.stats.push_back( { "tick", int64_t( m_current.frame.tick ) } );
	out.stats.push_back( { "replay_seconds", double( target ) * seconds } );
	out.stats.push_back( { "replay_length_seconds", double( m_records.size() ) * seconds } );
	out.stats.push_back( { "replay_speed", m_speed } );
	out.stats.push_back( { "replay_paused", m_paused } );
	out.stats.push_back( { "replay_ended", ended } );
	out.stats.push_back( { "replay_follow", int64_t( slot ) } );
	return true;
}

bool ReadViewFileInfo( const std::string& path, ViewFileInfo& info, std::string& error )
{
	ViewFileSource file( path );
	info = ViewFileInfo();
	for ( size_t i = 0; i < file.m_records.size() && file.m_error.empty(); ++i )
	{
		const ViewFileSource::Record& record = file.m_records[i];
		if ( file.DecodeTo( i, false ) == false )
		{
			break;
		}
		const PresentationFrame& f = file.m_current.frame;
		size_t players = size_t( std::count_if( f.entities.begin(), f.entities.end(), []( const FrameEntity& e ) { return e.kind == VisualKind::Player; } ) );
		info.frames += 1;
		info.keys += record.key ? 1 : 0;
		( record.key ? info.keyBytes : info.deltaBytes ) += record.size;
		info.deltaMost = record.key ? info.deltaMost : std::max( info.deltaMost, record.size );
		info.seconds += file.FrameSeconds();
		info.mostEntities = std::max( info.mostEntities, f.entities.size() );
		info.mostPlayers = std::max( info.mostPlayers, players );
		info.map = file.m_current.mapName;
		info.stride = file.m_current.stride;
		info.compact = ViewPacketCompact( file.m_data.data() + record.offset, record.size );
		if ( record.key == false )
		{
			// The same packet again, this time counting (m_scratch is the frame before).
			std::vector<uint8_t> again;
			EncodeView( file.m_current, &file.m_scratch, 0.0, again, info.compact ? ViewPrecision::Compact : ViewPrecision::Exact, &info.cost );
		}
	}
	error = file.m_error;
	return error.empty();
}

} // namespace cb::present
