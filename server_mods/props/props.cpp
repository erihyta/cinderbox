// Props: the spawn button. What the engine used to do itself, now a rule a server can change.
//
// A press with empty hands throws a prop in front of the player: the map's spawnable template if it
// has one, otherwise a random box or sphere. The engine still owns the budget (per-player and
// global caps, lifetimes); this mod only decides when and what.

#include "mod_api.h"

#include "detmath.h"

namespace
{

using namespace cb;
using namespace cb::mods;

class PropsMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "props";
	}

	void Declare( Declarations& declare ) override
	{
		m_spawn = declare.Action( "spawn_prop", "F" );
		// Published by the loadout mod when it runs; 0 (never set) counts as empty hands.
		m_loadout = declare.Field( "loadout.slot", BoardType::Int );
	}

	void Tick( Context& ctx ) override
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.InWorld( slot ) == false || ctx.Pressed( slot, m_spawn ) == false )
			{
				continue;
			}
			uint32_t netId = ctx.PlayerNetId( slot );
			const Character* c = ctx.PlayerCharacter( slot );
			const Transform* t = ctx.EntityTransform( netId );
			if ( c == nullptr || t == nullptr || c->dead || c->frozen || ctx.Get( netId, m_loadout ) > 1 )
			{
				continue;
			}

			b3Vec3 forward = detmath::YawForward( c->facingYaw );
			b3Vec3 position = b3Add( t->position, b3Add( b3MulSV( 1.2f, forward ), b3Vec3{ 0.0f, 0.6f, 0.0f } ) );
			b3Vec3 velocity = b3Add( c->velocity, b3Add( b3MulSV( 3.0f, forward ), b3Vec3{ 0.0f, 2.0f, 0.0f } ) );
			uint32_t lifetime = ctx.Config().PropLifetimeTicks();

			if ( ctx.Map().spawnTemplate != kNoTemplate )
			{
				ctx.SpawnTemplate( ctx.Map().spawnTemplate, position, c->facingYaw, velocity, SlotTarget( slot ), lifetime );
				continue;
			}
			bool sphere = ( ctx.Random() & 1 ) != 0;
			float size = ctx.RandomRange( 0.2f, 0.45f );
			b3Vec3 half = sphere ? b3Vec3{ size, 0.0f, 0.0f } : b3Vec3{ size, size, size };
			ctx.SpawnProp( sphere ? ShapeKind::Sphere : ShapeKind::Box, half, position, c->facingYaw, velocity, SlotTarget( slot ),
						   lifetime );
		}
	}

private:
	ActionHandle m_spawn;
	FieldHandle m_loadout;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_props()
{
	return std::make_unique<PropsMod>();
}
