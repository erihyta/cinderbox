#pragma once

// Text, as Godot's String did it for the sheet's parser before M283: the same answers, so a sheet
// parses to the same thing in plain C++. UTF-8 bytes; every position is a byte's.

#include <cstdint>
#include <string>
#include <vector>

namespace cb::sheet
{

// Without what is at most a space (any byte <= 32) at either end (String::strip_edges).
std::string Strip( const std::string& text );

// String::is_valid_float: an optional sign, digits with at most one point (before any exponent), and
// an optional exponent (a lowercase e, a sign, digits); at least one digit before the exponent.
bool IsValidFloat( const std::string& text );
// Its value (0 for what is not one).
double ToFloat( const std::string& text );

// String::is_valid_int: an optional sign (not alone) and digits.
bool IsValidInt( const std::string& text );
int64_t ToInt( const std::string& text );

// String::split: the parts between each `delimiter` (empty ones too, unless `allowEmpty` is false).
std::vector<std::string> Split( const std::string& text, const std::string& delimiter, bool allowEmpty = true );

bool BeginsWith( const std::string& text, const std::string& start );
bool EndsWith( const std::string& text, const std::string& end );
bool Contains( const std::string& text, const std::string& part );
// Every `from` replaced by `to`.
std::string Replace( const std::string& text, const std::string& from, const std::string& to );

// The shortest text that reads back as the same float (a motions file's numbers).
std::string Num( double value );
// No tab, no line break, no #: what a motions file can carry in a field.
bool MotionTextPlain( const std::string& text );

} // namespace cb::sheet
