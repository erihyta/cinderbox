// A grappling hook: the example of a probe, a force and a link (sim/motions.h), and of an item
// used by its own key wherever it is carried.
//
// The hook is not in this file. It is an item (client/prefabs/hook.tscn, a CbItem used once a press,
// its own key Q) with its own CbMotion under it (a CbProbe, a CbForce and a CbLink under that),
// and its pose while it is on (its animation pack), baked and run by every simulation from the
// player's own input:
// press the hook's key (Q; or select it and press the use button) to throw a line at what is under the crosshair; it flies there, a
// projectile, takes hold of what it meets first, and pulls you in on a rope that is reeled in, so
// you swing; press the key again to let go. At nothing, it flies out its range and is gone.
// It holds only the level (anything static): a prop or a player in its way stops it, holding
// nothing. Pulling things in (a winch, a claw) would be a mod of its own.
//
// The key uses the item where it is: nothing comes into the hand, and what was there stays. The
// engine records "grapple.hook.used" on that tick in every simulation, and the motion starts (and
// ends) on that event. So the throw, the pull and the swing are predicted on the player's own
// screen. This file only declares the events; the scene says the rest (a hook for every life with
// the inventory mod: "inventory.start").

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
		declare.Slots( 4 );
		// Recorded by the engine when a player uses the hook; the motion reads it by name.
		m_used = declare.Event( "grapple.hook.used" );
		m_fired = declare.Event( "grapple.fired" );
		// Said by the hook's line where and when it takes hold (CbProbe.hold_emits): a puff there.
		m_hooked = declare.Event( "grapple.hooked" );
		// The hook, its motion and its pose: all of it is its scene's.
		declare.ItemsOfLook();
	}

	void Tick( Context& ) override
	{
	}

private:
	EventHandle m_used;
	EventHandle m_fired;
	EventHandle m_hooked;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_grapple()
{
	return std::make_unique<GrappleMod>();
}
