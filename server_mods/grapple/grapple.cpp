// A grappling hook: the example of a probe, a force and a link (sim/motions.h).
//
// The hook is not in this file. It is one CbMotion node (with a CbProbe, a CbForce and a CbLink
// under it) in the mod's client project
// (server_mods/grapple/client/motion_sets/grapple_moves.tscn), baked to motions/grapple.moves.cfg
// and run by every simulation from the player's own input: press Q to throw a line at what is under
// the crosshair; it flies there, takes hold, and pulls you in on a rope that is reeled in, so you
// swing; press Q again to let go. On a prop, the prop comes to you too.
//
// So the throw, the pull and the swing are predicted on the player's own screen. This file only
// declares the names the motion uses.

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
		// The motion reads these by name.
		m_grapple = declare.Action( "grapple", "Q" );
		m_fired = declare.Event( "grapple.fired" );
		m_moves = declare.Motions( "grapple.moves" );
	}

	void Tick( Context& ) override
	{
	}

private:
	ActionHandle m_grapple;
	EventHandle m_fired;
	MotionsHandle m_moves;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_grapple()
{
	return std::make_unique<GrappleMod>();
}
