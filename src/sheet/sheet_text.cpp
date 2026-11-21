#include "sheet_text.h"

#include "expr.h"

#include <charconv>
#include <cstdint>

namespace cb::sheet
{

std::string Strip( const std::string& text )
{
	size_t begin = 0, end = text.size();
	while ( begin < end && uint8_t( text[begin] ) <= 32 )
	{
		++begin;
	}
	while ( end > begin && uint8_t( text[end - 1] ) <= 32 )
	{
		--end;
	}
	return text.substr( begin, end - begin );
}

bool IsValidFloat( const std::string& text )
{
	const size_t n = text.size();
	if ( n == 0 )
	{
		return false;
	}
	size_t from = 0;
	if ( text[0] == '+' || text[0] == '-' )
	{
		++from;
	}
	bool exponent = false, period = false, sign = false, exponentDigits = false, digits = false;
	for ( size_t i = from; i < n; ++i )
	{
		const char c = text[i];
		if ( c >= '0' && c <= '9' )
		{
			( exponent ? exponentDigits : digits ) = true;
		}
		else if ( digits && exponent == false && c == 'e' )
		{
			exponent = true;
		}
		else if ( period == false && exponent == false && c == '.' )
		{
			period = true;
		}
		else if ( ( c == '-' || c == '+' ) && exponent && exponentDigits == false && sign == false )
		{
			sign = true;
		}
		else
		{
			return false;
		}
	}
	return digits;
}

double ToFloat( const std::string& text )
{
	double value = 0.0;
	return expr::ParseDouble( text.c_str(), text.size(), value ) ? value : 0.0;
}

bool IsValidInt( const std::string& text )
{
	const size_t n = text.size();
	if ( n == 0 )
	{
		return false;
	}
	size_t from = 0;
	if ( n != 1 && ( text[0] == '+' || text[0] == '-' ) )
	{
		++from;
	}
	for ( size_t i = from; i < n; ++i )
	{
		if ( text[i] < '0' || text[i] > '9' )
		{
			return false;
		}
	}
	return true;
}

int64_t ToInt( const std::string& text )
{
	size_t i = 0;
	bool negative = false;
	if ( i < text.size() && ( text[i] == '-' || text[i] == '+' ) )
	{
		negative = text[i] == '-';
		++i;
	}
	int64_t value = 0;
	for ( ; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i )
	{
		if ( value > ( INT64_MAX - 9 ) / 10 )
		{
			return negative ? INT64_MIN : INT64_MAX;
		}
		value = value * 10 + ( text[i] - '0' );
	}
	return negative ? -value : value;
}

std::vector<std::string> Split( const std::string& text, const std::string& delimiter, bool allowEmpty )
{
	std::vector<std::string> out;
	size_t from = 0;
	while ( true )
	{
		const size_t at = delimiter.empty() ? std::string::npos : text.find( delimiter, from );
		const std::string part = text.substr( from, at == std::string::npos ? std::string::npos : at - from );
		if ( allowEmpty || part.empty() == false )
		{
			out.push_back( part );
		}
		if ( at == std::string::npos )
		{
			break;
		}
		from = at + delimiter.size();
	}
	return out;
}

bool BeginsWith( const std::string& text, const std::string& start )
{
	return text.size() >= start.size() && text.compare( 0, start.size(), start ) == 0;
}

bool EndsWith( const std::string& text, const std::string& end )
{
	return text.size() >= end.size() && text.compare( text.size() - end.size(), end.size(), end ) == 0;
}

bool Contains( const std::string& text, const std::string& part )
{
	return text.find( part ) != std::string::npos;
}

std::string Replace( const std::string& text, const std::string& from, const std::string& to )
{
	if ( from.empty() )
	{
		return text;
	}
	std::string out;
	size_t at = 0;
	while ( true )
	{
		const size_t found = text.find( from, at );
		if ( found == std::string::npos )
		{
			out += text.substr( at );
			return out;
		}
		out += text.substr( at, found - at ) + to;
		at = found + from.size();
	}
}

std::string Num( double value )
{
	char buffer[32];
	auto end = std::to_chars( buffer, buffer + sizeof( buffer ), float( value ) ).ptr;
	return std::string( buffer, end );
}

bool MotionTextPlain( const std::string& text )
{
	return Contains( text, "\t" ) == false && Contains( text, "\n" ) == false && Contains( text, "#" ) == false;
}

} // namespace cb::sheet
