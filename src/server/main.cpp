// Headless dedicated server.
//
//   cb_server [--port N] [--tick-rate HZ] [--seed N] [--substeps N]
//             [--prop-lifetime SEC] [--props-per-player N] [--props-global N]
//             [--map FILE.cbmap | --map NAME] [--record FILE] [--record-view FILE [--view-rate HZ] [--view-compact]] [--mods A,B | --mods none] [--list-mods]
//             [--dump-names FILE] [--exit-when-empty SEC] [--items DIR] [--mod-option NAME=VALUE]... [--move NAME=VALUE]...
//             [--character NAME [--workshop DIR]] [--quiet]
//             [--fake-players N [--fake-seed N] [--fake-radius M] [--fake-rtt MS] [--fake-fight]] [--bench SEC]
//
// --fake-players N (the crowd bench, M251): the first N slots are played by bot brains in the
//   server, and each is a client that is sent what a player would be and acknowledges it a round
//   trip (--fake-rtt, 100 ms) later; nothing goes on the network, it is counted. --fake-radius M:
//   they walk back toward the spawn once M metres from it (a crowd). --fake-fight: they use what is
//   in their hands on whoever is within reach. --fake-seed: the same seed plays the same session.
// --bench SEC: the server runs as fast as it can on a clock of its own for SEC seconds of game,
//   prints what the fake players would have been sent (per player and per second, by part) and the
//   tick's parts, and stops.
//
// --map NAME: maps/NAME.cbmap, next to this executable, in the working folder, or in a checkout the
// project's godot/maps.
// --move NAME=VALUE: how players move (sim/move_params.h): walk_speed, sprint_speed, accelerate,
// friction, stop_speed, air_control, gravity, jump_speed, turn_rate, max_fall. A character that says
// how it moves (its anim.cfg, "move.walk_speed") wins over these; a mod's SetMove wins over both.
//
// Every gameplay mod compiled in (server_mods/) runs unless --mods names a subset. Mods with a look
// need their workshop item: its SHA-256 is read from <items dir>/<mod>.item (default: items/ next
// to this executable) and announced to clients, who must have that exact item to join.
//
// --look-source DIR: with --dump-names, the folder of the mods' client projects (server_mods): the
//   names a mod takes from its look are written too.
// --dump-names FILE: writes what every mod compiled into this server declares (its fields, events,
// actions, item kinds, stances, layers, sockets, animation packs and motion sets) and exits. The
// editor reads the file (res://cinderbox_names.cfg in a mod's project): names are completed
// while typing and one nobody declares is said, and publishing a look checks against it.
//
// --physics-threads N: the physics step runs on N more threads than the server's own (default: a
//   quarter of the machine's threads, at most 3; 0: none).
//   What is simulated is the same with any number.
// --exit-when-empty SEC: the server stops by itself once nobody has been connected for SEC seconds
// (from its start too: one nobody joins does not stay). For a server a game starts to try
// something (godot/host.gd): it cannot be left running by accident.
//
// --character NAME: everyone plays as NAME (default: ual_mannequin if this build has it, else
// mannequin, the game's own character).
//   - A workshop character (NAME has an item manifest) is announced like a mod's item, and the server
//     reads its skeleton, clips and hitboxes from the same file players have:
//     <workshop>/NAME/<sha256>.zip (default workshop: the game's user folder, where
//     tools/publish_mod.ps1 installs items).
//   - A character that ships with the game is read from characters/NAME/ next to this executable
//     (the build copies godot/characters/NAME/ there); players have it in their base game.
//   - none: the procedural placeholder rig.

#include "game_server.h"
#include "registry.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined( _WIN32 )
#include <windows.h>
#include <timeapi.h>
#endif

namespace
{

void Usage()
{
	std::printf( "usage: cb_server [--port N] [--tick-rate HZ] [--seed N] [--substeps N]\n"
				 "                 [--prop-lifetime SEC] [--props-per-player N] [--props-global N]\n"
				 "                 [--map FILE.cbmap | --map NAME] [--record FILE] [--record-view FILE [--view-rate HZ] [--view-compact]] [--mods A,B | --mods none] [--list-mods]\n"
				 "                 [--dump-names FILE] [--exit-when-empty SEC] [--items DIR] [--mod-option NAME=VALUE]... [--move NAME=VALUE]...\n"
				 "                 [--physics-threads N] [--net-model hybrid|inputs] [--every-character] [--state-budget N] [--client-budget KBITS] [--state-threads N] [--lag-compensation MS]\n"
				 "                 [--character NAME [--workshop DIR]] [--quiet]\n"
				 "                 [--fake-players N [--fake-seed N] [--fake-radius M] [--fake-rtt MS] [--fake-fight]] [--bench SEC]\n" );
}

// <dir>/<mod>.item: "sha256=<64 hex digits>" (written by tools/publish_mod.ps1).
bool ReadItem( const std::string& dir, const std::string& mod, cb::ModItem& out )
{
	std::ifstream in( std::filesystem::path( dir ) / ( mod + ".item" ) );
	std::string line;
	while ( std::getline( in, line ) )
	{
		while ( line.empty() == false && ( line.back() == '\r' || line.back() == ' ' ) )
		{
			line.pop_back();
		}
		if ( line.rfind( "sha256=", 0 ) == 0 && cb::IsSha256( line.substr( 7 ) ) )
		{
			out.mod = mod;
			out.sha256 = line.substr( 7 );
			return true;
		}
	}
	return false;
}

std::vector<std::string> SplitList( const std::string& list )
{
	std::vector<std::string> out;
	size_t start = 0;
	while ( start <= list.size() )
	{
		size_t comma = list.find( ',', start );
		std::string item = list.substr( start, comma == std::string::npos ? std::string::npos : comma - start );
		if ( item.empty() == false )
		{
			out.push_back( item );
		}
		if ( comma == std::string::npos )
		{
			break;
		}
		start = comma + 1;
	}
	return out;
}

// The names every compiled mod declares, one a line, for the editor and for publishing a look:
//
//   cinderbox_names	1
//   mod	dash
//   field	dash.charges	int	entity
//   event	dash.started
//   action	dash	V
//   item	pistol.gun
//   stance / layer / socket / pack / motions	<name>
//
// Every mod, the ones switched off by default too: a look may be written for any of them.
// `lookSource`: the folder the mods' client projects are in (server_mods): what a mod takes from its
// look (its items, the actions their motions bring) is read from the scenes' baked files there, so
// the editor knows those names too. Empty: only what the code declares.
bool DumpNames( const char* path, const std::string& lookSource )
{
	cb::mods::Declarations declarations;
	if ( lookSource.empty() == false )
	{
		declarations.SetLookItems( [lookSource]( const std::string& mod ) {
			std::vector<cb::LookItem> items;
			cb::ListLookItemsFolder( ( std::filesystem::path( lookSource ) / mod / "client" ).string(), items );
			return items;
		} );
	}
	std::string text = "cinderbox_names\t1\n";
	for ( const cb::mods::ModInfo& info : cb::mods::CompiledMods() )
	{
		std::unique_ptr<cb::mods::ServerMod> mod = cb::mods::CreateMod( info.name );
		if ( mod != nullptr )
		{
			declarations.BeginMod( mod->Name() );
			mod->Declare( declarations );
			text += std::string( "mod\t" ) + info.name + "\n";
		}
	}
	// (Two mods that cannot run together may disagree here: what each declared is still a name.)
	const cb::ModSchema& schema = declarations.Schema();
	static const char* const kTypes[] = { "int", "float", "bool" };
	static const char* const kScopes[] = { "entity", "global", "private" };
	for ( const cb::BoardField& f : schema.fields )
	{
		text += "field\t" + f.name + "\t" + kTypes[int( f.type ) % 3] + "\t" + kScopes[int( f.scope ) % 3] + "\n";
	}
	for ( const std::string& name : schema.events )
	{
		text += "event\t" + name + "\n";
	}
	for ( const cb::ModAction& a : schema.actions )
	{
		text += "action\t" + a.name + "\t" + a.key + "\n";
	}
	auto list = [&]( const char* what, const std::vector<std::string>& names ) {
		for ( const std::string& name : names )
		{
			text += std::string( what ) + "\t" + name + "\n";
		}
	};
	list( "item", schema.itemKinds );
	list( "intent", schema.intents );
	list( "stance", schema.stances );
	list( "layer", schema.layers );
	list( "socket", schema.sockets );
	for ( const cb::AnimPackInfo& pack : schema.animPacks )
	{
		text += "pack\t" + pack.name + "\n";
	}
	for ( const cb::MotionSetInfo& set : schema.motionSets )
	{
		text += "motions\t" + set.name + "\n";
	}
	// Written only when it changed, so nothing that watches the file is woken for nothing.
	{
		std::ifstream in( path, std::ios::binary );
		std::string had( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
		if ( in && had == text )
		{
			return true;
		}
	}
	std::ofstream out( path, std::ios::binary | std::ios::trunc );
	out << text;
	if ( !out )
	{
		std::printf( "cannot write %s\n", path );
		return false;
	}
	std::printf( "wrote %s: %zu fields, %zu events, %zu actions, %zu item kinds\n", path, schema.fields.size(), schema.events.size(),
				 schema.actions.size(), schema.itemKinds.size() );
	return true;
}

int g_physicsThreads = -1; // --physics-threads (-1: not given)

bool ParseArgs( int argc, char** argv, cb::ServerOptions& o, std::vector<std::string>& mods, bool& listMods,
				std::string& itemsDir, std::string& character, std::string& workshopDir )
{
	for ( int i = 1; i < argc; ++i )
	{
		std::string arg = argv[i];
		if ( arg == "--help" || arg == "-h" )
		{
			return false;
		}
		if ( arg == "--quiet" )
		{
			o.verbose = false;
			continue;
		}
		if ( arg == "--net-model" && i + 1 < argc )
		{
			// How clients' worlds are kept (ROADMAP step 3): every client simulating the whole world
			// from everyone's inputs, or the hybrid (the physics from the server's events, the characters
			// the server's, the client's own predicted).
			const std::string model = argv[++i];
			if ( model != "inputs" && model != "hybrid" )
			{
				std::printf( "--net-model is inputs or hybrid, not %s\n", model.c_str() );
				return false;
			}
			o.netModel = model == "hybrid" ? cb::net::NetModel::Hybrid : cb::net::NetModel::Inputs;
			continue;
		}
		if ( arg == "--every-character" )
		{
			// The hybrid: every client sent every character in every state, not by how far they are
			// (M250, ServerOptions::statesByDistance).
			o.statesByDistance = false;
			continue;
		}
		if ( arg == "--lag-compensation" && i + 1 < argc )
		{
			// The hybrid: how far back (ms) a player's hits may take the others, as it saw them (0: none).
			o.lagCompensationMs = uint32_t( std::max( 0, std::atoi( argv[++i] ) ) );
			continue;
		}
		if ( arg == "--state-budget" && i + 1 < argc )
		{
			// The most entities a client is sent in one state by distance (ServerOptions::stateBudget).
			o.stateBudget = uint32_t( std::max( 1, std::atoi( argv[++i] ) ) );
			continue;
		}
		if ( arg == "--client-budget" && i + 1 < argc )
		{
			// What the others' updates may take of a client's states, kbit/s (M257; 0: no limit).
			o.clientBudgetKbits = uint32_t( std::max( 0, std::atoi( argv[++i] ) ) );
			continue;
		}
		if ( arg == "--record" && i + 1 < argc )
		{
			o.recordPath = argv[++i];
			continue;
		}
		if ( arg == "--record-view" && i + 1 < argc )
		{
			o.recordViewPath = argv[++i];
			continue;
		}
		if ( arg == "--view-rate" && i + 1 < argc )
		{
			o.recordViewRate = uint32_t( std::max( 0, std::atoi( argv[++i] ) ) );
			continue;
		}
		if ( arg == "--view-compact" )
		{
			o.recordViewCompact = true;
			continue;
		}
		if ( arg == "--map" && i + 1 < argc )
		{
			o.mapPath = argv[++i];
			continue;
		}
		if ( arg == "--mods" && i + 1 < argc )
		{
			std::string list = argv[++i];
			mods = list == "none" ? std::vector<std::string>{} : SplitList( list );
			continue;
		}
		if ( arg == "--list-mods" )
		{
			listMods = true;
			continue;
		}
		if ( ( arg == "--dump-names" || arg == "--look-source" ) && i + 1 < argc )
		{
			++i; // (handled before anything starts)
			continue;
		}
		if ( ( arg == "--exit-when-empty" || arg == "--bench" ) && i + 1 < argc )
		{
			++i; // (read by main)
			continue;
		}
		if ( arg == "--fake-players" && i + 1 < argc )
		{
			o.fakePlayers = uint32_t( std::clamp( std::atoi( argv[++i] ), 0, cb::kMaxPlayers ) );
			continue;
		}
		if ( arg == "--fake-seed" && i + 1 < argc )
		{
			o.fakeSeed = std::strtoull( argv[++i], nullptr, 10 );
			continue;
		}
		if ( arg == "--fake-radius" && i + 1 < argc )
		{
			o.fakeRadius = std::max( 0.0f, float( std::atof( argv[++i] ) ) );
			continue;
		}
		if ( arg == "--fake-rtt" && i + 1 < argc )
		{
			o.fakeRoundTripMs = uint32_t( std::clamp( std::atoi( argv[++i] ), 0, 2000 ) );
			continue;
		}
		if ( arg == "--fake-fight" )
		{
			o.fakeFight = true;
			continue;
		}
		if ( arg == "--state-threads" && i + 1 < argc )
		{
			// Threads besides the server's own that encode each client's state by distance (0: none).
			o.stateThreads = std::max( 0, std::atoi( argv[++i] ) );
			continue;
		}
		if ( arg == "--physics-threads" && i + 1 < argc )
		{
			// Threads for the physics step besides the server's own (0: none).
			g_physicsThreads = std::atoi( argv[++i] );
			continue;
		}
		if ( arg == "--items" && i + 1 < argc )
		{
			itemsDir = argv[++i];
			continue;
		}
		if ( arg == "--character" && i + 1 < argc )
		{
			character = argv[++i];
			continue;
		}
		if ( arg == "--workshop" && i + 1 < argc )
		{
			workshopDir = argv[++i];
			continue;
		}
		if ( arg == "--mod-option" && i + 1 < argc )
		{
			std::string kv = argv[++i];
			size_t eq = kv.find( '=' );
			if ( eq == std::string::npos )
			{
				std::printf( "--mod-option wants name=value, got %s\n", kv.c_str() );
				return false;
			}
			o.modOptions[kv.substr( 0, eq )] = kv.substr( eq + 1 );
			continue;
		}
		if ( arg == "--move" && i + 1 < argc )
		{
			std::string kv = argv[++i];
			size_t eq = kv.find( '=' );
			int param = eq == std::string::npos ? -1 : cb::MoveParamByName( kv.substr( 0, eq ).c_str() );
			const char* number = eq == std::string::npos ? "" : kv.c_str() + eq + 1;
			char* end = nullptr;
			float value = std::strtof( number, &end );
			if ( param < 0 || end == number || *end != '\0' )
			{
				std::printf( "--move wants name=value, got %s. Names:", kv.c_str() );
				for ( int p = 0; p < cb::kMoveParams; ++p )
				{
					std::printf( " %s", cb::MoveParamInfoOf( p ).name );
				}
				std::printf( "\n" );
				return false;
			}
			const cb::MoveParamInfo& info = cb::MoveParamInfoOf( param );
			if ( ( value >= info.min && value <= info.max ) == false )
			{
				std::printf( "--move %s must be in [%g, %g]\n", info.name, double( info.min ), double( info.max ) );
				return false;
			}
			o.config.move.values[param] = value;
			continue;
		}
		if ( i + 1 >= argc )
		{
			std::printf( "missing value for %s\n", arg.c_str() );
			return false;
		}
		unsigned long long v = std::strtoull( argv[++i], nullptr, 10 );
		if ( arg == "--port" )
			o.port = uint16_t( v );
		else if ( arg == "--tick-rate" )
			o.config.tickRate = uint32_t( v );
		else if ( arg == "--seed" )
			o.config.seed = v;
		else if ( arg == "--substeps" )
			o.config.subSteps = uint32_t( v );
		else if ( arg == "--prop-lifetime" )
			o.config.propLifetimeSeconds = uint32_t( v );
		else if ( arg == "--props-per-player" )
			o.config.propsPerPlayer = uint32_t( v );
		else if ( arg == "--props-global" )
			o.config.propsGlobal = uint32_t( v );
		else
		{
			std::printf( "unknown option %s\n", arg.c_str() );
			return false;
		}
	}
	if ( o.config.tickRate < 10 || o.config.tickRate > 240 )
	{
		std::printf( "--tick-rate must be in [10, 240]\n" );
		return false;
	}
	if ( o.config.subSteps < 1 || o.config.subSteps > 16 )
	{
		std::printf( "--substeps must be in [1, 16]\n" );
		return false;
	}
	return true;
}

} // namespace

int main( int argc, char** argv )
{
	cb::ServerOptions options;
	// The server's default is the hybrid (since 2026-11-01; ServerOptions' own stays inputs, which the tests
	// that build a server in place are written for).
	options.netModel = cb::net::NetModel::Hybrid;
	// Threads that encode each client's state by distance: a quarter of the machine's, at most 8.
	options.stateThreads = std::min( 8, int( std::thread::hardware_concurrency() / 4 ) );
	std::vector<std::string> modNames;
	for ( const cb::mods::ModInfo& info : cb::mods::CompiledMods() )
	{
		// A disabled mod (a "disabled" file in its folder) runs only when --mods names it.
		if ( info.onByDefault )
		{
			modNames.push_back( info.name );
		}
	}
	bool listMods = false;
	std::string itemsDir = ( std::filesystem::absolute( argv[0] ).parent_path() / "items" ).string();
	const std::filesystem::path exeDir = std::filesystem::absolute( argv[0] ).parent_path();
	// The default character: the first the game ships (ual_mannequin exists only where the paid
	// animation pack was built locally; it is never in the repository).
	// Where the game's own characters are: in a checkout, the project's folder itself, where saving
	// a character's scene bakes it (so a restart is all an edit takes); anywhere else, the copies
	// next to the executable.
	std::filesystem::path charactersDir = exeDir / "characters";
#if defined( CB_CHARACTERS_SOURCE_DIR )
	if ( std::filesystem::exists( std::filesystem::path( CB_CHARACTERS_SOURCE_DIR ) ) )
	{
		charactersDir = CB_CHARACTERS_SOURCE_DIR;
	}
#endif
	std::string characterName = "none";
	for ( const char* shipped : { "ual_mannequin", "mannequin" } )
	{
		if ( std::filesystem::exists( charactersDir / shipped / "anim.cfg" ) )
		{
			characterName = shipped;
			break;
		}
	}
	std::string workshopDir = cb::DefaultWorkshopDir();
	if ( ParseArgs( argc, argv, options, modNames, listMods, itemsDir, characterName, workshopDir ) == false )
	{
		Usage();
		return 1;
	}
	// A map named without a folder or an extension is maps/<name>.cbmap: next to this executable, in
	// the working folder, or (in a checkout) the project's own maps.
	if ( options.mapPath.empty() == false && std::filesystem::path( options.mapPath ).has_parent_path() == false &&
		 std::filesystem::path( options.mapPath ).has_extension() == false )
	{
		std::vector<std::filesystem::path> places = { exeDir / "maps", std::filesystem::current_path() / "maps" };
#if defined( CB_CHARACTERS_SOURCE_DIR )
		places.push_back( std::filesystem::path( CB_CHARACTERS_SOURCE_DIR ).parent_path() / "maps" );
#endif
		for ( const std::filesystem::path& place : places )
		{
			if ( std::filesystem::exists( place / ( options.mapPath + ".cbmap" ) ) )
			{
				options.mapPath = ( place / ( options.mapPath + ".cbmap" ) ).string();
				break;
			}
		}
	}
	cb::PhysicsThreads::Start( g_physicsThreads >= 0 ? g_physicsThreads : cb::PhysicsThreads::Suggested() );
	options.physicsThreaded = cb::PhysicsThreads::Count() > 0;
	for ( int i = 1; i + 1 < argc; ++i )
	{
		if ( std::string( argv[i] ) == "--dump-names" )
		{
			std::string lookSource;
			for ( int k = 1; k + 1 < argc; ++k )
			{
				lookSource = std::string( argv[k] ) == "--look-source" ? argv[k + 1] : lookSource;
			}
			return DumpNames( argv[i + 1], lookSource ) ? 0 : 1;
		}
	}
	if ( listMods )
	{
		for ( const cb::mods::ModInfo& info : cb::mods::CompiledMods() )
		{
			std::printf( "%s%s\n", info.name, info.onByDefault ? "" : "  (disabled: runs only when --mods names it)" );
		}
		return 0;
	}

#if defined( _WIN32 )
	timeBeginPeriod( 1 ); // 1 ms sleep resolution
#endif

	cb::GameServer server;
	for ( const std::string& name : modNames )
	{
		std::unique_ptr<cb::mods::ServerMod> mod = cb::mods::CreateMod( name );
		if ( mod == nullptr )
		{
			std::printf( "no mod named %s (see --list-mods)\n", name.c_str() );
			return 1;
		}
		server.AddMod( std::move( mod ) );

		// The item players need for this mod's look, if it has one.
		bool clientContent = false;
		for ( const cb::mods::ModInfo& info : cb::mods::CompiledMods() )
		{
			clientContent |= name == info.name && info.clientContent;
		}
		if ( clientContent == false )
		{
			continue;
		}
		cb::ModItem item;
		if ( ReadItem( itemsDir, name, item ) )
		{
			options.items.push_back( item );
		}
		else
		{
			std::printf( "warning: mod %s has client content but no item manifest in %s; clients will not load its look\n",
						 name.c_str(), itemsDir.c_str() );
		}
	}
	const std::filesystem::path builtIn = charactersDir / characterName;
	if ( characterName != "none" && characterName.empty() == false && std::filesystem::exists( builtIn / "anim.cfg" ) &&
		 std::filesystem::exists( std::filesystem::path( itemsDir ) / ( characterName + ".item" ) ) == false )
	{
		std::string error, warnings;
		options.character = cb::LoadCharacterFolder( builtIn.string(), characterName, error, warnings );
		if ( options.character == nullptr )
		{
			std::printf( "%s\n", error.c_str() );
			return 1;
		}
		if ( warnings.empty() == false )
		{
			std::printf( "character %s: %s\n", characterName.c_str(), warnings.c_str() );
		}
		std::printf( "character: %s, shipped with the game (%s, %zu hitboxes)\n", characterName.c_str(),
					 options.character->animations->Description().c_str(), options.character->hitboxes.boxes.size() );
	}
	else if ( characterName != "none" && characterName.empty() == false )
	{
		cb::ModItem item;
		if ( ReadItem( itemsDir, characterName, item ) == false )
		{
			std::printf( "no item manifest for character %s in %s (publish it with tools/publish_mod.ps1 -Character %s)\n",
						 characterName.c_str(), itemsDir.c_str(), characterName.c_str() );
			return 1;
		}
		std::string zip = ( std::filesystem::path( workshopDir ) / characterName / ( item.sha256 + ".zip" ) ).string();
		std::string error, warnings;
		options.character = cb::LoadCharacterItem( zip, item, error, warnings );
		if ( options.character == nullptr )
		{
			std::printf( "%s\n", error.c_str() );
			return 1;
		}
		if ( warnings.empty() == false )
		{
			std::printf( "character %s: %s\n", characterName.c_str(), warnings.c_str() );
		}
		std::printf( "character: %s (%s, %zu hitboxes)\n", characterName.c_str(),
					 options.character->animations->Description().c_str(), options.character->hitboxes.boxes.size() );
		options.items.push_back( item );
	}
	// Animation packs come from their mod's workshop item, like a workshop character.
	options.loadAnimPack = [itemsDir, workshopDir]( const std::string& mod, const std::string& pack, std::string& error,
													std::string& warnings ) -> std::shared_ptr<const cb::anim::AnimSet> {
		cb::ModItem item;
		if ( ReadItem( itemsDir, mod, item ) == false )
		{
			error = "no item manifest for mod " + mod;
			return nullptr;
		}
		std::string zip = ( std::filesystem::path( workshopDir ) / mod / ( item.sha256 + ".zip" ) ).string();
		return cb::LoadAnimPackItem( zip, item, pack, error, warnings );
	};
	// A character item's manifest (a map's CbCharacterBody names one).
	options.characterItem = [itemsDir]( const std::string& name, cb::ModItem& item ) { return ReadItem( itemsDir, name, item ); };
	// Characters a mod declares: characters/<name>/, baked from a CbCharacter scene of its look.
	options.loadCharacter = [itemsDir, workshopDir]( const std::string& mod, const std::string& name, std::string& error,
													 std::string& warnings ) -> std::shared_ptr<const cb::CharacterAsset> {
		cb::ModItem item;
		if ( ReadItem( itemsDir, mod, item ) == false )
		{
			error = "no item manifest for mod " + mod;
			return nullptr;
		}
		std::string zip = ( std::filesystem::path( workshopDir ) / mod / ( item.sha256 + ".zip" ) ).string();
		return cb::LoadModCharacterItem( zip, item, name, error, warnings );
	};
	// Motions: motions/<set>.cfg, baked from the mod's CbMotion nodes.
	options.loadMotions = [itemsDir, workshopDir]( const std::string& mod, const std::string& set, std::string& text, std::string& error ) {
		cb::ModItem item;
		if ( ReadItem( itemsDir, mod, item ) == false )
		{
			error = "no item manifest for mod " + mod;
			return false;
		}
		std::string zip = ( std::filesystem::path( workshopDir ) / mod / ( item.sha256 + ".zip" ) ).string();
		return cb::LoadMotionsItem( zip, item, set, text, error );
	};
	// And the items themselves, for a mod that takes them from its look.
	options.lookItems = [itemsDir, workshopDir]( const std::string& mod ) {
		std::vector<cb::LookItem> items;
		cb::ModItem item;
		std::string error;
		if ( ReadItem( itemsDir, mod, item ) )
		{
			std::string zip = ( std::filesystem::path( workshopDir ) / mod / ( item.sha256 + ".zip" ) ).string();
			if ( cb::ListLookItemsItem( zip, item, items, error ) == false )
			{
				std::printf( "mod %s: its look's items could not be read: %s\n", mod.c_str(), error.c_str() );
			}
		}
		return items;
	};
	// Item bodies too: items/<kind>.cfg, baked from the item's scene.
	options.loadItemShape = [itemsDir, workshopDir]( const std::string& mod, const std::string& kind, cb::ItemShape& shape,
													 std::string& error, std::map<std::string, float>& properties ) {
		cb::ModItem item;
		if ( ReadItem( itemsDir, mod, item ) == false )
		{
			return false;
		}
		std::string zip = ( std::filesystem::path( workshopDir ) / mod / ( item.sha256 + ".zip" ) ).string();
		return cb::LoadItemShapeItem( zip, item, kind, shape, error, &properties );
	};
	if ( server.Start( options ) == false )
	{
		return 1;
	}

	// --bench SEC (M251): the server runs on a clock of its own, a tick a step, as fast as it can,
	// for SEC seconds of game, then says what its fake players would have been sent and stops. A
	// session is then the same however slow the machine.
	double benchSeconds = 0.0;
	for ( int i = 1; i + 1 < argc; ++i )
	{
		if ( std::string( argv[i] ) == "--bench" )
		{
			// (At least the warm-up and five seconds counted.)
			benchSeconds = std::max( double( options.fakeWarmupSeconds ) + 5.0, std::atof( argv[i + 1] ) );
		}
	}
	double benchNow = 0.0;
	using Clock = std::chrono::steady_clock;
	const auto start = Clock::now();
	auto seconds = [&] { return benchSeconds > 0.0 ? benchNow : std::chrono::duration<double>( Clock::now() - start ).count(); };
	// The tick's parts over the whole run (the status lines' are since the last one).
	double runTicks = 0.0, runTickMs = 0.0, runMods = 0.0, runStep = 0.0, runSave = 0.0, runSend = 0.0, runTickMax = 0.0;
	cb::Simulation::StepProfile warm{}; // the step's profile as the bench's warm-up ended
	uint64_t lastWire = 0;
	auto keepTiming = [&] {
		const auto& s = server.GetStats();
		runTicks += double( s.ticks );
		runTickMs += s.tickMsTotal;
		runMods += s.modsMs;
		runStep += s.stepMs;
		runSave += s.saveMs;
		runSend += s.sendMs;
		runTickMax = std::max( runTickMax, s.tickMsMax );
	};

	double nextStatus = 5.0;
	double lastStatus = 0.0;
	uint64_t lastBytesSent = 0;
	uint64_t lastLate = 0;
	uint64_t lastInputTicks = 0;
	// --exit-when-empty: how long nobody may be connected before it stops (0: it never does).
	double emptySeconds = 0.0;
	for ( int i = 1; i + 1 < argc; ++i )
	{
		if ( std::string( argv[i] ) == "--exit-when-empty" )
		{
			emptySeconds = std::max( 1.0, std::atof( argv[i + 1] ) );
		}
	}
	double emptySince = 0.0;
	// A look published again while the server runs (tools/publish_mod.ps1 writes its manifest
	// here): taken up at once when it changes nothing the simulation reads; otherwise the server
	// says what has to be done, once, and keeps the item it started with.
	double nextItems = 1.0;
	std::map<std::string, std::string> refused; // mod -> the item that was not taken
	for ( ;; )
	{
		double now = seconds();
		server.Update( now );
		if ( now >= nextItems )
		{
			nextItems = now + 1.0;
			for ( const cb::ModItem& have : std::vector<cb::ModItem>( server.Schema().items ) )
			{
				cb::ModItem published;
				if ( ReadItem( itemsDir, have.mod, published ) == false || published.sha256 == have.sha256 || refused[have.mod] == published.sha256 )
				{
					continue;
				}
				auto zipOf = [&]( const cb::ModItem& item ) {
					return ( std::filesystem::path( workshopDir ) / item.mod / ( item.sha256 + ".zip" ) ).string();
				};
				std::map<std::string, std::string> was, is;
				std::string error;
				if ( cb::ItemServerParts( zipOf( have ), have, was, error ) == false || cb::ItemServerParts( zipOf( published ), published, is, error ) == false )
				{
					std::printf( "%s was published again, but not taken: %s\n", have.mod.c_str(), error.c_str() );
					refused[have.mod] = published.sha256;
					continue;
				}
				if ( was != is )
				{
					std::string what;
					for ( const auto& [name, content] : is )
					{
						auto old = was.find( name );
						what += old == was.end() || old->second != content ? ( what.empty() ? "" : ", " ) + name : "";
					}
					for ( const auto& [name, content] : was )
					{
						what += is.count( name ) == 0 ? ( what.empty() ? "" : ", " ) + name + " (gone)" : "";
					}
					std::printf( "%s was published again with changes the simulation reads (%s): restart the server to take it. Players keep the look it started with.\n",
								 have.mod.c_str(), what.c_str() );
					refused[have.mod] = published.sha256;
					continue;
				}
				server.ReplaceItem( published );
				std::printf( "%s was published again: its look is %s now, for everyone here and whoever joins\n", have.mod.c_str(),
							 published.sha256.substr( 0, 12 ).c_str() );
			}
		}
		if ( emptySeconds > 0.0 )
		{
			emptySince = server.ConnectedClients() > 0 ? now : emptySince;
			if ( now - emptySince >= emptySeconds )
			{
				std::printf( "nobody connected for %.0f s: stopping\n", emptySeconds );
				return 0;
			}
		}

		if ( now >= nextStatus )
		{
			const auto& s = server.GetStats();
			double avg = s.ticks ? s.tickMsTotal / double( s.ticks ) : 0.0;
			double upKbps = double( s.bytesSent - lastBytesSent ) * 8.0 / 1000.0 / ( now - lastStatus );
			uint64_t expected = s.inputTicks - lastInputTicks;
			double latePercent = expected ? 100.0 * double( s.lateInputs - lastLate ) / double( expected ) : 0.0;
			const double per = s.ticks ? 1.0 / double( s.ticks ) : 0.0;
			std::printf( "[server %6u] %d connected, %zu entities, %zu KB physics | tick %.2f ms avg %.2f max (mods %.2f, step %.2f, "
						 "state %.2f; sending %.2f) | late inputs %.2f%% | snapshots %llu | out %.0f kbit/s",
						 server.Tick(), server.ConnectedClients(), server.Sim().Entities().size(),
						 server.Sim().PhysicsBytesInUse() / 1024, avg, s.tickMsMax, s.modsMs * per, s.stepMs * per, s.saveMs * per,
						 s.sendMs * per, latePercent, (unsigned long long)s.snapshotsSent, upKbps );
			if ( server.Bench().fakes > 0 )
			{
				// (What the fake players would have been sent, on the wire, estimated.)
				const uint64_t wire = server.Bench().wire;
				std::printf( " | to the fake players %.0f kbit/s", double( wire - lastWire ) * 8.0 / 1000.0 / ( now - lastStatus ) );
				lastWire = wire;
			}
			std::printf( "\n" );
			std::fflush( stdout );
			// (The bench's times from the end of its warm-up on, as what it counts: the first windows
			// are the joins'.)
			if ( lastStatus + 1e-6 >= options.fakeWarmupSeconds )
			{
				keepTiming();
			}
			else if ( now + 1e-6 >= options.fakeWarmupSeconds )
			{
				warm = server.Sim().Profile();
			}
			server.ResetTickTiming();
			lastBytesSent = s.bytesSent;
			lastLate = s.lateInputs;
			lastInputTicks = s.inputTicks;
			lastStatus = now;
			nextStatus = now + 5.0;
		}

		if ( benchSeconds > 0.0 )
		{
			benchNow += 1.0 / double( options.config.tickRate );
			if ( benchNow < benchSeconds )
			{
				continue;
			}
			keepTiming();
			cb::Simulation::StepProfile sp = server.Sim().Profile();
			sp.steps -= warm.steps;
			sp.commandsNs -= warm.commandsNs;
			sp.charactersNs -= warm.charactersNs;
			sp.physicsNs -= warm.physicsNs;
			sp.afterNs -= warm.afterNs;
			const double steps = sp.steps > 0 ? double( sp.steps ) : 1.0;
			const double per = runTicks > 0.0 ? 1.0 / runTicks : 0.0;
			std::printf( "%s", cb::FormatBench( server.Bench() ).c_str() );
			// (Sending is outside the tick's own time; the bench's sampling is left out of it.)
			const double sending = std::max( 0.0, runSend - server.Bench().sampleMs ) * per;
			std::printf( "server tick   %.2f ms on average of the %.1f a tick has: the tick %.2f (%.2f at most: mods %.2f, step %.2f [commands %.2f, "
						 "characters %.2f, physics %.2f, after %.2f], saving states %.2f), then sending %.2f\n",
						 runTickMs * per + sending, 1000.0 / double( options.config.tickRate ), runTickMs * per, runTickMax, runMods * per,
						 runStep * per, double( sp.commandsNs ) / steps / 1e6, double( sp.charactersNs ) / steps / 1e6,
						 double( sp.physicsNs ) / steps / 1e6, double( sp.afterNs ) / steps / 1e6, runSave * per, sending );
			std::printf( "              (%.1f s of game in %.1f s)\n", benchSeconds, std::chrono::duration<double>( Clock::now() - start ).count() );
			return 0;
		}
		double wait = server.TimeUntilNextTick( seconds() );
		if ( wait > 0.002 )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		}
		else if ( wait > 0.0 )
		{
			std::this_thread::yield();
		}
	}
}
