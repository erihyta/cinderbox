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
constexpr uint32_t kForever = 0xFFFFFFFFu;

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
	static const char* const kFrames[] = { "look", "move", "facing", "up", "world", "to" };
	for ( int i = 0; i < 6; ++i )
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
		case MotionFrame::To:
			break;
	}
	return { 0.0f, 1.0f, 0.0f };
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
			if ( key != "cinderbox_motions" || w.size() < 2 || w[1] != "2" )
			{
				return fail( "not a motions file (it starts with \"cinderbox_motions\t2\")" );
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
		else if ( key == "if" || key == "until" )
		{
			std::string exprError;
			if ( w.size() < 2 || CompileAnimExpr( w[1], schema, key == "if" ? m.condition : m.until, exprError, warnings ) == false )
			{
				return fail( ( key == "if" ? "condition: " : "until: " ) + exprError );
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
		else if ( key == "probe" )
		{
			if ( w.size() < 3 || Number( w[1], 1.0f, 500.0f, m.probeRange ) == false || Number( w[2], 0.0f, 1000.0f, m.probeTravel ) == false )
			{
				return fail( "probe wants its range (1 to 500 m) and its travel speed (m/s, 0: at once)" );
			}
			m.probe = true;
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
			if ( w.size() < 5 || ParseTarget( w[1], schema, m.name, e.target, warnings ) == false ||
				 Number( w[2], -kMaxMotionImpulse, kMaxMotionImpulse, e.strength ) == false || ParseFrame( w[3], e.frame ) == false || replace < 0 )
			{
				return fail( "impulse wants a target (self, hit, @field), a speed (m/s), a frame (look, move, facing, up, world, to) and what "
							 "it replaces (none, vertical, horizontal, all)" );
			}
			e.replace = MotionEffect::Replace( replace );
			if ( e.frame == MotionFrame::World && ParseWorld( w, 5, e.direction ) == false )
			{
				return fail( "a world frame wants its direction: x y z, each -1 to 1, not all zero" );
			}
			m.effects.push_back( e );
		}
		else if ( key == "force" )
		{
			// force <target> <frame> <accel|force|velocity> <strength> <speed> <ramp> <react 0|1> [x y z]
			MotionEffect e;
			e.kind = MotionEffect::Kind::Force;
			int push = w.size() >= 8 ? ( w[3] == "accel" ? 0 : ( w[3] == "force" ? 1 : ( w[3] == "velocity" ? 2 : -1 ) ) ) : -1;
			float limit = push == 1 ? kMaxMotionForce : kMaxMotionAccel;
			if ( push < 0 || ParseTarget( w[1], schema, m.name, e.target, warnings ) == false || ParseFrame( w[2], e.frame ) == false ||
				 Number( w[4], -limit, limit, e.strength ) == false || Number( w[5], -kMaxMotionImpulse, kMaxMotionImpulse, e.speed ) == false ||
				 Number( w[6], 0.0f, 60.0f, e.ramp ) == false || ( w[7] != "0" && w[7] != "1" ) )
			{
				return fail( "force wants a target (self, hit, @field), a frame, a kind (accel, force, velocity), a strength, a speed, a "
							 "ramp (seconds) and whether it reacts (0 or 1)" );
			}
			e.push = MotionEffect::Push( push );
			e.react = w[7] == "1";
			if ( e.frame == MotionFrame::World && ParseWorld( w, 8, e.direction ) == false )
			{
				return fail( "a world frame wants its direction: x y z, each -1 to 1, not all zero" );
			}
			m.effects.push_back( e );
		}
		else if ( key == "link" )
		{
			// link <target> <length> <reel>
			MotionEffect e;
			e.kind = MotionEffect::Kind::Link;
			if ( w.size() < 4 || ParseTarget( w[1], schema, m.name, e.target, warnings ) == false || e.target.kind == MotionTarget::Kind::Self ||
				 Number( w[2], 0.0f, 500.0f, e.length ) == false || Number( w[3], 0.0f, 100.0f, e.reel ) == false )
			{
				return fail( "link wants a target that is not the player itself (hit, @field), a length (m; 0: the distance when the probe "
							 "takes hold) and a reel (m/s)" );
			}
			m.effects.push_back( e );
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
		return fail( "not a motions file (it starts with \"cinderbox_motions\t2\")" );
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
		for ( const MotionEffect& e : m.effects )
		{
			bool needsHit = e.target.kind == MotionTarget::Kind::Hit || ( e.frame == MotionFrame::To && e.target.kind != MotionTarget::Kind::Field );
			if ( needsHit && m.probe == false )
			{
				return wrong( "an effect on \"hit\", or along \"to\", needs the motion to have a probe (or a target that is a field)" );
			}
			if ( e.kind == MotionEffect::Kind::Link && e.length <= 0.0f && m.probe == false )
			{
				return wrong( "a link without a probe needs a length" );
			}
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

void RunMotions( const Motions& motions, const MotionInputs& in, MotionState& state, const Character& c, Blackboard& board,
				 bool& boardChanged, std::vector<ModEventRecord>& events, std::vector<MotionActive>& active )
{
	const uint32_t now = in.tick + 1; // 0 in a slot means "never"
	const float dt = 1.0f / float( in.tickRate );
	for ( size_t i = 0; i < motions.list.size(); ++i )
	{
		const Motion& m = motions.list[i];
		MotionSlot& slot = state.slots[i];
		const bool isWhile = m.when == Motion::When::While;
		// A while motion that was on last tick goes on without asking its cooldown again.
		const bool wasOn = isWhile && slot.lastTick != 0 && slot.untilTick == in.tick;
		bool on = slot.lastTick != 0 && in.tick < slot.untilTick && isWhile == false;

		// One that is held ends by its condition (not on the tick it started), or when what its
		// probe holds on to is gone.
		if ( on && m.Held() )
		{
			bool lost = m.probe && ( in.held != int( i ) || in.holdAlive == false );
			bool done = m.until.Empty() == false && slot.lastTick != now && EvaluateAnimExpr( m.until, *in.values, 0.0f ) != 0.0f;
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

		bool starts = in.canAct;
		if ( starts && m.when == Motion::When::Press )
		{
			starts = Pressed( m, in, state.prevActions );
		}
		else if ( starts && m.when == Motion::When::Event )
		{
			starts = m.trigger >= 0 && HeardEvent( m.trigger, in );
		}
		// One that is held does not start again while it is on.
		starts = starts && ( on == false || m.Held() == false );
		starts = starts && ( wasOn || slot.lastTick == 0 || now - slot.lastTick >= Ticks( m.cooldown, in.tickRate ) );
		starts = starts && ( isWhile || m.uses == 0 || slot.used < m.uses );
		starts = starts && ( m.condition.Empty() || EvaluateAnimExpr( m.condition, *in.values, 0.0f ) != 0.0f );
		// A probe has to find something to hold on to, or the motion does not happen.
		uint32_t effectTick = in.tick;
		if ( starts && m.probe && ( in.attach == nullptr || in.attach( in.user, m, i, effectTick ) == false ) )
		{
			starts = false;
		}

		if ( starts )
		{
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
				events.push_back( record );
			}

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
			}
		}

		// Its effects apply on every tick it is in effect, and on the tick it starts.
		bool inEffect = slot.lastTick != 0 && slot.sinceTick <= in.tick && in.tick < slot.untilTick;
		bool startsNow = slot.lastTick != 0 && slot.sinceTick == in.tick && ( starts || isWhile == false );
		if ( inEffect || ( starts && slot.sinceTick == in.tick ) )
		{
			active.push_back( { uint8_t( i ), startsNow } );
		}
	}
	state.prevActions = in.input->actions;
}

void ApplyMotionParams( const Motions& motions, const MotionState& state, uint32_t tick, MoveParams& params )
{
	for ( size_t i = 0; i < motions.list.size(); ++i )
	{
		const MotionSlot slot = state.slots[i];
		if ( slot.lastTick != 0 && slot.sinceTick <= tick && tick < slot.untilTick )
		{
			for ( const Motion::Param& p : motions.list[i].params )
			{
				params.values[p.param] = p.value;
			}
		}
	}
}

} // namespace cb
