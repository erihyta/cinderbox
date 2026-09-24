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

	auto character = std::make_shared<CharacterAsset>();
	character->name = item.mod;
	std::unique_ptr<anim::AnimSet> set = anim::AnimSet::Load( read, item.mod, error, warnings );
	std::string hitboxText;
	bool haveHitboxes = set != nullptr && read( "hitboxes.cfg", hitboxText );
	mz_zip_reader_end( &zip );
	if ( set == nullptr )
	{
		error = "character " + item.mod + ": " + error;
		return nullptr;
	}
	if ( haveHitboxes == false )
	{
		error = "character " + item.mod + " has no " + folder + "hitboxes.cfg";
		return nullptr;
	}
	if ( anim::ParseHitboxes( hitboxText, character->hitboxes, error ) == false )
	{
		error = "character " + item.mod + ": " + error;
		return nullptr;
	}
	anim::BindHitboxes( character->hitboxes, *set, warnings );
	if ( character->hitboxes.boxes.empty() )
	{
		error = "character " + item.mod + " has no hitboxes on its skeleton";
		return nullptr;
	}
	character->animations = std::move( set );
	return character;
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
