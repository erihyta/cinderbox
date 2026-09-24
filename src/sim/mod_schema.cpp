#include "mod_schema.h"

#include <algorithm>

namespace cb
{

namespace
{

constexpr uint32_t kSchemaMagic = 0x3442434Du; // 'MCB4': 2 workshop items, 3 the character, 4 layers and stances

void PutU8( std::vector<uint8_t>& out, uint8_t v )
{
	out.push_back( v );
}

void PutString( std::vector<uint8_t>& out, const std::string& s )
{
	size_t n = std::min( s.size(), kMaxSchemaName );
	out.push_back( uint8_t( n ) );
	out.insert( out.end(), s.begin(), s.begin() + std::ptrdiff_t( n ) );
}

struct Reader
{
	const uint8_t* data;
	size_t size;
	size_t at = 0;
	bool ok = true;

	uint8_t U8()
	{
		if ( at + 1 > size )
		{
			ok = false;
			return 0;
		}
		return data[at++];
	}

	std::string String()
	{
		uint8_t n = U8();
		if ( n > kMaxSchemaName || at + n > size )
		{
			ok = false;
			return {};
		}
		std::string s( reinterpret_cast<const char*>( data + at ), n );
		at += n;
		return s;
	}
};

} // namespace

const BoardField* ModSchema::FindField( const std::string& name ) const
{
	for ( const BoardField& f : fields )
	{
		if ( f.name == name )
		{
			return &f;
		}
	}
	return nullptr;
}

int ModSchema::FindLayer( const std::string& name ) const
{
	for ( size_t i = 0; i < layers.size(); ++i )
	{
		if ( layers[i] == name )
		{
			return int( i );
		}
	}
	return -1;
}

int ModSchema::FindStance( const std::string& name ) const
{
	for ( size_t i = 0; i < stances.size(); ++i )
	{
		if ( stances[i] == name )
		{
			return int( i );
		}
	}
	return -1;
}

int ModSchema::FindEvent( const std::string& name ) const
{
	for ( size_t i = 0; i < events.size(); ++i )
	{
		if ( events[i] == name )
		{
			return int( i );
		}
	}
	return -1;
}

const ModAction* ModSchema::FindAction( const std::string& name ) const
{
	for ( const ModAction& a : actions )
	{
		if ( a.name == name )
		{
			return &a;
		}
	}
	return nullptr;
}

uint16_t ModSchema::ActionMask( const std::string& name ) const
{
	const ModAction* a = FindAction( name );
	return a != nullptr ? uint16_t( 1u << a->bit ) : 0;
}

void EncodeSchema( const ModSchema& schema, std::vector<uint8_t>& out )
{
	out.clear();
	for ( int i = 0; i < 4; ++i )
	{
		PutU8( out, uint8_t( kSchemaMagic >> ( 8 * i ) ) );
	}
	PutU8( out, uint8_t( std::min<size_t>( schema.mods.size(), 255 ) ) );
	for ( size_t i = 0; i < schema.mods.size() && i < 255; ++i )
	{
		PutString( out, schema.mods[i] );
	}
	PutU8( out, uint8_t( std::min<size_t>( schema.fields.size(), 255 ) ) );
	for ( size_t i = 0; i < schema.fields.size() && i < 255; ++i )
	{
		const BoardField& f = schema.fields[i];
		PutString( out, f.name );
		PutU8( out, uint8_t( f.type ) );
		PutU8( out, uint8_t( f.scope ) );
		PutU8( out, f.slot );
	}
	PutU8( out, uint8_t( std::min<size_t>( schema.events.size(), 255 ) ) );
	for ( size_t i = 0; i < schema.events.size() && i < 255; ++i )
	{
		PutString( out, schema.events[i] );
	}
	PutU8( out, uint8_t( std::min<size_t>( schema.actions.size(), size_t( kMaxActions ) ) ) );
	for ( size_t i = 0; i < schema.actions.size() && i < size_t( kMaxActions ); ++i )
	{
		const ModAction& a = schema.actions[i];
		PutString( out, a.name );
		PutU8( out, a.bit );
		PutString( out, a.key );
	}
	PutU8( out, uint8_t( std::min<size_t>( schema.items.size(), 255 ) ) );
	for ( size_t i = 0; i < schema.items.size() && i < 255; ++i )
	{
		PutString( out, schema.items[i].mod );
		PutString( out, schema.items[i].sha256 );
	}
	PutString( out, schema.character );
	PutU8( out, uint8_t( std::min<size_t>( schema.layers.size(), size_t( kMaxAnimLayers ) ) ) );
	for ( size_t i = 0; i < schema.layers.size() && i < size_t( kMaxAnimLayers ); ++i )
	{
		PutString( out, schema.layers[i] );
	}
	PutU8( out, uint8_t( std::min<size_t>( schema.stances.size(), size_t( kMaxStances ) ) ) );
	for ( size_t i = 0; i < schema.stances.size() && i < size_t( kMaxStances ); ++i )
	{
		PutString( out, schema.stances[i] );
	}
}

bool IsSha256( const std::string& hex )
{
	if ( hex.size() != 64 )
	{
		return false;
	}
	for ( char c : hex )
	{
		if ( ( c < '0' || c > '9' ) && ( c < 'a' || c > 'f' ) )
		{
			return false;
		}
	}
	return true;
}

bool DecodeSchema( const uint8_t* data, size_t size, ModSchema& out )
{
	out = ModSchema{};
	if ( size == 0 )
	{
		return true; // a server without mods
	}
	Reader r{ data, size };
	uint32_t magic = 0;
	for ( int i = 0; i < 4; ++i )
	{
		magic |= uint32_t( r.U8() ) << ( 8 * i );
	}
	if ( magic != kSchemaMagic )
	{
		return false;
	}

	uint8_t mods = r.U8();
	for ( uint8_t i = 0; i < mods && r.ok; ++i )
	{
		out.mods.push_back( r.String() );
	}
	uint8_t fields = r.U8();
	for ( uint8_t i = 0; i < fields && r.ok; ++i )
	{
		BoardField f;
		f.name = r.String();
		uint8_t type = r.U8();
		uint8_t scope = r.U8();
		f.slot = r.U8();
		if ( type > uint8_t( BoardType::Bool ) || scope > uint8_t( BoardScope::Global ) || f.slot >= kBoardSlots )
		{
			return false;
		}
		f.type = BoardType( type );
		f.scope = BoardScope( scope );
		out.fields.push_back( std::move( f ) );
	}
	uint8_t events = r.U8();
	for ( uint8_t i = 0; i < events && r.ok; ++i )
	{
		out.events.push_back( r.String() );
	}
	uint8_t actions = r.U8();
	if ( actions > kMaxActions )
	{
		return false;
	}
	for ( uint8_t i = 0; i < actions && r.ok; ++i )
	{
		ModAction a;
		a.name = r.String();
		a.bit = r.U8();
		a.key = r.String();
		if ( a.bit >= kMaxActions )
		{
			return false;
		}
		out.actions.push_back( std::move( a ) );
	}
	uint8_t items = r.U8();
	for ( uint8_t i = 0; i < items && r.ok; ++i )
	{
		ModItem item;
		item.mod = r.String();
		item.sha256 = r.String();
		if ( IsSha256( item.sha256 ) == false || item.mod.empty() )
		{
			return false;
		}
		out.items.push_back( std::move( item ) );
	}
	out.character = r.String();
	if ( out.character.empty() == false &&
		 std::none_of( out.items.begin(), out.items.end(), [&]( const ModItem& i ) { return i.mod == out.character; } ) )
	{
		return false; // the character always comes as one of the items
	}
	uint8_t layers = r.U8();
	if ( layers > kMaxAnimLayers )
	{
		return false;
	}
	for ( uint8_t i = 0; i < layers && r.ok; ++i )
	{
		out.layers.push_back( r.String() );
	}
	uint8_t stances = r.U8();
	if ( stances > kMaxStances )
	{
		return false;
	}
	for ( uint8_t i = 0; i < stances && r.ok; ++i )
	{
		out.stances.push_back( r.String() );
	}
	return r.ok && r.at == size;
}

} // namespace cb
