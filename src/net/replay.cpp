#include "replay.h"

#include <fstream>
#include <iterator>

namespace cb::net
{

namespace
{
constexpr uint32_t kMagic = 0x50524243u; // "CBRP"
constexpr uint32_t kVersion = 4; // 2: per-field input encoding, 3: baked map, 4: commands and mod schema
constexpr uint8_t kRecordFrame = 1;
constexpr uint8_t kRecordChecksum = 2;
} // namespace

ReplayWriter::~ReplayWriter()
{
	Close();
}

bool ReplayWriter::Open( const std::string& path, uint64_t fingerprint, const SimConfig& config, const std::vector<uint8_t>& map,
						const std::vector<uint8_t>& schema )
{
	Close();
	m_file = std::fopen( path.c_str(), "wb" );
	if ( m_file == nullptr )
	{
		return false;
	}

	// The config goes through the Welcome encoder so both stay in sync.
	MsgWelcome header;
	header.fingerprint = fingerprint;
	header.config = config;
	header.map = map;
	header.mapHash = MapHash( map.data(), map.size() );
	header.schema = schema;
	std::vector<uint8_t> bytes;
	Encode( header, bytes );

	std::vector<uint8_t> out;
	ByteWriter w( out );
	w.Write( kMagic );
	w.Write( kVersion );
	w.WriteBlob( bytes );
	std::fwrite( out.data(), 1, out.size(), m_file );

	m_codec.Reset( {} );
	m_nextTick = 0;
	return true;
}

void ReplayWriter::WriteRecord( uint8_t type, const std::vector<uint8_t>& payload )
{
	uint32_t size = uint32_t( payload.size() );
	std::fwrite( &type, 1, 1, m_file );
	std::fwrite( &size, sizeof( size ), 1, m_file );
	std::fwrite( payload.data(), 1, payload.size(), m_file );
}

void ReplayWriter::AddFrame( const InputFrame& frame )
{
	if ( m_file == nullptr || frame.tick != m_nextTick )
	{
		return;
	}
	m_codec.Encode( frame, m_scratch );
	WriteRecord( kRecordFrame, m_scratch );
	m_nextTick += 1;
}

void ReplayWriter::AddChecksum( uint32_t tick, uint64_t hash )
{
	if ( m_file == nullptr )
	{
		return;
	}
	Encode( MsgChecksum{ tick, hash }, m_scratch );
	WriteRecord( kRecordChecksum, m_scratch );
}

void ReplayWriter::Flush()
{
	if ( m_file != nullptr )
	{
		std::fflush( m_file );
	}
}

void ReplayWriter::Close()
{
	if ( m_file != nullptr )
	{
		std::fclose( m_file );
		m_file = nullptr;
	}
}

bool ReplayReader::Open( const std::string& path, std::string& error )
{
	std::ifstream in( path, std::ios::binary );
	if ( in.good() == false )
	{
		error = "cannot open " + path;
		return false;
	}
	std::vector<uint8_t> data( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );

	ByteReader r( data.data(), data.size() );
	if ( r.Read<uint32_t>() != kMagic )
	{
		error = "not a replay file";
		return false;
	}
	if ( r.Read<uint32_t>() != kVersion )
	{
		error = "unsupported replay version";
		return false;
	}
	std::vector<uint8_t> headerBytes;
	r.ReadBlob( headerBytes );
	ByteReader hr( headerBytes.data(), headerBytes.size() );
	MsgWelcome header;
	if ( r.Ok() == false || ReadType( hr ) != MsgType::Welcome || Decode( hr, header ) == false )
	{
		error = "corrupt replay header";
		return false;
	}
	m_fingerprint = header.fingerprint;
	m_config = header.config;
	m_mapBytes = std::move( header.map );
	if ( DecodeSchema( header.schema.data(), header.schema.size(), m_schema ) == false )
	{
		error = "corrupt mod schema in the replay header";
		return false;
	}
	m_graph.reset();
	if ( m_schema.animGraph.empty() == false )
	{
		std::string graphError, warnings;
		m_graph = CompileAnimGraph( m_schema.animGraph, m_schema, graphError, warnings );
		if ( m_graph == nullptr )
		{
			error = "replay's animation state machine: " + graphError;
			return false;
		}
	}
	std::string mapError;
	if ( DeserializeMap( m_mapBytes.data(), m_mapBytes.size(), m_map, mapError ) == false )
	{
		error = "replay map: " + mapError;
		return false;
	}

	FrameCodec codec;
	m_frames.clear();
	m_checksums.clear();
	m_truncated = false;
	while ( r.AtEnd() == false )
	{
		uint8_t type = r.Read<uint8_t>();
		uint32_t size = r.Read<uint32_t>();
		const uint8_t* payload = r.Take( size );
		if ( payload == nullptr )
		{
			m_truncated = true;
			break;
		}

		ByteReader pr( payload, size );
		auto msgType = ReadType( pr );
		if ( type == kRecordFrame && msgType == MsgType::Frame )
		{
			InputFrame frame;
			if ( codec.Decode( pr, frame ) == false || frame.tick != m_frames.size() )
			{
				m_truncated = true;
				break;
			}
			m_frames.push_back( std::move( frame ) );
		}
		else if ( type == kRecordChecksum && msgType == MsgType::Checksum )
		{
			MsgChecksum c;
			if ( Decode( pr, c ) )
			{
				m_checksums.push_back( c );
			}
		}
	}
	return true;
}

} // namespace cb::net
