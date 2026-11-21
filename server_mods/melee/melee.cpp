// Melee: a bat (a sheet since M282, step 5: every item a sheet).
//
// What the bat is and does to bodies is its look's (client/prefabs/bat.tscn and its sheet,
// bat.rules), so it is predicted:
//   left mouse button   a swing: the pack (melee.hold, its clip's markers) plays it, and at its
//                       melee.strike marker a box where the player looks, pitch and all, finds the
//                       first it meets: melee.hit (a = who swung, b = what it hit), and shoves it.
//                       The next swing waits for melee.done: how long a swing lasts is the clip's
// The scene says the rest of what a bat is: slot 3, each life starts with one, on the back put
// away, its pack (inventory.start, holster, animation_pack). This file is what a sheet cannot say:
//   - while a bat is in the hand, its holder faces where the camera looks, so the upper body bends
//     with the pitch and the swing goes where it is drawn
//   - damage: a hit on a player is combat.damage (40); combat applies it and credits the kill
//     (mods cooperate by event, not by call)
//   - heat: a bat that hurt someone glows for 2 s (melee.hot, a field on the bat itself: it cools
//     the same on a back, on the ground or in someone else's hand). The language has no field of a
//     carried item to change, nor a timer on one (ROADMAP step 6)
//
// Until M282 the swing was this file's: five rays fanned from the chest at the strike, against the
// posed hitboxes, the stances "melee" and "melee_swing" (read by nothing), and the look guessed the
// swing (predict use as melee.swing, cooldown 1.1). A sheet's find meets capsules, as the fist's
// and the sword's do.

#include "mod_api.h"

#include <algorithm>
#include <vector>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr int32_t kDamage = 40;
constexpr float kHotSeconds = 2.0f; // how long the bat glows after it hurts someone
constexpr float kShove = 7.0f;		// m/s: what the sheet shoves with (the ragdoll's last hit)

// A bat that hurt someone, and when it cools. By the item, not by who held it.
struct HotBat
{
	uint32_t netId = 0;
	uint32_t until = 0;
};

class MeleeMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "melee";
	}

	void Declare( Declarations& declare ) override
	{
		// The bat is its look's: kind melee.bat, its pack, its holster, its sheet.
		for ( const Declarations::OwnItem& item : declare.ItemsOfLook() )
		{
			m_bat = item.look.kind == "melee.bat" ? item.kind : m_bat;
		}
		m_hand = declare.Socket( "RightHand" );
		m_hot = declare.Field( "melee.hot", BoardType::Bool, BoardScope::Entity, true ); // (shared: the bat glows on others too)
		// What the sheet says: a swing starting, its hit; the clip's markers.
		declare.Event( "melee.swing" );
		m_hit = declare.Event( "melee.hit" );
		declare.Event( "melee.strike" );
		declare.Event( "melee.done" );
		m_damage = declare.Event( "combat.damage" );
	}

	void Start( Context& ctx ) override
	{
		m_hotTicks = uint32_t( kHotSeconds * float( ctx.Config().tickRate ) + 0.5f );
	}

	void Tick( Context& ctx ) override
	{
		const uint32_t tick = ctx.Tick();
		for ( size_t i = 0; i < m_hotBats.size(); )
		{
			if ( tick >= m_hotBats[i].until )
			{
				ctx.Set( m_hotBats[i].netId, m_hot, 0 ); // nothing happens if the bat is gone
				m_hotBats.erase( m_hotBats.begin() + std::ptrdiff_t( i ) );
			}
			else
			{
				++i;
			}
		}
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			const PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				m_out[size_t( i )] = false;
			}
			const Character* c = ctx.PlayerCharacter( slot );
			if ( c == nullptr || ctx.PlayerNetId( slot ) == 0 )
			{
				continue;
			}
			const bool holding = c->dead == 0 && BatOf( ctx, slot ) != 0;
			if ( holding != m_out[size_t( i )] )
			{
				m_out[size_t( i )] = holding;
				// Out: it faces where it looks (and so bends with the pitch). Put away, the inventory mod
				// lets it go.
				if ( holding )
				{
					ctx.FaceCamera( SlotTarget( slot ), true );
				}
			}
		}

		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			if ( int( e.type ) == m_hit.index )
			{
				Hit( ctx, e.netIdA, e.netIdB, e.point );
			}
		}
	}

private:
	// The bat in the player's hand (0: none).
	uint32_t BatOf( Context& ctx, PlayerSlot slot ) const
	{
		const uint32_t inHand = ctx.HeldItem( slot, m_hand );
		return inHand != 0 && m_bat.Valid() && ctx.ItemKindOf( inHand ).index == m_bat.index ? inHand : 0;
	}

	void Hit( Context& ctx, uint32_t attacker, uint32_t victim, b3Vec3 point )
	{
		const int slot = ctx.SlotOf( attacker );
		if ( victim == 0 || victim == attacker || slot < 0 || ctx.IsPlayer( victim ) == false )
		{
			return; // (a prop: the sheet's shove is all)
		}
		const Transform* from = ctx.EntityTransform( attacker );
		const Transform* to = ctx.EntityTransform( victim );
		if ( from == nullptr || to == nullptr )
		{
			return;
		}
		b3Vec3 away = b3Sub( to->position, from->position );
		away.y = 0.0f;
		const float length = b3Length( away );
		away = length > 0.01f ? b3MulSV( 1.0f / length, away ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
		// (Combat spares a teammate itself.)
		ctx.Emit( m_damage, attacker, victim, kDamage, point, b3Add( b3MulSV( kShove, away ), b3Vec3{ 0.0f, 2.0f, 0.0f } ) );
		// A hit heats the bat: one field on the bat itself, until it cools.
		const uint32_t bat = BatOf( ctx, PlayerSlot( slot ) );
		if ( bat == 0 )
		{
			return;
		}
		ctx.Set( bat, m_hot, 1 );
		const uint32_t until = ctx.Tick() + m_hotTicks;
		auto hot = std::find_if( m_hotBats.begin(), m_hotBats.end(), [&]( const HotBat& b ) { return b.netId == bat; } );
		if ( hot != m_hotBats.end() )
		{
			hot->until = until;
		}
		else
		{
			m_hotBats.push_back( { bat, until } );
		}
	}

	std::vector<bool> m_out = std::vector<bool>( kMaxPlayers, false );
	std::vector<HotBat> m_hotBats;
	ItemKindHandle m_bat;
	SocketHandle m_hand;
	FieldHandle m_hot;
	EventHandle m_hit;
	EventHandle m_damage;
	uint32_t m_hotTicks = 120;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_melee()
{
	return std::make_unique<MeleeMod>();
}
