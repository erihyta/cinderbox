// Sneaking: while a player holds the crouch key, their character's "FullBody" layer (its locomotion) is
// this mod's animation pack instead: a crouch that walks. It is part of the pose, so a crouching
// player's head is lower for the server's hit tests too. A sneaking player is also slower: the mod
// sets that player's walk and sprint speeds (movement parameters) and gives them back.
//
// Server option: sneak.speed (m/s while sneaking, default 1.5; 0 leaves the speeds alone).
//
// The pack is authored in Godot like a character's own state machine (server_mods/sneak/client,
// anim/sneak.crouch, made by godot/addons/cinderbox_maps/make_sneak_pack.gd) and fitted to whatever
// character the server runs. This file only decides when.

#include "mod_api.h"

#include <array>

using namespace cb;
using namespace cb::mods;

namespace
{

class SneakMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "sneak";
	}

	void Declare( Declarations& declare ) override
	{
		m_crouch = declare.Action( "crouch", "C" );
		m_pack = declare.AnimPack( "sneak.crouch" );
		// For looks and HUDs: on while the player sneaks.
		m_sneaking = declare.Field( "sneak.sneaking", BoardType::Bool );
	}

	void Tick( Context& ctx ) override
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				m_sneaking_[size_t( i )] = false;
			}
			const Character* c = ctx.PlayerCharacter( slot );
			if ( ctx.InWorld( slot ) == false || c == nullptr )
			{
				continue;
			}
			bool sneaking = ctx.Held( slot, m_crouch ) && c->dead == 0;
			if ( sneaking == m_sneaking_[size_t( i )] )
			{
				continue;
			}
			m_sneaking_[size_t( i )] = sneaking;
			uint32_t target = SlotTarget( slot );
			float speed = float( ctx.Option( "sneak.speed", 1.5 ) );
			if ( sneaking )
			{
				ctx.SwapLayer( target, m_pack, "FullBody" );
				if ( speed > 0.0f )
				{
					// Sprint too: a crouch does not run.
					ctx.SetMove( target, MoveParam::WalkSpeed, speed );
					ctx.SetMove( target, MoveParam::SprintSpeed, speed );
				}
			}
			else
			{
				ctx.RestoreLayer( target, "FullBody" );
				ctx.ResetMove( target, MoveParam::WalkSpeed );
				ctx.ResetMove( target, MoveParam::SprintSpeed );
			}
			ctx.Set( target, m_sneaking, sneaking ? 1 : 0 );
		}
	}

private:
	std::array<bool, kMaxPlayers> m_sneaking_{};
	ActionHandle m_crouch;
	AnimPackHandle m_pack;
	FieldHandle m_sneaking;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_sneak()
{
	return std::make_unique<SneakMod>();
}
