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
