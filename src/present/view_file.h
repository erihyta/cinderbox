#pragma once

// View files: a session as the frames a viewer was (or would have been) shown, as bytes
// (view_codec.h). Unlike a replay (net/replay.h, inputs to re-simulate), a view file needs no
// simulation to play, so it plays on any build and in a viewer that has none.
//
// Layout: "CBVF", u32 version, then records of
//   u32 size, u8 key (1: the packet stands alone; otherwise a delta against the record before),
//   the packet.
// Every 300th record is a key, for seeking. A truncated last record (a crash) is ignored.
//
// A file may hold fewer frames than the session had ticks (a stride: every third tick is 20 frames
// a second), and its packets may be compact (view_codec.h): together, a much smaller file.

#include "view.h"
#include "view_codec.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cb::present
{

class ViewFileWriter
{
public:
	ViewFileWriter() = default;
	~ViewFileWriter();
	ViewFileWriter( const ViewFileWriter& ) = delete;
	ViewFileWriter& operator=( const ViewFileWriter& ) = delete;

	// `stride`: one frame is kept for every so many given. `precision`: how its packets are made.
	bool Open( const std::string& path, uint32_t stride = 1, ViewPrecision precision = ViewPrecision::Exact );
	// One frame per tick, in order. Its serial does not matter: the file numbers its own.
	void Add( const ViewFrame& frame );
	void Flush();
	void Close();
	bool IsOpen() const
	{
		return m_file != nullptr;
	}

private:
	FILE* m_file = nullptr;
	ViewFrame m_last;
	ViewFrame m_now;
	uint64_t m_count = 0; // frames written
	uint64_t m_given = 0; // frames handed to Add
	uint32_t m_stride = 1;
	ViewPrecision m_precision = ViewPrecision::Exact;
	std::vector<uint8_t> m_bytes;
};

// What a view file holds, by decoding all of it (the cb_replay tool prints it).
struct ViewFileInfo
{
	size_t frames = 0;
	size_t keys = 0;
	double seconds = 0.0;
	uint64_t keyBytes = 0;	 // all the frames that stand alone
	uint64_t deltaBytes = 0; // all the others
	uint32_t deltaMost = 0;	 // the biggest of those
	size_t mostEntities = 0;
	size_t mostPlayers = 0;
	uint32_t stride = 1; // ticks between frames
	bool compact = false;
	ViewCost cost;		 // where the bytes of all the delta frames went
	std::string map;
};
bool ReadViewFileInfo( const std::string& path, ViewFileInfo& info, std::string& error );

// A view source that plays a view file. No thread and no simulation: frames are decoded as the
// viewer asks for them.
//
// States: "playing" for as long as the file plays (paused and ended included: see the stats),
// "rejected" when it cannot (the "reject_reason" stat says why).
//
// Controls, as for a replay (client/replay_source.h):
//   "pause"        1 pauses, 0 plays on
//   "speed"        times real time, 0.125 to 16
//   "seek"         to this many seconds from the start
//   "skip"         by this many seconds from where it is (negative: back)
//   "step"         by this many frames (negative: back), and pauses
//   "follow"       the player in this slot is the local one; -1: nobody
//   "follow_next"  the next player in the world
// Until a "follow", the local player is the one the file was recorded as, or the first there is.
//
// Stats: "reject_reason", "tick", "replay_seconds", "replay_length_seconds", "replay_speed",
// "replay_paused", "replay_ended", "replay_follow" (the slot, or -1).
class ViewFileSource final : public ViewSource
{
public:
	explicit ViewFileSource( const std::string& path );

	bool Take( ViewFrame& out ) override;
	void Control( const std::string& name, double value ) override;

	// How many frames the file holds (0 when it could not be read).
	size_t Length() const
	{
		return m_records.size();
	}
	friend bool ReadViewFileInfo( const std::string& path, ViewFileInfo& info, std::string& error );

private:
	struct Record
	{
		size_t offset = 0;
		uint32_t size = 0;
		bool key = false;
	};

	// Decodes up to record `target`: on from where it is, or from the key before it. False when
	// the file turns out damaged (m_error says where).
	bool DecodeTo( size_t target, bool fromKey );
	// How long one of the file's frames lasts: its stride of ticks.
	double FrameSeconds() const;
	int FollowedSlot() const;
	uint32_t PlayerInSlot( int slot ) const;
	int NextSlot( int from ) const;

	std::vector<uint8_t> m_data;
	std::vector<Record> m_records;
	std::string m_error;
	bool m_errorReported = false;

	ViewFrame m_current; // record m_index, decoded
	ViewFrame m_scratch;
	size_t m_index = SIZE_MAX;

	double m_position = 0.0; // in frames: record floor(position), drawn its fraction of the way on
	double m_lastTake = -1.0;
	double m_speed = 1.0;
	bool m_paused = false;
	bool m_ended = false;
	bool m_dirty = true; // something the viewer should hear about changed
	bool m_seek = false; // the next decode is a jump, not playing on
	uint64_t m_jumps = 0;
	uint64_t m_serial = 0;

	int m_follow = -1; // slot
	bool m_autoFollow = true;
	ActionBits m_pressed = 0;
};

} // namespace cb::present
