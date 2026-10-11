// Mark: a second use for an item that says it has one. The example of adding a use to items made
// in the editor, and of a use that is not the item's main one.
//
// An item whose scene has the property "mark.range" (the pistol does) marks with the second use
// button (the item mod's "use2", the right mouse button): a ray that harms nothing, as far as the
// range, at most once every "mark.seconds" (1). The server keeps nothing about a mark:
//
//   item.use2    a mark was cast: a = who cast it, b = what the ray hit (0: nothing), point = where
//                it came from, vector = where it ended. The item's own scene shows it (a beam).
//   mark.found   it found a living player: a = who cast it, b = the marked player, point = where.
//                How a marked player looks and for how long is the look's business.
//
// The item's main use is not this mod's: a pistol still shoots by the gun mod's rules.

#include "mod_api.h"

#include <array>

namespace
{

using namespace cb;
using namespace cb::mods;

class MarkMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "mark";
	}

	void Declare( Declarations& declare ) override
	{
		m_second = declare.Action( "use2", "MouseRight" );
		m_use2 = declare.Event( "item.use2" );
		m_found = declare.Event( "mark.found" );
		m_hand = declare.Socket( "RightHand" );
	}

	void Tick( Context& ctx ) override
	{
		uint32_t tick = ctx.Tick();
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			const Character* c = ctx.PlayerCharacter( slot );
			if ( c == nullptr || c->dead != 0 || c->frozen != 0 || ctx.Pressed( slot, m_second ) == false || tick < m_next[size_t( i )] )
			{
				continue;
			}
			uint32_t item = ctx.HeldItem( slot, m_hand );
			ItemKindHandle kind = item != 0 ? ctx.ItemKindOf( item ) : ItemKindHandle{};
			float range = kind.Valid() ? ctx.ItemProperty( kind, "mark.range", 0.0f ) : 0.0f;
			if ( range <= 0.0f )
			{
				continue;
			}
			float seconds = ctx.ItemProperty( kind, "mark.seconds", 1.0f );
			m_next[size_t( i )] = tick + uint32_t( seconds * float( ctx.Config().tickRate ) + 0.5f );
			// The same ray as a shot, with no damage.
			uint32_t caster = SlotTarget( slot );
			b3Vec3 eye, dir;
			RayHit hit;
			bool found = ctx.CastAim( slot, range, hit, eye, dir );
			b3Vec3 end = found ? hit.point : b3MulAdd( eye, range, dir );
			ctx.Emit( m_use2, caster, found ? hit.netId : 0, 0, eye, end );
			int other = found ? ctx.SlotOf( hit.netId ) : -1;
			const Character* marked = other >= 0 ? ctx.PlayerCharacter( PlayerSlot( other ) ) : nullptr;
			if ( marked != nullptr && marked->dead == 0 )
			{
				ctx.Emit( m_found, caster, hit.netId, 0, hit.point, hit.normal );
			}
		}
	}

private:
	std::array<uint32_t, kMaxPlayers> m_next{};
	ActionHandle m_second;
	EventHandle m_use2;
	EventHandle m_found;
	SocketHandle m_hand;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_mark()
{
	return std::make_unique<MarkMod>();
}
