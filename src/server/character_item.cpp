#include "character_item.h"

#include "sha256.h"

#include "miniz.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cb
{

std::shared_ptr<const CharacterAsset> BuiltInCharacter()
{
	auto character = std::make_shared<CharacterAsset>();
	auto set = anim::AnimSet::CreateProcedural();
	character->hitboxes = anim::DefaultHitboxes();
	std::string warnings;
	anim::BindHitboxes( character->hitboxes, *set, warnings );
	character->animations = std::move( set );
	return character;
}

namespace
{

// The baked files of a character (anim.cfg, the .ozz files, hitboxes.cfg), wherever they are read from.
std::shared_ptr<const CharacterAsset> LoadCharacter( const anim::FileReader& read, const std::string& name, const std::string& where,
													 std::string& error, std::string& warnings )
{
	auto character = std::make_shared<CharacterAsset>();
	character->name = name;
	std::unique_ptr<anim::AnimSet> set = anim::AnimSet::Load( read, name, error, warnings );
	if ( set == nullptr )
	{
		error = "character " + name + ": " + error;
		return nullptr;
	}
	std::string hitboxText;
	if ( read( "hitboxes.cfg", hitboxText ) == false )
	{
		error = "character " + name + " has no " + where + "hitboxes.cfg";
		return nullptr;
	}
	if ( anim::ParseHitboxes( hitboxText, character->hitboxes, error ) == false )
	{
		error = "character " + name + ": " + error;
		return nullptr;
	}
	anim::BindHitboxes( character->hitboxes, *set, warnings );
	if ( character->hitboxes.boxes.empty() )
	{
		error = "character " + name + " has no hitboxes on its skeleton";
		return nullptr;
	}
	character->animations = std::move( set );
	return character;
}

} // namespace

std::shared_ptr<const CharacterAsset> LoadCharacterItem( const std::string& zipPath, const ModItem& item, std::string& error,
													std::string& warnings )
{
	std::ifstream in( zipPath, std::ios::binary );
	if ( in.good() == false )
	{
		error = "no workshop item at " + zipPath;
		return nullptr;
	}
	std::ostringstream all;
	all << in.rdbuf();
	const std::string bytes = all.str();
	std::string sha = Sha256Hex( bytes.data(), bytes.size() );
	if ( sha != item.sha256 )
	{
		error = zipPath + " is not the item the manifest names (sha256 " + sha + ", expected " + item.sha256 + ")";
		return nullptr;
	}

	mz_zip_archive zip = {};
	if ( mz_zip_reader_init_mem( &zip, bytes.data(), bytes.size(), 0 ) == MZ_FALSE )
	{
		error = zipPath + " is not a zip";
		return nullptr;
	}
	const std::string folder = "characters/" + item.mod + "/";
	anim::FileReader read = [&]( const std::string& name, std::string& out ) {
		std::string path = folder + name;
		int index = mz_zip_reader_locate_file( &zip, path.c_str(), nullptr, 0 );
		if ( index < 0 )
		{
			return false;
		}
		size_t size = 0;
		void* data = mz_zip_reader_extract_to_heap( &zip, mz_uint( index ), &size, 0 );
		if ( data == nullptr )
		{
			return false;
		}
		out.assign( static_cast<const char*>( data ), size );
		mz_free( data );
		return true;
	};

	auto character = LoadCharacter( read, item.mod, folder, error, warnings );
	mz_zip_reader_end( &zip );
	return character;
}

std::shared_ptr<const anim::AnimSet> LoadAnimPackItem( const std::string& zipPath, const ModItem& item, const std::string& pack,
													   std::string& error, std::string& warnings )
{
	std::ifstream in( zipPath, std::ios::binary );
	if ( in.good() == false )
	{
		error = "no workshop item at " + zipPath;
		return nullptr;
	}
	std::ostringstream all;
	all << in.rdbuf();
	const std::string bytes = all.str();
	if ( Sha256Hex( bytes.data(), bytes.size() ) != item.sha256 )
	{
		error = zipPath + " is not the item the manifest names";
		return nullptr;
	}
	mz_zip_archive zip = {};
	if ( mz_zip_reader_init_mem( &zip, bytes.data(), bytes.size(), 0 ) == MZ_FALSE )
	{
		error = zipPath + " is not a zip";
		return nullptr;
	}
	const std::string folder = "anim/" + pack + "/";
	anim::FileReader read = [&]( const std::string& name, std::string& out ) {
		std::string path = folder + name;
		int index = mz_zip_reader_locate_file( &zip, path.c_str(), nullptr, 0 );
		if ( index < 0 )
		{
			return false;
		}
		size_t size = 0;
		void* data = mz_zip_reader_extract_to_heap( &zip, mz_uint( index ), &size, 0 );
		if ( data == nullptr )
		{
			return false;
		}
		out.assign( static_cast<const char*>( data ), size );
		mz_free( data );
		return true;
	};
	std::shared_ptr<const anim::AnimSet> set = anim::AnimSet::Load( read, pack, error, warnings );
	mz_zip_reader_end( &zip );
	return set;
}

std::shared_ptr<const anim::AnimSet> LoadAnimPackFolder( const std::string& dir, const std::string& pack, std::string& error,
														 std::string& warnings )
{
	return anim::AnimSet::Load( anim::DiskReader( dir + "/anim/" + pack ), pack, error, warnings );
}

std::shared_ptr<const CharacterAsset> LoadCharacterFolder( const std::string& dir, const std::string& name, std::string& error,
														   std::string& warnings )
{
	return LoadCharacter( anim::DiskReader( dir ), name, dir + "/", error, warnings );
}

bool ParseItemShape( const std::string& text, ItemShape& out, std::string& error, ItemProperties* properties )
{
	ItemShape shape;
	ItemProperties found;
	bool haveShape = false;
	bool haveHalf = false;
	std::istringstream lines( text );
	std::string line;
	while ( std::getline( lines, line ) )
	{
		std::istringstream words( line );
		std::string key;
		if ( !( words >> key ) || key[0] == '#' )
		{
			continue;
		}
		if ( key == "shape" )
		{
			std::string kind;
			words >> kind;
			if ( kind != "box" && kind != "sphere" )
			{
				error = "shape must be box or sphere, not \"" + kind + "\"";
				return false;
			}
			shape.kind = kind == "sphere" ? 1 : 0;
			haveShape = true;
		}
		else if ( key == "half" || key == "center" )
		{
			Float3 v;
			if ( !( words >> v.x >> v.y >> v.z ) )
			{
				error = key + " needs three numbers";
				return false;
			}
			( key == "half" ? shape.half : shape.center ) = v;
			haveHalf |= key == "half";
		}
		else if ( key == "mass" )
		{
			if ( !( words >> shape.mass ) )
			{
				error = "mass needs a number";
				return false;
			}
		}
		else if ( key == "grip" )
		{
			// grip <x y z> <qx qy qz qw> <1 when the hand takes the rotation>
			float g[7];
			int align = 0;
			if ( !( words >> g[0] >> g[1] >> g[2] >> g[3] >> g[4] >> g[5] >> g[6] >> align ) )
			{
				error = "grip needs a position, a rotation (x y z w) and 0 or 1";
				return false;
			}
			float turn = g[3] * g[3] + g[4] * g[4] + g[5] * g[5] + g[6] * g[6];
			for ( int k = 0; k < 3; ++k )
			{
				if ( std::isfinite( g[k] ) == false || g[k] < -2.0f || g[k] > 2.0f )
				{
					error = "the grip must be within 2 m of the carrying hand";
					return false;
				}
			}
			if ( std::isfinite( turn ) == false || turn < 0.9f || turn > 1.1f )
			{
				error = "the grip's rotation is not a rotation";
				return false;
			}
			shape.grip = align != 0 ? 2 : 1;
			shape.gripPosition = { g[0], g[1], g[2] };
			for ( int k = 0; k < 4; ++k )
			{
				shape.gripRotation[k] = g[3 + k];
			}
		}
		else if ( key == "property" )
		{
			std::string name;
			float value = 0.0f;
			if ( !( words >> name >> value ) || std::isfinite( value ) == false )
			{
				error = "a property needs a name and a number";
				return false;
			}
			found[name] = value;
		}
	}
	auto sane = []( float f, float lo, float hi ) { return std::isfinite( f ) && f >= lo && f <= hi; };
	if ( haveShape == false || haveHalf == false )
	{
		error = "needs a shape and its half extents";
		return false;
	}
	for ( float h : { shape.half.x, shape.half.y, shape.half.z } )
	{
		if ( sane( h, 0.005f, 4.0f ) == false )
		{
			error = "half extents must be between 0.005 and 4 m";
			return false;
		}
	}
	for ( float c : { shape.center.x, shape.center.y, shape.center.z } )
	{
		if ( sane( c, -4.0f, 4.0f ) == false )
		{
			error = "the centre must be within 4 m of the grip";
			return false;
		}
	}
	if ( sane( shape.mass, 0.01f, 1000.0f ) == false )
	{
		error = "mass must be between 0.01 and 1000 kg";
		return false;
	}
	out = shape;
	if ( properties != nullptr )
	{
		*properties = found;
	}
	return true;
}

bool LoadItemShapeItem( const std::string& zipPath, const ModItem& item, const std::string& kind, ItemShape& out, std::string& error,
						ItemProperties* properties )
{
	std::ifstream in( zipPath, std::ios::binary );
	if ( in.good() == false )
	{
		return false;
	}
	std::ostringstream all;
	all << in.rdbuf();
	const std::string bytes = all.str();
	if ( Sha256Hex( bytes.data(), bytes.size() ) != item.sha256 )
	{
		error = zipPath + " is not the item the manifest names";
		return false;
	}
	mz_zip_archive zip = {};
	if ( mz_zip_reader_init_mem( &zip, bytes.data(), bytes.size(), 0 ) == MZ_FALSE )
	{
		error = zipPath + " is not a zip";
		return false;
	}
	std::string path = "items/" + kind + ".cfg";
	int index = mz_zip_reader_locate_file( &zip, path.c_str(), nullptr, 0 );
	size_t size = 0;
	void* data = index >= 0 ? mz_zip_reader_extract_to_heap( &zip, mz_uint( index ), &size, 0 ) : nullptr;
	bool ok = false;
	if ( data != nullptr )
	{
		ok = ParseItemShape( std::string( static_cast<const char*>( data ), size ), out, error, properties );
		mz_free( data );
	}
	mz_zip_reader_end( &zip );
	return ok;
}

bool LoadItemShapeFolder( const std::string& dir, const std::string& kind, ItemShape& out, std::string& error, ItemProperties* properties )
{
	std::ifstream in( dir + "/items/" + kind + ".cfg", std::ios::binary );
	if ( in.good() == false )
	{
		return false;
	}
	std::ostringstream all;
	all << in.rdbuf();
	return ParseItemShape( all.str(), out, error, properties );
}

std::string DefaultWorkshopDir()
{
	// Godot's user:// for the project named "Cinderbox" (godot/project.godot).
	const char* suffix = "/Godot/app_userdata/Cinderbox/workshop";
#if defined( _WIN32 )
	const char* appData = std::getenv( "APPDATA" );
	return appData != nullptr ? std::string( appData ) + suffix : std::string();
#elif defined( __APPLE__ )
	const char* home = std::getenv( "HOME" );
	return home != nullptr ? std::string( home ) + "/Library/Application Support" + suffix : std::string();
#else
	const char* data = std::getenv( "XDG_DATA_HOME" );
	if ( data != nullptr && data[0] != '\0' )
	{
		return std::string( data ) + "/godot/app_userdata/Cinderbox/workshop";
	}
	const char* home = std::getenv( "HOME" );
	return home != nullptr ? std::string( home ) + "/.local/share/godot/app_userdata/Cinderbox/workshop" : std::string();
#endif
}

} // namespace cb
