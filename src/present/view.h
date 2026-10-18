#pragma once

// The viewer protocol: everything a viewer (the Godot client) is told, and
// the little it says back. A viewer draws ViewFrames and never asks where they came from.
//
//   source  --ViewFrame-->  viewer       the world at one tick, who is who, and how the source is
//   source  <--input-----   viewer       the local player's input, for sources that play one
//   source  <--control---   viewer       named commands with a number ("pause" 1, "seek" 12.5)
//
// A source is anything that produces frames: a live connection that predicts and rolls back
// (client/live_source.h), a recording re-simulated (client/replay_source.h), a file of frames
// played back with no simulation at all (view_file.h). Frames travel as bytes wherever a source and its viewer do not share a library
// (view_codec.h).
//
// A frame is self-contained: a viewer that joins late, or skips frames, needs nothing but the
// newest one. Events (impacts, mod events) are counters with a ring of the most recent, so a
// skipped frame loses nothing and a repeated one plays nothing twice.

#include "frame.h"
#include "mod_schema.h"

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace cb::present
{

// Seconds on the clock sources stamp frames with (ViewFrame::publishedAt). Steady, process-wide.
double ViewClock();

// A number or a word about the source, for a debug HUD and for tests ("rtt_ms", "desyncs").
struct ViewStat
{
	std::string name;
	std::variant<int64_t, double, bool, std::string> value;

	bool operator==( const ViewStat& ) const = default;
};

// A line of typed text a mod said (net::MsgText): its channel, who it is from (kNoSlot: the
// server's own words) and the words.
struct ViewText
{
	std::string channel;
	PlayerSlot from = kNoSlot;
	std::string text;

	bool operator==( const ViewText& ) const = default;
};

struct ViewFrame
{
	// Changes with every new frame; Take() copies only a newer one.
	uint64_t serial = 0;

	// --- The source ---------------------------------------------------------------------------
	// "connecting", "joining", "playing", "reconnecting", "rejected". "playing" means frames are
	// coming; a source that cannot go on says "rejected" and why in the "reject_reason" stat.
	std::string state = "connecting";
	std::vector<ViewStat> stats;

	// --- The session: changes rarely ----------------------------------------------------------
	// Identity and name of the map, for loading its visuals; name and prefab of each map template.
	uint64_t mapHash = 0;
	std::string mapName;
	std::vector<std::string> templateNames;
	std::vector<std::string> templateVisuals;
	// What the server's mods declared: names of fields, events, actions, items; the character and
	// its state machine; the workshop items a viewer needs. The generation changes with it.
	ModSchema schema;
	uint64_t schemaGeneration = 0;
	// The local player's private fields (BoardScope::Private), by slot: what only it was sent. All 0
	// for a source that has none (a recording, a view file: they were never written down).
	Blackboard privates;
	// Players' names by slot ("" for an empty slot or a source that knows none).
	std::array<std::string, kMaxPlayers> names;
	uint64_t namesGeneration = 0;

	// --- The world at frame.tick --------------------------------------------------------------
	bool hasWorld = false; // false until the source has one (still connecting)
	PresentationFrame frame;
	// The first tick the server has not confirmed: every tick before it is the server's word. 0 for
	// a source that does not know (a recording, a view file).
	uint32_t confirmedTick = 0;
	// Drawing between frame.tick - 1 and frame.tick: where the source was at publishedAt (0..1),
	// and how fast it moves on (ticks of world per tick of real time: 1 playing, 0 frozen or
	// paused, 2 a recording at double speed).
	double publishedAt = 0.0;
	float alphaAtPublish = 0.0f;
	float rate = 0.0f;
	// How many ticks lie between this source's frames (1: every tick). A source that sends fewer
	// frames than ticks says so, and the viewer draws from the frame before to this one over that
	// many ticks.
	uint32_t stride = 1;
	// Mod actions the local player pressed since the last frame taken, for a source that plays
	// someone else's input (a recording). A source fed by the viewer's own input leaves it 0: the
	// viewer already knows what it pressed.
	ActionBits localPressed = 0;

	// The newest lines of typed text (at most kViewTexts, oldest first), and how many have come in
	// all: the viewer shows the ones past the count it has seen. Sources without a connection
	// have none.
	static constexpr size_t kViewTexts = 32;
	std::vector<ViewText> texts;
	uint64_t textCount = 0;
};

class ViewSource
{
public:
	virtual ~ViewSource() = default;

	// Copies the newest frame into `out` if it is newer than `out.serial`.
	virtual bool Take( ViewFrame& out ) = 0;

	// Whether the viewer's input drives the local player (a live connection), and that input.
	virtual bool TakesInput() const
	{
		return false;
	}
	virtual void SetInput( const PlayerInput& )
	{
	}
	// Words the viewer typed into a channel's box, for the server (a live connection only).
	virtual void SendText( const std::string&, const std::string& )
	{
	}

	// A named command with a number. Sources ignore names they do not know and say which they do
	// in their own header.
	virtual void Control( const std::string&, double )
	{
	}

	// How far a ball of `radius` travels from `origin` along `direction` (unit length) before the
	// level's static shapes stop it, at most `maxDistance`: a third-person camera's question, asked
	// on the viewer's thread. -1: this source cannot say (the viewer asks the frame instead).
	virtual float CameraFreeDistance( b3Vec3, b3Vec3, float, float )
	{
		return -1.0f;
	}
};

} // namespace cb::present
