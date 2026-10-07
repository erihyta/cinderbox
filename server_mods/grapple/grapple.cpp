// A grappling hook: the example of a probe, a force and a link (sim/motions.h), and of an item
// that is used by its slot's key.
//
// The hook is not in this file. It is an item (client/prefabs/hook.tscn, a CbItem that says "its
// slot key uses it") and one CbMotion (client/motion_sets/grapple_moves.tscn, with a CbProbe, a
// CbForce and a CbLink under it), baked and run by every simulation from the player's own input:
// press the hook's slot key to throw a line at what is under the crosshair; it flies there, takes
// hold, and pulls you in on a rope that is reeled in, so you swing; press the key again to let go.
// On a prop, the prop comes to you too.
//
// The key uses the item where it is: nothing comes into the hand, and what was there stays. The
// engine records "grapple.hook.used" on that tick in every simulation, and the motion starts (and
// ends) on that event. So the throw, the pull and the swing are predicted on the player's own
// screen. This file only declares the names, and gives every life a hook.

#include "mod_api.h"

using namespace cb;
using namespace cb::mods;

namespace
{

class GrappleMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "grapple";
	}

	void Declare( Declarations& declare ) override
	{
		// The hook: an item every life starts with, in a slot of its own (the fourth: its scene says
		// so), with no holster, so it is out of sight. Its scene also says it is used by its slot's key.
		m_hook = declare.ItemKind( "grapple.hook" );
		declare.ItemProperty( m_hook, "inventory.start", 1.0f );
		declare.Slots( 4 );
		// Recorded by the engine when a player uses the hook; the motion reads it by name.
		m_used = declare.Event( "grapple.hook.used" );
		m_fired = declare.Event( "grapple.fired" );
		m_moves = declare.Motions( "grapple.moves" );
	}

	void Tick( Context& ) override
	{
	}

private:
	ItemKindHandle m_hook;
	EventHandle m_used;
	EventHandle m_fired;
	MotionsHandle m_moves;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_grapple()
{
	return std::make_unique<GrappleMod>();
}
