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
//   send_text( channel: String, text: String ) words typed into a channel's box, for the server
//   camera_free_distance( origin: Vector3, direction: Vector3, max: float, radius: float ) -> float
//                                              how far a camera's ball goes before the level's
//                                              static shapes stop it (ViewSource); -1: cannot say
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
	void SendText( const std::string& channel, const std::string& text ) override;
	float CameraFreeDistance( b3Vec3 origin, b3Vec3 direction, float maxDistance, float radius ) override;

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
