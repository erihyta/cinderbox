// Cannon: walls broken by what hits them, not by a blast.
//
// The cannon is not in this file. It is an item (client/prefabs/cannon.tscn, a CbItem used once a
// press, its own key N, wherever it is carried) whose use launches a new cannon ball
// (client/prefabs/ball.tscn) along the look, gone again after 10 s. And what a ball does to a wall
// is the ball's scene too: its CbItem's "Breaks walls" settings (break_speed, break_hole,
// break_through). The simulation itself breaks the wall a ball meets that fast, in the step it
// meets it, so every player's own simulation does it on the same tick as the server: no waiting
// for the server, no correction afterwards. It says "cannon.ball.smash" there (the look's dust and
// crash), which this mod declares.
//
// This file is the rest of the rules: a ball that hits a player hurts it (the combat mod's
// "combat.damage"), by how fast it was going; health is the server's.
//
// Server option: cannon.damage (to a player at 30 m/s, default 70; 0: none).
//
// Off by default: a server runs it by name (cb_server --mods ...,cannon).

#include "mod_api.h"

#include <algorithm>
#include <vector>

using namespace cb;
using namespace cb::mods;

namespace
{

// The speed the damage is measured at.
constexpr float kReferenceSpeed = 30.0f;
// Slower than this, a ball only bumps a player.
constexpr float kHurtSpeed = 8.0f;

class CannonMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "cannon";
	}

	void Declare( Declarations& declare ) override
	{
		// The cannon and its ball, from its look.
		declare.ItemsOfLook();
		m_ball = declare.ItemKind( "cannon.ball" );
		// Said by the cannon's use (its scene's use_emits), and by the simulation where a ball
		// breaks a wall ("<kind>.smash").
		m_fired = declare.Event( "cannon.fired" );
		m_smash = declare.Event( "cannon.ball.smash" );
		// The combat mod's (see combat.cpp): what a hit on a player is said with.
		m_damage = declare.Event( "combat.damage" );
		// A ball lying about is not for picking up.
		declare.ItemProperty( m_ball, "pickup.never", 1.0f );
	}

	void Start( Context& ctx ) override
	{
		m_hurt = float( std::clamp( ctx.Option( "cannon.damage", 70.0 ), 0.0, 10000.0 ) );
	}

	void Tick( Context& ctx ) override
	{
		// What the balls ran into on the tick before this one: a player, each ball once.
		std::vector<uint32_t> done;
		for ( const Context::ItemHit& hit : ctx.Hits( m_ball ) )
		{
			if ( hit.speed < kHurtSpeed || ctx.IsPlayer( hit.other ) == false || std::find( done.begin(), done.end(), hit.item ) != done.end() )
			{
				continue;
			}
			done.push_back( hit.item );
			Hurt( ctx, hit );
		}
	}

private:
	void Hurt( Context& ctx, const Context::ItemHit& hit )
	{
		const int slot = ctx.SlotOf( hit.other );
		const Character* c = slot >= 0 ? ctx.PlayerCharacter( PlayerSlot( slot ) ) : nullptr;
		const Transform* ball = ctx.EntityTransform( hit.item );
		const Transform* body = ctx.EntityTransform( hit.other );
		if ( c == nullptr || c->dead != 0 || m_hurt <= 0.0f )
		{
			return;
		}
		const int32_t damage = int32_t( m_hurt * std::min( hit.speed / kReferenceSpeed, 1.5f ) + 0.5f );
		// Whose ball it was (for the kill): the one who fired it, as long as it is still here.
		const uint32_t shooter = ctx.ThrownBy( hit.item );
		const int shooterSlot = shooter != 0 ? ctx.SlotOf( shooter ) : -1;
		const uint32_t by = shooterSlot >= 0 ? SlotTarget( PlayerSlot( shooterSlot ) ) : 0;
		// Knocked away from where the ball met it.
		b3Vec3 away = ball != nullptr && body != nullptr ? b3Sub( body->position, ball->position ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
		away.y = 0.0f;
		const float far = b3Length( away );
		away = far > 0.01f ? b3MulSV( 1.0f / far, away ) : away;
		const b3Vec3 push = b3Add( b3MulSV( 0.2f * hit.speed, away ), b3Vec3{ 0.0f, 2.0f, 0.0f } );
		ctx.Emit( m_damage, by, hit.other, damage, hit.point, push );
	}

	ItemKindHandle m_ball;
	EventHandle m_fired;
	EventHandle m_smash;
	EventHandle m_damage;
	float m_hurt = 70.0f;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_cannon()
{
	return std::make_unique<CannonMod>();
}
