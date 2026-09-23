#pragma once

// Replay files: the authoritative input frames of a whole server session, starting at tick 0,
// plus periodic state checksums. Because the simulation is deterministic, that is enough to
// reproduce every tick of the session exactly.
//
// Layout: "CBRP", u32 version, a Welcome (fingerprint, config, map, mod schema), then records of
//   u8 type (1 = frame, 2 = checksum), u32 payload size, payload.
// Frames are delta-coded like on the wire. A truncated last record (crash) is ignored.

#include "map.h"
#include "mod_schema.h"
#include "protocol.h"

#include <cstdio>
#include <string>
#include <vector>

namespace cb::net
{

class ReplayWriter
{
public:
	ReplayWriter() = default;
	~ReplayWriter();
	ReplayWriter( const ReplayWriter& ) = delete;
	ReplayWriter& operator=( const ReplayWriter& ) = delete;

	// `schema` is the server's encoded ModSchema, so a viewer can name fields and events.
	bool Open( const std::string& path, uint64_t fingerprint, const SimConfig& config, const std::vector<uint8_t>& map,
			   const std::vector<uint8_t>& schema = {} );
	void AddFrame( const InputFrame& frame );
	void AddChecksum( uint32_t tick, uint64_t hash );
	void Flush();
	void Close();
	bool IsOpen() const
	{
		return m_file != nullptr;
	}

private:
	void WriteRecord( uint8_t type, const std::vector<uint8_t>& payload );

	FILE* m_file = nullptr;
	FrameCodec m_codec;
	std::vector<uint8_t> m_scratch;
	uint32_t m_nextTick = 0;
};

class ReplayReader
{
public:
	bool Open( const std::string& path, std::string& error );

	uint64_t Fingerprint() const
	{
		return m_fingerprint;
	}
	const SimConfig& Config() const
	{
		return m_config;
	}
	// The baked map the session was played on (sim/map.h). Empty only in replays of the built-in
	// sandbox written before maps existed, which no longer load.
	const std::vector<uint8_t>& MapBytes() const
	{
		return m_mapBytes;
	}
	const LevelLayout& Map() const
	{
		return m_map;
	}
	// What the server's mods declared (empty for a server without mods).
	const ModSchema& Schema() const
	{
		return m_schema;
	}
	// frames[i].tick == i
	const std::vector<InputFrame>& Frames() const
	{
		return m_frames;
	}
	const std::vector<MsgChecksum>& Checksums() const
	{
		return m_checksums;
	}
	bool Truncated() const
	{
		return m_truncated;
	}

private:
	uint64_t m_fingerprint = 0;
	SimConfig m_config;
	std::vector<uint8_t> m_mapBytes;
	LevelLayout m_map;
	ModSchema m_schema;
	std::vector<InputFrame> m_frames;
	std::vector<MsgChecksum> m_checksums;
	bool m_truncated = false;
};

} // namespace cb::net
