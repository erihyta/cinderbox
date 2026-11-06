#include "game_server.h"
#include "detmath.h"
#include "mover.h"

#include "capture.h"
#include "view_codec.h"
#include "fingerprint.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <random>

namespace cb
{

using namespace net;

GameServer::GameServer() = default;

GameServer::~GameServer()
{
	m_mods.clear(); // before the world their state lives in
	if ( m_modWorld )
	{
		ReleaseFlecsWorld( *m_modWorld );
	}
}

void GameServer::AddMod( std::unique_ptr<mods::ServerMod> mod )
{
	m_mods.push_back( std::move( mod ) );
}

bool GameServer::Start( const ServerOptions& options )
{
	m_options = options;
	m_options.stateEvery = std::clamp<uint32_t>( m_options.stateEvery, 1, 30 );

	mods::Declarations declarations;
	declarations.SetLookItems( options.lookItems );
	declarations.SetOptions( &m_options.modOptions );
	for ( const auto& mod : m_mods )
	{
		declarations.BeginMod( mod->Name() );
		mod->Declare( declarations );
	}
	for ( const std::string& error : declarations.Errors() )
	{
		Log( "mod error: %s", error.c_str() );
	}
	if ( declarations.Errors().empty() == false )
	{
		return false;
	}
	m_schema = declarations.Schema();
	m_schema.items = options.items;
	m_textChannels = declarations.TextChannels();
	m_itemProperties = declarations.ItemProperties();
	m_itemLayers = declarations.ItemLayersByKind();
	// Item bodies authored in Godot (baked into the declaring mod's item) replace declared ones, and
	// so do the properties authored with them: what the scene says wins over what the code says.
	for ( size_t kind = 0; kind < m_schema.itemKinds.size() && options.loadItemShape; ++kind )
	{
		for ( const std::string& mod : declarations.ItemMods()[kind] )
		{
			ItemShape shape;
			std::string shapeError;
			std::map<std::string, float> authored;
			if ( options.loadItemShape( mod, m_schema.itemKinds[kind], shape, shapeError, authored ) )
			{
				m_schema.itemShapes[kind] = shape;
				for ( const auto& [name, value] : authored )
				{
					m_itemProperties[std::make_pair( int( kind ), name )] = value;
					Log( "item %s: %s = %g", m_schema.itemKinds[kind].c_str(), name.c_str(), double( value ) );
				}
				Log( "item %s: body from mod %s's item (%s, %.2f kg)", m_schema.itemKinds[kind].c_str(), mod.c_str(),
					 shape.kind == 1 ? "sphere" : "box", double( shape.mass ) );
				break;
			}
			if ( shapeError.empty() == false )
			{
				Log( "item %s: %s (mod %s); using the declared body", m_schema.itemKinds[kind].c_str(), shapeError.c_str(), mod.c_str() );
			}
		}
	}
	// The mods' motions, from their items: the text goes to everyone in the schema, and every
	// simulation compiles it against the same names.
	for ( MotionSetInfo& set : m_schema.motionSets )
	{
		std::string setError;
		if ( options.loadMotions == nullptr || options.loadMotions( set.mod, set.name, set.text, setError ) == false )
		{
			set.text.clear();
			Log( "motions %s (mod %s) are not available%s%s; they do nothing", set.name.c_str(), set.mod.c_str(),
				 setError.empty() ? "" : ": ", setError.c_str() );
		}
	}
	{
		std::string warnings;
		m_motions = CompileMotions( m_schema, warnings );
		if ( warnings.empty() == false )
		{
			Log( "motions: %s", warnings.c_str() );
		}
		if ( m_motions )
		{
			Log( "motions: %zu", m_motions->list.size() );
		}
	}
	std::shared_ptr<const CharacterAsset> character = options.character ? options.character : BuiltInCharacter();
	m_schema.characters.front().name = character->name;
	// The characters the map places bodies of (CbCharacterBody): on the list like a mod's, each from
	// its own item (a character item: its name is its mod's), and that item announced to players.
	if ( options.mapPath.empty() == false )
	{
		LevelLayout placed;
		std::vector<uint8_t> bytes;
		std::string error;
		if ( LoadMapFile( options.mapPath, placed, bytes, error ) )
		{
			for ( const LevelBody& body : placed.bodies )
			{
				const bool known = std::any_of( m_schema.characters.begin(), m_schema.characters.end(),
												[&]( const CharacterInfo& c ) { return c.name == body.character; } );
				if ( known || m_schema.characters.size() >= kMaxCharacters )
				{
					continue;
				}
				CharacterInfo info;
				info.name = body.character;
				info.mod = body.character;
				m_schema.characters.push_back( info );
				ModItem item;
				if ( options.characterItem && options.characterItem( body.character, item ) &&
					 std::none_of( m_schema.items.begin(), m_schema.items.end(), [&]( const ModItem& i ) { return i.mod == item.mod; } ) )
				{
					m_schema.items.push_back( item );
				}
			}
		}
	}
	// How players move: the server's options, then what its own character says about itself.
	for ( const auto& [param, value] : character->animations->Movement() )
	{
		m_options.config.move.values[param] = value;
	}
	// The characters: [0] the server's own, then those its mods declared, each read from its mod's
	// item (one that cannot be read is the built-in rig, so a body made it still stands).
	std::vector<std::shared_ptr<const CharacterAsset>> assets = { character };
	for ( size_t i = 1; i < m_schema.characters.size(); ++i )
	{
		CharacterInfo& info = m_schema.characters[i];
		std::string error, warnings;
		std::shared_ptr<const CharacterAsset> asset = options.loadCharacter ? options.loadCharacter( info.mod, info.name, error, warnings ) : nullptr;
		if ( asset == nullptr )
		{
			Log( "character %s (mod %s) is not available%s%s; its bodies are the built-in rig", info.name.c_str(), info.mod.c_str(),
				 error.empty() ? "" : ": ", error.c_str() );
			asset = BuiltInCharacter();
		}
		else if ( warnings.empty() == false )
		{
			Log( "character %s: %s", info.name.c_str(), warnings.c_str() );
		}
		for ( const auto& [param, value] : asset->animations->Movement() )
		{
			info.move.push_back( { uint8_t( param ), value } );
		}
		assets.push_back( asset );
	}
	for ( size_t i = 0; i < assets.size(); ++i )
	{
		CharacterInfo& info = m_schema.characters[i];
		info.graph = assets[i]->animations->GraphText();
		info.radius = assets[i]->animations->CapsuleRadius();
		info.halfHeight = assets[i]->animations->CapsuleHalfHeight();
		info.firstPerson = assets[i]->animations->FirstPerson();
		info.ragdoll = assets[i]->animations->Humanoid();
	}
	m_hits = std::make_unique<HitTester>( character );
	{
		std::string warnings;
		// The mods' animation packs, from their items: their graphs go to everyone in the schema.
		std::vector<std::shared_ptr<const anim::AnimSet>> packSets;
		for ( AnimPackInfo& pack : m_schema.animPacks )
		{
			std::shared_ptr<const anim::AnimSet> set;
			std::string packError;
			if ( options.loadAnimPack )
			{
				set = options.loadAnimPack( pack.mod, pack.name, packError, warnings );
			}
			if ( set == nullptr || set->GraphText().empty() )
			{
				Log( "animation pack %s (mod %s) is not available%s%s; its swaps do nothing", pack.name.c_str(), pack.mod.c_str(),
					 packError.empty() ? "" : ": ", packError.c_str() );
			}
			else
			{
				pack.graph = set->GraphText();
			}
			packSets.push_back( set );
		}
		m_animPacks = CompileAnimPacks( m_schema, warnings );
		// Each character's state machine, if it has one: the simulation runs it for its bodies, the hit
		// tests pose them by it, and it goes to every client in the schema; the packs fitted to its
		// skeleton.
		m_graphs.assign( assets.size(), nullptr );
		for ( size_t i = 0; i < assets.size(); ++i )
		{
			if ( i > 0 )
			{
				m_hits->AddCharacter( assets[i] );
			}
			const std::string& text = m_schema.characters[i].graph;
			if ( text.empty() )
			{
				continue;
			}
			std::string error;
			m_graphs[i] = CompileAnimGraph( text, m_schema, error, warnings );
			if ( m_graphs[i] == nullptr )
			{
				Log( "character %s: its state machine does not load: %s", assets[i]->name.c_str(), error.c_str() );
				if ( i == 0 )
				{
					return false;
				}
				continue;
			}
			m_hits->SetGraph( i, m_graphs[i], warnings );
			std::vector<std::shared_ptr<const anim::PackClips>> fitted;
			std::vector<uint8_t> fits;
			for ( size_t p = 0; p < m_animPacks.size(); ++p )
			{
				fitted.push_back( m_animPacks[p] && packSets[p] ? anim::FitPack( packSets[p], *m_animPacks[p], *assets[i]->animations, warnings )
															   : nullptr );
				// (A pack whose files are missing is said to fit: it does nothing anywhere.)
				fits.push_back( fitted.back() != nullptr || packSets[p] == nullptr ? 1 : 0 );
			}
			if ( std::find( fits.begin(), fits.end(), uint8_t( 0 ) ) != fits.end() )
			{
				m_schema.characters[i].packs = fits;
			}
			m_hits->SetPacks( i, m_animPacks, fitted );
		}
		if ( warnings.empty() == false )
		{
			Log( "character %s: %s", character->name.empty() ? "built-in" : character->name.c_str(), warnings.c_str() );
		}
	}
	// How much state the mods asked for: every simulation of the session is sized by it.
	m_options.config.fields = uint16_t( m_schema.FieldCount( BoardScope::Entity ) );
	m_options.config.globalFields = uint16_t( m_schema.FieldCount( BoardScope::Global ) );
	m_options.config.motions = uint16_t( m_motions ? m_motions->list.size() : 0 );
	// (The most any character has: its layers and the overlays after them, a pack's layer the
	// character does not have.)
	size_t layers = m_schema.layers.size();
	for ( const auto& graph : m_graphs )
	{
		layers = std::max( layers, graph ? LayerSlots( *graph ) : size_t( 0 ) );
	}
	m_options.config.layers = uint8_t( std::min( layers, size_t( kLayerLimit ) ) );
	// The hybrid's lag compensation for the motions' finds and probes, in the simulation (a
	// recording plays it again): as far back as the mods' casts (an input's seenBack is a byte).
	m_options.config.compensation =
		m_options.netModel == NetModel::Hybrid ? uint8_t( std::min<uint32_t>( m_options.LagCompensationTicks(), 255 ) ) : uint8_t( 0 );
	{
		// What that costs: every client keeps, hashes and rolls back as much.
		const SimConfig& c = m_options.config;
		Log( "state the mods asked for: %d fields an entity (%d bytes for one that has any), %d for the game, %d motions and %d layers (%zu bytes a player)",
			 int( c.fields ), int( c.fields ) * 4, int( c.globalFields ), int( c.motions ), int( c.layers ),
			 AnimStateBytes( c.layers ) + ( c.motions > 0 ? 8 + size_t( c.motions ) * sizeof( MotionSlot ) : 0 ) );
	}
	// Slots: how many the mods asked for, the hand the selected one's item is held in, and what
	// each kind says about itself (item properties, authored or declared).
	m_options.config.slots = uint8_t( declarations.SlotCount() );
	if ( m_options.config.slots > 0 )
	{
		auto hand = std::find( m_schema.sockets.begin(), m_schema.sockets.end(), "RightHand" );
		if ( hand == m_schema.sockets.end() )
		{
			m_schema.sockets.push_back( "RightHand" );
			hand = m_schema.sockets.end() - 1;
		}
		m_options.config.slotHand = uint8_t( hand - m_schema.sockets.begin() );
		for ( size_t kind = 0; kind < m_schema.itemShapes.size(); ++kind )
		{
			auto property = [&]( const char* name ) {
				auto found = m_itemProperties.find( std::make_pair( int( kind ), std::string( name ) ) );
				return found != m_itemProperties.end() ? int( found->second ) : 0;
			};
			int slot = property( "slot" );
			int holster = property( "holster" ); // a socket's place + 1 (Declarations::ItemProperty)
			m_schema.itemShapes[kind].slot = slot >= 1 && slot <= int( m_options.config.slots ) ? uint8_t( slot ) : uint8_t( 0 );
			m_schema.itemShapes[kind].holster = holster >= 1 && holster <= int( m_schema.sockets.size() ) ? uint8_t( holster - 1 ) : kNoSocket;
			// How it is used, and the event that says so: "<kind>.used", where a mod declared one.
			int use = property( "use" );
			m_schema.itemShapes[kind].use = use == kItemUsePress || use == kItemUseHold ? uint8_t( use ) : kItemUseInHand;
			m_schema.itemShapes[kind].hand = property( "hand" ) == kItemHandLeft ? kItemHandLeft : kItemHandSelected;
			// Its slot's key uses it (CbItem.slot_key_uses), for one used once a press or while held.
			m_schema.itemShapes[kind].slotKeyUses = property( "slot_key_uses" ) != 0 && m_schema.itemShapes[kind].use != kItemUseInHand ? 1 : 0;
			// Its own key, if its scene gives one (CbItem.use_key): an action's bit.
			auto useAction = declarations.ItemUseActions().find( int( kind ) );
			m_schema.itemShapes[kind].useAction = useAction != declarations.ItemUseActions().end() ? useAction->second : uint8_t( 255 );
			int usedEvent = m_schema.FindEvent( m_schema.itemKinds[kind] + ".used" );
			m_schema.itemShapes[kind].usedEvent = usedEvent >= 0 ? uint16_t( usedEvent ) : uint16_t( 0xFFFF );
		}
		Log( "slots: %d a player, the selected one's item in %s", int( m_options.config.slots ), hand->c_str() );
	}
	// Thrown fast: a bullet in the world (the item property "bullet", its CbItem's). Any kind, with
	// or without slots.
	for ( size_t kind = 0; kind < m_schema.itemShapes.size(); ++kind )
	{
		auto found = m_itemProperties.find( std::make_pair( int( kind ), std::string( "bullet" ) ) );
		m_schema.itemShapes[kind].bullet = found != m_itemProperties.end() && found->second != 0.0f ? 1 : 0;
		// How many a slot holds (the item property "stack": its CbItem's).
		auto stack = m_itemProperties.find( std::make_pair( int( kind ), std::string( "stack" ) ) );
		m_schema.itemShapes[kind].stack = stack != m_itemProperties.end() ? uint8_t( std::clamp( stack->second, 0.0f, 255.0f ) ) : 0;
	}
	// What says that a copy's time is up: "<kind>.expired", where a mod declares it (with or without
	// slots).
	for ( size_t kind = 0; kind < m_schema.itemShapes.size() && kind < m_schema.itemKinds.size(); ++kind )
	{
		int expired = m_schema.FindEvent( m_schema.itemKinds[kind] + ".expired" );
		m_schema.itemShapes[kind].expiredEvent = expired >= 0 ? uint16_t( expired ) : uint16_t( 0xFFFF );
		// What it breaks (its CbItem's break_speed, break_hole, break_through), and what says so:
		// "<kind>.smash", where a mod declares it.
		auto number = [&]( const char* name, float low, float high ) {
			auto found = m_itemProperties.find( std::make_pair( int( kind ), std::string( name ) ) );
			return found != m_itemProperties.end() ? std::clamp( found->second, low, high ) : 0.0f;
		};
		ItemShape& shape = m_schema.itemShapes[kind];
		shape.breakSpeed = number( "break_speed", 0.0f, 1000.0f );
		shape.breakHole = number( "break_hole", 0.0f, 20.0f );
		shape.breakThrough = number( "break_through", 0.0f, 1.0f );
		int smash = m_schema.FindEvent( m_schema.itemKinds[kind] + ".smash" );
		shape.breakEvent = smash >= 0 ? uint16_t( smash ) : uint16_t( 0xFFFF );
	}
	// What a new copy of each kind starts with in its own fields (declared by its scene).
	for ( const auto& [kind, defaults] : declarations.ItemFieldDefaults() )
	{
		if ( kind >= 0 && size_t( kind ) < m_schema.itemShapes.size() )
		{
			m_schema.itemShapes[size_t( kind )].fieldDefaults = defaults;
		}
	}
	EncodeSchema( m_schema, m_schemaBytes );

	m_map = GetLevelLayout();
	if ( options.mapPath.empty() == false )
	{
		std::string error;
		if ( LoadMapFile( options.mapPath, m_map, m_mapBytes, error ) == false )
		{
			Log( "cannot load map %s: %s", options.mapPath.c_str(), error.c_str() );
			return false;
		}
	}
	// The mods' templates (a chest) join the map's, unless it has one by that name: the map says.
	bool added = false;
	for ( const EntityTemplate& t : declarations.Templates() )
	{
		bool known = std::any_of( m_map.templates.begin(), m_map.templates.end(), [&]( const EntityTemplate& m ) { return m.name == t.name; } );
		if ( known == false )
		{
			m_map.templates.push_back( t );
			added = true;
		}
	}
	if ( options.mapPath.empty() || added )
	{
		SerializeMap( m_map, m_mapBytes );
	}
	m_mapHash = MapHash( m_mapBytes.data(), m_mapBytes.size() );

	m_sim = std::make_unique<Simulation>( m_options.config, m_map );
	m_sim->SetPhysicsThreaded( m_options.physicsThreaded );
	m_sim->SetCharacters( CharacterBodies() );
	m_sim->SetAnimPacks( m_animPacks );
	m_sim->SetMotions( m_motions );
	m_sim->SetItemShapes( m_schema.itemShapes );
	{
		// Where the server's own character can walk (mods find their way on it): what does not
		// move, as the simulation's physics has it.
		NavAgent agent;
		const CharacterInfo& own = m_schema.characters.front();
		agent.radius = own.radius;
		agent.height = mover::FeetBelowCenter( own.radius, own.halfHeight ) + own.halfHeight + own.radius;
		agent.maxSlope = m_options.config.move[MoveParam::MaxSlope];
		const auto started = std::chrono::steady_clock::now();
		std::string error;
		if ( m_nav.Build( *m_sim, agent, error ) )
		{
			Log( "navigation mesh: %zu polygons from %zu triangles, in %lld ms", m_nav.Polygons(), m_nav.Triangles(),
				 (long long)std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count() );
		}
		else
		{
			Log( "no navigation mesh: %s (FindPath finds nothing)", error.c_str() );
		}
	}
	m_modWorld = std::make_unique<flecs::world>( CreateFlecsWorld() );
	m_modRng = options.config.seed ^ 0x6D6F6473ull; // "mods"
	m_history.assign( kFrameHistory, InputFrame{} );
	for ( InputFrame& f : m_history )
	{
		f.tick = UINT32_MAX;
	}

	if ( m_transport.Listen( options.port, options.maxClients ) == false )
	{
		Log( "failed to listen on port %u", options.port );
		return false;
	}

	std::random_device rd;
	m_tokenState = ( uint64_t( rd() ) << 32 ) ^ rd() ^ uint64_t( std::chrono::steady_clock::now().time_since_epoch().count() );
	AddFakePlayers();

	if ( options.recordHashes )
	{
		m_hashes.push_back( m_sim->ComputeHash() );
	}

	if ( options.recordPath.empty() == false )
	{
		if ( m_replay.Open( options.recordPath, BuildFingerprint(), m_options.config, m_mapBytes, m_schemaBytes ) == false )
		{
			Log( "cannot write replay %s", options.recordPath.c_str() );
			return false;
		}
		m_replay.AddChecksum( 0, m_sim->ComputeHash() );
		Log( "recording to %s", options.recordPath.c_str() );
	}
	if ( options.recordViewPath.empty() == false )
	{
		uint32_t stride = options.recordViewRate > 0 ? std::max<uint32_t>( ( options.config.tickRate + options.recordViewRate / 2 ) / options.recordViewRate, 1 ) : 1;
		present::ViewPrecision precision = options.recordViewCompact ? present::ViewPrecision::Compact : present::ViewPrecision::Exact;
		if ( m_viewFile.Open( options.recordViewPath, stride, precision ) == false )
		{
			Log( "cannot write view file %s", options.recordViewPath.c_str() );
			return false;
		}
		Log( "recording the view to %s (%u frames a second, %s)", options.recordViewPath.c_str(), options.config.tickRate / stride,
			 options.recordViewCompact ? "compact" : "exact" );
	}

	Log( "listening on port %u, %u Hz, fingerprint %016llx, map %s (%u statics, %u props, hash %016llx), the %s network model",
		 options.port, options.config.tickRate, (unsigned long long)BuildFingerprint(),
		 options.mapPath.empty() ? "built-in sandbox" : options.mapPath.c_str(), unsigned( m_map.statics.size() ),
		 unsigned( m_map.props.size() ), (unsigned long long)m_mapHash, ToString( options.netModel ) );

	{
		// Only what differs from the engine's: most servers print nothing here.
		std::string moves;
		const MoveParams defaults;
		for ( int i = 0; i < kMoveParams; ++i )
		{
			if ( m_options.config.move.values[i] != defaults.values[i] )
			{
				char text[64];
				std::snprintf( text, sizeof( text ), "%s%s = %g", moves.empty() ? "" : ", ", MoveParamInfoOf( i ).name,
							   double( m_options.config.move.values[i] ) );
				moves += text;
			}
		}
		if ( moves.empty() == false )
		{
			Log( "movement: %s", moves.c_str() );
		}
	}

	if ( m_mods.empty() == false )
	{
		std::string names;
		for ( const auto& mod : m_mods )
		{
			names += names.empty() ? "" : ", ";
			names += mod->Name();
		}
		Log( "mods: %s (%zu fields, %zu events, %zu actions)", names.c_str(), m_schema.fields.size(), m_schema.events.size(),
			 m_schema.actions.size() );
		for ( const ModItem& item : m_schema.items )
		{
			Log( "clients need workshop item %s %.12s", item.mod.c_str(), item.sha256.c_str() );
		}
		InputFrame none;
		none.tick = m_sim->Tick();
		mods::Context ctx( *m_sim, m_schema, none, m_lastInputs, *m_modWorld, m_modRng );
		ctx.SetOptions( &m_options.modOptions );
		ctx.SetItemProperties( &m_itemProperties );
		ctx.SetHitTester( m_hits.get() );
		ctx.SetNavMesh( &m_nav );
		for ( const auto& mod : m_mods )
		{
			mod->Start( ctx );
		}
	}
	return true;
}

std::vector<Simulation::CharacterBody> GameServer::CharacterBodies() const
{
	std::vector<Simulation::CharacterBody> bodies;
	for ( size_t i = 0; i < m_schema.characters.size(); ++i )
	{
		// As every client's simulation has them (Simulation::CharactersOf), with the graph compiled here.
		const CharacterInfo& info = m_schema.characters[i];
		Simulation::CharacterBody body;
		body.graph = i < m_graphs.size() ? m_graphs[i] : nullptr;
		body.radius = info.radius;
		body.halfHeight = info.halfHeight;
		body.move = info.move;
		body.firstPerson = info.firstPerson;
		body.ragdoll = info.ragdoll;
		body.packs = info.packs;
		bodies.push_back( std::move( body ) );
	}
	return bodies;
}

void GameServer::Log( const char* fmt, ... ) const
{
	if ( m_options.verbose == false )
	{
		return;
	}
	va_list args;
	va_start( args, fmt );
	std::printf( "[server %6u] ", m_sim ? m_sim->Tick() : 0 );
	std::vprintf( fmt, args );
	std::printf( "\n" );
	std::fflush( stdout );
	va_end( args );
}

int GameServer::ConnectedClients() const
{
	int n = 0;
	for ( const Client& c : m_clients )
	{
		n += ( c.used && c.connected ) ? 1 : 0;
	}
	return n;
}

bool GameServer::GetRecordedHash( uint32_t tick, uint64_t& hash ) const
{
	if ( tick >= m_hashes.size() )
	{
		return false;
	}
	hash = m_hashes[tick];
	return true;
}

GameServer::Client* GameServer::FindByPeer( PeerId peer )
{
	for ( Client& c : m_clients )
	{
		if ( c.used && c.connected && c.peer == peer )
		{
			return &c;
		}
	}
	return nullptr;
}

void GameServer::Reject( PeerId peer, const std::string& reason )
{
	Log( "rejecting peer %u: %s", peer, reason.c_str() );
	Encode( MsgReject{ reason }, m_buffer );
	m_transport.Send( peer, ChannelReliable, m_buffer, true );
	m_transport.Disconnect( peer );
}

double GameServer::TimeUntilNextTick( double now ) const
{
	if ( m_nextTickTime < 0.0 )
	{
		return 0.0;
	}
	return m_nextTickTime - now;
}

void GameServer::Update( double now )
{
	m_events.clear();
	m_transport.Poll( m_events );
	for ( const NetEvent& ev : m_events )
	{
		HandleEvent( ev, now );
	}
	if ( m_bench.fakes > 0 )
	{
		FakeAcknowledgements( now );
	}

	// Players whose connection did not come back in time leave the world.
	for ( Client& c : m_clients )
	{
		if ( c.used && c.connected == false && now - c.disconnectedAt > m_options.reconnectGraceSeconds )
		{
			if ( c.inWorld )
			{
				m_pendingEvents.push_back( { PlayerEventType::Leave, c.slot } );
			}
			Log( "slot %u left (reconnect grace expired)", c.slot );
			m_privates[c.slot] = Blackboard{}; // the next player in this slot starts with nothing
			c = Client{};
			m_namesDirty = true;
		}
	}

	const double dt = 1.0 / double( m_options.config.tickRate );
	if ( m_nextTickTime < 0.0 )
	{
		m_nextTickTime = now;
	}

	int ran = 0;
	while ( now >= m_nextTickTime && ran < 8 )
	{
		auto start = std::chrono::steady_clock::now();
		RunTick( now );
		double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
		m_stats.ticks += 1;
		m_stats.tickMsTotal += ms;
		m_stats.tickMsMax = std::max( m_stats.tickMsMax, ms );
		m_nextTickTime += dt;
		++ran;
	}
	if ( ran > 0 )
	{
		// Once for all the ticks of this update, not once a tick: packets sent in one go leave in
		// one datagram, and are lost together. A frame's three packets have to be three datagrams.
		const auto sendFrom = std::chrono::steady_clock::now();
		SendFrames( now );
		m_bench.framesMs += BenchCounts() ? std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - sendFrom ).count() : 0.0;
		if ( m_options.netModel == NetModel::Hybrid )
		{
			SendStates();
		}
		m_stats.sendMs += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - sendFrom ).count();
	}
	if ( now - m_nextTickTime > 0.25 )
	{
		Log( "running %.0f ms behind, skipping ahead", ( now - m_nextTickTime ) * 1000.0 );
		m_nextTickTime = now;
	}

	if ( m_namesDirty )
	{
		m_namesDirty = false;
		SendNames();
	}
	if ( m_bench.fakes > 0 && ran > 0 )
	{
		FakeSent();
	}

	m_transport.Flush();
	m_stats.bytesSent = m_transport.BytesSent();
	m_stats.bytesReceived = m_transport.BytesReceived();
}

void GameServer::HandleEvent( const NetEvent& ev, double now )
{
	switch ( ev.type )
	{
		case NetEvent::Type::Connected:
			Log( "peer %u connected", ev.peer );
			break;

		case NetEvent::Type::Disconnected:
			if ( Client* c = FindByPeer( ev.peer ) )
			{
				Log( "slot %u disconnected, holding for %.0f s", c->slot, m_options.reconnectGraceSeconds );
				c->connected = false;
				c->welcomed = false;
				c->peer = 0;
				c->disconnectedAt = now;
				c->lastInput = {};
			}
			break;

		case NetEvent::Type::Received:
		{
			ByteReader r( ev.data.data(), ev.data.size() );
			auto type = ReadType( r );
			if ( !type )
			{
				break;
			}

			Client* c = FindByPeer( ev.peer );
			if ( *type == MsgType::Hello )
			{
				MsgHello hello;
				if ( c == nullptr && Decode( r, hello ) )
				{
					HandleHello( ev.peer, hello, now );
				}
			}
			else if ( *type == MsgType::Input && c != nullptr && c->welcomed )
			{
				MsgInput msg;
				if ( Decode( r, msg ) )
				{
					HandleInput( *c, msg, now );
				}
			}
			else if ( *type == MsgType::InfoRequest && c == nullptr )
			{
				// Someone asking what this server is (a menu, before joining): told, never welcomed.
				MsgInfo info;
				info.fingerprint = BuildFingerprint();
				info.map = m_map.name;
				info.maxPlayers = uint16_t( kMaxPlayers );
				for ( const Client& other : m_clients )
				{
					info.players += other.used ? 1 : 0;
				}
				info.items = m_schema.items;
				info.model = m_options.netModel;
				Encode( info, m_buffer );
				m_transport.Send( ev.peer, ChannelReliable, m_buffer, true );
				m_stats.infoRequests += 1;
			}
			else if ( *type == MsgType::Text && c != nullptr && c->welcomed )
			{
				MsgText msg;
				if ( Decode( r, msg ) )
				{
					HandleText( *c, msg );
				}
			}
			else if ( *type == MsgType::ResyncRequest && c != nullptr && c->welcomed )
			{
				MsgResyncRequest msg;
				if ( Decode( r, msg ) )
				{
					m_stats.resyncRequests += 1;
					if ( now - c->lastResyncAt >= m_options.resyncCooldownSeconds )
					{
						Log( "slot %u reported a desync at tick %u, resending state", c->slot, msg.tick );
						c->needsSnapshot = true;
					}
				}
			}
			break;
		}
	}
}

void GameServer::HandleHello( PeerId peer, const MsgHello& hello, double now )
{
	if ( hello.version != kProtocolVersion )
	{
		Reject( peer, "protocol version mismatch" );
		return;
	}
	if ( hello.fingerprint != BuildFingerprint() )
	{
		Reject( peer, "simulation build mismatch (client and server were built differently)" );
		return;
	}

	Client* target = nullptr;
	if ( hello.reconnectToken != 0 )
	{
		for ( Client& c : m_clients )
		{
			if ( c.used && c.token == hello.reconnectToken )
			{
				target = &c;
				break;
			}
		}
		if ( target != nullptr )
		{
			if ( target->connected )
			{
				// The old connection is stale (the client noticed before we did).
				m_transport.DropHard( target->peer );
			}
			Log( "slot %u reconnected (peer %u)", target->slot, peer );
			m_stats.reconnects += 1;
		}
	}

	if ( target == nullptr )
	{
		for ( Client& c : m_clients )
		{
			if ( c.used == false )
			{
				target = &c;
				break;
			}
		}
		if ( target == nullptr )
		{
			Reject( peer, "server full" );
			return;
		}

		PlayerSlot slot = PlayerSlot( target - m_clients.data() );
		*target = Client{};
		m_privates[slot] = Blackboard{};
		target->used = true;
		target->slot = slot;
		target->token = NextRandom( m_tokenState ) | 1;
		target->inWorld = true;
		m_pendingEvents.push_back( { PlayerEventType::Join, slot } );
		m_stats.joins += 1;
		target->name = UniqueName( SanitizeName( hello.name, slot ), *target );
		Log( "peer %u joins as slot %u (%s)", peer, slot, target->name.c_str() );
		m_namesDirty = true;
	}

	target->connected = true;
	target->peer = peer;
	target->welcomed = false;
	target->needsSnapshot = true;
	target->lastInput = {};
	target->lastResyncAt = now;
	for ( auto& s : target->inputs )
	{
		s.tick = UINT32_MAX;
	}
}

void GameServer::HandleInput( Client& client, const MsgInput& msg, double now )
{
	uint32_t current = m_sim->Tick();
	if ( m_options.netModel == NetModel::Hybrid )
	{
		// The newest state it has, if it is one still kept (packets come out of order: only forward).
		if ( msg.stateAck != 0 && ( client.stateAck == kNoStateBase || msg.stateAck > client.stateAck ) &&
			 msg.stateAck <= client.stateSent && KeptState( msg.stateAck ) != nullptr )
		{
			client.stateAck = msg.stateAck;
			StateAcknowledged( client, msg.stateAck );
		}
	}
	{
		// Acknowledgements only move forward (input packets can arrive out of order).
		uint32_t acked = std::max( client.ackTick, std::min( msg.ackTick, current ) );
		if ( acked > client.ackTick && acked - 1 < client.sentThrough )
		{
			// How long an answer takes: from sending the newest frame it now has to hearing so.
			double took = now - client.frameSentAt[( acked - 1 ) % 256];
			if ( took >= 0.0 && took < 5.0 )
			{
				client.roundTrip += ( took - client.roundTrip ) * 0.1;
			}
		}
		client.ackTick = acked;
	}
	uint32_t count = uint32_t( msg.inputs.size() );
	uint32_t first = msg.newestTick + 1 - count;
	for ( uint32_t i = 0; i < count; ++i )
	{
		uint32_t tick = first + i;
		if ( tick < current || tick >= current + kInputBuffer )
		{
			continue;
		}
		client.inputs[tick % kInputBuffer] = { tick, SanitizeInput( msg.inputs[i] ) };
	}
}

void GameServer::SendSnapshots()
{
	bool built = false;
	for ( Client& c : m_clients )
	{
		if ( c.used == false || c.connected == false || c.needsSnapshot == false )
		{
			continue;
		}
		if ( c.fake )
		{
			// (A fake player that fell behind starts over, from what it would have been welcomed with.)
			c.needsSnapshot = false;
			c.ackTick = m_sim->Tick();
			c.sentThrough = c.ackTick;
			c.stateAck = kNoStateBase;
			c.stateSent = 0;
			c.known.clear();
			c.sent.clear();
			c.acks.clear();
			m_bench.resyncs += 1;
			continue;
		}

		if ( built == false )
		{
			m_sim->SavePortable( m_image );
			built = true;
		}

		MsgWelcome welcome;
		welcome.fingerprint = BuildFingerprint();
		welcome.config = m_options.config;
		welcome.slot = c.slot;
		welcome.reconnectToken = c.token;
		welcome.snapshotTick = m_sim->Tick();
		welcome.baseInputs = m_lastInputs;
		welcome.mapHash = m_mapHash;
		welcome.map = m_mapBytes;
		welcome.image = m_image;
		welcome.schema = m_schemaBytes;
		welcome.model = m_options.netModel;
		welcome.stateEvery = uint8_t( m_options.stateEvery );
		Encode( welcome, m_buffer );
		m_transport.Send( c.peer, ChannelReliable, m_buffer, true );

		c.needsSnapshot = false;
		c.welcomed = true;
		SendNames( c.peer );
		SendPrivates( c );
		// The welcome carries the inputs of snapshotTick - 1, so frames start at snapshotTick.
		c.ackTick = m_sim->Tick();
		c.sentThrough = c.ackTick;
		c.stateAck = kNoStateBase;
		c.stateSent = 0;
		c.known.clear();
		c.sent.clear();
		m_stats.snapshotsSent += 1;
	}
}

void GameServer::RunTick( double now )
{
	uint32_t tick = m_sim->Tick();

	InputFrame frame;
	frame.tick = tick;
	frame.events.swap( m_pendingEvents );

	for ( Client& c : m_clients )
	{
		if ( c.used == false || c.inWorld == false )
		{
			continue;
		}
		if ( c.fake )
		{
			const auto from = std::chrono::steady_clock::now();
			c.lastInput = FakeInput( c );
			frame.inputs[c.slot] = c.lastInput;
			m_bench.fakeInputMs += BenchCounts() ? std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - from ).count() : 0.0;
			continue;
		}

		if ( c.connected && c.welcomed )
		{
			m_stats.inputTicks += 1;
			const Client::Slot& s = c.inputs[tick % kInputBuffer];
			if ( s.tick == tick )
			{
				c.lastInput = s.input;
			}
			else
			{
				m_stats.lateInputs += 1;
			}
		}
		else
		{
			c.lastInput = {};
		}
		frame.inputs[c.slot] = c.lastInput;
	}

	// What did not fit in the frames before first (Context::SetOverflow), as many as fit.
	if ( m_overflow.empty() == false )
	{
		const size_t take = std::min( m_overflow.size(), kMaxCommandsPerFrame );
		frame.commands.insert( frame.commands.end(), m_overflow.begin(), m_overflow.begin() + std::ptrdiff_t( take ) );
		m_overflow.erase( m_overflow.begin(), m_overflow.begin() + std::ptrdiff_t( take ) );
	}
	const bool overflowing = m_overflow.empty() == false;
	const auto modsFrom = std::chrono::steady_clock::now();
	RunMods( frame );
	m_stats.modsMs += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - modsFrom ).count();
	// The bodies the map places, on the first tick (as commands, so every simulation has them, a
	// joining one from the snapshot). After the mods' and by the same rule (Context::Add): until
	// M252's review they went past a full frame, simulated here and cut from the frame sent (a
	// server's fake players all join on the first tick and fill it).
	for ( const LevelBody& body : m_map.bodies )
	{
		if ( m_mapBodiesPlaced )
		{
			break;
		}
		SimCommand c;
		c.type = CommandType::SpawnBody;
		for ( size_t i = 0; i < m_schema.characters.size(); ++i )
		{
			c.index = m_schema.characters[i].name == body.character ? uint16_t( i ) : c.index;
		}
		c.a = { body.feet.x, body.feet.y, body.feet.z };
		c.value = int32_t( detmath::RadiansToYaw( body.yaw ) );
		if ( frame.commands.size() < kMaxCommandsPerFrame && m_overflow.empty() )
		{
			frame.commands.push_back( c );
		}
		else
		{
			m_overflow.push_back( c );
		}
	}
	m_mapBodiesPlaced = true;
	if ( overflowing == false && m_overflow.empty() == false )
	{
		Log( "tick %u: the commands are more than a frame holds (%zu): %zu wait for the next tick", tick, kMaxCommandsPerFrame, m_overflow.size() );
	}

	// Joining / reconnecting / desynced clients get the state before this tick.
	for ( Client& c : m_clients )
	{
		if ( c.used && c.connected && c.needsSnapshot )
		{
			c.lastResyncAt = now;
		}
	}
	SendSnapshots();

	const auto stepFrom = std::chrono::steady_clock::now();
	m_sim->Step( frame );
	m_stats.stepMs += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - stepFrom ).count();
	RecordPast();

	m_lastInputs = frame.inputs;
	m_replay.AddFrame( frame );
	RecordView( frame );
	if ( m_options.netModel == NetModel::Hybrid )
	{
		// What its clients' physics is given for the tick: the physics events and pushers' places
		// it made (the replay has the frame as played: it makes them again).
		frame.physics = m_sim->PhysicsMade().physics;
		frame.pushers = m_sim->PhysicsMade().pushers;
	}
	if ( m_bench.fakes > 0 )
	{
		CountFrame( frame );
	}
	m_history[tick % kFrameHistory] = std::move( frame );

	uint32_t stateTick = tick + 1;
	uint64_t hash = 0;
	bool haveHash = false;
	if ( m_options.recordHashes )
	{
		hash = m_sim->ComputeHash();
		haveHash = true;
		m_hashes.push_back( hash );
	}

	if ( m_replay.IsOpen() && m_options.replayChecksumInterval > 0 && stateTick % m_options.replayChecksumInterval == 0 )
	{
		if ( haveHash == false )
		{
			hash = m_sim->ComputeHash();
			haveHash = true;
		}
		m_replay.AddChecksum( stateTick, hash );
		if ( stateTick % ( 10 * m_options.replayChecksumInterval ) == 0 )
		{
			m_replay.Flush();
		}
	}

	// The hybrid: the physics checksum, every few ticks; the characters' state, every few ticks (as
	// they are now, compact: what clients are sent), kept for the clients' acknowledgements.
	if ( m_options.netModel == NetModel::Hybrid )
	{
		m_physicsSums.resize( kFrameHistory );
		m_physicsSums[tick % kFrameHistory] =
			m_options.checksumInterval > 0 && stateTick % m_options.checksumInterval == 0 ? m_sim->PhysicsHash() : 0;
	}
	if ( m_options.netModel == NetModel::Hybrid && stateTick % m_options.stateEvery == 0 && ConnectedClients() > 0 )
	{
		auto start = std::chrono::steady_clock::now();
		StateFrame state;
		state.tick = stateTick;
		state.inputs = m_lastInputs;
		m_sim->SaveState( state.state, true );
		m_states.push_back( std::move( state ) );
		KeptLayout kept;
		if ( net::ReadLayout( m_states.back().state, kept.layout ) )
		{
			// Where everything is (by NetId, as the state has them): what a client is near.
			kept.placed.resize( kept.layout.entities.size() );
			for ( size_t i = 0; i < kept.layout.entities.size(); ++i )
			{
				const uint32_t netId = kept.layout.entities[i].netId;
				const flecs::entity e = m_sim->FindEntity( netId );
				const cb::HeldItem* held = e.is_valid() ? e.try_get<cb::HeldItem>() : nullptr;
				const uint32_t holder = held != nullptr ? held->holder : 0;
				const Transform* at = m_sim->EntityTransform( holder != 0 ? holder : netId );
				kept.placed[i] = { at != nullptr ? at->position : b3Vec3{}, holder };
			}
			kept.head.resize( kept.layout.entities.size() );
			for ( size_t i = 0; i < kept.layout.entities.size(); ++i )
			{
				const net::StateSpan* holder = kept.placed[i].holder != 0 ? kept.layout.Find( kept.placed[i].holder ) : nullptr;
				kept.head[i] = holder != nullptr ? uint32_t( holder - kept.layout.entities.data() ) : uint32_t( i );
			}
			KeepPuppets( kept );
		}
		m_layouts.push_back( std::move( kept ) );
		while ( m_states.size() > kStateHistory )
		{
			m_states.pop_front();
			m_layouts.pop_front();
		}
		const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
		m_stats.stateMsTotal += ms;
		m_stats.saveMs += ms;
	}

	if ( m_options.checksumInterval > 0 && stateTick % m_options.checksumInterval == 0 && m_options.netModel == NetModel::Inputs )
	{
		if ( haveHash == false )
		{
			hash = m_sim->ComputeHash();
		}
		Encode( MsgChecksum{ stateTick, hash }, m_buffer );
		for ( const Client& c : m_clients )
		{
			if ( c.used && c.connected && c.welcomed )
			{
				Deliver( c, m_buffer, true, &CrowdBench::other );
			}
		}
	}

	if ( m_tickObserver )
	{
		m_tickObserver();
	}
}

// The mods see this tick's inputs and the world before it, and add their commands to the frame.
void GameServer::RunMods( InputFrame& frame )
{
	if ( m_mods.empty() )
	{
		return;
	}
	mods::Context ctx( *m_sim, m_schema, frame, m_lastInputs, *m_modWorld, m_modRng );
	ctx.SetOptions( &m_options.modOptions );
	ctx.SetItemProperties( &m_itemProperties );
	ctx.SetLayers( &m_layerWishes, &m_itemLayers );
	ctx.SetPrivates( &m_privates, &m_privatesChanged );
	ctx.SetHitTester( m_hits.get() );
	ctx.SetNavMesh( &m_nav );
	ctx.SetOverflow( &m_overflow );
	ctx.SetTexts( &m_typed, &m_said );
	// Each player's view of the others, as its input says it saw them (this tick's mods see the
	// world as of Tick(): a view `seenBack` ticks before, kept by RecordPast), no further back
	// than lagCompensationTicks.
	m_seenBy.fill( nullptr );
	if ( m_options.netModel == NetModel::Hybrid && m_past.empty() == false )
	{
		for ( int slot = 0; slot < kMaxPlayers; ++slot )
		{
			const uint32_t back = std::min<uint32_t>( frame.inputs[slot].seenBack, m_options.LagCompensationTicks() );
			const uint32_t at = m_sim->Tick() - std::min( back, m_sim->Tick() );
			for ( const PastPoses& p : m_past )
			{
				m_seenBy[size_t( slot )] = back > 0 && p.tick == at ? &p : m_seenBy[size_t( slot )];
			}
		}
	}
	ctx.SetPast( &m_seenBy );
	for ( const auto& mod : m_mods )
	{
		mod->Tick( ctx );
	}
	m_typed.clear();
	SendSaid();
	// What a mod told one player goes to that player, and to nobody else.
	for ( const Client& c : m_clients )
	{
		if ( c.used && m_privatesChanged[c.slot] )
		{
			SendPrivates( c );
		}
	}
	m_privatesChanged.fill( false );
	// Layers: what mods asked for, then what held items bring.
	ctx.ResolveLayers();
	// Whatever the mods produced, clients must be able to apply exactly the same list.
	frame.commands.erase( std::remove_if( frame.commands.begin(), frame.commands.end(),
										  []( const SimCommand& c ) { return IsSendableCommand( c ) == false; } ),
						  frame.commands.end() );
}

void GameServer::HandleText( const Client& c, const MsgText& text )
{
	// Only into a channel a mod declared, and not more than a few a tick from one player (what is
	// too many for a chat is its mod's to say).
	if ( std::find( m_textChannels.begin(), m_textChannels.end(), text.channel ) == m_textChannels.end() )
	{
		return;
	}
	std::string words = SanitizeText( text.text, kMaxText );
	size_t mine = size_t( std::count_if( m_typed.begin(), m_typed.end(), [&]( const mods::TypedText& t ) { return t.slot == c.slot; } ) );
	if ( words.empty() || mine >= 4 )
	{
		return;
	}
	m_typed.push_back( { c.slot, text.channel, std::move( words ) } );
}

void GameServer::SendSaid()
{
	for ( const mods::SaidText& said : m_said )
	{
		MsgText msg;
		msg.channel = said.channel;
		msg.from = said.from;
		msg.text = said.text;
		Encode( msg, m_buffer );
		for ( const Client& c : m_clients )
		{
			if ( c.used && c.connected && c.welcomed && ( said.to == kNoPlayer || said.to == c.slot ) )
			{
				Deliver( c, m_buffer, true, &CrowdBench::other );
			}
		}
	}
	m_said.clear();
}

std::string GameServer::UniqueName( const std::string& wanted, const Client& self ) const
{
	auto taken = [&]( const std::string& name ) {
		for ( const Client& c : m_clients )
		{
			if ( &c != &self && c.used && c.name == name )
			{
				return true;
			}
		}
		return false;
	};
	if ( taken( wanted ) == false )
	{
		return wanted;
	}
	for ( int n = 2;; ++n )
	{
		std::string suffix = " (" + std::to_string( n ) + ")";
		std::string candidate = wanted.substr( 0, kMaxPlayerName - suffix.size() ) + suffix;
		if ( taken( candidate ) == false )
		{
			return candidate;
		}
	}
}

void GameServer::SendPrivates( const Client& c )
{
	if ( c.connected == false || c.welcomed == false )
	{
		return;
	}
	MsgPrivateFields msg;
	msg.values = m_privates[c.slot];
	Encode( msg, m_buffer );
	Deliver( c, m_buffer, true, &CrowdBench::other );
}

bool GameServer::ReplaceItem( const ModItem& item )
{
	bool known = false;
	for ( ModItem& mine : m_schema.items )
	{
		if ( mine.mod == item.mod )
		{
			mine.sha256 = item.sha256;
			known = true;
		}
	}
	if ( known == false )
	{
		return false;
	}
	// (Whoever joins from now on gets it in the welcome's schema.)
	EncodeSchema( m_schema, m_schemaBytes );
	MsgItems msg;
	msg.items = m_schema.items;
	Encode( msg, m_itemsBuffer );
	for ( const Client& c : m_clients )
	{
		if ( c.used && c.connected && c.welcomed )
		{
			Deliver( c, m_itemsBuffer, true, &CrowdBench::other );
		}
	}
	return true;
}

void GameServer::SendNames( PeerId only )
{
	MsgPlayerNames msg;
	for ( const Client& c : m_clients )
	{
		if ( c.used )
		{
			msg.names.push_back( { c.slot, c.name } );
		}
	}
	Encode( msg, m_buffer );
	for ( const Client& c : m_clients )
	{
		if ( c.used && c.connected && c.welcomed && ( only == 0 || c.peer == only ) )
		{
			Deliver( c, m_buffer, true, &CrowdBench::other );
		}
	}
}

void GameServer::RecordView( const InputFrame& frame )
{
	if ( m_viewFile.IsOpen() == false )
	{
		return;
	}
	present::ViewFrame& v = m_viewFrame;
	v.state = "playing";
	v.rate = 1.0f;
	if ( v.schemaGeneration == 0 )
	{
		// The session: fixed for as long as the server runs.
		v.mapHash = m_mapHash;
		v.mapName = m_map.name;
		for ( const EntityTemplate& t : m_map.templates )
		{
			v.templateNames.push_back( t.name );
			v.templateVisuals.push_back( t.visual );
		}
		v.schema = m_schema;
		v.schemaGeneration = 1;
	}
	std::array<std::string, kMaxPlayers> names;
	for ( const Client& c : m_clients )
	{
		if ( c.used )
		{
			names[c.slot] = c.name;
		}
	}
	if ( v.namesGeneration == 0 || names != v.names )
	{
		v.names = names;
		v.namesGeneration += 1;
	}
	present::CaptureFrame( *m_sim, v.frame );
	v.frame.resetGeneration = 1;
	v.frame.hasInputs = true;
	v.frame.inputs = frame.inputs;
	v.hasWorld = true;
	v.stats.clear();
	v.stats.push_back( { "tick", int64_t( v.frame.tick ) } );
	m_viewFile.Add( v );
	if ( v.frame.tick % 600 == 0 )
	{
		m_viewFile.Flush();
	}
}

const InputFrame* GameServer::HistoryFrame( uint32_t tick ) const
{
	const InputFrame& f = m_history[tick % kFrameHistory];
	return f.tick == tick ? &f : nullptr;
}

// Every client gets all frames from its acknowledgement up to the newest, unreliably, every tick.
// Clients that acknowledge the same tick share one encoded batch.
void GameServer::SendFrames( double now )
{
	uint32_t newest = m_sim->Tick() - 1;
	struct Encoded
	{
		uint32_t ack;
		size_t frames;
		std::vector<uint8_t> bytes;
	};
	std::vector<Encoded> cache;
	std::vector<const InputFrame*> frames;

	for ( Client& c : m_clients )
	{
		if ( c.used == false || c.connected == false || c.welcomed == false || c.ackTick > newest )
		{
			continue;
		}

		if ( newest - c.ackTick >= kFrameHistory - 1 )
		{
			// Too far behind to catch up with frames; send the state instead.
			if ( c.needsSnapshot == false )
			{
				Log( "slot %u is %u ticks behind, resending state", c.slot, newest - c.ackTick );
				m_stats.ackTooOld += 1;
				c.needsSnapshot = true;
				c.lastResyncAt = now;
			}
			continue;
		}

		// Where this packet starts. Usually where the packet two before it began its new frames:
		// each frame is in three packets, so two lost in a row cost nothing. Counted in packets,
		// and one packet an update: a server that steps several ticks in one update (a hitch) sends
		// them in one packet. (M138 counted ticks and sent a packet a tick: those packets left in
		// one datagram and were lost together, and a client waited for the resend below far more
		// often than three losses in a row would have it.) From the oldest unacknowledged frame when
		// that has been out for longer than an answer takes: it, or what followed it, was lost.
		uint32_t first = c.ackTick;
		bool waited = c.ackTick < c.sentThrough && now - c.frameSentAt[c.ackTick % 256] > std::max( 0.06, c.roundTrip * 1.5 + 0.03 );
		if ( waited == false && c.sentThrough > c.ackTick )
		{
			first = std::max( c.ackTick, std::min( c.sentBefore[1], c.sentThrough ) );
		}
		c.sentBefore[1] = c.sentBefore[0];
		c.sentBefore[0] = c.sentThrough;
		if ( waited )
		{
			m_stats.framesResent += newest + 1 - first;
		}
		for ( uint32_t t = first; t <= newest; ++t )
		{
			// (When it went out: the first time, or again from the start after a wait.)
			if ( t >= c.sentThrough || waited )
			{
				c.frameSentAt[t % 256] = now;
			}
		}
		c.sentThrough = std::max( c.sentThrough, newest + 1 );

		Encoded* hit = nullptr;
		for ( Encoded& e : cache )
		{
			if ( e.ack == first )
			{
				hit = &e;
				break;
			}
		}
		if ( hit == nullptr )
		{
			frames.clear();
			for ( uint32_t t = first; t <= newest && frames.size() < kMaxBatchFrames; ++t )
			{
				frames.push_back( HistoryFrame( t ) );
			}
			InputArray base{};
			if ( first > 0 )
			{
				base = HistoryFrame( first - 1 ) ? HistoryFrame( first - 1 )->inputs : m_lastInputs;
			}
			cache.push_back( { first, frames.size(), {} } );
			hit = &cache.back();
			if ( m_options.netModel == NetModel::Hybrid )
			{
				// (Without the inputs: the clients simulate no one else's character. With the physics
				// checksums.)
				std::vector<uint64_t> sums( frames.size(), 0 );
				for ( size_t i = 0; i < frames.size(); ++i )
				{
					sums[i] = frames[i] != nullptr && m_physicsSums.size() == kFrameHistory ? m_physicsSums[frames[i]->tick % kFrameHistory] : 0;
				}
				EncodeFrameBatch( base, frames.data(), frames.size(), hit->bytes, false, sums.data() );
			}
			else
			{
				EncodeFrameBatch( base, frames.data(), frames.size(), hit->bytes );
			}
		}
		// Out of order is fine, dropped for being overtaken is not: a packet holds frames the next
		// one may not (it starts later), and the client keeps one it cannot use yet.
		Deliver( c, hit->bytes, false, &CrowdBench::frames );
		m_stats.batchesSent += 1;
		m_stats.framesSent += hit->frames;
	}
}

const StateFrame* GameServer::KeptState( uint32_t tick ) const
{
	for ( const StateFrame& s : m_states )
	{
		if ( s.tick == tick )
		{
			return &s;
		}
	}
	return nullptr;
}

void GameServer::RecordPast()
{
	// The hybrid: where every character is and how it stands, after the step (as of Tick(): the
	// state of this tick is what clients are sent, and stand the others as).
	if ( m_options.netModel != NetModel::Hybrid || m_options.LagCompensationTicks() == 0 )
	{
		m_past.clear();
		return;
	}
	PastPoses now;
	if ( m_past.size() > m_options.LagCompensationTicks() + 1 )
	{
		now = std::move( m_past.front() ); // (its buffer, again)
		m_past.pop_front();
	}
	now.tick = m_sim->Tick();
	now.poses.clear();
	for ( const Simulation::EntityRef& r : m_sim->Entities() )
	{
		const flecs::entity e( m_sim->World(), r.entity );
		const Character* c = e.try_get<Character>();
		const Transform* t = c != nullptr ? m_sim->EntityTransform( r.netId ) : nullptr;
		AnimStateCopy anim = t != nullptr ? m_sim->EntityAnimState( r.netId ) : AnimStateCopy{};
		if ( anim != nullptr )
		{
			now.poses.push_back( { r.netId, *t, *anim, c->dead != 0 } );
		}
	}
	m_past.push_back( std::move( now ) );
}

const GameServer::KeptLayout* GameServer::KeptLayoutOf( uint32_t tick ) const
{
	// (Kept in tick order: found by halving.)
	size_t lo = 0, hi = m_states.size();
	while ( lo < hi )
	{
		const size_t mid = ( lo + hi ) / 2;
		if ( m_states[mid].tick < tick )
		{
			lo = mid + 1;
		}
		else
		{
			hi = mid;
		}
	}
	return lo < m_states.size() && m_states[lo].tick == tick && lo < m_layouts.size() ? &m_layouts[lo] : nullptr;
}

void GameServer::KeepPuppets( KeptLayout& kept ) const
{
	const size_t n = kept.layout.entities.size();
	kept.puppetGroup.assign( n, 0 );
	kept.puppets.clear();
	kept.viewBytes.clear();
	kept.fullHash.assign( n, 0 );
	kept.viewHash.assign( n, 0 );
	// Every character a client may draw as a puppet, its slot's last input saying how it looks.
	std::vector<int32_t> puppetAt( n, -1 );
	for ( size_t i = 0; i < n; ++i )
	{
		const uint32_t netId = kept.layout.entities[i].netId;
		const flecs::entity e = m_sim->FindEntity( netId );
		if ( m_sim->IsPuppet( e ) == false )
		{
			continue;
		}
		const PlayerSlot slot = e.get<Character>().slot;
		const PlayerInput* input = slot < kMaxPlayers && m_sim->PlayerNetId( slot ) == netId ? &m_lastInputs[slot] : nullptr;
		Puppet p;
		if ( m_sim->PuppetOf( netId, input, p ) )
		{
			puppetAt[i] = int32_t( kept.puppets.size() );
			kept.puppets.push_back( p );
			kept.puppetGroup[i] = 1;
		}
	}
	// And what each holds (its group's).
	for ( size_t i = 0; i < n; ++i )
	{
		if ( kept.placed[i].holder != 0 && kept.head[i] != i && kept.puppetGroup[kept.head[i]] == 1 )
		{
			kept.puppetGroup[i] = 2;
		}
	}
	// Their views, then where the spans find them (once nothing moves the bytes).
	std::vector<size_t> viewAt( n, SIZE_MAX );
	std::vector<uint8_t> view;
	for ( size_t i = 0; i < n; ++i )
	{
		if ( kept.puppetGroup[i] != 0 )
		{
			const net::StateSpan& s = kept.layout.entities[i];
			m_sim->PuppetView( s.record, s.recordSize, view );
			viewAt[i] = kept.viewBytes.size();
			kept.viewBytes.insert( kept.viewBytes.end(), view.begin(), view.end() );
		}
	}
	// What a client is sent of each, hashed: its record (or view), its bodies, its puppet.
	auto version = []( const uint8_t* record, uint32_t size, uint64_t bodies, const Puppet* puppet ) {
		uint64_t h = HashBytes( kHashSeed, record, size );
		h = HashBytes( h, &bodies, sizeof( bodies ) );
		return puppet != nullptr ? HashBytes( h, puppet, sizeof( Puppet ) ) : h;
	};
	for ( size_t i = 0; i < n; ++i )
	{
		net::StateSpan& s = kept.layout.entities[i];
		s.view = viewAt[i] != SIZE_MAX ? kept.viewBytes.data() + viewAt[i] : nullptr;
		s.viewSize = s.view != nullptr ? s.recordSize : 0;
		s.puppet = puppetAt[i] >= 0 ? &kept.puppets[size_t( puppetAt[i] )] : nullptr;
		kept.fullHash[i] = version( s.record, s.recordSize, s.hash, nullptr );
		kept.viewHash[i] = s.view != nullptr ? version( s.view, s.viewSize, s.hash, s.puppet ) : 0;
	}
}

const net::StateLayout* GameServer::LayoutOfTick( const void* server, uint32_t tick )
{
	const KeptLayout* kept = static_cast<const GameServer*>( server )->KeptLayoutOf( tick );
	return kept != nullptr ? &kept->layout : nullptr;
}

namespace
{

// How often a client is sent the record of a character this far from its own (and of what that
// one holds): every state near, every 2nd, 4th or 8th further off (M250).
uint32_t StatePeriod( float distance )
{
	return distance < 25.0f ? 1u : distance < 50.0f ? 2u : distance < 100.0f ? 4u : 8u;
}

// How old the state a client's record of an entity is from may be, in states, for a change
// against it (the client keeps 32: GameClient::HandleState).
constexpr uint32_t kOldestBase = 24;

} // namespace

void GameServer::StateAcknowledged( Client& c, uint32_t tick )
{
	// (Acknowledgements only move forward: the states sent before this one are done with too.)
	while ( c.sent.empty() == false && c.sent.front().tick < tick )
	{
		c.sent.pop_front();
	}
	if ( c.sent.empty() || c.sent.front().tick != tick )
	{
		return;
	}
	const Client::Sent& sent = c.sent.front();
	auto find = [&]( uint32_t netId ) {
		auto it = std::lower_bound( c.known.begin(), c.known.end(), netId, []( const Client::Known& k, uint32_t id ) { return k.netId < id; } );
		return it != c.known.end() && it->netId == netId ? it : c.known.end();
	};
	for ( size_t i = 0; i < sent.updated.size(); ++i )
	{
		if ( auto it = find( sent.updated[i] ); it != c.known.end() && it->removing == false && tick >= it->acked )
		{
			it->acked = tick;
			it->ackedHash = i < sent.hashes.size() ? sent.hashes[i] : 0;
			it->ackedPuppet = i < sent.puppets.size() && sent.puppets[i] != 0;
		}
	}
	for ( uint32_t netId : sent.removed )
	{
		if ( auto it = find( netId ); it != c.known.end() && it->removing )
		{
			c.known.erase( it );
		}
	}
	c.sent.pop_front();
}

// What a client is sent of the newest state, by distance (SendStates). Reads only the plan, the
// client's own bookkeeping and the kept states: a worker thread's.
void GameServer::PlanState( Plan& plan, Client& c, const StateFrame& newest, const KeptLayout& now,
							const std::vector<std::pair<uint32_t, PlayerSlot>>& players )
{
	const uint32_t every = std::max( m_options.stateEvery, 1u );
	const uint32_t baseTick = plan.base != nullptr ? plan.base->tick : kNoStateBase;
	plan.updates.clear();
	plan.inputSlots.clear();
	plan.says.clear();
	plan.sent.tick = newest.tick;
	plan.sent.updated.clear();
	plan.sent.removed.clear();
	plan.next.clear();
	plan.next.reserve( c.known.size() + 16 );
	plan.updates.reserve( now.layout.entities.size() );
	plan.sent.hashes.clear();
	plan.sent.puppets.clear();
	size_t k = 0;
	auto gone = [&]( const Client::Known& had ) {
		Client::Known removing = had;
		removing.removing = true;
		plan.next.push_back( removing );
		plan.sent.removed.push_back( had.netId );
	};
	// Whether a state is kept, by tick: the same few ticks come up for most entities.
	uint32_t lastAsked = 0;
	bool lastKept = false;
	auto keptAt = [&]( uint32_t tick ) {
		if ( tick != lastAsked )
		{
			lastAsked = tick;
			lastKept = newest.tick - tick <= kOldestBase * every && KeptLayoutOf( tick ) != nullptr;
		}
		return lastKept;
	};
	// How each entity stands: had or not, and how overdue (the time since its last update, in
	// intervals of its distance's: 1 is due). Its own character and what it holds, and what it was
	// never sent, before anything else.
	plan.entries.resize( now.layout.entities.size() );
	plan.due.clear();
	for ( size_t i = 0; i < now.layout.entities.size(); ++i )
	{
		const uint32_t netId = now.layout.entities[i].netId;
		while ( k < c.known.size() && c.known[k].netId < netId )
		{
			gone( c.known[k++] );
		}
		Plan::Entry& entry = plan.entries[i];
		entry.had = k < c.known.size() && c.known[k].netId == netId ? &c.known[k++] : nullptr;
		entry.chosen = false;
		const Placed& placed = now.placed[i];
		const bool mine = plan.ownId != 0 && ( netId == plan.ownId || placed.holder == plan.ownId );
		// Another's character, or what another's holds: as a puppet (M253). A record it has of the
		// other kind is no base for this one.
		entry.puppet = mine == false && i < now.puppetGroup.size() && now.puppetGroup[i] != 0;
		const bool had = entry.had != nullptr && entry.had->removing == false;
		const bool otherKind = had && entry.had->acked != 0 && entry.had->ackedPuppet != entry.puppet;
		entry.acked = had && otherKind == false ? entry.had->acked : 0;
		const uint64_t version = entry.puppet ? now.viewHash[i] : now.fullHash[i];
		entry.same = entry.acked != 0 && entry.had->ackedHash == version && entry.had->sentHash == version;
		const uint32_t period = plan.hasOwn == false ? 1u : StatePeriod( b3Length( b3Sub( placed.at, plan.ownAt ) ) );
		const uint32_t sentAt = entry.had != nullptr && otherKind == false ? entry.had->sentAt : 0;
		const float overdue = sentAt == 0 ? 1e3f : float( newest.tick - sentAt ) / float( period * every );
		// (One never sent first; one sent and not acknowledged yet waits its turn as any other does,
		// and goes whole again: resent every state, a crowd's new ones took a small budget's every
		// slot while acknowledgements were slow.)
		entry.score = mine ? 1e9f : sentAt == 0 ? 1e6f : overdue >= 1.0f ? overdue : 0.0f;
	}
	while ( k < c.known.size() )
	{
		gone( c.known[k++] );
	}
	// By group (a character with what it holds): as overdue as its most overdue.
	const size_t n = now.layout.entities.size();
	plan.groupScore.assign( n, 0.0f );
	plan.groupChosen.assign( n, 0 );
	std::vector<uint32_t> sizes; // (by head)
	sizes.assign( n, 0 );
	for ( size_t i = 0; i < n; ++i )
	{
		// (What it has already costs nothing: it is not sent.)
		const uint32_t h = now.head[i];
		plan.groupScore[h] = std::max( plan.groupScore[h], plan.entries[i].score );
		sizes[h] += plan.entries[i].same ? 0 : 1;
	}
	// The budget: the most overdue groups first, whole (the rest stay overdue, and come first next
	// time); the own one aside.
	plan.due.clear();
	for ( size_t h = 0; h < n; ++h )
	{
		if ( sizes[h] > 0 && plan.groupScore[h] > 0.0f )
		{
			plan.due.push_back( { plan.groupScore[h], uint32_t( h ) } );
		}
	}
	std::sort( plan.due.begin(), plan.due.end(), []( const auto& x, const auto& y ) { return x.first > y.first || ( x.first == y.first && x.second < y.second ); } );
	size_t spent = 0;
	for ( const auto& [score, h] : plan.due )
	{
		const bool own = score >= 1e9f;
		if ( own == false && spent > 0 && spent + sizes[h] > m_options.stateBudget )
		{
			continue; // (a smaller group may still fit; the first goes, however big)
		}
		plan.groupChosen[h] = 1;
		spent += own ? 0 : sizes[h];
	}
	for ( size_t i = 0; i < n; ++i )
	{
		plan.entries[i].chosen = plan.groupChosen[now.head[i]] != 0;
	}
	// What it will have: the gone ones are in `next` already (in NetId order, from the walk above),
	// the rest follow (in the state's order), and the two are merged into one NetId order below. The
	// next state's walk needs it: out of order (M250 left it so), everything after a gone entity was
	// taken as new, sent whole in every state, and the list grew by the gone ones it no longer found
	// (found by the crowd bench, M251: fighting fake players, 87 records whole a state at 64).
	const size_t goneCount = plan.next.size();
	for ( size_t i = 0; i < now.layout.entities.size(); ++i )
	{
		const uint32_t netId = now.layout.entities[i].netId;
		const Plan::Entry& entry = plan.entries[i];
		const bool had = entry.had != nullptr && entry.had->removing == false;
		if ( entry.chosen && entry.same )
		{
			// What it has (acknowledged, and nothing else sent since): not sent, its turn taken.
			Client::Known known = *entry.had;
			known.sentAt = newest.tick;
			plan.next.push_back( known );
		}
		else if ( entry.chosen )
		{
			// Against the record it has, if that one is still kept here and there.
			const bool kept = entry.acked != 0 && keptAt( entry.acked );
			const uint64_t version = entry.puppet ? now.viewHash[i] : now.fullHash[i];
			plan.updates.push_back( { netId, kept ? entry.acked : 0u, entry.puppet } );
			plan.sent.updated.push_back( netId );
			plan.sent.hashes.push_back( version );
			plan.sent.puppets.push_back( entry.puppet ? 1 : 0 );
			Client::Known known = had ? *entry.had : Client::Known{};
			known.netId = netId;
			known.removing = false;
			known.sentAt = newest.tick;
			known.sentHash = version;
			plan.next.push_back( known );
		}
		else if ( entry.had != nullptr )
		{
			plan.next.push_back( *entry.had );
		}
	}
	std::inplace_merge( plan.next.begin(), plan.next.begin() + std::ptrdiff_t( goneCount ), plan.next.end(),
						[]( const Client::Known& a, const Client::Known& b ) { return a.netId < b.netId; } );
	// A slot's input goes with its character's update (not a puppet's: where it looks is in it).
	size_t pl = 0;
	for ( const net::EntityUpdate& u : plan.updates )
	{
		if ( u.puppet )
		{
			continue;
		}
		while ( pl < players.size() && players[pl].first < u.netId )
		{
			++pl;
		}
		if ( pl < players.size() && players[pl].first == u.netId )
		{
			plan.inputSlots.push_back( players[pl].second );
		}
	}
	std::sort( plan.inputSlots.begin(), plan.inputSlots.end() );
	plan.says.push_back( baseTick );
	plan.says.push_back( uint32_t( plan.updates.size() ) );
	for ( const net::EntityUpdate& u : plan.updates )
	{
		plan.says.push_back( u.netId );
		plan.says.push_back( u.baseTick );
		plan.says.push_back( u.puppet ? 1u : 0u );
	}
	plan.says.push_back( uint32_t( plan.sent.removed.size() ) );
	plan.says.insert( plan.says.end(), plan.sent.removed.begin(), plan.sent.removed.end() );
	plan.says.insert( plan.says.end(), plan.inputSlots.begin(), plan.inputSlots.end() );
	// (A word at a time: only to tell plans apart quickly, compared in full when equal.)
	uint64_t hash = kHashSeed;
	for ( uint32_t v : plan.says )
	{
		hash = ( hash ^ v ) * 0x100000001b3ull;
	}
	plan.hash = hash;
}

// The newest state to every client that does not have it, as a change against the newest it has
// (whole when that is no longer kept, or it has none). Unreliable and in any order: the client
// keeps the newest, and the next one is a few ticks away.
//
// By distance (M250): each client is sent updates. Its own character, what it holds, everything
// near it, and what it was never sent, every state; a far one every few states (StatePeriod),
// each a change against the record of it the client last acknowledged; no more than stateBudget
// a state, the most overdue first. What is left out it keeps;
// what is gone is named until a state naming it is acknowledged. Each client's plan is made, and
// each different message encoded, on the state threads (stateThreads); the bytes are the same
// without them.
void GameServer::SendStates()
{
	if ( m_states.empty() )
	{
		return;
	}
	auto start = std::chrono::steady_clock::now();
	const StateFrame& newest = m_states.back();
	const KeptLayout* now = KeptLayoutOf( newest.tick );
	auto due = [&]( const Client& c ) { return c.used && c.connected && c.welcomed && c.stateSent < newest.tick; };

	if ( m_options.statesByDistance && now != nullptr )
	{
		m_stateWorkers.Start( m_options.stateThreads );
		// Whose slot each character is (by NetId), and where each client's own one is: read here,
		// so the threads read no simulation.
		std::vector<std::pair<uint32_t, PlayerSlot>> players;
		for ( int slot = 0; slot < kMaxPlayers; ++slot )
		{
			if ( const uint32_t id = m_sim->PlayerNetId( PlayerSlot( slot ) ); id != 0 )
			{
				players.push_back( { id, PlayerSlot( slot ) } );
			}
		}
		std::sort( players.begin(), players.end() );
		m_plans.resize( m_clients.size() );
		std::vector<size_t> planned;
		for ( size_t i = 0; i < m_clients.size(); ++i )
		{
			Client& c = m_clients[i];
			Plan& plan = m_plans[i];
			plan.active = due( c );
			plan.message = -1;
			if ( plan.active == false )
			{
				continue;
			}
			plan.base = c.stateAck != kNoStateBase ? KeptState( c.stateAck ) : nullptr;
			plan.ownId = m_sim->PlayerNetId( c.slot );
			const Transform* at = plan.ownId != 0 ? m_sim->EntityTransform( plan.ownId ) : nullptr;
			plan.hasOwn = at != nullptr;
			plan.ownAt = at != nullptr ? at->position : b3Vec3{};
			planned.push_back( i );
		}
		const auto planFrom = std::chrono::steady_clock::now();
		m_stateWorkers.Run( planned.size(), [&]( size_t n, int ) {
			const size_t i = planned[n];
			PlanState( m_plans[i], m_clients[i], newest, *now, players );
		} );
		const auto encodeFrom = std::chrono::steady_clock::now();
		m_bench.planMs += BenchCounts() ? std::chrono::duration<double, std::milli>( encodeFrom - planFrom ).count() : 0.0;
		// One message for the plans that say the same.
		m_messages.clear();
		for ( size_t i : planned )
		{
			Plan& plan = m_plans[i];
			for ( size_t m = 0; m < m_messages.size() && plan.message < 0; ++m )
			{
				const Plan& first = m_plans[m_messages[m].plan];
				plan.message = first.hash == plan.hash && first.says == plan.says ? int( m ) : -1;
			}
			if ( plan.message < 0 )
			{
				plan.message = int( m_messages.size() );
				m_messages.push_back( { i, false, {} } );
			}
		}
		m_updateCaches.resize( size_t( m_stateWorkers.Threads() ) + 1 );
		m_stateWorkers.Run( m_messages.size(), [&]( size_t m, int worker ) {
			Message& message = m_messages[m];
			const Plan& plan = m_plans[message.plan];
			message.ok = net::EncodeUpdates( newest, plan.base, plan.updates, plan.sent.removed, plan.inputSlots, &GameServer::LayoutOfTick, this,
											 message.bytes, &m_updateCaches[size_t( worker )] );
		} );
		m_bench.encodeMs += BenchCounts() ? std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - encodeFrom ).count() : 0.0;
		for ( size_t i : planned )
		{
			Client& c = m_clients[i];
			Plan& plan = m_plans[i];
			const Message& message = m_messages[size_t( plan.message )];
			if ( message.ok == false )
			{
				continue; // (whole, below)
			}
			if ( c.fake && c.slot % 8 == 0 && BenchCounts() )
			{
				const auto sampleFrom = std::chrono::steady_clock::now();
				SampleState( plan, c.slot, newest, *now, message.bytes );
				m_bench.sampleMs += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - sampleFrom ).count();
			}
			c.known.swap( plan.next );
			if ( c.fake )
			{
				// (What a player is thought to have may hold more than the state only by the gone
				// ones not yet acknowledged.)
				m_bench.knownMost = std::max<uint64_t>( m_bench.knownMost, c.known.size() );
				m_bench.entitiesMost = std::max<uint64_t>( m_bench.entitiesMost, now->layout.entities.size() );
			}
			c.sent.push_back( plan.sent );
			while ( c.sent.size() > kStateHistory )
			{
				c.sent.pop_front();
			}
			Deliver( c, message.bytes, false, &CrowdBench::states );
			c.stateSent = newest.tick;
			m_stats.statesSent += 1;
			m_stats.statesWhole += plan.base == nullptr ? 1 : 0;
			m_stats.stateBytes += message.bytes.size();
			m_bench.statesWhole += c.fake && plan.base == nullptr && BenchCounts() ? 1 : 0;
		}
	}

	// Every entity as it is now (and by distance, a client whose updates could not be made): clients
	// with the same base share one encoding. (By distance, what a client has is not the state of its
	// tick: whole, then, and it has it all.)
	struct Encoded
	{
		uint32_t base;
		std::vector<uint8_t> bytes;
	};
	std::vector<Encoded> cache;
	for ( Client& c : m_clients )
	{
		if ( due( c ) == false )
		{
			continue;
		}
		const StateFrame* base = c.stateAck != kNoStateBase ? KeptState( c.stateAck ) : nullptr;
		const StateFrame* against = m_options.statesByDistance ? nullptr : base;
		const uint32_t againstTick = against != nullptr ? against->tick : kNoStateBase;
		Encoded* hit = nullptr;
		for ( Encoded& e : cache )
		{
			hit = e.base == againstTick ? &e : hit;
		}
		if ( hit == nullptr )
		{
			cache.push_back( { againstTick, {} } );
			hit = &cache.back();
			EncodeState( newest, against, hit->bytes );
		}
		c.known.clear();
		c.sent.clear();
		Deliver( c, hit->bytes, false, &CrowdBench::states );
		c.stateSent = newest.tick;
		m_stats.statesSent += 1;
		m_stats.statesWhole += against == nullptr ? 1 : 0;
		m_stats.stateBytes += hit->bytes.size();
		m_bench.statesWhole += c.fake && against == nullptr && BenchCounts() ? 1 : 0;
	}
	m_stats.stateMsTotal += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
}

void GameServer::DropClientHard( PlayerSlot slot, double now )
{
	Client& c = m_clients[slot];
	if ( c.used && c.connected )
	{
		Log( "test: dropping slot %u without notice", slot );
		m_transport.DropHard( c.peer );
		c.connected = false;
		c.welcomed = false;
		c.peer = 0;
		c.disconnectedAt = now;
		c.lastInput = {};
	}
}

void GameServer::Deliver( const Client& c, const std::vector<uint8_t>& bytes, bool reliable, CrowdBench::Kind CrowdBench::*kind )
{
	if ( c.fake )
	{
		if ( BenchCounts() )
		{
			CrowdBench::Kind& counted = m_bench.*kind;
			counted.messages += 1;
			counted.bytes += bytes.size();
			m_outbox[c.slot].push_back( { uint32_t( bytes.size() ), reliable } );
		}
		return;
	}
	if ( reliable )
	{
		m_transport.Send( c.peer, ChannelReliable, bytes, true );
	}
	else
	{
		m_transport.SendUnordered( c.peer, ChannelInput, bytes );
	}
}

void GameServer::AddFakePlayers()
{
	const uint32_t count = std::min<uint32_t>( m_options.fakePlayers, uint32_t( m_clients.size() ) );
	m_bench = CrowdBench{};
	m_bench.fakes = count;
	m_bench.tickRate = m_options.config.tickRate;
	m_bench.fromTick = uint32_t( std::max( 0.0f, m_options.fakeWarmupSeconds ) * float( m_options.config.tickRate ) );
	if ( count == 0 )
	{
		return;
	}
	m_outbox.assign( m_clients.size(), {} );
	m_fakeTargets.assign( m_clients.size(), 0 );
	for ( const auto& [name, size] : m_sim->StateComponents() )
	{
		m_bench.components.push_back( name );
		m_bench.componentSizes.push_back( size );
	}
	m_bench.changedCharacters.assign( m_bench.components.size(), 0 );
	m_bench.changedItems.assign( m_bench.components.size(), 0 );
	m_bench.eventNames = m_schema.events;
	m_bench.events.assign( m_schema.events.size(), 0 );
	const ActionBits spawn = m_schema.ActionMask( "spawn_prop" );
	for ( uint32_t i = 0; i < count; ++i )
	{
		// The first slots: connected and welcomed from the start (nothing to load), so they are sent
		// what a player is from the first tick.
		Client& c = m_clients[i];
		c = Client{};
		c.used = true;
		c.connected = true;
		c.welcomed = true;
		c.inWorld = true;
		c.fake = true;
		c.slot = PlayerSlot( i );
		c.token = NextRandom( m_tokenState ) | 1;
		c.name = "fake " + std::to_string( i + 1 );
		c.brain = BotBrain( m_options.fakeSeed * 100003ull + i + 1 );
		c.brain.spawnAction = spawn;
		m_privates[c.slot] = Blackboard{};
		m_pendingEvents.push_back( { PlayerEventType::Join, c.slot } );
	}
	m_namesDirty = true;
	Log( "%u fake players (a round trip of %u ms%s%s)", count, m_options.fakeRoundTripMs,
		 m_options.fakeRadius > 0.0f ? ", near the spawn" : "", m_options.fakeFight ? ", fighting" : "" );
}

PlayerInput GameServer::FakeInput( Client& c )
{
	PlayerInput in = c.brain.Next();
	const uint32_t netId = m_sim->PlayerNetId( c.slot );
	const Transform* mine = netId != 0 ? m_sim->EntityTransform( netId ) : nullptr;
	if ( mine == nullptr )
	{
		return in;
	}
	auto face = [&]( b3Vec3 at ) {
		const b3Vec3 d = b3Sub( at, mine->position );
		in.cameraYaw = detmath::RadiansToYaw( detmath::Atan2( d.x, d.z ) );
	};
	if ( m_options.fakeRadius > 0.0f )
	{
		// Too far from the spawn's centre: back toward it (and on that way once inside).
		const b3Vec3 d = b3Sub( m_map.spawnCenter, mine->position );
		if ( d.x * d.x + d.z * d.z > m_options.fakeRadius * m_options.fakeRadius )
		{
			face( m_map.spawnCenter );
			in.moveForward = 127;
			in.moveRight = 0;
			c.brain.held.cameraYaw = in.cameraYaw;
		}
	}
	if ( m_options.fakeFight )
	{
		// Whatever is in the hand (the brawl kit's fist), used on the nearest player in reach:
		// faced, clicked for 3 ticks twice a second. Who that is is looked for every 10 ticks.
		const uint32_t tick = m_sim->Tick();
		// The first slot taken out while the hands are empty (an intent counts when its number
		// changes; the same slot's key again would empty them).
		if ( m_sim->SelectedSlot( netId ) == kNoSlot && ( tick + c.slot ) % 30 == 0 )
		{
			c.intents = uint8_t( c.intents + 1 );
		}
		in.intent = uint8_t( SlotIntent::Select );
		in.intentA = 0;
		in.intentSeq = c.intents;
		uint32_t& target = m_fakeTargets[c.slot]; // (its slot + 1; 0: none)
		if ( ( tick + c.slot ) % 10 == 0 )
		{
			target = 0;
			float best = 2.5f * 2.5f;
			for ( size_t slot = 0; slot < m_clients.size(); ++slot )
			{
				const uint32_t id = slot != c.slot ? m_sim->PlayerNetId( PlayerSlot( slot ) ) : 0;
				const Transform* t = id != 0 ? m_sim->EntityTransform( id ) : nullptr;
				const Character* who = t != nullptr ? m_sim->PlayerCharacter( PlayerSlot( slot ) ) : nullptr;
				if ( who == nullptr || who->dead )
				{
					continue;
				}
				const b3Vec3 d = b3Sub( t->position, mine->position );
				const float d2 = b3Dot( d, d );
				if ( d2 < best )
				{
					best = d2;
					target = uint32_t( slot ) + 1;
				}
			}
		}
		const uint32_t id = target != 0 ? m_sim->PlayerNetId( PlayerSlot( target - 1 ) ) : 0;
		if ( const Transform* t = id != 0 ? m_sim->EntityTransform( id ) : nullptr )
		{
			// Walking at it, slowly once close: in freelook the body turns toward where it walks, and the
			// fist hits where the body faces.
			face( t->position );
			in.cameraPitch = 0;
			in.moveRight = 0;
			in.moveForward = b3Distance( t->position, mine->position ) > 1.2f ? int8_t( 127 ) : int8_t( 24 );
			in.buttons &= uint8_t( ~BtnSprint );
			if ( ( tick + c.slot ) % 30 < 3 )
			{
				in.buttons |= BtnUse;
			}
		}
	}
	return in;
}

void GameServer::FakeAcknowledgements( double now )
{
	const uint32_t tick = m_sim->Tick();
	for ( Client& c : m_clients )
	{
		while ( c.fake && c.acks.empty() == false && c.acks.front().due <= tick )
		{
			MsgInput msg;
			msg.newestTick = tick;
			msg.ackTick = c.acks.front().ackTick;
			msg.stateAck = c.acks.front().stateAck;
			c.acks.pop_front();
			HandleInput( c, msg, now );
		}
	}
}

void GameServer::FakeSent()
{
	const uint32_t roundTrip = std::max<uint32_t>( 1, uint32_t( uint64_t( m_options.fakeRoundTripMs ) * m_options.config.tickRate / 1000 ) );
	for ( Client& c : m_clients )
	{
		if ( c.fake == false )
		{
			continue;
		}
		std::vector<std::pair<uint32_t, bool>>& box = m_outbox[c.slot];
		if ( box.empty() == false )
		{
			m_bench.wire += WireBytes( box, m_bench.datagrams );
			box.clear();
		}
		// What it has now, said a round trip later.
		c.acks.push_back( { m_sim->Tick() + roundTrip, c.sentThrough, c.stateSent } );
	}
}

void GameServer::CountFrame( const InputFrame& frame )
{
	if ( BenchCounts() == false )
	{
		// (Warming up: only where the event rings stand, so the first counted tick has only its own.)
		m_bench.impactCount = m_sim->Globals().impactCount;
		m_bench.modEventCount = m_sim->Globals().modEventCount;
		return;
	}
	m_bench.ticks += 1;
	for ( const SimCommand& command : frame.commands )
	{
		m_bench.commands[std::min<size_t>( size_t( command.type ), m_bench.commands.size() - 1 )] += 1;
	}
	m_bench.physicsEvents += frame.physics.size();
	m_bench.pushers += frame.pushers.size();
	// The tick's own events, from the rings (the newest kModEventHistory, if more came at once).
	const SimGlobals& g = m_sim->Globals();
	m_bench.impacts += g.impactCount - m_bench.impactCount;
	m_bench.impactCount = g.impactCount;
	const uint32_t from = std::max( m_bench.modEventCount, g.modEventCount - std::min( g.modEventCount, kModEventHistory ) );
	for ( uint32_t n = from; n < g.modEventCount; ++n )
	{
		const uint16_t type = g.modEvents[n % kModEventHistory].type;
		if ( type < m_bench.events.size() )
		{
			m_bench.events[type] += 1;
		}
	}
	m_bench.modEventCount = g.modEventCount;
	// The frame alone, as a batch of one, and again without each part.
	const bool inputs = m_options.netModel != NetModel::Hybrid;
	const InputFrame* before = frame.tick > 0 ? HistoryFrame( frame.tick - 1 ) : nullptr;
	const InputArray base = before != nullptr ? before->inputs : InputArray{};
	std::vector<uint8_t> out;
	auto size = [&]( const InputFrame& f ) {
		const InputFrame* one = &f;
		EncodeFrameBatch( base, &one, 1, out, inputs );
		return uint64_t( out.size() );
	};
	m_bench.frameAlone += size( frame );
	InputFrame part = frame;
	part.commands.clear();
	m_bench.frameNoCommands += size( part );
	part = frame;
	part.physics.clear();
	m_bench.frameNoPhysics += size( part );
	part = frame;
	part.pushers.clear();
	m_bench.frameNoPushers += size( part );
}

void GameServer::SampleState( const Plan& plan, PlayerSlot slot, const StateFrame& newest, const KeptLayout& now, const std::vector<uint8_t>& message )
{
	// Whose each update is: the player's own (its character, what it holds), another's character, or
	// what another holds. What changed in another's, against the record the player has.
	std::vector<net::EntityUpdate> withoutItems, ownOnly;
	for ( const net::EntityUpdate& u : plan.updates )
	{
		const net::StateSpan* span = now.layout.Find( u.netId );
		if ( span == nullptr )
		{
			continue;
		}
		const size_t i = size_t( span - now.layout.entities.data() );
		const uint32_t holder = now.placed[i].holder;
		if ( plan.ownId != 0 && ( u.netId == plan.ownId || holder == plan.ownId ) )
		{
			withoutItems.push_back( u );
			ownOnly.push_back( u );
			continue;
		}
		if ( holder != 0 )
		{
			m_bench.othersItemsUpdated += 1;
		}
		else
		{
			withoutItems.push_back( u );
			m_bench.othersUpdated += 1;
		}
		const KeptLayout* then = u.baseTick != 0 ? KeptLayoutOf( u.baseTick ) : nullptr;
		const net::StateSpan* was = then != nullptr ? then->layout.Find( u.netId ) : nullptr;
		if ( was == nullptr )
		{
			const Plan::Entry& entry = plan.entries[i];
			m_bench.wholeRecords += 1;
			m_bench.wholeRecordBytes += span->recordSize;
			m_bench.wholeNew += entry.had == nullptr ? 1 : 0;
			m_bench.wholeUnacknowledged += entry.had != nullptr && entry.acked == 0 ? 1 : 0;
			m_bench.wholeOld += entry.had != nullptr && entry.acked != 0 ? 1 : 0;
			m_bench.wholeCharacters += holder == 0 ? 1 : 0;
			continue;
		}
		// (A puppet's view, as it is sent: M253; and its puppet, as its change.)
		if ( u.puppet && span->view != nullptr && was->view != nullptr )
		{
			CountChangedBytes( span->view, span->viewSize, was->view, was->viewSize, m_bench.componentSizes,
							   holder != 0 ? m_bench.changedItems : m_bench.changedCharacters );
			if ( span->puppet != nullptr )
			{
				std::vector<uint8_t> change;
				net::EncodePuppet( *span->puppet, was->puppet, newest.tick, change );
				m_bench.puppets += 1;
				m_bench.puppetBytes += change.size();
				if ( const Puppet* a = span->puppet; was->puppet != nullptr )
				{
					const Puppet& b = *was->puppet;
					m_bench.puppetPlaces += a->x != b.x || a->y != b.y || a->z != b.z ? 1 : 0;
					m_bench.puppetFacings += a->facing != b.facing ? 1 : 0;
					m_bench.puppetAims += a->aimYaw != b.aimYaw || a->aimPitch != b.aimPitch ? 1 : 0;
					m_bench.puppetFlags += a->flags != b.flags ? 1 : 0;
					uint32_t layers = 0;
					for ( size_t l = 0; l < kPuppetLayers; ++l )
					{
						layers += a->layers[l] == b.layers[l] ? 0 : 1;
					}
					m_bench.puppetLayerChanges += layers > 0 ? 1 : 0;
					m_bench.puppetLayers += layers;
				}
			}
			continue;
		}
		CountChangedBytes( span->record, span->recordSize, was->record, was->recordSize, m_bench.componentSizes,
						   holder != 0 ? m_bench.changedItems : m_bench.changedCharacters );
	}
	std::vector<PlayerSlot> ownInput;
	for ( PlayerSlot s : plan.inputSlots )
	{
		if ( s == slot )
		{
			ownInput.push_back( s );
		}
	}
	std::vector<uint8_t> out;
	auto size = [&]( const std::vector<net::EntityUpdate>& updates, const std::vector<uint32_t>& removed, const std::vector<PlayerSlot>& slots ) {
		return net::EncodeUpdates( newest, plan.base, updates, removed, slots, &GameServer::LayoutOfTick, this, out ) ? uint64_t( out.size() ) : 0;
	};
	m_bench.sampled += 1;
	m_bench.sampledBytes += message.size();
	m_bench.withoutItems += size( withoutItems, plan.sent.removed, plan.inputSlots );
	m_bench.withoutInputs += size( plan.updates, plan.sent.removed, ownInput );
	m_bench.ownOnly += size( ownOnly, {}, ownInput );
	m_bench.headOnly += size( {}, {}, {} );
}

} // namespace cb
