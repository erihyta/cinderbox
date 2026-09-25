#include "anim_graph.h"

#include "simulation.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace cb
{

namespace
{

constexpr int kMaxExprStack = 32;
constexpr float kMaxStateTime = 600.0f;
constexpr float kMinClipLength = 1.0f / 1024.0f;

const char* const kBuiltinNames[AnimExpr::BuiltinCount] = {
	"speed", "forward_speed", "vertical_speed", "grounded", "airborne_time", "jumped", "aiming", "backward", "state_time",
};

// --- Expressions ----------------------------------------------------------------------------------

struct Token
{
	enum Kind
	{
		End,
		Number,
		Name,
		Symbol,
	} kind = End;
	std::string text;
	float value = 0.0f;
};

class ExprCompiler
{
public:
	ExprCompiler( const std::string& text, const ModSchema& schema, AnimExpr& out, std::string& warnings )
		: m_text( text ), m_schema( schema ), m_out( out ), m_warnings( warnings )
	{
	}

	bool Compile( std::string& error )
	{
		m_out.steps.clear();
		m_out.text = m_text;
		Next();
		if ( m_token.kind == Token::End )
		{
			return true; // empty: always true
		}
		if ( Or() == false || Expect( Token::End, "" ) == false )
		{
			error = "'" + m_text + "': " + m_error;
			return false;
		}
		// Every step pushes at most one value; a depth check keeps evaluation on a fixed stack.
		int depth = 0, deepest = 0;
		for ( const AnimExpr::Step& s : m_out.steps )
		{
			depth += ( s.op == AnimExpr::Op::Const || s.op == AnimExpr::Op::Var ) ? 1
					 : ( s.op == AnimExpr::Op::Not || s.op == AnimExpr::Op::Negate ) ? 0
																					 : -1;
			deepest = std::max( deepest, depth );
		}
		if ( deepest > kMaxExprStack )
		{
			error = "'" + m_text + "' is too deeply nested";
			return false;
		}
		return true;
	}

private:
	void Next()
	{
		const std::string& s = m_text;
		while ( m_pos < s.size() && std::isspace( static_cast<unsigned char>( s[m_pos] ) ) )
		{
			++m_pos;
		}
		m_token = Token{};
		if ( m_pos >= s.size() )
		{
			return;
		}
		char c = s[m_pos];
		if ( std::isdigit( static_cast<unsigned char>( c ) ) || ( c == '.' && m_pos + 1 < s.size() && std::isdigit( static_cast<unsigned char>( s[m_pos + 1] ) ) ) )
		{
			size_t start = m_pos;
			while ( m_pos < s.size() && ( std::isdigit( static_cast<unsigned char>( s[m_pos] ) ) || s[m_pos] == '.' ) )
			{
				++m_pos;
			}
			if ( m_pos < s.size() && ( s[m_pos] == 'e' || s[m_pos] == 'E' ) )
			{
				++m_pos;
				if ( m_pos < s.size() && ( s[m_pos] == '+' || s[m_pos] == '-' ) )
				{
					++m_pos;
				}
				while ( m_pos < s.size() && std::isdigit( static_cast<unsigned char>( s[m_pos] ) ) )
				{
					++m_pos;
				}
			}
			m_token.kind = Token::Number;
			m_token.text = s.substr( start, m_pos - start );
			if ( ParseAnimFloat( m_token.text.data(), m_token.text.size(), m_token.value ) == false )
			{
				m_token.kind = Token::Symbol; // reported as unexpected
			}
			return;
		}
		if ( std::isalpha( static_cast<unsigned char>( c ) ) || c == '_' )
		{
			size_t start = m_pos;
			while ( m_pos < s.size() &&
					( std::isalnum( static_cast<unsigned char>( s[m_pos] ) ) || s[m_pos] == '_' || s[m_pos] == '.' ) )
			{
				++m_pos;
			}
			m_token.kind = Token::Name;
			m_token.text = s.substr( start, m_pos - start );
			return;
		}
		static const char* const kTwo[] = { "==", "!=", "<=", ">=", "&&", "||" };
		for ( const char* two : kTwo )
		{
			if ( s.compare( m_pos, 2, two ) == 0 )
			{
				m_token.kind = Token::Symbol;
				m_token.text = two;
				m_pos += 2;
				return;
			}
		}
		m_token.kind = Token::Symbol;
		m_token.text = std::string( 1, c );
		++m_pos;
	}

	bool Is( const char* symbolOrWord ) const
	{
		return ( m_token.kind == Token::Symbol || m_token.kind == Token::Name ) && m_token.text == symbolOrWord;
	}

	bool Expect( Token::Kind kind, const char* text )
	{
		if ( m_token.kind == kind && ( kind == Token::End || m_token.text == text ) )
		{
			Next();
			return true;
		}
		m_error = m_token.kind == Token::End ? std::string( "unexpected end" ) : "unexpected '" + m_token.text + "'";
		return false;
	}

	void Emit( AnimExpr::Op op )
	{
		AnimExpr::Step step;
		step.op = op;
		m_out.steps.push_back( step );
	}

	bool Or()
	{
		if ( And() == false )
		{
			return false;
		}
		while ( Is( "or" ) || Is( "||" ) )
		{
			Next();
			if ( And() == false )
			{
				return false;
			}
			Emit( AnimExpr::Op::Or );
		}
		return true;
	}

	bool And()
	{
		if ( Not() == false )
		{
			return false;
		}
		while ( Is( "and" ) || Is( "&&" ) )
		{
			Next();
			if ( Not() == false )
			{
				return false;
			}
			Emit( AnimExpr::Op::And );
		}
		return true;
	}

	bool Not()
	{
		if ( Is( "not" ) || Is( "!" ) )
		{
			Next();
			if ( Not() == false )
			{
				return false;
			}
			Emit( AnimExpr::Op::Not );
			return true;
		}
		return Compare();
	}

	bool Compare()
	{
		if ( Add() == false )
		{
			return false;
		}
		static const std::pair<const char*, AnimExpr::Op> kOps[] = {
			{ "==", AnimExpr::Op::Equal },	 { "!=", AnimExpr::Op::NotEqual },	 { "<=", AnimExpr::Op::LessEqual },
			{ ">=", AnimExpr::Op::GreaterEqual }, { "<", AnimExpr::Op::Less }, { ">", AnimExpr::Op::Greater },
		};
		for ( const auto& [symbol, op] : kOps )
		{
			if ( m_token.kind == Token::Symbol && m_token.text == symbol )
			{
				Next();
				if ( Add() == false )
				{
					return false;
				}
				Emit( op );
				return true;
			}
		}
		return true;
	}

	bool Add()
	{
		if ( Mul() == false )
		{
			return false;
		}
		while ( m_token.kind == Token::Symbol && ( m_token.text == "+" || m_token.text == "-" ) )
		{
			AnimExpr::Op op = m_token.text == "+" ? AnimExpr::Op::Add : AnimExpr::Op::Sub;
			Next();
			if ( Mul() == false )
			{
				return false;
			}
			Emit( op );
		}
		return true;
	}

	bool Mul()
	{
		if ( Unary() == false )
		{
			return false;
		}
		while ( m_token.kind == Token::Symbol && ( m_token.text == "*" || m_token.text == "/" ) )
		{
			AnimExpr::Op op = m_token.text == "*" ? AnimExpr::Op::Mul : AnimExpr::Op::Div;
			Next();
			if ( Unary() == false )
			{
				return false;
			}
			Emit( op );
		}
		return true;
	}

	bool Unary()
	{
		if ( m_token.kind == Token::Symbol && m_token.text == "-" )
		{
			Next();
			if ( Unary() == false )
			{
				return false;
			}
			Emit( AnimExpr::Op::Negate );
			return true;
		}
		return Primary();
	}

	bool Primary()
	{
		if ( m_token.kind == Token::Number )
		{
			AnimExpr::Step step;
			step.op = AnimExpr::Op::Const;
			step.value = m_token.value;
			m_out.steps.push_back( step );
			Next();
			return true;
		}
		if ( m_token.kind == Token::Symbol && m_token.text == "(" )
		{
			Next();
			return Or() && Expect( Token::Symbol, ")" );
		}
		if ( m_token.kind == Token::Name && m_token.text != "and" && m_token.text != "or" && m_token.text != "not" )
		{
			AnimExpr::Step step;
			if ( m_token.text == "true" || m_token.text == "false" )
			{
				step.op = AnimExpr::Op::Const;
				step.value = m_token.text == "true" ? 1.0f : 0.0f;
			}
			else
			{
				step.op = AnimExpr::Op::Var;
				Resolve( m_token.text, step );
			}
			m_out.steps.push_back( step );
			Next();
			return true;
		}
		m_error = m_token.kind == Token::End ? std::string( "unexpected end" ) : "unexpected '" + m_token.text + "'";
		return false;
	}

	void Resolve( const std::string& name, AnimExpr::Step& step )
	{
		for ( int b = 0; b < AnimExpr::BuiltinCount; ++b )
		{
			if ( name == kBuiltinNames[b] )
			{
				step.kind = AnimExpr::VarKind::Builtin;
				step.index = uint8_t( b );
				return;
			}
		}
		if ( const BoardField* field = m_schema.FindField( name ) )
		{
			step.kind = field->scope == BoardScope::Global ? AnimExpr::VarKind::GlobalField : AnimExpr::VarKind::EntityField;
			step.index = field->slot;
			step.type = field->type;
			return;
		}
		int event = m_schema.FindEvent( name );
		if ( event >= 0 && event < 256 )
		{
			step.kind = AnimExpr::VarKind::Event;
			step.index = uint8_t( event );
			return;
		}
		int stance = m_schema.FindStance( name );
		if ( stance >= 0 && stance < kMaxStances )
		{
			step.kind = AnimExpr::VarKind::Stance;
			step.index = uint8_t( stance + 1 );
			return;
		}
		step.kind = AnimExpr::VarKind::Zero;
		if ( m_warnings.find( "'" + name + "'" ) == std::string::npos )
		{
			m_warnings += "unknown name '" + name + "' (no mod declares it; it reads as 0); ";
		}
	}

	const std::string& m_text;
	const ModSchema& m_schema;
	AnimExpr& m_out;
	std::string& m_warnings;
	size_t m_pos = 0;
	Token m_token;
	std::string m_error;
};

float ReadVar( const AnimExpr::Step& step, const AnimGraphInputs& in, float stateTime )
{
	switch ( step.kind )
	{
		case AnimExpr::VarKind::Builtin:
			return step.index == AnimExpr::StateTime ? stateTime : in.builtins[step.index];
		case AnimExpr::VarKind::Stance:
			if ( in.state != nullptr )
			{
				for ( uint8_t stance : in.state->stances )
				{
					if ( stance == step.index )
					{
						return 1.0f;
					}
				}
			}
			return 0.0f;
		case AnimExpr::VarKind::Event:
		{
			uint32_t kept = std::min( in.eventCount, kModEventHistory );
			for ( uint32_t i = 0; in.events != nullptr && i < kept; ++i )
			{
				const ModEventRecord& e = in.events[( in.eventCount - 1 - i ) % kModEventHistory];
				if ( e.tick != in.tick )
				{
					break; // newest first: the rest are older
				}
				if ( e.type == step.index && e.netIdA == in.netId )
				{
					return 1.0f;
				}
			}
			return 0.0f;
		}
		case AnimExpr::VarKind::EntityField:
		case AnimExpr::VarKind::GlobalField:
		{
			const int32_t* board = step.kind == AnimExpr::VarKind::EntityField ? in.board : in.globalBoard;
			int32_t value = board != nullptr && step.index < kBoardSlots ? board[step.index] : 0;
			switch ( step.type )
			{
				case BoardType::Float:
					return BoardToFloat( value );
				case BoardType::Bool:
					return value != 0 ? 1.0f : 0.0f;
				case BoardType::Int:
					return float( value );
			}
			return 0.0f;
		}
		case AnimExpr::VarKind::Zero:
			return 0.0f;
	}
	return 0.0f;
}

// --- graph.cfg ------------------------------------------------------------------------------------

std::vector<std::string> SplitTabs( const std::string& line )
{
	std::vector<std::string> out;
	size_t start = 0;
	while ( true )
	{
		size_t tab = line.find( '\t', start );
		out.push_back( line.substr( start, tab == std::string::npos ? std::string::npos : tab - start ) );
		if ( tab == std::string::npos )
		{
			return out;
		}
		start = tab + 1;
	}
}

bool ToFloat( const std::string& text, float& out )
{
	return ParseAnimFloat( text.data(), text.size(), out );
}

bool ToInt( const std::string& text, int& out )
{
	if ( text.empty() || text.size() > 9 )
	{
		return false;
	}
	int value = 0;
	size_t i = 0;
	bool negative = text[0] == '-';
	if ( negative )
	{
		i = 1;
	}
	if ( i >= text.size() )
	{
		return false;
	}
	for ( ; i < text.size(); ++i )
	{
		if ( std::isdigit( static_cast<unsigned char>( text[i] ) ) == 0 )
		{
			return false;
		}
		value = value * 10 + ( text[i] - '0' );
	}
	out = negative ? -value : value;
	return true;
}

int FindState( const AnimGraphLayer& layer, const std::string& name )
{
	for ( size_t i = 0; i < layer.states.size(); ++i )
	{
		if ( layer.states[i].name == name )
		{
			return int( i );
		}
	}
	return -1;
}

// --- Running --------------------------------------------------------------------------------------

} // namespace

float AnimGraphPointWeight( const AnimGraphState& state, float blend, size_t point )
{
	const auto& p = state.points;
	if ( p.size() == 1 || state.blend == false )
	{
		return point == 0 ? 1.0f : 0.0f;
	}
	if ( blend <= p.front().position )
	{
		return point == 0 ? 1.0f : 0.0f;
	}
	if ( blend >= p.back().position )
	{
		return point + 1 == p.size() ? 1.0f : 0.0f;
	}
	for ( size_t i = 0; i + 1 < p.size(); ++i )
	{
		float a = p[i].position, b = p[i + 1].position;
		if ( blend >= a && blend <= b )
		{
			float t = b > a ? ( blend - a ) / ( b - a ) : 0.0f;
			if ( point == i )
			{
				return 1.0f - t;
			}
			if ( point == i + 1 )
			{
				return t;
			}
			return 0.0f;
		}
	}
	return 0.0f;
}

namespace
{

// Advances a state's clock by dt. Returns true when it came round (a loop, or a blend space's
// cycle); `ended` is set when a one-shot clip is within `window` seconds of its end.
bool Advance( const AnimGraph& graph, const AnimGraphState& state, float blend, float& time, float dt, float window, bool& ended )
{
	ended = false;
	if ( state.points.empty() )
	{
		return false;
	}
	if ( state.blend )
	{
		// Phase-synced: every clip at the same fraction of its cycle, like the feet of walk and run.
		float rate = 0.0f;
		for ( size_t i = 0; i < state.points.size(); ++i )
		{
			float w = AnimGraphPointWeight( state, blend, i );
			if ( w > 0.0f )
			{
				rate += w / graph.clips[size_t( state.points[i].clip )].length;
			}
		}
		time += rate * dt;
		bool wrapped = false;
		while ( time >= 1.0f )
		{
			time -= 1.0f;
			wrapped = true;
		}
		return wrapped;
	}
	const AnimGraphClip& clip = graph.clips[size_t( state.points[0].clip )];
	time += dt;
	if ( clip.loops )
	{
		bool wrapped = false;
		while ( time >= clip.length )
		{
			time -= clip.length;
			wrapped = true;
		}
		return wrapped;
	}
	if ( time >= clip.length )
	{
		time = clip.length;
	}
	ended = time >= clip.length - window;
	return false;
}

void FireMarkers( const AnimGraph& graph, const AnimGraphState& state, float from, float to, bool wrapped, bool entered,
				  std::vector<int>& markers )
{
	if ( state.blend || state.points.empty() )
	{
		return; // markers play from single-clip states
	}
	const AnimGraphClip& clip = graph.clips[size_t( state.points[0].clip )];
	for ( const AnimGraphClip::Marker& m : clip.markers )
	{
		if ( m.event < 0 )
		{
			continue;
		}
		// Where the marker sits on the state's clock (a backward clip plays from its end).
		float at = state.points[0].backward ? clip.length - m.time : m.time;
		bool after = at > from || ( entered && at >= from );
		bool crossed = wrapped ? ( after || at <= to ) : ( after && at <= to );
		if ( crossed )
		{
			markers.push_back( m.event );
		}
	}
}

} // namespace

bool ParseAnimFloat( const char* text, size_t length, float& out )
{
	size_t i = 0;
	bool negative = false;
	if ( i < length && ( text[i] == '-' || text[i] == '+' ) )
	{
		negative = text[i] == '-';
		++i;
	}
	uint64_t mantissa = 0;
	int digits = 0, exponent = 0;
	bool any = false, dot = false;
	for ( ; i < length; ++i )
	{
		char c = text[i];
		if ( c == '.' && dot == false )
		{
			dot = true;
			continue;
		}
		if ( c < '0' || c > '9' )
		{
			break;
		}
		any = true;
		if ( digits < 18 )
		{
			if ( mantissa != 0 || c != '0' )
			{
				++digits;
			}
			mantissa = mantissa * 10 + uint64_t( c - '0' );
			if ( dot )
			{
				--exponent;
			}
		}
		else if ( dot == false )
		{
			++exponent; // digits past what fits only scale
		}
	}
	if ( any == false )
	{
		return false;
	}
	if ( i < length && ( text[i] == 'e' || text[i] == 'E' ) )
	{
		++i;
		bool negativeExponent = false;
		if ( i < length && ( text[i] == '-' || text[i] == '+' ) )
		{
			negativeExponent = text[i] == '-';
			++i;
		}
		int e = 0;
		bool anyExponent = false;
		for ( ; i < length && text[i] >= '0' && text[i] <= '9'; ++i )
		{
			e = std::min( e * 10 + ( text[i] - '0' ), 1000 );
			anyExponent = true;
		}
		if ( anyExponent == false )
		{
			return false;
		}
		exponent += negativeExponent ? -e : e;
	}
	if ( i != length )
	{
		return false;
	}
	// Plain IEEE double arithmetic in a fixed order, then one rounding to float: the same bits on
	// every machine.
	static const double kPowers[] = { 1e0,	1e1,  1e2,	1e3,  1e4,	1e5,  1e6,	1e7,  1e8,	1e9,  1e10, 1e11,
									  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
	double value = double( mantissa );
	int e = exponent;
	while ( e > 0 && value != 0.0 )
	{
		int step = std::min( e, 22 );
		value *= kPowers[step];
		e -= step;
		if ( value > 1e300 )
		{
			break;
		}
	}
	while ( e < 0 && value != 0.0 )
	{
		int step = std::min( -e, 22 );
		value /= kPowers[step];
		e += step;
		if ( value < 1e-300 )
		{
			value = 0.0;
		}
	}
	out = float( negative ? -value : value );
	return true;
}

int AnimGraph::FindClip( const std::string& name ) const
{
	for ( size_t i = 0; i < clips.size(); ++i )
	{
		if ( clips[i].name == name )
		{
			return int( i );
		}
	}
	return -1;
}

bool AnimGraph::EmitsEvent( int event ) const
{
	for ( const AnimGraphClip& clip : clips )
	{
		for ( const AnimGraphClip::Marker& m : clip.markers )
		{
			if ( m.event == event && event >= 0 )
			{
				return true;
			}
		}
	}
	return false;
}

bool CompileAnimExpr( const std::string& text, const ModSchema& schema, AnimExpr& out, std::string& error, std::string& warnings )
{
	ExprCompiler compiler( text, schema, out, warnings );
	return compiler.Compile( error );
}

std::shared_ptr<const AnimGraph> CompileAnimGraph( const std::string& text, const ModSchema& schema, std::string& error,
												   std::string& warnings )
{
	auto graph = std::make_shared<AnimGraph>();
	graph->text = text;
	struct PendingTransition
	{
		size_t layer;
		int line;
		std::vector<std::string> fields;
	};
	std::vector<PendingTransition> transitions;
	std::vector<std::pair<size_t, std::string>> starts;

	int lineNumber = 0;
	size_t start = 0;
	bool header = false;
	while ( start <= text.size() )
	{
		size_t end = text.find( '\n', start );
		std::string line = text.substr( start, end == std::string::npos ? std::string::npos : end - start );
		start = end == std::string::npos ? text.size() + 1 : end + 1;
		++lineNumber;
		if ( line.empty() == false && line.back() == '\r' )
		{
			line.pop_back();
		}
		if ( line.empty() || line[0] == '#' )
		{
			continue;
		}
		std::vector<std::string> f = SplitTabs( line );
		auto fail = [&]( const std::string& why ) {
			error = "graph.cfg line " + std::to_string( lineNumber ) + ": " + why;
			return nullptr;
		};
		const std::string& kind = f[0];
		if ( header == false )
		{
			if ( kind != "cinderbox_graph" || f.size() < 2 || f[1] != "1" )
			{
				return fail( "not a version 1 graph" );
			}
			header = true;
			continue;
		}
		if ( kind == "clip" )
		{
			AnimGraphClip clip;
			if ( f.size() < 4 || ToFloat( f[2], clip.length ) == false )
			{
				return fail( "clip needs a name, a length and a loop flag" );
			}
			clip.name = f[1];
			clip.length = std::max( clip.length, kMinClipLength );
			clip.loops = f[3] == "1";
			if ( graph->FindClip( clip.name ) >= 0 )
			{
				return fail( "clip '" + clip.name + "' twice" );
			}
			graph->clips.push_back( std::move( clip ) );
		}
		else if ( kind == "marker" )
		{
			int clip = f.size() >= 4 ? graph->FindClip( f[1] ) : -1;
			AnimGraphClip::Marker marker;
			if ( clip < 0 || ToFloat( f[2], marker.time ) == false )
			{
				return fail( "marker needs a known clip, a time and a name" );
			}
			marker.name = f[3];
			marker.event = schema.FindEvent( marker.name );
			graph->clips[size_t( clip )].markers.push_back( marker );
		}
		else if ( kind == "layer" )
		{
			if ( f.size() < 2 || graph->layers.size() >= size_t( kMaxAnimLayers ) )
			{
				return fail( "a layer needs a name, and there are at most " + std::to_string( kMaxAnimLayers ) );
			}
			AnimGraphLayer layer;
			layer.name = f[1];
			graph->layers.push_back( std::move( layer ) );
		}
		else if ( graph->layers.empty() )
		{
			return fail( "'" + kind + "' before any layer" );
		}
		else if ( kind == "mask" )
		{
			graph->layers.back().mask.assign( f.begin() + 1, f.end() );
		}
		else if ( kind == "weight" )
		{
			std::string why;
			if ( CompileAnimExpr( f.size() > 1 ? f[1] : "", schema, graph->layers.back().weight, why, warnings ) == false )
			{
				return fail( why );
			}
		}
		else if ( kind == "state" )
		{
			AnimGraphLayer& layer = graph->layers.back();
			if ( f.size() < 4 || int( layer.states.size() ) >= kMaxGraphStates || FindState( layer, f[1] ) >= 0 )
			{
				return fail( "bad or repeated state, or too many states (at most " + std::to_string( kMaxGraphStates ) + ")" );
			}
			AnimGraphState state;
			state.name = f[1];
			if ( f[2] == "clip" )
			{
				int clip = graph->FindClip( f[3] );
				if ( clip < 0 )
				{
					return fail( "state '" + state.name + "' plays an unknown clip '" + f[3] + "'" );
				}
				state.points.push_back( { 0.0f, clip, f.size() > 4 && f[4] == "1" } );
			}
			else if ( f[2] == "blend" )
			{
				state.blend = true;
				std::string why;
				if ( CompileAnimExpr( f[3], schema, state.input, why, warnings ) == false )
				{
					return fail( why );
				}
			}
			else
			{
				return fail( "state kind '" + f[2] + "'" );
			}
			layer.states.push_back( std::move( state ) );
		}
		else if ( kind == "point" )
		{
			AnimGraphLayer& layer = graph->layers.back();
			AnimGraphState::Point point;
			if ( layer.states.empty() || layer.states.back().blend == false || f.size() < 3 ||
				 ToFloat( f[1], point.position ) == false || ( point.clip = graph->FindClip( f[2] ) ) < 0 )
			{
				return fail( "point needs a blend state before it, a position and a known clip" );
			}
			point.backward = f.size() > 3 && f[3] == "1";
			layer.states.back().points.push_back( point );
		}
		else if ( kind == "start" )
		{
			starts.push_back( { graph->layers.size() - 1, f.size() > 1 ? f[1] : "" } );
		}
		else if ( kind == "transition" )
		{
			transitions.push_back( { graph->layers.size() - 1, lineNumber, f } );
		}
		else
		{
			return fail( "unknown line '" + kind + "'" );
		}
	}
	if ( header == false || graph->layers.empty() )
	{
		error = "graph.cfg has no layers";
		return nullptr;
	}

	for ( const auto& [layerIndex, name] : starts )
	{
		AnimGraphLayer& layer = graph->layers[layerIndex];
		layer.start = FindState( layer, name );
		if ( layer.start < 0 )
		{
			error = "layer '" + layer.name + "' starts in an unknown state '" + name + "'";
			return nullptr;
		}
	}
	for ( const PendingTransition& t : transitions )
	{
		AnimGraphLayer& layer = graph->layers[t.layer];
		const auto& f = t.fields;
		int from = f.size() >= 7 ? FindState( layer, f[1] ) : -1;
		AnimGraphTransition transition;
		transition.to = f.size() >= 7 ? FindState( layer, f[2] ) : -1;
		if ( from < 0 || transition.to < 0 || ToInt( f[3], transition.priority ) == false || ToFloat( f[4], transition.xfade ) == false ||
			 ( f[5] != "immediate" && f[5] != "at_end" ) )
		{
			error = "graph.cfg line " + std::to_string( t.line ) + ": transition needs two states of its layer, a priority, a crossfade and a switch mode";
			return nullptr;
		}
		transition.xfade = std::max( transition.xfade, 0.0f );
		transition.atEnd = f[5] == "at_end";
		std::string why;
		if ( CompileAnimExpr( f.size() > 7 ? f[7] : "", schema, transition.condition, why, warnings ) == false )
		{
			error = "graph.cfg line " + std::to_string( t.line ) + ": " + why;
			return nullptr;
		}
		layer.states[size_t( from )].transitions.push_back( std::move( transition ) );
	}
	for ( AnimGraphLayer& layer : graph->layers )
	{
		if ( layer.states.empty() )
		{
			error = "layer '" + layer.name + "' has no states";
			return nullptr;
		}
		for ( AnimGraphState& state : layer.states )
		{
			if ( state.points.empty() )
			{
				error = "state '" + state.name + "' has no clips";
				return nullptr;
			}
			std::stable_sort( state.points.begin(), state.points.end(),
							  []( const AnimGraphState::Point& a, const AnimGraphState::Point& b ) { return a.position < b.position; } );
			// Godot tries lower priorities first; equal ones keep their order.
			std::stable_sort( state.transitions.begin(), state.transitions.end(),
							  []( const AnimGraphTransition& a, const AnimGraphTransition& b ) { return a.priority < b.priority; } );
		}
	}
	for ( const AnimGraphClip& clip : graph->clips )
	{
		for ( const AnimGraphClip::Marker& m : clip.markers )
		{
			if ( m.event < 0 && warnings.find( "'" + m.name + "'" ) == std::string::npos )
			{
				warnings += "marker '" + m.name + "' on " + clip.name + " emits nothing (no mod declares that event); ";
			}
		}
	}
	return graph;
}

float EvaluateAnimExpr( const AnimExpr& expr, const AnimGraphInputs& in, float stateTime )
{
	float stack[kMaxExprStack];
	int top = 0;
	for ( const AnimExpr::Step& s : expr.steps )
	{
		switch ( s.op )
		{
			case AnimExpr::Op::Const:
				stack[top++] = s.value;
				continue;
			case AnimExpr::Op::Var:
				stack[top++] = ReadVar( s, in, stateTime );
				continue;
			case AnimExpr::Op::Not:
				stack[top - 1] = stack[top - 1] != 0.0f ? 0.0f : 1.0f;
				continue;
			case AnimExpr::Op::Negate:
				stack[top - 1] = -stack[top - 1];
				continue;
			default:
				break;
		}
		float b = stack[--top];
		float a = stack[top - 1];
		float r = 0.0f;
		switch ( s.op )
		{
			case AnimExpr::Op::Add:
				r = a + b;
				break;
			case AnimExpr::Op::Sub:
				r = a - b;
				break;
			case AnimExpr::Op::Mul:
				r = a * b;
				break;
			case AnimExpr::Op::Div:
				r = b != 0.0f ? a / b : 0.0f;
				break;
			case AnimExpr::Op::Less:
				r = a < b ? 1.0f : 0.0f;
				break;
			case AnimExpr::Op::LessEqual:
				r = a <= b ? 1.0f : 0.0f;
				break;
			case AnimExpr::Op::Greater:
				r = a > b ? 1.0f : 0.0f;
				break;
			case AnimExpr::Op::GreaterEqual:
				r = a >= b ? 1.0f : 0.0f;
				break;
			case AnimExpr::Op::Equal:
				r = a == b ? 1.0f : 0.0f;
				break;
			case AnimExpr::Op::NotEqual:
				r = a != b ? 1.0f : 0.0f;
				break;
			case AnimExpr::Op::And:
				r = ( a != 0.0f && b != 0.0f ) ? 1.0f : 0.0f;
				break;
			case AnimExpr::Op::Or:
				r = ( a != 0.0f || b != 0.0f ) ? 1.0f : 0.0f;
				break;
			default:
				break;
		}
		stack[top - 1] = r;
	}
	return top > 0 ? stack[top - 1] : 0.0f;
}

void UpdateAnimGraph( AnimState& s, const AnimGraph& graph, AnimGraphInputs& in, float dt, std::vector<int>& markers )
{
	in.state = &s;
	for ( size_t l = 0; l < graph.layers.size() && l < size_t( kMaxAnimLayers ); ++l )
	{
		const AnimGraphLayer& layer = graph.layers[l];
		AnimGraphLayerState& L = s.graph[l];
		if ( L.started == 0 || L.state >= layer.states.size() || L.previous >= layer.states.size() )
		{
			L = AnimGraphLayerState{};
			L.started = 1;
			L.state = L.previous = uint8_t( layer.start );
			const AnimGraphState& first = layer.states[size_t( layer.start )];
			L.blend = L.previousBlend = first.blend ? EvaluateAnimExpr( first.input, in, 0.0f ) : 0.0f;
			L.weight = ( l == 0 || layer.weight.Empty() ) ? 1.0f : std::clamp( EvaluateAnimExpr( layer.weight, in, 0.0f ), 0.0f, 1.0f );
		}

		// The layer's weight eases toward its expression, like a stance fading in.
		if ( l > 0 )
		{
			float target = layer.weight.Empty() ? 1.0f : std::clamp( EvaluateAnimExpr( layer.weight, in, L.stateTime ), 0.0f, 1.0f );
			float step = dt / kStanceFadeSeconds;
			L.weight += std::clamp( target - L.weight, -step, step );
		}

		const AnimGraphState& state = layer.states[L.state];
		if ( state.blend )
		{
			L.blend = EvaluateAnimExpr( state.input, in, L.stateTime );
		}
		bool entered = L.stateTime == 0.0f;
		float before = L.time;
		bool ended = false, previousEnded = false;
		bool wrapped = Advance( graph, state, L.blend, L.time, dt, 0.0f, ended );
		if ( L.stateTime < L.fadeLength )
		{
			Advance( graph, layer.states[L.previous], L.previousBlend, L.previousTime, dt, 0.0f, previousEnded );
		}
		L.stateTime = std::min( L.stateTime + dt, kMaxStateTime );
		FireMarkers( graph, state, before, L.time, wrapped, entered, markers );

		for ( const AnimGraphTransition& t : state.transitions )
		{
			if ( t.atEnd )
			{
				bool ready = wrapped;
				if ( state.blend == false && graph.clips[size_t( state.points[0].clip )].loops == false )
				{
					float length = graph.clips[size_t( state.points[0].clip )].length;
					ready = L.time >= length - t.xfade;
				}
				if ( ready == false )
				{
					continue;
				}
			}
			if ( t.condition.Empty() == false && EvaluateAnimExpr( t.condition, in, L.stateTime ) == 0.0f )
			{
				continue;
			}
			const AnimGraphState& next = layer.states[size_t( t.to )];
			L.previous = L.state;
			L.previousTime = L.time;
			L.previousBlend = L.blend;
			L.state = uint8_t( t.to );
			L.time = 0.0f; // the state starts over (Godot's reset; this has no memory of where it was)
			L.stateTime = 0.0f;
			L.fadeLength = t.xfade;
			L.blend = next.blend ? EvaluateAnimExpr( next.input, in, 0.0f ) : 0.0f;
			break;
		}
	}
}

} // namespace cb
