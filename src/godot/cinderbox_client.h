#pragma once

// The Godot viewer of a Cinderbox world.
//
// It draws what a view source hands it (present/view.h) and never asks what the source is: a view
// file it plays itself, or an object from elsewhere that hands frames over as bytes (the peer
// extension's CinderboxPeer: a live connection, a recording; or a script). This library holds no
// simulation and no networking. It mirrors the frames and turns them into Godot nodes, one
// instance of a prefab scene per simulation entity, moved every frame, with ozz poses applied to
// players. Everything visual is data the game or mods provide:
//   <prefab_dir>/static_box.tscn   level geometry        (unit box, scaled to size)
//   <prefab_dir>/prop_box.tscn      box props             (unit box)
//   <prefab_dir>/prop_sphere.tscn   sphere props          (unit-diameter sphere)
//   <prefab_dir>/player.tscn        players               (origin at the feet, facing +Z;
//                                                          contains a CinderboxSkeleton)
//   <prefab_dir>/ragdoll.tscn       ragdolls (optional; player.tscn is used without it)
// Gameplay never depends on these: the simulation does not know Godot exists.
//
// What the server's mods add reaches presentation as names, never as code: the actions a player
// can press (with suggested keys), board fields ("pistol.ammo") and mod events ("pistol.fired").
// This node exposes them to scripts and HUD nodes. Entity nodes live under its World node, a
// CbDirector (cue_director.h), which it tells about entities, their board as state, and events as
// cues; the CbReaction nodes in the scenes do the rest.

#include "anim_set.h"
#include "cinderbox_item_look.h"
#include "cue_director.h"
#include "fields.h"
#include "mirror.h"
#include "view.h"

#include <godot_cpp/classes/animation_library.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <memory>
#include <unordered_map>

namespace cb::gd
{

class CinderboxClient : public godot::Node3D
{
	GDCLASS( CinderboxClient, godot::Node3D )

public:
	CinderboxClient();
	~CinderboxClient() override;

	void _process( double delta ) override;
	void _enter_tree() override;
	void _exit_tree() override;

	// Scripting API
	// Sources: what this viewer draws. One at a time; setting one drops the one before.
	// An object that hands frames over as bytes: take( whole ) -> PackedByteArray, and optionally
	// takes_input(), set_input( bytes ), control( name, value ) (see object_source.h). The viewer
	// keeps a reference to it; starting and stopping it is its owner's business.
	void set_source( const godot::Variant& source );
	// Plays a view file (cb_server --record-view): frames as they were, with no simulation. `path`
	// may be res://, user:// or a file system path.
	void open_view( const godot::String& path );
	// Drops the source (the last frame stays drawn).
	void stop();
	bool is_running() const;
	// A named command for the source, with a number ("pause" 1, "skip" -5: see the source's
	// header, e.g. client/replay_source.h). Sources ignore names they do not know.
	void control( const godot::String& name, double value );
	// Whether set_input drives the local player (a live connection; not a recording).
	bool takes_input() const;
	// camera_yaw / camera_pitch: the Godot camera's rotation (radians). actions: bits of the
	// server's mod actions (get_actions() says which bit is which).
	void set_input( const godot::Vector2& move, double camera_yaw, double camera_pitch, bool jump, bool sprint, int64_t actions,
					int64_t view = 0 );
	// Where the local player's line of sight starts for a view (ViewMode: 0 behind, 1 first person,
	// 2 / 3 over the right / left shoulder), with the camera turned as `camera` is: the point the
	// server takes for it too (mods::Context::ViewPosition), so the crosshair is exact.
	godot::Vector3 get_view_position( int64_t view, const godot::Basis& camera ) const;

	// What the server's mods declared. Each action: { name, bit, key }.
	godot::Array get_actions() const;
	godot::PackedStringArray get_mod_names() const;
	// A board value by name (int, float or bool as declared; null when it is not declared).
	godot::Variant get_field( int64_t net_id, const godot::String& name ) const;
	godot::Variant get_local_field( const godot::String& name ) const;
	bool check_conditions( int64_t net_id, const godot::PackedStringArray& conditions ) const;
	bool check_local_conditions( const godot::PackedStringArray& conditions ) const;
	godot::Variant evaluate( int64_t net_id, const godot::String& expression ) const;
	godot::Variant evaluate_local( const godot::String& expression ) const;
	// "{pistol.ammo} / 12" with the local player's values filled in.
	godot::String format_local_fields( const godot::String& format ) const;
	int64_t get_local_net_id() const;
	bool is_local_player_dead() const;
	// The point the camera should orbit: the local player's head, or its ragdoll while dead.
	// The game's clock as drawn: the simulation tick, with the fraction of the one being shown, and
	// how many ticks make a second. For looks that count from a tick a field holds.
	double get_tick_time() const;
	double get_tick_rate() const;
	godot::Vector3 get_camera_target() const;
	// How far a camera can back away from `target` along `direction` before the map is in the way
	// (at most `max_distance`; `radius` keeps it that far off the surface). Props and players never
	// block it.
	double get_camera_distance( const godot::Vector3& target, const godot::Vector3& direction, double max_distance, double radius ) const;
	// Where a joint of an entity's character is ("RightHand"), or its node's position.
	godot::Vector3 get_bone_position( int64_t net_id, const godot::String& bone ) const;
	// "player", "prop", "static", "ragdoll", or "" if the entity has no visual.
	godot::String get_kind( int64_t net_id ) const;
	godot::String get_entity_template_name( int64_t net_id ) const;
	godot::Node3D* get_entity_node( int64_t net_id ) const;
	// A world reactions scene (res://vfx/reactions*.tscn): kept under this node, its CbReaction
	// nodes react to every event, its CbItemLook nodes say how held items look.
	void add_world_scene( godot::Node* scene );
	void clear_world_scenes();
	// The World node (creates it on first use).
	CbDirector* get_director();

	// Players: net ids of everyone in the world, and their names.
	godot::PackedInt64Array get_players() const;
	godot::String get_player_name( int64_t net_id ) const;
	// "{name}: {combat.kills}" for one entity: {name} is its player's name, the rest board fields.
	godot::String format_fields( int64_t net_id, const godot::String& format ) const;
	// The workshop items this server's mods need: [{ mod, sha256 }].
	godot::Array get_required_items() const;
	// The character item everyone plays as on this server ("" for the built-in rig).
	godot::String get_character() const;
	// Plays as characters/<name>/ from the mounted packs: its baked ozz skeleton and clips
	// (anim.cfg) drive every player, and character.tscn is the player prefab. "" goes back to the
	// built-in rig. Returns an error message, or "" when it worked. Nothing is imported here: the
	// files were baked in the editor and shipped in the item.
	godot::String use_character( const godot::String& name );
	// The schema's state machine for this character, compiled; warns when it will not fit.
	std::shared_ptr<const AnimGraph> ServerGraph( const anim::AnimSet& set, const godot::String& name );
	// The server's animation packs, compiled, and their clips (res://anim/<pack>/, from the mods'
	// items) fitted to this character.
	void ServerPacks( const anim::AnimSet& set, AnimGraphPacks& packs, std::vector<std::shared_ptr<const anim::PackClips>>& clips );

	// What the source says about itself ("rtt_ms", "desyncs", ...), plus "state", "entities" and
	// "animation".
	godot::Dictionary get_stats() const;
	// "stopped", "starting", then the source's state: "connecting", "joining", "playing",
	// "reconnecting", "rejected".
	godot::String get_source_state() const;
	bool has_local_player() const;
	godot::Vector3 get_local_player_position() const;
	godot::Node3D* get_visual_node( int64_t visual_id ) const;

	// Properties
	void set_prefab_dir( const godot::String& v )
	{
		m_prefabDir = v;
	}
	godot::String get_prefab_dir() const
	{
		return m_prefabDir;
	}
	void set_map_dir( const godot::String& v )
	{
		m_mapDir = v;
	}
	godot::String get_map_dir() const
	{
		return m_mapDir;
	}
	// Name of the map the server is running, "" until it has been received.
	godot::String get_map_name() const;

protected:
	static void _bind_methods();

private:
	godot::String TemplateName( uint32_t index ) const;
	void EnsureAnimations();
	void UpdateMapVisual();
	void HandleEvents();
	void UpdateNodes();
	godot::Ref<godot::PackedScene> Prefab( const present::Visual& v );
	godot::Ref<godot::PackedScene> LoadPrefab( const char* name );
	godot::Ref<godot::PackedScene> LoadScene( const godot::String& path );
	godot::Node3D* CreateNode( uint64_t visual, const present::Visual& v );
	// Players and ragdolls again with the current prefab (after the character changed).
	void RebuildCharacterNodes();
	const Blackboard* BoardOf( uint32_t netId ) const;
	std::vector<std::string> Conditions( const godot::PackedStringArray& conditions ) const;

	// Properties
	godot::String m_prefabDir = "res://prefabs";
	godot::String m_mapDir = "res://maps";

	std::shared_ptr<const anim::AnimSet> m_animSet;
	std::unique_ptr<present::Mirror> m_mirror;
	godot::String m_character;		 // the character in use ("" = built-in)
	godot::String m_characterFolder; // res://characters/<name>/
	// What the character's animations do besides moving bones, copied out of its AnimationPlayer
	// when the first one is drawn (null: nothing, or not looked yet).
	godot::Ref<godot::AnimationLibrary> m_trackLibrary;
	bool m_trackLibraryBuilt = false;
	std::unordered_map<uint64_t, godot::ObjectID> m_trackPlayers; // visual id -> CbTrackPlayer
	std::unique_ptr<present::ViewSource> m_source;
	void Open( std::unique_ptr<present::ViewSource> source );
	// How many sources this viewer has had: mixed into each frame's reset generation, so the first
	// frame of a new source replaces the world instead of blending into the old one.
	uint64_t m_sourceCount = 0;
	present::ViewFrame m_frame;
	bool m_haveFrame = false;
	float m_alpha = 0.0f; // of the frame being drawn
	godot::String m_lastState;

	std::unordered_map<uint64_t, godot::ObjectID> m_nodes; // visual id -> node
	// The map's own scene, when it ships one. With it loaded the baked collision boxes are not
	// drawn: the mapper's geometry stands in for them.
	godot::ObjectID m_mapVisual;
	uint64_t m_visualMapHash = 0;
	bool m_hideStaticBoxes = false;
	std::unordered_map<std::string, godot::Ref<godot::PackedScene>> m_prefabs;

	// The World node, a CbDirector: every entity node, world reaction scene and placed effect lives
	// under it, and it runs the CbReaction nodes in them from cues and entity state.
	godot::ObjectID m_director;
	CbDirector* Director();
	std::vector<godot::ObjectID> m_worldScenes;
	std::unordered_map<uint64_t, uint64_t> m_stateHashes; // visual -> hash of the board last pushed
	uint64_t m_worldStateHash = 0;
	void PushStates();
	void Cue( const std::string& name, uint32_t a, uint32_t b, const godot::Dictionary& args );
	// The local player's private fields: this frame's, with what its predictions change. Null for
	// anyone else (they were never sent).
	Blackboard m_privates;
	const Blackboard* PrivatesOf( uint32_t netId ) const
	{
		return netId != 0 && netId == m_frame.frame.localNetId ? &m_privates : nullptr;
	}
	// What the looks' predictions say until the server answers (CbPrediction), for the local player:
	// its upper body shown ahead (present/anim_lead.h), before the mirror updates...
	void LeadLocalPlayer( float delta );
	float m_lead = 0.0f; // seconds the local player's upper layers are shown ahead
	// ... and its fields changed, after this frame's cues were heard.
	void ApplyPredictedFields();
	// The "action_pressed" signal for each of these action bits, and the press itself to the
	// director, whose predictions show what the server will answer.
	void AnnouncePresses( uint16_t pressed );
	godot::String EntityName( const present::Visual& v ) const;
	// Sockets are moved to their entity's root in the game (so "^^/RightHand/Item" means the same on
	// every rig); the animation tracks that reached an item through the socket's authored place are
	// pointed at the new one, once per library.
	std::vector<std::pair<godot::String, godot::String>> m_socketMoves; // from the entity: old path, new
	godot::ObjectID m_retargetedLibrary;
	void RetargetTracks( godot::Node* entity, godot::Node* root );

	// Held items: their looks by kind, and each character's sockets (placed from the pose every
	// frame; items are their children).
	std::map<std::string, godot::String> m_itemLooks;
	std::map<std::string, godot::String> m_itemNames; // what prompts call a kind ("Bat")
	std::unordered_map<uint64_t, uint32_t> m_itemHolders; // item visual -> the holder it was last drawn with
	// What every player holds, by item kind (refreshed every frame): an item kind's name is a
	// condition, true while the player holds one ("pistol.gun"), as in the state machines.
	std::unordered_map<uint32_t, std::vector<uint16_t>> m_heldKinds;
	void RefreshHeldKinds();
	bool Holds( uint32_t netId, uint16_t kind ) const;
	// "{key:pickup}" -> the binding of the server's action; "{look:pickup.target}" -> what the entity
	// whose NetId the field holds is called (an item's display name, a player's name).
	godot::String ResolveKeysAndLooks( int64_t net_id, const godot::String& format ) const;
	struct SocketPlace
	{
		godot::ObjectID node;
		godot::String name;
		godot::String bone;
		godot::Transform3D local; // in the bone's frame, or in the items' hand frame
		bool itemFrame = false;	  // a built-in hand: `local` is in AnimSet::AttachFrame's frame
	};
	std::map<uint64_t, std::vector<SocketPlace>> m_sockets;
	void CollectSockets( uint64_t visual, godot::Node3D* node );
	void PlaceSockets( uint64_t visual, godot::Node3D* node );
	godot::Node3D* SocketNode( uint32_t holderNetId, uint8_t socket ) const;
	void UpdateItem( uint64_t visual, const present::Visual& v, const present::RenderPose& pose, godot::Node3D* node );
	// Puts an item node in a socket as its "Item" (a leaving one steps aside), and tells the holder's
	// animation tracks to look again.
	void PlaceItem( uint32_t holderNetId, godot::Node3D* socket, godot::Node3D* item );
	void ItemsChanged( uint32_t holderNetId );
	present::Models m_pose;		// scratch: the pose being built
	uint16_t m_lastActions = 0;
	uint64_t m_schemaGeneration = 0;
	uint64_t m_namesGeneration = 0;
	int SlotOfNetId( uint32_t netId ) const;
	godot::String ResolveNameFields( int64_t net_id, const godot::String& format ) const;
};

} // namespace cb::gd
