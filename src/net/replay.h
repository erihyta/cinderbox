#pragma once

// Replay files: the authoritative input frames of a whole server session, starting at tick 0,
// plus periodic state checksums. Because the simulation is deterministic, that is enough to
// reproduce every tick of the session exactly.
//
// Layout: "CBRP", u32 version, u64 fingerprint, config, then records of
//   u8 type (1 = frame, 2 = checksum), u32 payload size, payload.
// Frames are delta-coded like on the wire. A truncated last record (crash) is ignored.

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

	bool Open( const std::string& path, uint64_t fingerprint, const SimConfig& config );
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
	std::vector<InputFrame> m_frames;
	std::vector<MsgChecksum> m_checksums;
	bool m_truncated = false;
};

} // namespace cb::net
