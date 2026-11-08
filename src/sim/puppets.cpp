// Other players' characters as puppets (M253, M254; ROADMAP step 3b; sim/puppet.h): what a server
// makes of each of its characters for the clients that do not drive it, and what a client does
// with one: the character placed where it says, and its animation run here.

#include "simulation.h"

#include "anim_controller.h"
#include "anim_graph.h"
#include "detmath.h"
#include "puppet.h"

#include "box3d/box3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cb
{

namespace
{

// A puppet sent at most this many ticks apart is near (sent every state or two, as it changes).
constexpr uint32_t kNearGap = 4;
// Faster than this between two of its puppets, it was put somewhere (a respawn), not running.
constexpr float kMostPuppetSpeed = 40.0f;
// The most ticks of a puppet's animation run at once (one that was out of sight a long time).
constexpr uint32_t kMostAnimTicks = 120;

int32_t ToGrid( float v )
{
	return int32_t( std::lround( v * kPuppetGrid ) );
}

b3Vec3 FromGrid( const Puppet& p )
{
	return { float( p.x ) / kPuppetGrid, float( p.y ) / kPuppetGrid, float( p.z ) / kPuppetGrid };
}

int8_t ToTurnByte( float radians )
{
	const float turns = detmath::WrapAngle( radians ) / detmath::kTwoPi;
	return int8_t( std::clamp( long( std::lround( turns * 256.0f ) ), -128L, 127L ) );
}

float FromTurnByte( int8_t v )
{
	return float( v ) * ( detmath::kTwoPi / 256.0f );
}

} // namespace

Transform Simulation::PuppetPlace( const Puppet& p )
{
	return { FromGrid( p ), detmath::YawRotation( detmath::WrapAngle( detmath::YawToRadians( p.facing ) ) ) };
}

bool Simulation::IsPuppet( flecs::entity e ) const
{
	// A character a slot or a mod drives (not a prop driven, not a body nobody drives).
	return e.is_valid() && e.has<Character>() && e.has<DrivenProp>() == false && e.has<IdleBody>() == false && e.has<Transform>();
}

void Simulation::KindsOf( uint32_t netId, uint16_t* kinds, uint8_t& count, uint16_t& inUse ) const
{
	count = 0;
	inUse = 0;
	const flecs::entity e = netId != 0 ? FindEntity( netId ) : flecs::entity();
	if ( const PuppetState* ps = e.is_valid() ? e.try_get<PuppetState>() : nullptr )
	{
		// (A client's stand-in of another's: as its puppet says.)
		count = std::min<uint8_t>( ps->now.kindCount, uint8_t( kPuppetKinds ) );
		std::copy( ps->now.kinds, ps->now.kinds + count, kinds );
		inUse = ps->now.kindsInUse;
		return;
	}
	for ( const EntityRef& item : ItemsOf( netId ) )
	{
		const HeldItem* h = flecs::entity( m_world, item.entity ).try_get<HeldItem>();
		if ( h == nullptr || h->holder != netId || count >= kPuppetKinds )
		{
			continue;
		}
		inUse = uint16_t( inUse | ( h->stowed == 0 ? 1u << count : 0u ) );
		kinds[count++] = h->kind;
	}
}

bool Simulation::PuppetOf( uint32_t netId, const PlayerInput* input, Puppet& out ) const
{
	const flecs::entity e = FindEntity( netId );
	if ( IsPuppet( e ) == false )
	{
		return false;
	}
	const Character& c = e.get<Character>();
	const Transform& t = e.get<Transform>();
	const AnimState anim = GetAnimState( e );
	PlayerInput npc;
	if ( input == nullptr )
	{
		const ModDriven* driven = e.try_get<ModDriven>();
		npc = driven != nullptr ? driven->input : PlayerInput{};
		input = &npc;
	}
	out = Puppet{};
	out.x = ToGrid( t.position.x );
	out.y = ToGrid( t.position.y );
	out.z = ToGrid( t.position.z );
	out.facing = detmath::RadiansToYaw( c.facingYaw );
	out.aimYaw = ToTurnByte( anim.aimYaw );
	out.aimPitch = ToTurnByte( anim.aimPitch );
	const bool usingItem = std::any_of( m_usingScratch.begin(), m_usingScratch.end(), [&]( const auto& u ) { return u.first == netId; } );
	out.flags = uint8_t( ( c.grounded ? PuppetGrounded : 0 ) | ( c.sprinting ? PuppetSprinting : 0 ) | ( c.dead ? PuppetDead : 0 ) |
						 ( c.frozen ? PuppetFrozen : 0 ) | ( FacesCamera( c, *input ) ? PuppetFacesCamera : 0 ) |
						 ( anim.aiming ? PuppetAiming : 0 ) | ( usingItem ? PuppetUsing : 0 ) );
	out.slot = c.slot;
	out.lastJumpTick = c.lastJumpTick;
	// (The state is after the tick before Tick(): the tick it left the ground.)
	const uint32_t last = m_globals.tick > 0 ? m_globals.tick - 1 : 0;
	out.airSince = c.grounded ? 0 : ( last > c.airTicks ? last - c.airTicks : 0 );
	out.stepCount = c.stepCount;
	out.motionsOn = EntityMotionsOn( netId );
	// What it carries (M260): its items put away out of sight are not sent to the others.
	KindsOf( netId, out.kinds, out.kindCount, out.kindsInUse );

	// Where each layer of its state machine is. (Not while it is dead or down: nothing runs it then,
	// here or there, and its layers' clocks stand, so when they began would change every tick.)
	const AnimGraph* graph = c.dead || e.has<Downed>() ? nullptr : Body( CharacterOf( e ) ).graph.get();
	const float dt = m_config.TimeStep();
	const size_t layers = std::min( { anim.Layers(), kPuppetLayers, size_t( m_config.layers ) } );
	out.layerCount = uint8_t( layers );
	for ( size_t l = 0; l < layers; ++l )
	{
		out.stances[l] = l < anim.stances.size() ? anim.stances[l] : 0;
		PuppetLayer& stamp = out.layers[l];
		if ( graph == nullptr || l >= anim.graph.size() || anim.graph[l].started == 0 )
		{
			continue; // (state 0xFF: not started)
		}
		const AnimGraphLayerState& L = anim.graph[l];
		const AnimGraph* owner = nullptr;
		const AnimGraphLayer& layer = ResolveLayer( *graph, m_animPacks, l, L.source, owner, OverlayNth( *graph, anim.graph, l ) );
		if ( layer.states.empty() )
		{
			continue;
		}
		stamp.state = L.state;
		stamp.previous = L.previous;
		stamp.source = L.source;
		// When it began: 0 for long ago (its clock at the most it counts), and for a OneShot that is not
		// playing (its clock stands): then the tick says nothing, and never changes.
		const uint32_t ago = uint32_t( std::lround( L.stateTime / dt ) );
		const bool still = ( layer.shot && L.previousBlend == 0.0f ) || L.stateTime >= kMaxAnimStateTime;
		stamp.start = still || last <= ago ? 0 : last - ago;
		// (A OneShot's: whether its shot plays.)
		stamp.via = layer.shot ? uint8_t( L.previousBlend != 0.0f ? 1 : 0 ) : AnimGraphVia( layer, L );
	}
	return true;
}

namespace
{
// What a puppet's view does to each component of a record (Simulation::PrepareViews).
enum ViewKind : uint8_t
{
	ViewKeep = 0,
	ViewZero = 1,	// what a puppet's client keeps of its own
	ViewShared = 2, // the board: its shared fields alone
	ViewSlots = 3,	// what the slots show, not their last keys
};
} // namespace

void Simulation::PrepareViews()
{
	const flecs::entity_t local[] = {
		m_world.component<Transform>().id(),	 m_world.component<Velocity>().id(),	  m_world.component<PhysicsBody>().id(),
		m_world.component<Character>().id(),	 m_animId,								  m_motionId,
		m_world.component<MoveOverrides>().id(), m_world.component<MoveEffects>().id(), m_world.component<MoveValues>().id(),
		m_world.component<ModDriven>().id(),
	};
	const flecs::entity_t slots = m_world.component<Slots>().id();
	m_viewKinds.assign( m_snapComponents.size(), ViewKeep );
	for ( size_t i = 0; i < m_snapComponents.size(); ++i )
	{
		const flecs::entity_t id = m_snapComponents[i].id;
		m_viewKinds[i] = std::find( std::begin( local ), std::end( local ), id ) != std::end( local ) ? ViewZero
						 : id == m_boardId && m_sharedFields.empty() == false						  ? ViewShared
						 : id == slots																  ? ViewSlots
																									  : ViewKeep;
	}
}

void Simulation::PuppetView( const uint8_t* record, uint32_t size, std::vector<uint8_t>& out ) const
{
	if ( m_viewKinds.size() != m_snapComponents.size() )
	{
		const_cast<Simulation*>( this )->PrepareViews(); // (the simulation's thread: PuppetView's callers)
	}
	out.resize( size );
	PuppetViewTo( record, size, out.data() );
}

void Simulation::PuppetViewTo( const uint8_t* record, uint32_t size, uint8_t* to ) const
{
	// What a puppet's client keeps of its own (sim/puppet.h): zeros here, so they never change.
	std::memcpy( to, record, size );
	if ( size < 4 )
	{
		return;
	}
	uint32_t mask = 0;
	std::memcpy( &mask, record, 4 );
	size_t at = 4;
	for ( size_t i = 0; i < m_snapComponents.size() && i < m_viewKinds.size() && at <= size; ++i )
	{
		if ( ( mask & ( uint32_t( 1 ) << i ) ) == 0 || m_snapComponents[i].size == 0 )
		{
			continue;
		}
		const SnapComponent& sc = m_snapComponents[i];
		if ( at + sc.size > size )
		{
			return;
		}
		uint8_t* out = to; // (as the code below names it)
		if ( m_viewKinds[i] == ViewZero )
		{
			std::memset( out + at, 0, sc.size );
		}
		else if ( m_viewKinds[i] == ViewShared )
		{
			// Its fields others see (M255): the rest are its player's alone.
			for ( size_t slot = 0; slot * 4 + 4 <= sc.size; ++slot )
			{
				if ( slot >= m_sharedFields.size() || m_sharedFields[slot] == 0 )
				{
					std::memset( out + at + slot * 4, 0, 4 );
				}
			}
		}
		else if ( m_viewKinds[i] == ViewSlots )
		{
			// What it shows (how many, which is out, an open chest), not what its keys did last tick.
			Slots s;
			std::memcpy( &s, out + at, sizeof( s ) );
			s.seq = 0;
			s.resync = 0;
			s.prevKeys = 0;
			s.prevActions = 0;
			std::memcpy( out + at, &s, sizeof( s ) );
		}
		at += sc.size;
	}
}

void Simulation::ApplyPuppets( const std::vector<PuppetEntry>& puppets, uint32_t tick, bool step )
{
	PhysicsArena::Scope scope( *m_arena ); // (bodies moved)
	const float dt = m_config.TimeStep();
	for ( const PuppetEntry& entry : puppets )
	{
		flecs::entity e = FindEntity( entry.netId );
		if ( e.is_valid() == false || StandsIn( e ) == false || e.has<Character>() == false )
		{
			continue;
		}
		PuppetState ps = e.has<PuppetState>() ? e.get<PuppetState>() : PuppetState{};
		if ( ps.started == 0 )
		{
			ps.started = 1;
			ps.now = ps.before = entry.puppet;
			ps.nowTick = ps.beforeTick = entry.tick;
			ps.animThrough = tick > 1 ? tick - 2 : 0; // (one tick: from where it stands)
		}
		else if ( entry.tick != ps.nowTick )
		{
			ps.before = ps.now;
			ps.beforeTick = ps.nowTick;
			ps.now = entry.puppet;
			ps.nowTick = entry.tick;
		}
		const Puppet& p = ps.now;

		// How fast it went between its last two (none across a jump in place: a respawn).
		const uint32_t gap = ps.nowTick > ps.beforeTick ? ps.nowTick - ps.beforeTick : 0;
		const uint32_t since = tick > ps.nowTick ? tick - ps.nowTick : 0;
		b3Vec3 velocity = {};
		bool jumped = false;
		if ( gap > 0 )
		{
			velocity = b3MulSV( 1.0f / ( float( gap ) * dt ), b3Sub( FromGrid( ps.now ), FromGrid( ps.before ) ) );
			jumped = b3Length( velocity ) > kMostPuppetSpeed;
			velocity = jumped ? b3Vec3{} : velocity;
		}
		// Where it is at `tick`. One sent every state or two: where it was last said to be (one that
		// stopped is sent again until the client has it, so it stands, and goes at no speed). One sent
		// less often (far off): from the one before to the last, over as long as between them, so it
		// moves on until the next comes, an interval late; and still once that is over.
		// (Not across a jump in place: a respawn, a mod's teleport, there at once. M258: a far one
		// slid across the map, found by net_hybrid_far under load: 59 m off.)
		Transform t;
		bool moving = since <= std::max( gap, kNearGap );
		if ( gap > kNearGap && jumped == false )
		{
			t.position = b3Lerp( FromGrid( ps.before ), FromGrid( p ), std::min( 1.0f, float( since ) / float( gap ) ) );
			moving = since < gap;
		}
		else
		{
			t.position = FromGrid( p );
		}
		velocity = moving ? velocity : b3Vec3{};
		const float facing = detmath::WrapAngle( detmath::YawToRadians( p.facing ) );
		t.rotation = detmath::YawRotation( facing );
		e.set<Transform>( t );
		e.set<Velocity>( { velocity, { 0.0f, 0.0f, 0.0f } } );
		if ( const PhysicsBody* pb = e.try_get<PhysicsBody>() )
		{
			const b3BodyId body = BodyOf( *pb );
			if ( b3Body_IsValid( body ) && b3Body_GetType( body ) != b3_staticBody )
			{
				b3Body_SetTransform( body, t.position, t.rotation );
			}
		}
		// What it holds is where it is (presentation puts it in its socket).
		for ( const EntityRef& item : ItemsOf( entry.netId ) )
		{
			flecs::entity( m_world, item.entity ).set<Transform>( t );
		}

		// Its character: what the puppet says, and how fast it goes from where it went.
		Character c = e.get<Character>();
		c.velocity = velocity;
		c.facingYaw = facing;
		c.slot = p.slot;
		c.grounded = ( p.flags & PuppetGrounded ) != 0 ? 1 : 0;
		c.sprinting = ( p.flags & PuppetSprinting ) != 0 ? 1 : 0;
		c.dead = ( p.flags & PuppetDead ) != 0 ? 1 : 0;
		c.frozen = ( p.flags & PuppetFrozen ) != 0 ? 1 : 0;
		c.faceCamera = ( p.flags & PuppetFacesCamera ) != 0 ? 1 : 0;
		c.lastJumpTick = p.lastJumpTick;
		c.stepCount = p.stepCount;

		// Its animation, run here up to the state's (the end of the tick before `tick`), each layer
		// in the state the puppet says (M254).
		const uint32_t through = tick > 0 ? tick - 1 : 0;
		const AnimGraph* graph = Body( CharacterOf( e ) ).graph.get();
		if ( step && through > ps.animThrough )
		{
			AnimState anim = GetAnimState( e );
			anim.aiming = ( p.flags & PuppetAiming ) != 0 ? 1 : 0;
			for ( size_t l = 0; l < p.layerCount && l < size_t( m_config.layers ); ++l )
			{
				if ( anim.stances.size() <= l )
				{
					anim.stances.resize( l + 1 );
				}
				anim.stances[l] = p.stances[l];
			}
			// What it holds, carries and uses, for its state machine: as its puppet says (M260: its
			// items put away out of sight are not here).
			uint16_t held[kPuppetKinds], carried[kPuppetKinds];
			uint32_t heldCount = 0, carriedCount = 0;
			for ( size_t k = 0; k < p.kindCount && k < kPuppetKinds; ++k )
			{
				carried[carriedCount++] = p.kinds[k];
				if ( ( p.kindsInUse >> k ) & 1 )
				{
					held[heldCount++] = p.kinds[k];
				}
			}
			const Blackboard board = GetBoard( e );
			PlayerInput in;
			in.cameraYaw = detmath::RadiansToYaw( facing + FromTurnByte( p.aimYaw ) );
			in.cameraPitch = int16_t( std::lround( FromTurnByte( p.aimPitch ) * ( 65536.0f / detmath::kTwoPi ) ) );
			const uint32_t from = std::max( ps.animThrough + 1, through > kMostAnimTicks ? through - kMostAnimTicks + 1 : 0u );
			// (Dead or down, nothing runs it: as on the server, MoveCharacters.)
			const bool still = c.dead != 0 || e.has<Downed>();
			for ( uint32_t at = from; at <= through && still == false; ++at )
			{
				c.airTicks = c.grounded != 0 || at < p.airSince ? 0 : at - p.airSince;
				UpdateAnimState( anim, c, in, at, dt );
				if ( graph == nullptr )
				{
					continue;
				}
				AnimGraphInputs graphIn;
				const float speed = anim.groundSpeed;
				graphIn.builtins[AnimExpr::Speed] = speed;
				graphIn.builtins[AnimExpr::ForwardSpeed] = anim.legsBackward != 0 ? -speed : speed;
				graphIn.builtins[AnimExpr::VerticalSpeed] = c.velocity.y;
				graphIn.builtins[AnimExpr::Grounded] = c.grounded != 0 ? 1.0f : 0.0f;
				graphIn.builtins[AnimExpr::AirborneTime] = c.grounded != 0 ? 0.0f : float( c.airTicks ) * dt;
				graphIn.builtins[AnimExpr::Jumped] = c.lastJumpTick != 0 && c.lastJumpTick == at ? 1.0f : 0.0f;
				graphIn.builtins[AnimExpr::Aiming] = anim.aiming != 0 ? 1.0f : 0.0f;
				graphIn.builtins[AnimExpr::Backward] = anim.legsBackward != 0 ? 1.0f : 0.0f;
				graphIn.builtins[AnimExpr::MoveForward] = anim.moveForward;
				graphIn.builtins[AnimExpr::MoveRight] = anim.moveRight;
				graphIn.board = &board.values;
				graphIn.globalBoard = &m_globalBoard;
				graphIn.events = m_globals.modEvents;
				graphIn.eventCount = m_globals.modEventCount;
				graphIn.tick = at;
				graphIn.netId = entry.netId;
				graphIn.heldKinds = held;
				graphIn.heldCount = heldCount;
				graphIn.carriedKinds = carried;
				graphIn.carriedCount = carriedCount;
				graphIn.usingKinds = held;
				graphIn.usingCount = ( p.flags & PuppetUsing ) != 0 ? heldCount : 0;
				graphIn.motionsOn = p.motionsOn;
				m_markerScratch.clear(); // (the server's markers come as its events)
				UpdateAnimGraph( anim, *graph, m_animPacks, graphIn, dt, m_markerScratch, p.layers, std::min<size_t>( p.layerCount, kPuppetLayers ) );
			}
			SetAnimState( e, anim );
			ps.animThrough = through;
		}
		c.airTicks = c.grounded != 0 || through < p.airSince ? 0 : through - p.airSince;
		e.set<Character>( c );
		e.set<PuppetState>( ps );
	}
}

} // namespace cb
