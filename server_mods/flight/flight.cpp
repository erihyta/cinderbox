// Flying, a jetpack and a glide: the example of motions that hold while conditions do
// (sim/motions.h).
//
// None of the three is in this file. They are CbMotion nodes in the mod's client project
// (server_mods/flight/client/motion_sets/flight_moves.tscn), baked to motions/flight.moves.cfg and
// run by every simulation from the player's own input:
//
//   T                   flight on and off: gravity gone, WASD moves along the camera, up and down too
//   Space, in the air   the jetpack: a thrust upward while it has fuel; the fuel comes back on the ground
//   Shift, falling      a glide: a slow fall
//
// So all of it is predicted on the player's own screen, the fuel gauge included: the fuel is a
// field the motions count down and up themselves. This file is the rules: the names the motions
// use, and how much fuel a player starts with.
//
// Server options: flight.fuel (what a player joins with, default 100: a full tank).

#include "mod_api.h"

#include <algorithm>

using namespace cb;
using namespace cb::mods;

namespace
{

class FlightMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "flight";
	}

	void Declare( Declarations& declare ) override
	{
		// The motions read and write these by name.
		m_fly = declare.Action( "fly", "T" );
		m_on = declare.Field( "flight.on", BoardType::Bool );
		m_fuel = declare.Field( "flight.fuel", BoardType::Float );
		m_started = declare.Event( "flight.started" );
		m_stopped = declare.Event( "flight.stopped" );
		m_thrust = declare.Event( "flight.thrust" );
		m_moves = declare.Motions( "flight.moves" );
	}

	void Start( Context& ctx ) override
	{
		m_startFuel = float( std::clamp( ctx.Option( "flight.fuel", 100.0 ), 0.0, 100.0 ) );
	}

	void Tick( Context& ctx ) override
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Joining( slot ) )
			{
				ctx.SetFloat( SlotTarget( slot ), m_fuel, m_startFuel );
			}
		}
	}

private:
	ActionHandle m_fly;
	FieldHandle m_on;
	FieldHandle m_fuel;
	EventHandle m_started;
	EventHandle m_stopped;
	EventHandle m_thrust;
	MotionsHandle m_moves;
	float m_startFuel = 100.0f;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_flight()
{
	return std::make_unique<FlightMod>();
}
