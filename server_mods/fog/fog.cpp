// Fog: clients that are sent frames (streaming clients) are shown only what is near their player.
//
//   cb_server --mod-option fog.radius=25
//
// Off by default (fog.radius 0). The level itself is always shown; players, props, items and
// ragdolls further than the radius from the viewer's player are not sent to it at all, so there is
// nothing for it to find in its own memory. A client that simulates the world cannot be kept in
// the dark this way, and is never asked about.
//
// The whole mod is the hook: no fields, no events, no commands.

#include "mod_api.h"

namespace
{

using namespace cb;
using namespace cb::mods;

class FogMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "fog";
	}

	void Declare( Declarations& ) override
	{
	}

	void Tick( Context& ctx ) override
	{
		m_radius = float( ctx.Option( "fog.radius", 0.0 ) );
	}

	bool Sees( Context& ctx, PlayerSlot viewer, uint32_t netId ) override
	{
		if ( m_radius <= 0.0f )
		{
			return true;
		}
		// The level: neither a player nor anything that moves.
		if ( ctx.IsPlayer( netId ) == false && ctx.IsDynamic( netId ) == false && ctx.ItemKindOf( netId ).Valid() == false )
		{
			return true;
		}
		const Transform* eye = ctx.EntityTransform( ctx.PlayerNetId( viewer ) );
		const Transform* there = ctx.EntityTransform( netId );
		if ( eye == nullptr || there == nullptr )
		{
			return true;
		}
		return b3Length( b3Sub( there->position, eye->position ) ) <= m_radius;
	}

private:
	float m_radius = 0.0f;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_fog()
{
	return std::make_unique<FogMod>();
}
