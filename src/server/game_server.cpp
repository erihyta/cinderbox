#include "game_server.h"

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

	mods::Declarations declarations;
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
	std::shared_ptr<const CharacterAsset> character = options.character ? options.character : BuiltInCharacter();
	m_schema.character = character->name;
	// How players move: the server's options, then what the character says about itself.
	for ( const auto& [param, value] : character->animations->Movement() )
	{
		m_options.config.move.values[param] = value;
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
		// The character's state machine, if it has one: the simulation runs it, the hit tests pose by
		// it, and it goes to every client in the schema.
		m_schema.animGraph = character->animations->GraphText();
		if ( m_schema.animGraph.empty() == false )
		{
			std::string error;
			m_animGraph = CompileAnimGraph( m_schema.animGraph, m_schema, error, warnings );
			if ( m_animGraph == nullptr )
			{
				Log( "character %s: its state machine does not load: %s", character->name.c_str(), error.c_str() );
				return false;
			}
			m_hits->SetGraph( m_animGraph, warnings );
			m_animPacks = CompileAnimPacks( m_schema, warnings );
			std::vector<std::shared_ptr<const anim::PackClips>> fitted;
			for ( size_t i = 0; i < m_animPacks.size(); ++i )
			{
				fitted.push_back( m_animPacks[i] && packSets[i] ? anim::FitPack( packSets[i], *m_animPacks[i], *character->animations, warnings )
															   : nullptr );
			}
			m_hits->SetPacks( m_animPacks, fitted );
		}
		if ( warnings.empty() == false )
		{
			Log( "character %s: %s", character->name.empty() ? "built-in" : character->name.c_str(), warnings.c_str() );
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
	else
	{
		SerializeMap( m_map, m_mapBytes );
	}
	m_mapHash = MapHash( m_mapBytes.data(), m_mapBytes.size() );

	m_sim = std::make_unique<Simulation>( m_options.config, m_map );
	m_sim->SetAnimGraph( m_animGraph );
	m_sim->SetAnimPacks( m_animPacks );
	m_sim->SetItemShapes( m_schema.itemShapes );
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

	Log( "listening on port %u, %u Hz, fingerprint %016llx, map %s (%u statics, %u props, hash %016llx)", options.port,
		 options.config.tickRate, (unsigned long long)BuildFingerprint(),
		 options.mapPath.empty() ? "built-in sandbox" : options.mapPath.c_str(), unsigned( m_map.statics.size() ),
		 unsigned( m_map.props.size() ), (unsigned long long)m_mapHash );

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
		for ( const auto& mod : m_mods )
		{
			mod->Start( ctx );
		}
	}
	return true;
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
					HandleInput( *c, msg );
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

		PlayerSlot slot = PlayerSlot( target - m_clients );
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

void GameServer::HandleInput( Client& client, const MsgInput& msg )
{
	uint32_t current = m_sim->Tick();
	// Acknowledgements only move forward (input packets can arrive out of order).
	client.ackTick = std::max( client.ackTick, std::min( msg.ackTick, current ) );
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
		Encode( welcome, m_buffer );
		m_transport.Send( c.peer, ChannelReliable, m_buffer, true );

		c.needsSnapshot = false;
		c.welcomed = true;
		SendNames( c.peer );
		SendPrivates( c );
		// The welcome carries the inputs of snapshotTick - 1, so frames start at snapshotTick.
		c.ackTick = m_sim->Tick();
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

	RunMods( frame );

	// Joining / reconnecting / desynced clients get the state before this tick.
	for ( Client& c : m_clients )
	{
		if ( c.used && c.connected && c.needsSnapshot )
		{
			c.lastResyncAt = now;
		}
	}
	SendSnapshots();

	m_sim->Step( frame );

	m_history[tick % kFrameHistory] = frame;
	m_lastInputs = frame.inputs;
	SendFrames( now );
	m_replay.AddFrame( frame );
	RecordView( frame );

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

	if ( m_options.checksumInterval > 0 && stateTick % m_options.checksumInterval == 0 )
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
				m_transport.Send( c.peer, ChannelReliable, m_buffer, true );
			}
		}
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
	for ( const auto& mod : m_mods )
	{
		mod->Tick( ctx );
	}
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
	m_transport.Send( c.peer, ChannelReliable, m_buffer, true );
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
			m_transport.Send( c.peer, ChannelReliable, m_buffer, true );
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

		Encoded* hit = nullptr;
		for ( Encoded& e : cache )
		{
			if ( e.ack == c.ackTick )
			{
				hit = &e;
				break;
			}
		}
		if ( hit == nullptr )
		{
			frames.clear();
			for ( uint32_t t = c.ackTick; t <= newest && frames.size() < kMaxBatchFrames; ++t )
			{
				frames.push_back( HistoryFrame( t ) );
			}
			InputArray base{};
			if ( c.ackTick > 0 )
			{
				base = HistoryFrame( c.ackTick - 1 ) ? HistoryFrame( c.ackTick - 1 )->inputs : m_lastInputs;
			}
			cache.push_back( { c.ackTick, frames.size(), {} } );
			hit = &cache.back();
			EncodeFrameBatch( base, frames.data(), frames.size(), hit->bytes );
		}
		m_transport.Send( c.peer, ChannelInput, hit->bytes, false );
		m_stats.batchesSent += 1;
		m_stats.framesSent += hit->frames;
	}
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

} // namespace cb
