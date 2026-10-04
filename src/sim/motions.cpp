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

b3Vec3 Direction( const Motion& m, const Character& c, const PlayerInput& in )
{
	float yaw = detmath::YawToRadians( in.cameraYaw );
	switch ( m.frame )
	{
		case Motion::Frame::Look:
		{
			b3CosSin pitch = detmath::CosSin( float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f ) );
			b3Vec3 forward = detmath::YawForward( yaw );
			return { forward.x * pitch.cosine, pitch.sine, forward.z * pitch.cosine };
		}
		case Motion::Frame::Move:
		{
			// The mover's own wish direction; standing still, a dash goes where the body faces.
			float forward = float( std::clamp<int>( in.moveForward, -127, 127 ) );
			float right = float( std::clamp<int>( in.moveRight, -127, 127 ) );
			b3Vec3 wish = b3Add( b3MulSV( forward, detmath::YawForward( yaw ) ), b3MulSV( right, detmath::YawRight( yaw ) ) );
			float length = 0.0f;
			b3Vec3 direction = b3GetLengthAndNormalize( &length, wish );
			return length > 0.0f ? direction : detmath::YawForward( c.facingYaw );
		}
		case Motion::Frame::Facing:
			return detmath::YawForward( c.facingYaw );
		case Motion::Frame::Up:
			return { 0.0f, 1.0f, 0.0f };
		case Motion::Frame::World:
			return m.direction;
	}
	return { 0.0f, 1.0f, 0.0f };
}

bool Pressed( const Motion& m, const MotionInputs& in, uint16_t previousActions )
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
	uint16_t bit = uint16_t( 1u << m.action );
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
		if ( e.tick != in.tick )
		{
			break; // newest first: the rest are older
		}
		if ( int( e.type ) == event && e.netIdA == in.netId )
		{
			return true;
		}
	}
	return false;
}

void ApplyChange( const Motion::Change& change, Blackboard& board )
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

} // namespace

bool CompileMotionSet( const std::string& set, const std::string& text, const ModSchema& schema, std::vector<Motion>& out,
					   std::string& error, std::string& warnings )
{
	std::istringstream lines( text );
	std::string line;
	bool header = false;
	std::vector<Motion> motions;
	int number = 0;
	auto fail = [&]( const std::string& what ) {
		error = "motions " + set + ", line " + std::to_string( number ) + ": " + what;
		return false;
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
			if ( key != "cinderbox_motions" || w.size() < 2 || w[1] != "1" )
			{
				return fail( "not a motions file (it starts with \"cinderbox_motions\t1\")" );
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
			Motion m;
			m.name = set + "/" + w[1];
			motions.push_back( m );
			continue;
		}
		if ( motions.empty() )
		{
			return fail( "\"" + key + "\" before the first motion" );
		}
		Motion& m = motions.back();
		if ( key == "when" )
		{
			if ( w.size() >= 2 && w[1] == "while" )
			{
				m.when = Motion::When::While;
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
		else if ( key == "if" )
		{
			std::string exprError;
			if ( w.size() < 2 || CompileAnimExpr( w[1], schema, m.condition, exprError, warnings ) == false )
			{
				return fail( "condition: " + exprError );
			}
		}
		else if ( key == "cooldown" )
		{
			if ( w.size() < 2 || Number( w[1], 0.0f, kMaxMotionSeconds, m.cooldown ) == false )
			{
				return fail( "cooldown wants seconds, 0 to 3600" );
			}
		}
		else if ( key == "duration" )
		{
			if ( w.size() < 2 || Number( w[1], 0.0f, kMaxMotionSeconds, m.duration ) == false )
			{
				return fail( "duration wants seconds, 0 to 3600" );
			}
		}
		else if ( key == "uses" )
		{
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
		else if ( key == "impulse" )
		{
			static const char* const kFrames[] = { "look", "move", "facing", "up", "world" };
			static const char* const kReplaces[] = { "none", "vertical", "horizontal", "all" };
			int frame = -1, replace = -1;
			for ( int i = 0; w.size() >= 4 && i < 5; ++i )
			{
				frame = w[2] == kFrames[i] ? i : frame;
			}
			for ( int i = 0; w.size() >= 4 && i < 4; ++i )
			{
				replace = w[3] == kReplaces[i] ? i : replace;
			}
			if ( frame < 0 || replace < 0 || Number( w[1], -kMaxMotionImpulse, kMaxMotionImpulse, m.impulse ) == false )
			{
				return fail( "impulse wants a speed (m/s), a frame (look, move, facing, up, world) and what it replaces (none, vertical, "
							 "horizontal, all)" );
			}
			m.frame = Motion::Frame( frame );
			m.replace = Motion::Replace( replace );
			if ( m.frame == Motion::Frame::World )
			{
				b3Vec3 d = {};
				if ( w.size() < 7 || Number( w[4], -1.0f, 1.0f, d.x ) == false || Number( w[5], -1.0f, 1.0f, d.y ) == false ||
					 Number( w[6], -1.0f, 1.0f, d.z ) == false )
				{
					return fail( "a world impulse wants its direction: x y z, each -1 to 1" );
				}
				float length = 0.0f;
				d = b3GetLengthAndNormalize( &length, d );
				if ( length < 0.001f )
				{
					return fail( "a world impulse wants a direction that is not zero" );
				}
				m.direction = d;
			}
		}
		else if ( key == "param" )
		{
			int param = w.size() >= 3 ? MoveParamByName( w[1].c_str() ) : -1;
			Motion::Param p;
			if ( param < 0 || Number( w[2], MoveParamInfoOf( param ).min, MoveParamInfoOf( param ).max, p.value ) == false )
			{
				return fail( "param wants a movement parameter and a value in its range" );
			}
			p.param = uint8_t( param );
			m.params.push_back( p );
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
			m.changes.push_back( change );
		}
		else if ( key == "tether" )
		{
			if ( w.size() < 6 || ( w[5] != "rope" && w[5] != "free" ) || Number( w[1], 1.0f, 500.0f, m.tetherRange ) == false ||
				 Number( w[2], 0.0f, 1000.0f, m.tetherTravel ) == false || Number( w[3], 0.0f, 500.0f, m.tetherPull ) == false ||
				 Number( w[4], 0.0f, 100.0f, m.tetherReel ) == false )
			{
				return fail( "tether wants its range (1 to 500 m), travel speed (m/s, 0: at once), pull (m/s^2), reel (m/s) and \"rope\" "
							 "or \"free\"" );
			}
			m.tether = true;
			m.tetherRope = w[5] == "rope";
		}
		else if ( key == "until" )
		{
			std::string exprError;
			if ( w.size() < 2 || CompileAnimExpr( w[1], schema, m.tetherUntil, exprError, warnings ) == false )
			{
				return fail( "until: " + exprError );
			}
		}
		else if ( key == "emit" )
		{
			if ( w.size() < 2 || w[1].empty() )
			{
				return fail( "emit wants an event" );
			}
			m.event = schema.FindEvent( w[1] );
			if ( m.event < 0 )
			{
				warnings += m.name + ": no mod declares the event \"" + w[1] + "\": nothing is emitted; ";
			}
		}
		else
		{
			return fail( "unknown line \"" + key + "\"" );
		}
	}
	if ( header == false )
	{
		number = 1;
		return fail( "not a motions file (it starts with \"cinderbox_motions\t1\")" );
	}
	for ( Motion& m : motions )
	{
		if ( m.when == Motion::When::While && m.tether )
		{
			error = "motions " + set + ": " + m.name + ": a tether is thrown by a press or an event, not by a while motion";
			return false;
		}
		if ( m.when != Motion::When::While )
		{
			continue;
		}
		// Per second, a whole number never moves: a tick's share of it rounds to nothing.
		for ( size_t i = m.changes.size(); i-- > 0; )
		{
			if ( m.changes[i].op != Motion::ChangeOp::Set && m.changes[i].type != BoardType::Float )
			{
				warnings += m.name + ": a while motion's += and -= are per second and need a Float field: the change is skipped; ";
				m.changes.erase( m.changes.begin() + std::ptrdiff_t( i ) );
			}
		}
	}
	out.insert( out.end(), motions.begin(), motions.end() );
	return true;
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
	if ( motions->list.size() > size_t( kMaxMotions ) )
	{
		warnings += "more than " + std::to_string( kMaxMotions ) + " motions: " + motions->list[size_t( kMaxMotions )].name +
					" and the ones after it are left out; ";
		motions->list.resize( size_t( kMaxMotions ) );
	}
	if ( motions->list.empty() )
	{
		return nullptr;
	}
	return motions;
}

void RunMotions( const Motions& motions, const MotionInputs& in, MotionState& state, Character& c, Blackboard& board, bool& boardChanged,
				 std::vector<ModEventRecord>& events )
{
	const uint32_t now = in.tick + 1; // 0 in a slot means "never"
	const float dt = 1.0f / float( in.tickRate );
	for ( size_t i = 0; in.canAct && i < motions.list.size() && i < size_t( kMaxMotions ); ++i )
	{
		const Motion& m = motions.list[i];
		MotionSlot& slot = state.slots[i];
		const bool isWhile = m.when == Motion::When::While;
		// A while motion that was on last tick goes on without asking its cooldown again.
		const bool wasOn = isWhile && slot.lastTick != 0 && slot.untilTick == in.tick;

		if ( m.uses > 0 && slot.used > 0 )
		{
			bool byTime = m.refillSeconds > 0.0f && slot.lastTick != 0 && now - slot.lastTick >= Ticks( m.refillSeconds, in.tickRate );
			if ( ( m.refillOnGround && c.grounded != 0 ) || byTime )
			{
				slot.used = 0;
			}
		}

		switch ( m.when )
		{
			case Motion::When::Press:
				if ( Pressed( m, in, state.prevActions ) == false )
				{
					continue;
				}
				break;
			case Motion::When::Event:
				if ( m.trigger < 0 || HeardEvent( m.trigger, in ) == false )
				{
					continue;
				}
				break;
			case Motion::When::While:
				break;
		}
		if ( wasOn == false && slot.lastTick != 0 && now - slot.lastTick < Ticks( m.cooldown, in.tickRate ) )
		{
			continue;
		}
		if ( isWhile == false && m.uses > 0 && slot.used >= m.uses )
		{
			continue;
		}
		if ( m.condition.Empty() == false && EvaluateAnimExpr( m.condition, *in.values, 0.0f ) == 0.0f )
		{
			continue;
		}

		// A tether has to find something to hold on to, or the motion does not happen.
		if ( m.tether && ( in.attach == nullptr || in.attach( in.user, m, i ) == false ) )
		{
			continue;
		}

		b3Vec3 push = b3MulSV( m.impulse, Direction( m, c, *in.input ) );
		if ( isWhile )
		{
			// A thrust: metres per second, every second it is on.
			push = b3MulSV( dt, push );
		}
		else
		{
			switch ( m.replace )
			{
				case Motion::Replace::None:
					break;
				case Motion::Replace::Vertical:
					c.velocity.y = 0.0f;
					break;
				case Motion::Replace::Horizontal:
					c.velocity.x = 0.0f;
					c.velocity.z = 0.0f;
					break;
				case Motion::Replace::All:
					c.velocity = { 0.0f, 0.0f, 0.0f };
					break;
			}
		}
		c.velocity = b3Add( c.velocity, push );
		if ( push.y > 0.0f )
		{
			// A grounded character has its vertical speed cleared by the mover; a push upward has
			// to leave the ground to count, the same way a jump does.
			c.grounded = 0;
		}

		for ( const Motion::Change& change : m.changes )
		{
			if ( isWhile == false )
			{
				ApplyChange( change, board );
			}
			else if ( change.op != Motion::ChangeOp::Set )
			{
				// Per second.
				Motion::Change step = change;
				step.value = change.value * dt;
				ApplyChange( step, board );
			}
			else if ( wasOn == false )
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
		if ( m.event >= 0 && wasOn == false )
		{
			ModEventRecord record;
			record.type = uint16_t( m.event );
			record.vector = isWhile ? b3Vec3{ 0.0f, 0.0f, 0.0f } : push;
			events.push_back( record );
		}

		slot.lastTick = now;
		if ( isWhile )
		{
			slot.untilTick = in.tick + 1; // on this tick; next tick says for itself
		}
		else
		{
			slot.used += 1;
			slot.untilTick = in.tick + Ticks( m.duration, in.tickRate );
		}
	}
	state.prevActions = in.input->actions;
}

void ApplyMotionParams( const Motions& motions, const MotionState& state, uint32_t tick, MoveParams& params )
{
	for ( size_t i = 0; i < motions.list.size() && i < size_t( kMaxMotions ); ++i )
	{
		if ( tick < state.slots[i].untilTick )
		{
			for ( const Motion::Param& p : motions.list[i].params )
			{
				params.values[p.param] = p.value;
			}
		}
	}
}

} // namespace cb
