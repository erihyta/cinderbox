#pragma once

// What a body is, said one way wherever a scene describes one: what it weighs, how it grips, how it
// bounces. A map's prop (CbProp) and an item (CbItem) are both one, so both have these; a template
// (CbTemplate) says the same three through its Material component, which is the simulation's own
// name for them.
//
//   mass      kilograms. 0: its volume times `density`
//   density   kilograms per cubic metre, used when no mass is given (40: a 1 m crate is 40 kg)
//   friction  0 is ice, 0.6 the usual, more grips harder
//   bounce    0 does not bounce, 1 loses nothing
//   rolling   rolling resistance (balls, capsules): a ball on the ground slows by about this times
//             gravity. 0 rolls on until something stops it (a map's prop starts at 0.05)
//   drag      how fast its speed and spin fade, per second, in the air too (a beach ball: 1)
//
// Not a node to place: the base of the ones that are.

#include <godot_cpp/classes/node3d.hpp>

namespace cb::gd
{

class CbBody : public godot::Node3D
{
	GDCLASS( CbBody, godot::Node3D )

public:
	void set_mass( double v )
	{
		m_mass = v;
	}
	double get_mass() const
	{
		return m_mass;
	}
	void set_density( double v )
	{
		m_density = v;
	}
	double get_density() const
	{
		return m_density;
	}
	void set_friction( double v )
	{
		m_friction = v;
	}
	double get_friction() const
	{
		return m_friction;
	}
	void set_bounce( double v )
	{
		m_bounce = v;
	}
	double get_bounce() const
	{
		return m_bounce;
	}

	void set_rolling( double v )
	{
		m_rolling = v;
	}
	double get_rolling() const
	{
		return m_rolling;
	}
	void set_drag( double v )
	{
		m_drag = v;
	}
	double get_drag() const
	{
		return m_drag;
	}

	// What it weighs for a volume: its mass, or its density's worth.
	double MassFor( double volume ) const
	{
		return m_mass > 0.0 ? m_mass : m_density * volume;
	}

protected:
	static void _bind_methods();

	double m_mass = 0.0;
	double m_density = 40.0;
	double m_friction = 0.6;
	double m_bounce = 0.0;
	double m_rolling = 0.0;
	double m_drag = 0.0;
};

} // namespace cb::gd
