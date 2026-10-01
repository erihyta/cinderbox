#pragma once

// What a scene may be made of before it is instantiated under a director: the other half of
// "presentation only, and contained". Scenes come from packs other people made. A pack has no
// scripts (the game's pack validator refuses them), but a scene without a script can still do
// things: any built-in node can be in it, signals can be wired to methods, animations call methods
// and move properties, and an AnimationTree evaluates expressions. So, before a scene is used:
//
//   nodes         only classes on a list (meshes, particles, lights, sounds, animation, UI controls,
//                 and the Cb* / Cinderbox* nodes); never an HTTPRequest, a Window, a Camera, a
//                 viewport, anything that reaches outside the scene or the process
//   scripts       none, on a node or on a resource a node holds
//   connections   none (a wired signal is a method call nobody listed)
//   node paths    a property that is a NodePath stays inside the scene: it climbs no higher than
//                 the scene's root, and is not absolute
//   animations    method tracks call only listed methods; track paths do not climb or start at
//                 the root; no track sets a script
//   expressions   an AnimationTree's advance expressions are cleared in the game (they run
//                 methods; the baked state machines the simulation runs do not need them)
//   scenes inside the same, all the way down
//
// The same method list is what a CbReaction may call.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

namespace cb::cue
{

bool NodeClassAllowed( const godot::StringName& type );
// Methods a reaction or an animation's method track may call: verbs of presentation (restart, play,
// stop, show, hide, a few setters), nothing that names another method, frees, or rewires.
bool MethodAllowed( const godot::String& method );
// Properties a reaction or an animation may set: anything but a script or metadata.
bool PropertyAllowed( const godot::String& property );

// "" when the scene may be instantiated, otherwise why not (the first reason found). Remembered
// per scene.
godot::String CheckScene( const godot::Ref<godot::PackedScene>& scene );
// The same for animations loaded on their own (an AnimationLibrary, an Animation).
godot::String CheckResource( const godot::Ref<godot::Resource>& resource );

// An instance of the scene, or null (with a warning, once per scene) when it is refused or empty.
godot::Node* Instantiate( const godot::Ref<godot::PackedScene>& scene );

} // namespace cb::cue
