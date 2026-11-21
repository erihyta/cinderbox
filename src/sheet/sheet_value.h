#pragma once

// A value of a parsed sheet (M283): what Godot's Variant held when the sheet's parser was Godot's,
// in plain C++, so the compiler runs where Godot does not (the server, the cooker, the tests).
//
// A List and a Dict are shared when copied, as Godot's Array and Dictionary are: the parser was
// written against that (a find's statements are added to the list its step holds), and keeps it.
// Duplicate() makes a shallow copy. A Dict keeps its keys in the order they were set, as Godot's
// does. Strings (Godot's PackedStringArray) are a plain vector: copied, never shared.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cb::sheet
{

struct Float2
{
	float x = 0.0f, y = 0.0f;
};

struct Float3
{
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Color4
{
	float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

using Strings = std::vector<std::string>;

class Value;
using ListItems = std::vector<Value>;
using DictEntries = std::vector<std::pair<std::string, Value>>;

class List
{
public:
	List(); // a new one (its own storage)
	size_t size() const;
	bool empty() const
	{
		return size() == 0;
	}
	void push_back( const Value& value );
	Value& operator[]( size_t index );
	const Value& operator[]( size_t index ) const;
	List Duplicate() const;

private:
	friend class Value;
	explicit List( std::shared_ptr<ListItems> items )
		: m_items( std::move( items ) )
	{
	}
	std::shared_ptr<ListItems> m_items;
};

class Dict
{
public:
	Dict(); // a new one (its own storage)
	bool Has( const std::string& key ) const;
	// The value, or `fallback` when there is none (a copy: a List or a Dict in it is shared).
	Value Get( const std::string& key, const Value& fallback ) const;
	// The value, made (nil, at the end) when there is none.
	Value& operator[]( const std::string& key );
	const Value& At( const std::string& key ) const; // (nil when there is none)
	void Erase( const std::string& key );
	// Every key of `other` set here (overwritten when `overwrite`, else only where missing).
	void Merge( const Dict& other, bool overwrite );
	Dict Duplicate() const;
	size_t size() const;
	bool empty() const
	{
		return size() == 0;
	}
	const DictEntries& Entries() const;
	Strings Keys() const;

private:
	friend class Value;
	explicit Dict( std::shared_ptr<DictEntries> entries )
		: m_entries( std::move( entries ) )
	{
	}
	std::shared_ptr<DictEntries> m_entries;
};

class Value
{
public:
	enum class Type : uint8_t
	{
		Nil,
		Bool,
		Int,
		Float,
		String,
		Vec2,
		Vec3,
		Color,
		Strings,
		List,
		Dict,
	};

	Value() = default;
	Value( bool v );
	Value( int v );
	Value( int64_t v );
	Value( double v );
	Value( const char* v );
	Value( const std::string& v );
	Value( Float2 v );
	Value( Float3 v );
	Value( Color4 v );
	Value( const Strings& v );
	Value( const List& v );
	Value( const Dict& v );

	Type type() const
	{
		return m_type;
	}
	bool IsNil() const
	{
		return m_type == Type::Nil;
	}

	// As Godot's Variant converts: a number from a bool, an int or a float; a truth from any of them;
	// text only from text (anything else: empty).
	bool AsBool() const;
	int64_t AsInt() const;
	double AsFloat() const;
	const std::string& AsString() const;
	Float2 AsVec2() const;
	Float3 AsVec3() const;
	Color4 AsColor() const;
	const Strings& AsStrings() const;
	// The list or the dict itself (shared); an empty new one for any other kind.
	List AsList() const;
	Dict AsDict() const;

private:
	Type m_type = Type::Nil;
	bool m_bool = false;
	int64_t m_int = 0;
	double m_float = 0.0;
	std::string m_string;
	Float2 m_vec2;
	Float3 m_vec3;
	Color4 m_color;
	Strings m_strings;
	std::shared_ptr<ListItems> m_list;
	std::shared_ptr<DictEntries> m_dict;
};

} // namespace cb::sheet
