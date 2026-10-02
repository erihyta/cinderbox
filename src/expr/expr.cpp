#include "expr.h"

#include <algorithm>
#include <cctype>

namespace cb::expr
{

namespace
{

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

bool IsDigit( char c )
{
	return std::isdigit( static_cast<unsigned char>( c ) ) != 0;
}

// A plain name's characters: letters, digits, '_' and '.' ("pistol.ammo").
bool IsNameChar( char c )
{
	return std::isalnum( static_cast<unsigned char>( c ) ) != 0 || c == '_' || c == '.';
}

// What a path may hold before its colon: names, the anchors and Godot's separators ("$other/Head",
// "^^", "$local@pickup.target", "../Barrel").
bool IsPathChar( char c )
{
	return IsNameChar( c ) || c == '/' || c == '$' || c == '^' || c == '@' || c == '%';
}

class Compiler
{
public:
	Compiler( const std::string& text, Program& out )
		: m_text( text ), m_out( out )
	{
	}

	bool Compile( std::string& error )
	{
		m_out.steps.clear();
		m_out.names.clear();
		m_out.text = m_text;
		Next();
		if ( m_token.kind == Token::End && m_error.empty() )
		{
			return true; // empty: always true
		}
		if ( m_error.empty() == false || Or() == false || Expect( Token::End, "" ) == false )
		{
			error = "'" + m_text + "': " + m_error;
			return false;
		}
		// Every step pushes at most one value; a depth check keeps evaluation on a fixed stack.
		int depth = 0, deepest = 0;
		for ( const Step& s : m_out.steps )
		{
			depth += ( s.op == Op::Const || s.op == Op::Name || s.op == Op::Known ) ? 1 : ( s.op == Op::Not || s.op == Op::Negate ) ? 0 : -1;
			deepest = std::max( deepest, depth );
		}
		if ( deepest > kMaxStack )
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
		if ( IsDigit( c ) || ( c == '.' && m_pos + 1 < s.size() && IsDigit( s[m_pos + 1] ) ) )
		{
			size_t start = m_pos;
			while ( m_pos < s.size() && ( IsDigit( s[m_pos] ) || s[m_pos] == '.' ) )
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
				while ( m_pos < s.size() && IsDigit( s[m_pos] ) )
				{
					++m_pos;
				}
			}
			m_token.kind = Token::Number;
			m_token.text = s.substr( start, m_pos - start );
			if ( ParseFloat( m_token.text.data(), m_token.text.size(), m_token.value ) == false )
			{
				m_token.kind = Token::Symbol; // reported as unexpected
			}
			return;
		}
		if ( std::isalpha( static_cast<unsigned char>( c ) ) || c == '_' )
		{
			size_t start = m_pos;
			while ( m_pos < s.size() && IsNameChar( s[m_pos] ) )
			{
				++m_pos;
			}
			m_token.kind = Token::Name;
			m_token.text = s.substr( start, m_pos - start );
			return;
		}
		// A path and a colon before the name: "^^:combat.dead", "$other/Head:x". "../Barrel:x" too.
		if ( c == '$' || c == '^' || c == '@' || ( c == '.' && m_pos + 1 < s.size() && s[m_pos + 1] == '.' ) )
		{
			size_t start = m_pos;
			while ( m_pos < s.size() && IsPathChar( s[m_pos] ) )
			{
				++m_pos;
			}
			size_t colon = m_pos;
			if ( colon < s.size() && s[colon] == ':' )
			{
				++m_pos;
				while ( m_pos < s.size() && std::isspace( static_cast<unsigned char>( s[m_pos] ) ) )
				{
					++m_pos;
				}
				size_t name = m_pos;
				while ( m_pos < s.size() && IsNameChar( s[m_pos] ) )
				{
					++m_pos;
				}
				if ( m_pos > name )
				{
					m_token.kind = Token::Name;
					m_token.text = s.substr( start, colon - start ) + ":" + s.substr( name, m_pos - name );
					return;
				}
				m_error = "no name after the colon of '" + s.substr( start, colon - start ) + ":'";
			}
			else
			{
				// "^^combat.health": a path without its colon would name nothing.
				std::string path = s.substr( start, colon - start );
				size_t anchor = 0;
				while ( anchor < path.size() && ( path[anchor] == '^' || path[anchor] == '$' ) )
				{
					++anchor;
				}
				m_error = "put a colon between the path and the name, like " +
						  ( path[0] == '^' && anchor < path.size() ? path.substr( 0, anchor ) + ":" + path.substr( anchor )
																   : std::string( "$other:combat.health" ) );
			}
			m_token.kind = Token::End;
			m_pos = s.size();
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

	bool Fail()
	{
		if ( m_error.empty() )
		{
			m_error = m_token.kind == Token::End ? std::string( "unexpected end" ) : "unexpected '" + m_token.text + "'";
		}
		return false;
	}

	bool Expect( Token::Kind kind, const char* text )
	{
		if ( m_error.empty() && m_token.kind == kind && ( kind == Token::End || m_token.text == text ) )
		{
			Next();
			return m_error.empty();
		}
		return Fail();
	}

	void Emit( Op op )
	{
		Step step;
		step.op = op;
		m_out.steps.push_back( step );
	}

	void EmitName( Op op, const std::string& name )
	{
		Step step;
		step.op = op;
		auto found = std::find( m_out.names.begin(), m_out.names.end(), name );
		step.name = uint16_t( found - m_out.names.begin() );
		if ( found == m_out.names.end() )
		{
			m_out.names.push_back( name );
		}
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
			Emit( Op::Or );
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
			Emit( Op::And );
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
			Emit( Op::Not );
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
		static const std::pair<const char*, Op> kOps[] = {
			{ "==", Op::Equal }, { "!=", Op::NotEqual }, { "<=", Op::LessEqual }, { ">=", Op::GreaterEqual }, { "<", Op::Less }, { ">", Op::Greater },
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
			Op op = m_token.text == "+" ? Op::Add : Op::Sub;
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
			Op op = m_token.text == "*" ? Op::Mul : Op::Div;
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
			Emit( Op::Negate );
			return true;
		}
		return Primary();
	}

	bool IsWord() const
	{
		return m_token.kind == Token::Name && m_token.text != "and" && m_token.text != "or" && m_token.text != "not";
	}

	bool Primary()
	{
		if ( m_error.empty() == false )
		{
			return false;
		}
		if ( m_token.kind == Token::Number )
		{
			Step step;
			step.op = Op::Const;
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
		if ( m_token.kind == Token::Symbol && m_token.text == "?" )
		{
			// "?name": whether the name is known.
			Next();
			if ( IsWord() == false )
			{
				return Fail();
			}
			EmitName( Op::Known, m_token.text );
			Next();
			return true;
		}
		if ( IsWord() )
		{
			if ( m_token.text == "true" || m_token.text == "false" )
			{
				Step step;
				step.op = Op::Const;
				step.value = m_token.text == "true" ? 1.0f : 0.0f;
				m_out.steps.push_back( step );
			}
			else
			{
				EmitName( Op::Name, m_token.text );
			}
			Next();
			return true;
		}
		return Fail();
	}

	const std::string& m_text;
	Program& m_out;
	size_t m_pos = 0;
	Token m_token;
	std::string m_error;
};

} // namespace

bool Compile( const std::string& text, Program& out, std::string& error )
{
	Compiler compiler( text, out );
	return compiler.Compile( error );
}

float Apply( Op op, float a, float b )
{
	switch ( op )
	{
		case Op::Add:
			return a + b;
		case Op::Sub:
			return a - b;
		case Op::Mul:
			return a * b;
		case Op::Div:
			return b != 0.0f ? a / b : 0.0f;
		case Op::Less:
			return a < b ? 1.0f : 0.0f;
		case Op::LessEqual:
			return a <= b ? 1.0f : 0.0f;
		case Op::Greater:
			return a > b ? 1.0f : 0.0f;
		case Op::GreaterEqual:
			return a >= b ? 1.0f : 0.0f;
		case Op::Equal:
			return a == b ? 1.0f : 0.0f;
		case Op::NotEqual:
			return a != b ? 1.0f : 0.0f;
		case Op::And:
			return ( a != 0.0f && b != 0.0f ) ? 1.0f : 0.0f;
		case Op::Or:
			return ( a != 0.0f || b != 0.0f ) ? 1.0f : 0.0f;
		default:
			return 0.0f;
	}
}

float Evaluate( const Program& program, const Reader& read )
{
	if ( program.steps.empty() )
	{
		return 1.0f;
	}
	float stack[kMaxStack];
	int top = 0;
	for ( const Step& s : program.steps )
	{
		switch ( s.op )
		{
			case Op::Const:
				stack[top++] = s.value;
				continue;
			case Op::Name:
			{
				float value = 0.0f;
				bool known = read && read( program.names[s.name], value );
				stack[top++] = known ? value : 0.0f;
				continue;
			}
			case Op::Known:
			{
				float value = 0.0f;
				stack[top++] = read && read( program.names[s.name], value ) ? 1.0f : 0.0f;
				continue;
			}
			case Op::Not:
				stack[top - 1] = stack[top - 1] != 0.0f ? 0.0f : 1.0f;
				continue;
			case Op::Negate:
				stack[top - 1] = -stack[top - 1];
				continue;
			default:
				break;
		}
		float b = stack[--top];
		float a = stack[top - 1];
		stack[top - 1] = Apply( s.op, a, b );
	}
	return top > 0 ? stack[top - 1] : 0.0f;
}

void SplitName( const std::string& name, std::string& path, std::string& plain )
{
	size_t colon = name.rfind( ':' );
	path = colon == std::string::npos ? std::string() : name.substr( 0, colon );
	plain = colon == std::string::npos ? name : name.substr( colon + 1 );
}

bool ParseFloat( const char* text, size_t length, float& out )
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

} // namespace cb::expr
