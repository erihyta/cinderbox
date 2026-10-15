// Chests: the example of a container, an entity with slots of its own (sim/components.h,
// Container).
//
// The engine has the mechanism: a container's items are held by it, out of sight, in its slots; a
// player looking at one within reach opens it with the inventory (the Open intent, in its own input,
// so it is predicted), and drags items between its slots and the chest's; walking away closes it.
// What a chest looks like is this mod's look (client/prefabs/chest.tscn).
//
// This file is the rules: what a chest is (a template it declares: "chest", a static box with
// chest.slots slots, drawn as "chest"), where they stand (chest.count of them, in a ring
// chest.radius metres around the map's spawn point, on the ground, facing it), and what is in one
// when it is placed (chest.items items, of kinds picked at random among those a player may pick
// up).
//
// Server options: chest.count (default 2), chest.radius (default 6), chest.slots (default 9),
// chest.items (default 3).

#include "mod_api.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace cb;
using namespace cb::mods;

namespace
{

class ChestMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "chest";
	}

	void Declare( Declarations& declare ) override
	{
		int slots = int( std::clamp( declare.Option( "chest.slots", 9.0 ), 1.0, double( kMaxSlots ) ) );
		declare.ContainerTemplate( "chest", "chest", b3Vec3{ 0.9f, 0.6f, 0.55f }, slots );
	}

	void Start( Context& ctx ) override
	{
		m_count = int( std::clamp( ctx.Option( "chest.count", 2.0 ), 0.0, 32.0 ) );
		m_radius = float( std::clamp( ctx.Option( "chest.radius", 6.0 ), 1.0, 100.0 ) );
		m_items = int( std::clamp( ctx.Option( "chest.items", 3.0 ), 0.0, double( kMaxSlots ) ) );
	}

	void Tick( Context& ctx ) override
	{
		if ( m_placed == false )
		{
			Place( ctx );
			m_placed = true;
			return;
		}
		// What is in one when it is placed (the tick after: it has its NetId by then).
		for ( uint32_t chest : ctx.Containers() )
		{
			if ( std::find( m_filled.begin(), m_filled.end(), chest ) == m_filled.end() )
			{
				m_filled.push_back( chest );
				Fill( ctx, chest );
			}
		}
	}

private:
	// In a ring around the spawn point, on the ground, facing it.
	void Place( Context& ctx )
	{
		int chest = ctx.FindTemplate( "chest" );
		if ( chest < 0 )
		{
			return;
		}
		const b3Vec3 center = ctx.Map().spawnCenter;
		for ( int i = 0; i < m_count; ++i )
		{
			float angle = 6.2831853f * ( float( i ) + 0.5f ) / float( std::max( m_count, 1 ) );
			b3Vec3 at = { center.x + m_radius * std::cos( angle ), center.y + 6.0f, center.z + m_radius * std::sin( angle ) };
			RayHit ground;
			if ( ctx.CastRay( at, b3Vec3{ 0.0f, -30.0f, 0.0f }, 0, ground ) == false )
			{
				continue;
			}
			b3Vec3 place = { at.x, ground.point.y + 0.3f, at.z };
			float facing = std::atan2( center.x - place.x, center.z - place.z );
			ctx.SpawnTemplate( uint32_t( chest ), place, facing, b3Vec3{}, 0, 0 );
		}
	}

	// Items of kinds a player may pick up, at random.
	void Fill( Context& ctx, uint32_t chest )
	{
		std::vector<int> kinds;
		for ( int k = 0; k < int( ctx.Schema().itemKinds.size() ); ++k )
		{
			if ( ctx.ItemProperty( ItemKindHandle{ k }, "pickup.never", 0.0f ) == 0.0f )
			{
				kinds.push_back( k );
			}
		}
		for ( int i = 0; i < m_items && kinds.empty() == false; ++i )
		{
			ctx.PutInContainer( chest, ItemKindHandle{ kinds[size_t( ctx.Random() % kinds.size() )] } );
		}
	}

	int m_count = 2;
	float m_radius = 6.0f;
	int m_items = 3;
	bool m_placed = false;
	std::vector<uint32_t> m_filled;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_chest()
{
	return std::make_unique<ChestMod>();
}
