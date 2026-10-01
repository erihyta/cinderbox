#pragma once

// A view source that is a Godot object in another library, or a script: frames cross as bytes
// (present/view_codec.h) and nothing else does. The object needs one method,
//
//   take( whole: bool ) -> PackedByteArray     the newest frame as a packet, or empty when there is
//                                              none newer than the last one it returned. `whole`
//                                              asks for a packet that stands alone (the first
//                                              time, and after one that did not decode); otherwise
//                                              it may be a delta against the last it returned.
// and may have
//   takes_input() -> bool                      whether set_input drives a local player
//   set_input( input: PackedByteArray )        a PlayerInput (sim/types.h), 10 bytes
//   control( name: String, value: float )      a named command
//
// The peer extension's CinderboxPeer is one (src/godot/peer); a GDScript object can be another.

#include "view.h"

#include <godot_cpp/variant/variant.hpp>

namespace cb::gd
{

class ObjectSource final : public present::ViewSource
{
public:
	// Holds a reference to `object` (a RefCounted stays alive; a Node is the caller's to keep).
	explicit ObjectSource( const godot::Variant& object );

	bool Take( present::ViewFrame& out ) override;
	bool TakesInput() const override;
	void SetInput( const PlayerInput& input ) override;
	void Control( const std::string& name, double value ) override;

private:
	godot::Object* Target() const;

	godot::Variant m_object;
	present::ViewFrame m_last; // the frame the next delta is against
	present::ViewFrame m_scratch;
	bool m_haveLast = false;
	bool m_wantWhole = true;
	bool m_warned = false;
};

} // namespace cb::gd
