#include "motions.h"

#include "detmath.h"
#include "events.h"

#include <algorithm>
#include <sstream>

namespace cb
{

namespace
{

constexpr float kMaxMotionSeconds = 3600.0f;
constexpr float kMaxMotionImpulse = 200.0f; // m/s
constexpr float kMaxMotionAccel = 1000.0f;	// m/s^2
constexpr float kMaxMotionForce = 1.0e6f;	// N
constexpr uint32_t kMaxMotionUses = 1000;

std::vector<std::string> Words( const std::string& line )
{
	std::vector<std::string> words;
	size_t at = 0;
	while ( at <= line.size() )
	{
		size_t tab = line.find( '\t', at );
		if ( tab == std::string::npos )
		{
			tab = line.size();
		}
		words.push_back( line.substr( at, tab - at ) );
		at = tab + 1;
	}
	return words;
}

bool Number( const std::string& text, float lo, float hi, float& out )
{
	float value = 0.0f;
	if ( ParseAnimFloat( text.c_str(), text.size(), value ) == false || ( value >= lo && value <= hi ) == false )
	{
		return false;
	}
	out = value;
	return true;
}

uint32_t Ticks( float seconds, uint32_t tickRate )
{
	return uint32_t( seconds * float( tickRate ) + 0.5f );
}

bool Pressed( const Motion& m, const MotionInputs& in, ActionBits previousActions )
{
	if ( m.action == kMotionActionJump )
	{
		return ( in.pressedButtons & BtnJump ) != 0;
	}
	if ( m.action == kMotionActionSprint )
	{
		return ( in.pressedButtons & BtnSprint ) != 0;
	}
	if ( m.action < 0 || m.action >= kMaxActions )
	{
		return false;
	}
	ActionBits bit = ActionBits( 1 ) << m.action;
	return ( in.input->actions & bit ) != 0 && ( previousActions & bit ) == 0;
}

// Whether the mod event was recorded at this player this tick (a server mod's Emit: commands run
// before motions).
bool HeardEvent( int event, const MotionInputs& in )
{
	const AnimGraphInputs& values = *in.values;
	uint32_t kept = std::min( values.eventCount, kModEventHistory );
	for ( uint32_t i = 0; values.events != nullptr && i < kept; ++i )
	{
		const ModEventRecord& e = values.events[( values.eventCount - 1 - i ) % kModEventHistory];
		if ( e.tick + 1 < in.tick )
		{
			break; // newest first: the rest are older
		}
		// This tick's, and last tick's that came after the motions had run (they did not hear it).
		bool heard = e.tick == in.tick || e.late != 0;
		if ( heard && int( e.type ) == event && e.netIdA == in.netId )
		{
			return true;
		}
	}
	return false;
}

void ApplyChange( const MotionStep::Change& change, Blackboard& board )
{
	int32_t& stored = board.values[change.slot];
	if ( change.type == BoardType::Float )
	{
		float value = BoardToFloat( stored );
		value = change.op == Motion::ChangeOp::Set ? change.value
													: ( change.op == Motion::ChangeOp::Add ? value + change.value : value - change.value );
		stored = BoardFromFloat( value );
		return;
	}
	// Whole numbers: the amount is rounded once, when the set is compiled.
	int64_t amount = int64_t( change.value );
	int64_t value = stored;
	value = change.op == Motion::ChangeOp::Set ? amount : ( change.op == Motion::ChangeOp::Add ? value + amount : value - amount );
	value = std::clamp<int64_t>( value, INT32_MIN, INT32_MAX );
	stored = change.type == BoardType::Bool ? ( value != 0 ? 1 : 0 ) : int32_t( value );
}

// "self", "hit", or "@field.name".
bool ParseTarget( const std::string& word, const ModSchema& schema, const std::string& motion, MotionTarget& out, std::string& warnings )
{
	if ( word == "self" )
	{
		out.kind = MotionTarget::Kind::Self;
		return true;
	}
	if ( word == "hit" )
	{
		out.kind = MotionTarget::Kind::Hit;
		return true;
	}
	if ( word == "found" )
	{
		out.kind = MotionTarget::Kind::Found;
		return true;
	}
	if ( word.size() > 1 && word[0] == '@' )
	{
		out.kind = MotionTarget::Kind::Field;
		const BoardField* field = schema.FindField( word.substr( 1 ) );
		if ( field == nullptr || field->scope != BoardScope::Entity )
		{
			out.known = false;
			warnings += motion + ": \"" + word.substr( 1 ) + "\" is not a field of the player a mod declares: an effect on " + word +
						" does nothing; ";
			return true;
		}
		out.slot = field->slot;
		return true;
	}
	return false;
}

bool ParseFrame( const std::string& word, MotionFrame& out )
{
	static const char* const kFrames[] = { "look", "move", "facing", "up", "world", "to", "away", "marker" };
	for ( int i = 0; i < 8; ++i )
	{
		if ( word == kFrames[i] )
		{
			out = MotionFrame( i );
			return true;
		}
	}
	return false;
}

// The "x y z" a world frame ends its line with, from word `at`.
bool ParseWorld( const std::vector<std::string>& w, size_t at, b3Vec3& out )
{
	b3Vec3 d = {};
	if ( w.size() < at + 3 || Number( w[at], -1.0f, 1.0f, d.x ) == false || Number( w[at + 1], -1.0f, 1.0f, d.y ) == false ||
		 Number( w[at + 2], -1.0f, 1.0f, d.z ) == false )
	{
		return false;
	}
	float length = 0.0f;
	d = b3GetLengthAndNormalize( &length, d );
	if ( length < 0.001f )
	{
		return false;
	}
	out = d;
	return true;
}

} // namespace

b3Vec3 MotionDirection( MotionFrame frame, b3Vec3 world, const Character& c, const PlayerInput& in )
{
	float yaw = detmath::YawToRadians( in.cameraYaw );
	switch ( frame )
	{
		case MotionFrame::Look:
		{
			b3CosSin pitch = detmath::CosSin( float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f ) );
			b3Vec3 forward = detmath::YawForward( yaw );
			return { forward.x * pitch.cosine, pitch.sine, forward.z * pitch.cosine };
		}
		case MotionFrame::Move:
		{
			// The mover's own wish direction; standing still, a dash goes where the body faces.
			float forward = float( std::clamp<int>( in.moveForward, -127, 127 ) );
			float right = float( std::clamp<int>( in.moveRight, -127, 127 ) );
			b3Vec3 wish = b3Add( b3MulSV( forward, detmath::YawForward( yaw ) ), b3MulSV( right, detmath::YawRight( yaw ) ) );
			float length = 0.0f;
			b3Vec3 direction = b3GetLengthAndNormalize( &length, wish );
			return length > 0.0f ? direction : detmath::YawForward( c.facingYaw );
		}
		case MotionFrame::Facing:
			return detmath::YawForward( c.facingYaw );
		case MotionFrame::Up:
			return { 0.0f, 1.0f, 0.0f };
		case MotionFrame::World:
			return world;
		case MotionFrame::Marker:
		{
			// As drawn, turned as the look is: its heading, then its pitch.
			b3CosSin pitch = detmath::CosSin( float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f ) );
			b3Vec3 up = { world.x, world.y * pitch.cosine + world.z * pitch.sine, -world.y * pitch.sine + world.z * pitch.cosine };
			return b3RotateVector( detmath::YawRotation( yaw ), up );
		}
		case MotionFrame::To:
		case MotionFrame::Away:
			break;
	}
	return { 0.0f, 1.0f, 0.0f };
}

b3Vec3 MotionTurn( const MotionEffect& effect, b3Vec3 d )
{
	if ( effect.turned == false )
	{
		return d;
	}
	// Its heading, about up; then its pitch, about its own right (straight up or down has no right:
	// it keeps it).
	b3Vec3 r = { d.x * effect.yawCos + d.z * effect.yawSin, d.y, -d.x * effect.yawSin + d.z * effect.yawCos };
	float flat = std::sqrt( r.x * r.x + r.z * r.z );
	if ( flat < 1.0e-6f )
	{
		return r;
	}
	// (Rodrigues' turn about k = (-z, 0, x) / flat: k x r is (-xy / flat, flat, -zy / flat), k . r is 0.)
	b3Vec3 across = { -r.x * r.y / flat, flat, -r.z * r.y / flat };
	return b3Add( b3MulSV( effect.pitchCos, r ), b3MulSV( effect.pitchSin, across ) );
}

b3Vec3 MotionPathAt( const Motion::Path& path, float u )
{
	const size_t count = path.points.size();
	if ( count == 0 )
	{
		return { 0.0f, 0.0f, 0.0f };
	}
	if ( count == 1 )
	{
		return path.points.front();
	}
	u = u > 0.0f ? ( u < 1.0f ? u : 1.0f ) : 0.0f;
	float at = u * float( count - 1 );
	size_t i = std::min( size_t( at ), count - 2 );
	float f = at - float( i );
	return b3Add( path.points[i], b3MulSV( f, b3Sub( path.points[i + 1], path.points[i] ) ) );
}

float MotionRamp( const MotionEffect& effect, uint32_t ticksOn, uint32_t tickRate )
{
	uint32_t ramp = Ticks( effect.ramp, tickRate );
	// The first tick already pushes a little: a ramp of n ticks is full on its nth.
	return ramp == 0 || ticksOn + 1 >= ramp ? 1.0f : float( ticksOn + 1 ) / float( ramp );
}

bool CompileMotionSet( const std::string& set, const std::string& text, const ModSchema& schema, std::vector<Motion>& out,
					   std::string& error, std::string& warnings )
{
	std::istringstream lines( text );
	std::string line;
	bool header = false;
	std::vector<Motion> motions;
	// Per step of the motion being read: its current guard, and its guards' conditions as written.
	std::vector<int8_t> guards;
	std::vector<std::vector<std::string>> guardTexts;
	bool inFind = false; // between a "find" and its "endfind": what happens to each thing found
	int number = 0;
	auto fail = [&]( const std::string& what ) {
		SetAnimCurveNames( nullptr );
		error = "motions " + set + ", line " + std::to_string( number ) + ": " + what;
		return false;
	};
	// A statement's number: as written, or an expression (kept in the motion's exprs) read when it acts.
	auto valueOf = [&]( Motion& m, const std::string& word, float lo, float hi, MotionValue& out, std::string& why ) {
		if ( Number( word, lo, hi, out.number ) )
		{
			return true;
		}
		float number = 0.0f;
		if ( ParseAnimFloat( word.c_str(), word.size(), number ) )
		{
			why = "a number from " + std::to_string( lo ) + " to " + std::to_string( hi );
			return false;
		}
		AnimExpr expr;
		if ( CompileAnimExpr( word, schema, expr, why, warnings ) == false )
		{
			return false;
		}
		out.number = 0.0f;
		out.expr = int16_t( m.exprs.size() );
		m.exprs.push_back( expr );
		return true;
	};
	// A strength: a number, or an expression (kept in the motion's exprs) read when it acts.
	auto strength = [&]( Motion& m, const std::string& word, float limit, MotionEffect& e, std::string& why ) {
		if ( Number( word, -limit, limit, e.strength ) )
		{
			return true;
		}
		float number = 0.0f;
		if ( ParseAnimFloat( word.c_str(), word.size(), number ) )
		{
			why = "a number from " + std::to_string( int( -limit ) ) + " to " + std::to_string( int( limit ) );
			return false;
		}
		AnimExpr expr;
		if ( CompileAnimExpr( word, schema, expr, why, warnings ) == false )
		{
			return false;
		}
		e.strength = 0.0f;
		e.strengthExpr = int16_t( m.exprs.size() );
		m.exprs.push_back( expr );
		return true;
	};
	while ( std::getline( lines, line ) )
	{
		number += 1;
		while ( line.empty() == false && ( line.back() == '\r' || line.back() == ' ' ) )
		{
			line.pop_back();
		}
		if ( line.empty() || line[0] == '#' )
		{
			continue;
		}
		std::vector<std::string> w = Words( line );
		const std::string& key = w[0];
		if ( header == false )
		{
			// 3: steps, guards, ends, a parameter's seconds, more than one emit, an edge (a sheet's blocks)
			if ( key != "cinderbox_motions" || w.size() < 2 || ( w[1] != "2" && w[1] != "3" ) )
			{
				return fail( "not a motions file (it starts with \"cinderbox_motions\t2\" or 3)" );
			}
			header = true;
			continue;
		}
		if ( key == "motion" )
		{
			if ( w.size() < 2 || w[1].empty() )
			{
				return fail( "a motion needs a name" );
			}
			if ( inFind )
			{
				return fail( "a find ends (endfind) before the next motion" );
			}
			Motion m;
			m.name = set + "/" + w[1];
			motions.push_back( m );
			guards.assign( 1, -1 );
			guardTexts.assign( 1, {} );
			continue;
		}
		if ( motions.empty() )
		{
			return fail( "\"" + key + "\" before the first motion" );
		}
		Motion& m = motions.back();
		MotionStep& step = m.steps.back();
		SetAnimCurveNames( &m.curves ); // ("curve(NAME, x)" names this motion's)
		// The step's guard for what follows ("guard": none), as its bit.
		int8_t& guard = guards.back();
		// What a motion as a whole says comes before its first step's statements end.
		auto whole = [&]() {
			return m.steps.size() == 1 ? true : fail( "\"" + key + "\" is the motion's: before its first \"step\"" );
		};
		// --- a find: its shape, then what happens to each thing it found, then endfind
		if ( key == "find" )
		{
			// find <players|bodies|all> <self 0|1> <at most> <facing|look|world>
			if ( inFind )
			{
				return fail( "a find inside a find" );
			}
			MotionFind find;
			find.who = w.size() >= 2 ? ( w[1] == "players" ? kProbeHoldsPlayers : w[1] == "bodies" ? kProbeHoldsBodies : w[1] == "all" ? uint8_t( kProbeHoldsPlayers | kProbeHoldsBodies ) : 0 ) : 0;
			float most = 0.0f;
			if ( w.size() < 5 || find.who == 0 || ( w[2] != "0" && w[2] != "1" ) || Number( w[3], 1.0f, float( kFindMost ), most ) == false ||
				 ( w[4] != "facing" && w[4] != "look" && w[4] != "world" ) )
			{
				return fail( "find wants who (players, bodies, all), whether the player itself counts (0 or 1), at most how many (1 to " +
							 std::to_string( kFindMost ) + ") and how its shape is turned (facing, look, world)" );
			}
			find.self = w[2] == "1";
			find.most = uint8_t( most );
			find.frame = w[4] == "facing" ? MotionFrame::Facing : w[4] == "look" ? MotionFrame::Look : MotionFrame::World;
			find.guard = guard;
			step.finds.push_back( find );
			inFind = true;
			continue;
		}
		if ( key == "shape" )
		{
			// shape <radius> <x y z>...: a convex piece, in the player's frame
			MotionFind::Piece piece;
			size_t count = w.size() >= 2 ? ( w.size() - 2 ) / 3 : 0;
			if ( inFind == false || w.size() < 5 || ( w.size() - 2 ) % 3 != 0 || count > 128 || Number( w[1], 0.0f, 100.0f, piece.radius ) == false )
			{
				return fail( "shape (inside a find) wants a radius (0 to 100 m) and its points, x y z each (1 to 128)" );
			}
			for ( size_t k = 0; k < count; ++k )
			{
				b3Vec3 point = {};
				if ( Number( w[2 + 3 * k], -100.0f, 100.0f, point.x ) == false || Number( w[3 + 3 * k], -100.0f, 100.0f, point.y ) == false ||
					 Number( w[4 + 3 * k], -100.0f, 100.0f, point.z ) == false )
				{
					return fail( "a shape's points are within 100 m of the player" );
				}
				piece.points.push_back( point );
			}
			step.finds.back().pieces.push_back( piece );
			continue;
		}
		if ( key == "endfind" )
		{
			if ( inFind == false || step.finds.back().pieces.empty() )
			{
				return fail( inFind ? "a find has a shape" : "endfind with no find" );
			}
			MotionFind& done = step.finds.back();
			for ( const MotionFind::Piece& piece : done.pieces )
			{
				for ( const b3Vec3& p : piece.points )
				{
					done.extent = std::max( done.extent, std::sqrt( p.x * p.x + p.z * p.z ) + piece.radius );
				}
			}
			inFind = false;
			continue;
		}
		if ( inFind && key != "impulse" && key != "emit" && key != "param" && key != "turn" )
		{
			return fail( "\"" + key + "\" inside a find: what happens to each thing found is an impulse, an emit or a param (with its seconds)" );
		}
		if ( key == "when" )
		{
			if ( whole() == false )
			{
				return false;
			}
			if ( w.size() >= 2 && w[1] == "while" )
			{
				m.when = Motion::When::While;
				continue;
			}
			if ( w.size() >= 2 && w[1] == "edge" )
			{
				std::string exprError;
				m.when = Motion::When::Edge;
				if ( w.size() < 3 || CompileAnimExpr( w[2], schema, m.edge, exprError, warnings ) == false )
				{
					return fail( "when edge: " + ( w.size() < 3 ? std::string( "a condition" ) : exprError ) );
				}
				continue;
			}
			if ( w.size() >= 3 && w[1] == "event" && w[2].empty() == false )
			{
				m.when = Motion::When::Event;
				m.trigger = schema.FindEvent( w[2] );
				if ( m.trigger < 0 || m.trigger > 255 )
				{
					m.trigger = -1;
					warnings += m.name + ": no mod declares the event \"" + w[2] + "\": it never happens; ";
				}
				continue;
			}
			if ( w.size() < 3 || w[1] != "press" || w[2].empty() )
			{
				return fail( "when wants \"press\" and an action, \"while\", or \"event\" and an event" );
			}
			m.when = Motion::When::Press;
			if ( w[2] == "jump" )
			{
				m.action = kMotionActionJump;
			}
			else if ( w[2] == "sprint" )
			{
				m.action = kMotionActionSprint;
			}
			else if ( const ModAction* action = schema.FindAction( w[2] ) )
			{
				m.action = action->bit;
			}
			else
			{
				warnings += m.name + ": no mod declares the action \"" + w[2] + "\": it never happens; ";
			}
		}
		else if ( key == "if" || key == "until" )
		{
			if ( whole() == false )
			{
				return false;
			}
			std::string exprError;
			if ( w.size() < 2 || CompileAnimExpr( w[1], schema, key == "if" ? m.condition : m.until, exprError, warnings ) == false )
			{
				return fail( ( key == "if" ? "condition: " : "until: " ) + exprError );
			}
		}
		else if ( key == "cooldown" )
		{
			if ( whole() == false )
			{
				return false;
			}
			if ( w.size() < 2 || Number( w[1], 0.0f, kMaxMotionSeconds, m.cooldown ) == false )
			{
				return fail( "cooldown wants seconds, 0 to 3600" );
			}
		}
		else if ( key == "duration" )
		{
			if ( whole() == false )
			{
				return false;
			}
			if ( w.size() < 2 || Number( w[1], 0.0f, kMaxMotionSeconds, m.duration ) == false )
			{
				return fail( "duration wants seconds, 0 to 3600" );
			}
		}
		else if ( key == "uses" )
		{
			if ( whole() == false )
			{
				return false;
			}
			float uses = 0.0f;
			if ( w.size() < 3 || Number( w[1], 0.0f, float( kMaxMotionUses ), uses ) == false )
			{
				return fail( "uses wants a count and its refill: \"ground\" or seconds" );
			}
			m.uses = uint32_t( uses );
			m.refillOnGround = w[2] == "ground";
			if ( m.refillOnGround == false && Number( w[2], 0.0f, kMaxMotionSeconds, m.refillSeconds ) == false )
			{
				return fail( "uses wants a count and its refill: \"ground\" or seconds" );
			}
		}
		else if ( key == "probe" )
		{
			if ( whole() == false )
			{
				return false;
			}
			if ( w.size() < 3 || Number( w[1], 1.0f, 500.0f, m.probeRange ) == false || Number( w[2], 0.0f, 1000.0f, m.probeTravel ) == false )
			{
				return fail( "probe wants its range (1 to 500 m) and its travel speed (m/s, 0: at once)" );
			}
			m.probe = true;
		}
		else if ( key == "owner" )
		{
			// owner item: the item runs it, lying in the world (its set is the item's: named by its kind)
			if ( whole() == false )
			{
				return false;
			}
			if ( w.size() < 2 || ( w[1] != "item" && w[1] != "player" ) )
			{
				return fail( "owner wants item (or player: the default)" );
			}
			m.ownerItem = w[1] == "item";
			m.itemKind = m.ownerItem ? schema.FindItemKind( set ) : -1;
			if ( m.ownerItem && m.itemKind < 0 && schema.itemKinds.empty() == false )
			{
				warnings += m.name + ": owner item, but no mod declares the item kind \"" + set + "\" (an item's set is named by its kind): it never runs; ";
			}
		}
		else if ( key == "break" )
		{
			// break <radius> <speed> <seconds>: walls where the owner is
			MotionStep::Break b;
			std::string why;
			if ( w.size() < 4 || valueOf( m, w[1], 0.0f, 20.0f, b.radius, why ) == false || valueOf( m, w[2], 0.0f, 200.0f, b.speed, why ) == false ||
				 valueOf( m, w[3], 0.0f, kMaxMotionSeconds, b.seconds, why ) == false )
			{
				return fail( "break wants a radius (to 20 m), a speed (m/s) and how long its pieces stay (seconds, 0: for good)" +
							 ( why.empty() ? std::string() : ": " + why ) );
			}
			b.guard = guard;
			step.breaks.push_back( b );
		}
		else if ( key == "remove" )
		{
			step.removes.push_back( guard );
		}
		else if ( key == "turn" || key == "from" )
		{
			// turn <yaw> <pitch>: degrees, right and up; from <x y z>: a launch's start. Both are of
			// the effect on the line before.
			std::vector<MotionEffect>& list = inFind ? step.finds.back().effects : step.effects;
			if ( list.empty() )
			{
				return fail( key + " is of the effect on the line before: none" );
			}
			MotionEffect& e = list.back();
			if ( key == "turn" )
			{
				float yaw = 0.0f, pitch = 0.0f;
				if ( w.size() < 3 || Number( w[1], -360.0f, 360.0f, yaw ) == false || Number( w[2], -90.0f, 90.0f, pitch ) == false )
				{
					return fail( "turn wants a heading (-360 to 360 degrees, right) and a pitch (-90 to 90, up)" );
				}
				// (Right is a turn about up the other way round from YawForward's: hence the minus.)
				b3CosSin turnYaw = detmath::CosSin( -yaw * ( detmath::kTwoPi / 360.0f ) );
				b3CosSin turnPitch = detmath::CosSin( pitch * ( detmath::kTwoPi / 360.0f ) );
				e.turned = true;
				e.yawCos = turnYaw.cosine;
				e.yawSin = turnYaw.sine;
				e.pitchCos = turnPitch.cosine;
				e.pitchSin = turnPitch.sine;
			}
			else
			{
				if ( e.kind != MotionEffect::Kind::Launch || w.size() < 4 || Number( w[1], -20.0f, 20.0f, e.from.x ) == false ||
					 Number( w[2], -20.0f, 20.0f, e.from.y ) == false || Number( w[3], -20.0f, 20.0f, e.from.z ) == false )
				{
					return fail( "from is a launch's: where it starts, x y z within 20 m of the player's feet" );
				}
				e.fromMarker = true;
			}
		}
		else if ( key == "path" )
		{
			// path <name> <x y z>...: points at even steps along it, from where it starts
			if ( whole() == false )
			{
				return false;
			}
			Motion::Path path;
			path.name = w.size() >= 2 ? w[1] : std::string();
			size_t count = w.size() >= 2 ? ( w.size() - 2 ) / 3 : 0;
			if ( path.name.empty() || w.size() < 8 || ( w.size() - 2 ) % 3 != 0 || count > 256 )
			{
				return fail( "path wants a name and its points, x y z each (2 to 256)" );
			}
			for ( size_t k = 0; k < count; ++k )
			{
				b3Vec3 point = {};
				if ( Number( w[2 + 3 * k], -100.0f, 100.0f, point.x ) == false || Number( w[3 + 3 * k], -100.0f, 100.0f, point.y ) == false ||
					 Number( w[4 + 3 * k], -100.0f, 100.0f, point.z ) == false )
				{
					return fail( "a path's points are within 100 m of where it starts" );
				}
				path.points.push_back( point );
			}
			for ( const Motion::Path& other : m.paths )
			{
				if ( other.name == path.name )
				{
					return fail( "the motion has a path named " + path.name + " already" );
				}
			}
			m.paths.push_back( path );
		}
		else if ( key == "follow" )
		{
			// follow <path> <seconds>: the player carried along it
			MotionEffect e;
			e.kind = MotionEffect::Kind::Follow;
			e.guard = guard;
			for ( size_t k = 0; w.size() >= 2 && k < m.paths.size(); ++k )
			{
				e.path = m.paths[k].name == w[1] ? int16_t( k ) : e.path;
			}
			if ( w.size() < 3 || e.path < 0 || Number( w[2], 0.05f, kMaxMotionSeconds, e.seconds ) == false )
			{
				return fail( "follow wants a path the motion has (a path line before it) and its seconds" );
			}
			step.effects.push_back( e );
		}
		else if ( key == "curve" )
		{
			// curve <name> <min> <max> <values>...: what "curve(name, x)" reads, at even steps
			if ( whole() == false )
			{
				return false;
			}
			AnimCurve curve;
			curve.name = w.size() >= 2 ? w[1] : std::string();
			if ( w.size() < 6 || curve.name.empty() || w.size() - 4 > 256 || Number( w[2], -1.0e6f, 1.0e6f, curve.min ) == false ||
				 Number( w[3], -1.0e6f, 1.0e6f, curve.max ) == false || curve.max <= curve.min )
			{
				return fail( "curve wants a name, where it starts and ends (the end past the start) and its values (2 to 252)" );
			}
			for ( size_t k = 4; k < w.size(); ++k )
			{
				float value = 0.0f;
				if ( Number( w[k], -1.0e6f, 1.0e6f, value ) == false )
				{
					return fail( "a curve's values are numbers" );
				}
				curve.samples.push_back( value );
			}
			for ( const AnimCurve& other : m.curves )
			{
				if ( other.name == curve.name )
				{
					return fail( "the motion has a curve named " + curve.name + " already" );
				}
			}
			m.curves.push_back( curve );
		}
		else if ( key == "probe_shape" )
		{
			// probe_shape <radius> <x y z>...: what the line sweeps, around its tip
			if ( whole() == false )
			{
				return false;
			}
			size_t count = w.size() >= 2 ? ( w.size() - 2 ) / 3 : 0;
			if ( w.size() < 5 || ( w.size() - 2 ) % 3 != 0 || count > 128 || Number( w[1], 0.0f, 20.0f, m.probeRadius ) == false )
			{
				return fail( "probe_shape wants a radius (0 to 20 m) and its points, x y z each (1 to 128)" );
			}
			m.probePoints.clear();
			for ( size_t k = 0; k < count; ++k )
			{
				b3Vec3 point = {};
				if ( Number( w[2 + 3 * k], -20.0f, 20.0f, point.x ) == false || Number( w[3 + 3 * k], -20.0f, 20.0f, point.y ) == false ||
					 Number( w[4 + 3 * k], -20.0f, 20.0f, point.z ) == false )
				{
					return fail( "a probe's shape is within 20 m of its tip" );
				}
				m.probePoints.push_back( point );
			}
		}
		else if ( key == "holds" )
		{
			if ( whole() == false )
			{
				return false;
			}
			// holds <level|bodies|players>...
			m.probeHolds = 0;
			for ( size_t k = 1; k < w.size(); ++k )
			{
				uint8_t bit = w[k] == "level" ? kProbeHoldsLevel : w[k] == "bodies" ? kProbeHoldsBodies : w[k] == "players" ? kProbeHoldsPlayers : 0;
				if ( bit == 0 )
				{
					return fail( "holds wants what the probe may hold on to: level, bodies, players" );
				}
				m.probeHolds |= bit;
			}
			if ( m.probeHolds == 0 )
			{
				return fail( "holds wants what the probe may hold on to: level, bodies, players" );
			}
		}
		else if ( key == "impulse" )
		{
			// impulse <target> <speed> <frame> <replace> [x y z]
			static const char* const kReplaces[] = { "none", "vertical", "horizontal", "all" };
			MotionEffect e;
			e.kind = MotionEffect::Kind::Impulse;
			int replace = -1;
			for ( int i = 0; w.size() >= 5 && i < 4; ++i )
			{
				replace = w[4] == kReplaces[i] ? i : replace;
			}
			std::string why;
			if ( w.size() >= 3 && strength( m, w[2], kMaxMotionImpulse, e, why ) == false )
			{
				return fail( "impulse: its speed: " + why );
			}
			if ( w.size() < 5 || ParseTarget( w[1], schema, m.name, e.target, warnings ) == false || ParseFrame( w[3], e.frame ) == false || replace < 0 )
			{
				return fail( "impulse wants a target (self, hit, @field), a speed (m/s), a frame (look, move, facing, up, world, to) and what "
							 "it replaces (none, vertical, horizontal, all)" );
			}
			e.replace = MotionEffect::Replace( replace );
			e.guard = guard;
			if ( ( e.frame == MotionFrame::World || e.frame == MotionFrame::Marker ) && ParseWorld( w, 5, e.direction ) == false )
			{
				return fail( "a world frame wants its direction: x y z, each -1 to 1, not all zero" );
			}
			if ( inFind != ( e.target.kind == MotionTarget::Kind::Found || e.frame == MotionFrame::Away ) && inFind == false )
			{
				return fail( "\"found\" and \"away\" are what a find found: inside a find" );
			}
			if ( inFind )
			{
				e.guard = -1; // (the find's guard holds for all of it)
				step.finds.back().effects.push_back( e );
				continue;
			}
			step.effects.push_back( e );
		}
		else if ( key == "force" )
		{
			// force <target> <frame> <accel|force|velocity> <strength> <speed> <ramp> <react 0|1> [x y z]
			MotionEffect e;
			e.kind = MotionEffect::Kind::Force;
			int push = w.size() >= 8 ? ( w[3] == "accel" ? 0 : ( w[3] == "force" ? 1 : ( w[3] == "velocity" ? 2 : -1 ) ) ) : -1;
			float limit = push == 1 ? kMaxMotionForce : kMaxMotionAccel;
			std::string why;
			if ( push >= 0 && strength( m, w[4], limit, e, why ) == false )
			{
				return fail( "force: its strength: " + why );
			}
			if ( push < 0 || ParseTarget( w[1], schema, m.name, e.target, warnings ) == false || ParseFrame( w[2], e.frame ) == false ||
				 Number( w[5], -kMaxMotionImpulse, kMaxMotionImpulse, e.speed ) == false ||
				 Number( w[6], 0.0f, 60.0f, e.ramp ) == false || ( w[7] != "0" && w[7] != "1" ) )
			{
				return fail( "force wants a target (self, hit, @field), a frame, a kind (accel, force, velocity), a strength, a speed, a "
							 "ramp (seconds) and whether it reacts (0 or 1)" );
			}
			e.push = MotionEffect::Push( push );
			e.react = w[7] == "1";
			e.guard = guard;
			if ( ( e.frame == MotionFrame::World || e.frame == MotionFrame::Marker ) && ParseWorld( w, 8, e.direction ) == false )
			{
				return fail( "a world frame wants its direction: x y z, each -1 to 1, not all zero" );
			}
			step.effects.push_back( e );
		}
		else if ( key == "link" )
		{
			// link <target> <length> <reel> [react: 1, or 0 for the player alone]
			MotionEffect e;
			e.kind = MotionEffect::Kind::Link;
			e.react = true;
			if ( w.size() < 4 || ParseTarget( w[1], schema, m.name, e.target, warnings ) == false || e.target.kind == MotionTarget::Kind::Self ||
				 Number( w[2], 0.0f, 500.0f, e.length ) == false || Number( w[3], 0.0f, 100.0f, e.reel ) == false ||
				 ( w.size() > 4 && w[4] != "0" && w[4] != "1" ) )
			{
				return fail( "link wants a target that is not the player itself (hit, @field), a length (m; 0: the distance when the probe "
							 "takes hold), a reel (m/s) and, if it acts on the player alone, 0" );
			}
			e.react = w.size() <= 4 || w[4] == "1";
			e.guard = guard;
			step.effects.push_back( e );
		}
		else if ( key == "launch" || key == "launch_carried" )
		{
			// launch <item kind> <speed> <frame> <lift> <ahead> <seconds> [x y z]: a new one;
			// launch_carried, the same: the one of that kind the player carries
			MotionEffect e;
			e.kind = MotionEffect::Kind::Launch;
			e.carried = key == "launch_carried";
			std::string why;
			if ( w.size() >= 3 && strength( m, w[2], kMaxMotionImpulse, e, why ) == false )
			{
				return fail( "launch: its speed: " + why );
			}
			if ( w.size() < 7 || w[1].empty() || ParseFrame( w[3], e.frame ) == false ||
				 e.frame == MotionFrame::To || Number( w[4], -kMaxMotionImpulse, kMaxMotionImpulse, e.lift ) == false ||
				 Number( w[5], 0.0f, 5.0f, e.ahead ) == false || Number( w[6], 0.0f, 600.0f, e.seconds ) == false )
			{
				return fail( "launch wants an item kind, a speed (m/s), a frame (look, move, facing, up, world), a lift (m/s upward), how far "
							 "ahead it starts (0 to 5 m) and its seconds (0: it stays)" );
			}
			if ( ( e.frame == MotionFrame::World || e.frame == MotionFrame::Marker ) && ParseWorld( w, 7, e.direction ) == false )
			{
				return fail( "a world frame wants its direction: x y z, each -1 to 1, not all zero" );
			}
			int kind = schema.FindItemKind( w[1] );
			e.known = kind >= 0;
			e.itemKind = uint16_t( kind >= 0 ? kind : 0 );
			if ( e.known == false )
			{
				warnings += m.name + ": no mod declares the item kind \"" + w[1] + "\": nothing is launched; ";
			}
			e.guard = guard;
			step.effects.push_back( e );
		}
		else if ( key == "param" )
		{
			// param <name> <value> [seconds from when the step starts]
			int param = w.size() >= 3 ? MoveParamByName( w[1].c_str() ) : -1;
			Motion::Param p;
			if ( param < 0 || Number( w[2], MoveParamInfoOf( param ).min, MoveParamInfoOf( param ).max, p.value ) == false ||
				 ( w.size() > 3 && Number( w[3], 0.0f, kMaxMotionSeconds, p.seconds ) == false ) )
			{
				return fail( "param wants a movement parameter, a value in its range and, if it holds for less than its step, seconds" );
			}
			p.param = uint8_t( param );
			p.guard = guard;
			if ( inFind )
			{
				if ( p.seconds <= 0.0f )
				{
					return fail( "a param inside a find is given to what it found for some seconds: give them" );
				}
				step.finds.back().moves.push_back( { p.param, p.value, p.seconds } );
				continue;
			}
			step.params.push_back( p );
			step.length = std::max( step.length, p.seconds );
		}
		else if ( key == "change" )
		{
			Motion::Change change;
			if ( w.size() < 4 || ( w[2] != "=" && w[2] != "+=" && w[2] != "-=" ) || Number( w[3], -1.0e9f, 1.0e9f, change.value ) == false )
			{
				return fail( "change wants a field, an operator (=, +=, -=) and a number" );
			}
			change.op = w[2] == "=" ? Motion::ChangeOp::Set : ( w[2] == "+=" ? Motion::ChangeOp::Add : Motion::ChangeOp::Sub );
			const BoardField* field = schema.FindField( w[1] );
			if ( field == nullptr || field->scope != BoardScope::Entity )
			{
				warnings += m.name + ": \"" + w[1] + "\" is not a field of the player a mod declares: the change is skipped; ";
				continue;
			}
			change.slot = field->slot;
			change.type = field->type;
			change.guard = guard;
			step.changes.push_back( change );
		}
		else if ( key == "hold_emit" )
		{
			if ( whole() == false )
			{
				return false;
			}
			if ( w.size() < 2 || w[1].empty() )
			{
				return fail( "hold_emit wants an event" );
			}
			m.holdEvent = schema.FindEvent( w[1] );
			if ( m.holdEvent < 0 )
			{
				warnings += m.name + ": no mod declares the event \"" + w[1] + "\": nothing is emitted when its line takes hold; ";
			}
		}
		else if ( key == "emit" )
		{
			if ( w.size() < 2 || w[1].empty() )
			{
				return fail( "emit wants an event" );
			}
			MotionStep::Emit emit;
			emit.event = schema.FindEvent( w[1] );
			emit.guard = guard;
			if ( emit.event < 0 )
			{
				warnings += m.name + ": no mod declares the event \"" + w[1] + "\": nothing is emitted; ";
			}
			if ( inFind )
			{
				step.finds.back().emits.push_back( emit.event );
				continue;
			}
			step.emits.push_back( emit );
		}
		else if ( key == "end" )
		{
			step.ends.push_back( guard );
		}
		else if ( key == "guard" )
		{
			// guard [condition]: what follows counts only if it held when the step started; none: all
			// of it does. One condition is one bit, however often it is named.
			if ( w.size() < 2 || w[1].empty() )
			{
				guard = -1;
				continue;
			}
			std::vector<std::string>& texts = guardTexts.back();
			auto known = std::find( texts.begin(), texts.end(), w[1] );
			if ( known != texts.end() )
			{
				guard = int8_t( known - texts.begin() );
				continue;
			}
			if ( int( texts.size() ) >= kMotionGuards )
			{
				return fail( "a step has at most " + std::to_string( kMotionGuards ) + " conditions (if, else)" );
			}
			AnimExpr expr;
			std::string exprError;
			if ( CompileAnimExpr( w[1], schema, expr, exprError, warnings ) == false )
			{
				return fail( "guard: " + exprError );
			}
			guard = int8_t( texts.size() );
			texts.push_back( w[1] );
			step.guards.push_back( expr );
		}
		else if ( key == "step" || key == "timeout" )
		{
			// step <after seconds> <within seconds> [condition]: what comes once its wait is over.
			// timeout: what the step before runs when its wait gives up.
			if ( int( m.steps.size() ) >= kMotionSteps )
			{
				return fail( "a motion has at most " + std::to_string( kMotionSteps ) + " steps" );
			}
			MotionStep next;
			if ( key == "timeout" )
			{
				if ( step.within <= 0.0f || step.timeout || m.steps.size() < 2 )
				{
					return fail( "timeout comes right after a step that waits within some seconds" );
				}
				next.timeout = true;
			}
			else
			{
				if ( w.size() < 3 || Number( w[1], 0.0f, kMaxMotionSeconds, next.after ) == false ||
					 Number( w[2], 0.0f, kMaxMotionSeconds, next.within ) == false )
				{
					return fail( "step wants its seconds after the step before (0: none), the seconds its wait may take (0: any) and, if it "
								 "waits for one, a condition" );
				}
				std::string exprError;
				if ( w.size() > 3 && w[3].empty() == false && CompileAnimExpr( w[3], schema, next.wait, exprError, warnings ) == false )
				{
					return fail( "step: " + exprError );
				}
				if ( next.within > 0.0f && next.wait.Empty() )
				{
					return fail( "a step that gives up within some seconds waits for a condition" );
				}
			}
			m.steps.push_back( next );
			guards.push_back( -1 );
			guardTexts.emplace_back();
		}
		else
		{
			return fail( "unknown line \"" + key + "\"" );
		}
	}
	SetAnimCurveNames( nullptr );
	if ( header == false )
	{
		number = 1;
		return fail( "not a motions file (it starts with \"cinderbox_motions\t2\" or 3)" );
	}
	if ( inFind )
	{
		return fail( "a find ends (endfind)" );
	}
	for ( Motion& m : motions )
	{
		auto wrong = [&]( const std::string& what ) {
			error = "motions " + set + ": " + m.name + ": " + what;
			return false;
		};
		if ( m.when == Motion::When::While && m.probe )
		{
			return wrong( "a probe is thrown by a press or an event, not by a while motion" );
		}
		if ( m.probePoints.empty() == false && m.probe == false )
		{
			return wrong( "a probe's shape without a probe" );
		}
		if ( m.ownerItem )
		{
			// Once, when its event is said at the item: nothing that lasts, waits or counts.
			const MotionStep& only = m.steps.front();
			if ( m.when != Motion::When::Event || m.steps.size() > 1 || m.probe || m.cooldown > 0.0f || m.uses > 0 || m.duration > 0.0f ||
				 m.until.Empty() == false || m.condition.Empty() == false || only.params.empty() == false || only.ends.empty() == false )
			{
				return wrong( "an item's motion happens once, on an event (on expired): no steps, cast, cooldown, uses, lasts, until, if, move or end" );
			}
			for ( const MotionEffect& e : only.effects )
			{
				if ( e.kind != MotionEffect::Kind::Impulse || e.frame == MotionFrame::Look || e.frame == MotionFrame::Move ||
					 e.frame == MotionFrame::Facing || e.frame == MotionFrame::Marker || e.target.kind != MotionTarget::Kind::Self )
				{
					return wrong( "an item's motion pushes itself (along up or world) or what it finds; it has no look" );
				}
			}
			for ( const MotionFind& find : only.finds )
			{
				for ( const MotionEffect& e : find.effects )
				{
					if ( e.frame == MotionFrame::Look || e.frame == MotionFrame::Move || e.frame == MotionFrame::Facing || e.frame == MotionFrame::Marker )
					{
						return wrong( "an item has no look: what it finds goes along away, to, up or world" );
					}
				}
			}
		}
		else if ( m.steps.front().removes.empty() == false || std::any_of( m.steps.begin(), m.steps.end(), []( const MotionStep& s ) { return s.removes.empty() == false; } ) )
		{
			return wrong( "remove self is an item's (owner item)" );
		}
		if ( m.when == Motion::When::While && m.steps.size() > 1 )
		{
			return wrong( "a while motion has no steps: it starts again on every tick its condition holds" );
		}
		for ( size_t s = 0; s < m.steps.size(); ++s )
		{
			MotionStep& step = m.steps[s];
			for ( const MotionEffect& e : step.effects )
			{
				if ( e.target.kind == MotionTarget::Kind::Found || e.frame == MotionFrame::Away )
				{
					return wrong( "\"found\" and \"away\" are what a find found: inside a find" );
				}
				bool needsHit = e.target.kind == MotionTarget::Kind::Hit || ( e.frame == MotionFrame::To && e.target.kind != MotionTarget::Kind::Field );
				if ( needsHit && m.probe == false )
				{
					return wrong( "an effect on \"hit\", or along \"to\", needs the motion to have a probe (or a target that is a field)" );
				}
				if ( e.kind == MotionEffect::Kind::Link && e.length <= 0.0f && m.probe == false )
				{
					return wrong( "a link without a probe needs a length" );
				}
				if ( step.timeout && ( e.kind == MotionEffect::Kind::Force || e.kind == MotionEffect::Kind::Link || e.kind == MotionEffect::Kind::Follow ) )
				{
					return wrong( "a timeout's statements happen once (no force, link or parameter): the motion ends after it" );
				}
			}
			if ( step.timeout && step.params.empty() == false )
			{
				return wrong( "a timeout's statements happen once (no force, link or parameter): the motion ends after it" );
			}
			// The step after this one: the next that is not a timeout; its timeout, the one right after.
			if ( step.timeout == false )
			{
				for ( size_t n = s + 1; n < m.steps.size() && step.next < 0; ++n )
				{
					step.next = m.steps[n].timeout ? -1 : int( n );
					if ( m.steps[n].timeout == false )
					{
						break;
					}
				}
			}
			if ( s + 1 < m.steps.size() && m.steps[s + 1].timeout )
			{
				step.onTimeout = int( s + 1 );
			}
		}
		if ( m.when != Motion::When::While )
		{
			continue;
		}
		// Per second, a whole number never moves: a tick's share of it rounds to nothing.
		std::vector<Motion::Change>& changes = m.steps.front().changes;
		for ( size_t i = changes.size(); i-- > 0; )
		{
			if ( changes[i].op != Motion::ChangeOp::Set && changes[i].type != BoardType::Float )
			{
				warnings += m.name + ": a while motion's += and -= are per second and need a Float field: the change is skipped; ";
				changes.erase( changes.begin() + std::ptrdiff_t( i ) );
			}
		}
	}
	out.insert( out.end(), motions.begin(), motions.end() );
	return true;
}

namespace
{
// Set while the motions are compiled only to list their names: a condition that asks for a motion
// then reads nothing (the list is what it would be looked up in).
thread_local bool t_listingMotions = false;
} // namespace

std::vector<std::string> MotionNames( const ModSchema& schema )
{
	std::vector<std::string> names;
	if ( t_listingMotions )
	{
		return names;
	}
	t_listingMotions = true;
	std::string ignored;
	if ( std::shared_ptr<const Motions> motions = CompileMotions( schema, ignored ) )
	{
		for ( const Motion& m : motions->list )
		{
			names.push_back( m.name );
		}
	}
	t_listingMotions = false;
	return names;
}

int FindMotionByName( const ModSchema& schema, const std::string& name )
{
	if ( t_listingMotions )
	{
		return -1;
	}
	std::vector<std::string> names = MotionNames( schema );
	int byNode = -1;
	for ( size_t i = 0; i < names.size(); ++i )
	{
		size_t slash = names[i].find( '/' );
		std::string set = names[i].substr( 0, slash );
		std::string node = slash == std::string::npos ? std::string() : names[i].substr( slash + 1 );
		if ( name == set + "." + node )
		{
			return int( i );
		}
		if ( name == node )
		{
			byNode = byNode == -1 ? int( i ) : -2;
		}
	}
	return byNode;
}

bool ListingMotionNames()
{
	return t_listingMotions;
}

uint64_t MotionsOnAt( const MotionState& state, uint32_t tick )
{
	uint64_t on = 0;
	for ( size_t i = 0; i < state.slots.size() && i < 64; ++i )
	{
		const MotionSlot& slot = state.slots[i];
		if ( slot.lastTick != 0 && slot.sinceTick <= tick && tick < slot.untilTick )
		{
			on |= uint64_t( 1 ) << i;
		}
	}
	return on;
}

std::shared_ptr<const Motions> CompileMotions( const ModSchema& schema, std::string& warnings )
{
	auto motions = std::make_shared<Motions>();
	for ( const MotionSetInfo& set : schema.motionSets )
	{
		if ( set.text.empty() )
		{
			continue;
		}
		std::string error;
		if ( CompileMotionSet( set.name, set.text, schema, motions->list, error, warnings ) == false )
		{
			warnings += error + "; ";
		}
	}
	if ( motions->list.size() > size_t( kMotionLimit ) )
	{
		warnings += "more than " + std::to_string( kMotionLimit ) + " motions: " + motions->list[size_t( kMotionLimit )].name +
					" and the ones after it are left out; ";
		motions->list.resize( size_t( kMotionLimit ) );
	}
	if ( motions->list.empty() )
	{
		return nullptr;
	}
	return motions;
}

namespace
{

// A step's guards as they are now: a bit each.
uint16_t GuardBits( const MotionStep& step, const AnimGraphInputs& values )
{
	uint16_t bits = 0;
	for ( size_t g = 0; g < step.guards.size(); ++g )
	{
		if ( EvaluateAnimExpr( step.guards[g], values, 0.0f ) != 0.0f )
		{
			bits = uint16_t( bits | ( 1u << g ) );
		}
	}
	return bits;
}

} // namespace

uint16_t MotionGuardBits( const MotionStep& step, const AnimGraphInputs& values )
{
	return GuardBits( step, values );
}

float MotionValueOf( const Motion& m, const MotionValue& value, const AnimGraphInputs& values, float lo, float hi )
{
	float v = value.expr >= 0 && size_t( value.expr ) < m.exprs.size() ? EvaluateAnimExpr( m.exprs[size_t( value.expr )], values, 0.0f ) : value.number;
	// (Not a NaN's way: a comparison that fails gives lo.)
	return v > lo ? ( v < hi ? v : hi ) : lo;
}

uint32_t MotionStepStart( const MotionSlot& slot )
{
	return slot.step == 0 ? slot.sinceTick : slot.stepTick;
}

float MotionForceLimit( const MotionEffect& effect )
{
	return effect.push == MotionEffect::Push::Force ? kMaxMotionForce : kMaxMotionAccel;
}

float MotionStrength( const Motion& m, const MotionEffect& effect, const AnimGraphInputs& values, float limit )
{
	if ( effect.strengthExpr < 0 || size_t( effect.strengthExpr ) >= m.exprs.size() )
	{
		return effect.strength;
	}
	float value = EvaluateAnimExpr( m.exprs[size_t( effect.strengthExpr )], values, 0.0f );
	// (Not a NaN's way: a comparison that fails gives 0.)
	return value > -limit ? ( value < limit ? value : limit ) : ( value <= -limit ? -limit : 0.0f );
}

bool MotionGuardHolds( const MotionSlot& slot, int8_t guard )
{
	return guard < 0 || ( ( slot.guards >> guard ) & 1u ) != 0;
}

void RunMotions( const Motions& motions, const MotionInputs& in, MotionState& state, const Character& c, Blackboard& board,
				 bool& boardChanged, std::vector<ModEventRecord>& events, std::vector<MotionActive>& active )
{
	const uint32_t now = in.tick + 1; // 0 in a slot means "never"
	const float dt = 1.0f / float( in.tickRate );
	for ( size_t i = 0; i < motions.list.size(); ++i )
	{
		const Motion& m = motions.list[i];
		MotionSlot& slot = state.slots[i];
		if ( m.ownerItem )
		{
			continue; // (an item's: Simulation::RunItemMotions)
		}
		const bool isWhile = m.when == Motion::When::While;
		// What its expressions read: the player's values, its curves, and its step's time (0 when off).
		AnimGraphInputs values = *in.values;
		values.curves = &m.curves;
		values.builtins[AnimExpr::Time] =
			slot.lastTick != 0 && in.tick < slot.untilTick && in.tick >= MotionStepStart( slot ) ? float( in.tick - MotionStepStart( slot ) ) * dt : 0.0f;
		// A while motion that was on last tick goes on without asking its cooldown again.
		const bool wasOn = isWhile && slot.lastTick != 0 && slot.untilTick == in.tick;
		bool on = slot.lastTick != 0 && in.tick < slot.untilTick && isWhile == false;

		// One that is held ends by its condition (not on the tick it started), or when what its
		// probe holds on to is gone.
		if ( on && m.Held() )
		{
			bool lost = m.probe && ( in.held != int( i ) || in.holdAlive == false );
			bool done = m.until.Empty() == false && slot.lastTick != now && EvaluateAnimExpr( m.until, values, 0.0f ) != 0.0f;
			if ( lost || done )
			{
				slot.untilTick = in.tick;
				on = false;
			}
		}

		if ( m.uses > 0 && slot.used > 0 )
		{
			bool byTime = m.refillSeconds > 0.0f && slot.lastTick != 0 && now - slot.lastTick >= Ticks( m.refillSeconds, in.tickRate );
			if ( ( m.refillOnGround && c.grounded != 0 ) || byTime )
			{
				slot.used = 0;
			}
		}

		// An edge's condition is asked every tick, so a turn to true is seen whenever it comes (the
		// first answer is only remembered: a player who spawns standing has not landed).
		bool turned = false;
		if ( m.when == Motion::When::Edge )
		{
			bool holds = EvaluateAnimExpr( m.edge, values, 0.0f ) != 0.0f;
			turned = ( slot.edge & 1u ) != 0 && ( slot.edge & 2u ) == 0 && holds;
			slot.edge = uint8_t( 1u | ( holds ? 2u : 0u ) );
		}

		bool starts = in.canAct;
		if ( starts && m.when == Motion::When::Press )
		{
			starts = Pressed( m, in, state.prevActions );
		}
		else if ( starts && m.when == Motion::When::Event )
		{
			starts = m.trigger >= 0 && HeardEvent( m.trigger, in );
		}
		else if ( starts && m.when == Motion::When::Edge )
		{
			starts = turned;
		}
		// One that is held does not start again while it is on.
		starts = starts && ( on == false || m.Held() == false );
		starts = starts && ( wasOn || slot.lastTick == 0 || now - slot.lastTick >= Ticks( m.cooldown, in.tickRate ) );
		starts = starts && ( isWhile || m.uses == 0 || slot.used < m.uses );
		starts = starts && ( m.condition.Empty() || EvaluateAnimExpr( m.condition, values, 0.0f ) != 0.0f );
		// A probe has to find something to hold on to, or the motion does not happen.
		uint32_t effectTick = in.tick;
		if ( starts && m.probe && ( in.attach == nullptr || in.attach( in.user, m, i, effectTick ) == false ) )
		{
			starts = false;
		}

		// A step's once statements: its changes, its emits, its ends (in that order).
		auto onceStatements = [&]( const MotionStep& step, bool perSecond, bool firstOfWhile ) {
			for ( const MotionStep::Change& change : step.changes )
			{
				if ( MotionGuardHolds( slot, change.guard ) == false )
				{
					continue;
				}
				if ( perSecond == false )
				{
					ApplyChange( change, board );
				}
				else if ( change.op != Motion::ChangeOp::Set )
				{
					// Per second.
					MotionStep::Change part = change;
					part.value = change.value * dt;
					ApplyChange( part, board );
				}
				else if ( firstOfWhile )
				{
					ApplyChange( change, board );
				}
				else
				{
					continue;
				}
				boardChanged = true;
			}
			// When it happens; for a while motion, when it starts.
			for ( const MotionStep::Emit& emit : step.emits )
			{
				if ( emit.event >= 0 && ( perSecond == false || firstOfWhile ) && MotionGuardHolds( slot, emit.guard ) )
				{
					ModEventRecord record;
					record.type = uint16_t( emit.event );
					events.push_back( record );
				}
			}
			bool ends = false;
			for ( int8_t guard : step.ends )
			{
				ends = ends || MotionGuardHolds( slot, guard );
			}
			return ends;
		};

		bool entered = false;
		if ( starts )
		{
			const MotionStep& first = m.steps.front();
			if ( isWhile == false || wasOn == false )
			{
				slot.step = 0;
				slot.guards = GuardBits( first, values );
			}
			bool ends = onceStatements( first, isWhile, wasOn == false );

			slot.lastTick = now;
			if ( isWhile )
			{
				if ( wasOn == false )
				{
					slot.sinceTick = in.tick;
				}
				slot.untilTick = in.tick + 1; // on this tick; next tick says for itself
			}
			else
			{
				slot.used += 1;
				// In effect from now, or from when its probe takes hold, for its duration; one that
				// is held, until something ends it.
				slot.sinceTick = effectTick;
				uint32_t length = Ticks( m.duration, in.tickRate );
				slot.untilTick = m.Held() && length == 0 ? kForever : effectTick + length;
				if ( ends )
				{
					slot.untilTick = std::min( slot.untilTick, effectTick + 1 );
				}
			}
		}
		else if ( on && m.steps.size() > 1 && in.tick >= slot.sinceTick && size_t( slot.step ) < m.steps.size() )
		{
			// Waiting for its next step (once its first took effect: a probe has to hold first).
			const int nextIndex = m.steps[slot.step].next;
			if ( nextIndex >= 0 )
			{
				const MotionStep& next = m.steps[size_t( nextIndex )];
				const uint32_t waited = in.tick - MotionStepStart( slot );
				bool ready = waited >= std::max<uint32_t>( 1, Ticks( next.after, in.tickRate ) ) &&
							 ( next.wait.Empty() || EvaluateAnimExpr( next.wait, values, 0.0f ) != 0.0f );
				int enter = ready ? nextIndex : -1;
				if ( ready == false && next.within > 0.0f && waited >= Ticks( next.within, in.tickRate ) )
				{
					// It gave up: its timeout, if it has one, then the end.
					enter = next.onTimeout;
					if ( enter < 0 )
					{
						slot.untilTick = in.tick;
						on = false;
					}
				}
				if ( enter >= 0 )
				{
					const MotionStep& step = m.steps[size_t( enter )];
					slot.step = uint16_t( enter );
					slot.stepTick = in.tick;
					slot.guards = GuardBits( step, values );
					bool ends = onceStatements( step, false, false );
					entered = true;
					if ( step.timeout || ends )
					{
						slot.untilTick = std::min( slot.untilTick, in.tick + 1 );
					}
					else if ( step.next < 0 && m.duration <= 0.0f && m.until.Empty() && m.probe == false )
					{
						// The last step: on for its longest parameter's seconds (one tick at least).
						slot.untilTick = in.tick + std::max<uint32_t>( 1, Ticks( step.length, in.tickRate ) );
					}
				}
			}
		}

		// Its effects apply on every tick it is in effect, and on the tick it starts.
		bool inEffect = slot.lastTick != 0 && slot.sinceTick <= in.tick && in.tick < slot.untilTick;
		bool startsNow = slot.lastTick != 0 && slot.sinceTick == in.tick && ( starts || isWhile == false );
		if ( inEffect || ( starts && slot.sinceTick == in.tick ) )
		{
			active.push_back( { uint16_t( i ), startsNow || entered } );
		}
	}
	state.prevActions = in.input->actions;
}

void ApplyMotionParams( const Motions& motions, const MotionState& state, uint32_t tick, uint32_t tickRate, MoveParams& params )
{
	for ( size_t i = 0; i < motions.list.size(); ++i )
	{
		const MotionSlot slot = state.slots[i];
		const Motion& m = motions.list[i];
		if ( slot.lastTick != 0 && slot.sinceTick <= tick && tick < slot.untilTick && size_t( slot.step ) < m.steps.size() )
		{
			const MotionStep& step = m.steps[slot.step];
			for ( const Motion::Param& p : step.params )
			{
				// Under a guard that did not hold; past its seconds.
				if ( MotionGuardHolds( slot, p.guard ) == false ||
					 ( p.seconds > 0.0f && tick - MotionStepStart( slot ) >= std::max<uint32_t>( 1, Ticks( p.seconds, tickRate ) ) ) )
				{
					continue;
				}
				params.values[p.param] = p.value;
			}
		}
	}
}

} // namespace cb
