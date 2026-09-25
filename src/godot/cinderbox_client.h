#pragma once

// The Godot face of the Cinderbox client.
//
// Owns the client thread (networking, prediction, rollback), mirrors its presentation frames and
// turns them into Godot nodes: one instance of a prefab scene per simulation entity, moved every
// frame, with ozz poses applied to players. Everything visual is data the game or mods provide:
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
// This node exposes them to scripts and HUD nodes, and applies CbStateBindings (held items, aimed
// arms, AnimationTree parameters) while their conditions hold.

#include "anim_set.h"
#include "cinderbox_effects.h"
#include "client_thread.h"
#include "fields.h"
#include "mirror.h"

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
	void connect_to_server();
	void disconnect_from_server();
	bool is_running() const;
	// camera_yaw / camera_pitch: the Godot camera's rotation (radians). actions: bits of the
	// server's mod actions (get_actions() says which bit is which).
	void set_input( const godot::Vector2& move, double camera_yaw, double camera_pitch, bool jump, bool sprint, int64_t actions );

	// What the server's mods declared. Each action: { name, bit, key }.
	godot::Array get_actions() const;
	godot::PackedStringArray get_mod_names() const;
	// A board value by name (int, float or bool as declared; null when it is not declared).
	godot::Variant get_field( int64_t net_id, const godot::String& name ) const;
	godot::Variant get_local_field( const godot::String& name ) const;
	bool check_conditions( int64_t net_id, const godot::PackedStringArray& conditions ) const;
	bool check_local_conditions( const godot::PackedStringArray& conditions ) const;
	// "{pistol.ammo} / 12" with the local player's values filled in.
	godot::String format_local_fields( const godot::String& format ) const;
	int64_t get_local_net_id() const;
	bool is_local_player_dead() const;
	// The point the camera should orbit: the local player's head, or its ragdoll while dead.
	godot::Vector3 get_camera_target() const;
	// Where a joint of an entity's character is ("RightHand"), or its node's position.
	godot::Vector3 get_bone_position( int64_t net_id, const godot::String& bone ) const;
	// "player", "prop", "static", "ragdoll", or "" if the entity has no visual.
	godot::String get_kind( int64_t net_id ) const;
	godot::String get_entity_template_name( int64_t net_id ) const;
	godot::Node3D* get_entity_node( int64_t net_id ) const;
	void add_state_binding( const godot::Ref<CbStateBinding>& binding );
	// How a kind of held item looks (from a mod's effect table); cleared with the state bindings.
	void add_item_look( const godot::Ref<CbItemLook>& look );
	void clear_state_bindings();

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

	void set_player_name( const godot::String& v )
	{
		m_playerName = v;
	}
	godot::String get_player_name_setting() const
	{
		return m_playerName;
	}
	godot::Dictionary get_stats() const;
	godot::String get_connection_state() const;
	bool has_local_player() const;
	godot::Vector3 get_local_player_position() const;
	godot::Node3D* get_visual_node( int64_t visual_id ) const;

	// Properties
	void set_host( const godot::String& v )
	{
		m_host = v;
	}
	godot::String get_host() const
	{
		return m_host;
	}
	void set_port( int v )
	{
		m_port = v;
	}
	int get_port() const
	{
		return m_port;
	}
	void set_rollback_min( int v )
	{
		m_rollbackMin = v;
	}
	int get_rollback_min() const
	{
		return m_rollbackMin;
	}
	void set_rollback_max( int v )
	{
		m_rollbackMax = v;
	}
	int get_rollback_max() const
	{
		return m_rollbackMax;
	}
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
	void set_animation_dir( const godot::String& v )
	{
		m_animationDir = v;
	}
	godot::String get_animation_dir() const
	{
		return m_animationDir;
	}

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
	bool StateHolds( const CbStateBinding& state, const present::Visual& v ) const;
	// Aims, attaches and sets tree parameters for one visual; `models` is posed in place.
	void ApplyStates( uint64_t visual, const present::Visual& v, const present::RenderPose& pose, godot::Node3D* node,
					  present::Models* models );
	void PlaceAttachments( uint64_t visual, const present::Visual& v, godot::Node3D* node );

	// Properties
	godot::String m_host = "127.0.0.1";
	int m_port = 7777;
	int m_rollbackMin = 8;
	int m_rollbackMax = 20;
	godot::String m_prefabDir = "res://prefabs";
	godot::String m_mapDir = "res://maps";
	godot::String m_animationDir;

	std::shared_ptr<const anim::AnimSet> m_animSet;
	std::unique_ptr<present::Mirror> m_mirror;
	godot::String m_character;		 // the character in use ("" = built-in)
	godot::String m_characterFolder; // res://characters/<name>/
	godot::Ref<godot::AnimationLibrary> m_companionLibrary; // the character's companion.tres, if any
	std::unordered_map<uint64_t, godot::ObjectID> m_companions; // visual id -> CbCompanionPlayer
	ClientThread m_thread;
	PublishedFrame m_frame;
	bool m_haveFrame = false;
	godot::String m_lastState;

	std::unordered_map<uint64_t, godot::ObjectID> m_nodes; // visual id -> node
	// The map's own scene, when it ships one. With it loaded the baked collision boxes are not
	// drawn: the mapper's geometry stands in for them.
	godot::ObjectID m_mapVisual;
	uint64_t m_visualMapHash = 0;
	bool m_hideStaticBoxes = false;
	std::unordered_map<std::string, godot::Ref<godot::PackedScene>> m_prefabs;

	std::vector<godot::Ref<CbStateBinding>> m_states;

	// Held items: their looks by kind, and each character's sockets (placed from the pose every
	// frame; items are their children).
	std::map<std::string, godot::String> m_itemLooks;
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
	void UpdateItem( const present::Visual& v, godot::Node3D* node );
	// Puts an item node in a socket as its "Item" (a leaving one steps aside), and tells the holder's
	// companion tracks to look again.
	void PlaceItem( uint32_t holderNetId, godot::Node3D* socket, godot::Node3D* item );
	void ItemsChanged( uint32_t holderNetId );
	// Per visual: the attachment node of each state binding that holds (0 when none).
	std::unordered_map<uint64_t, std::vector<godot::ObjectID>> m_attachments;
	std::vector<bool> m_active; // scratch: which state bindings hold for the visual being posed
	present::Models m_pose;		// scratch: the pose being built
	uint16_t m_lastActions = 0;
	uint64_t m_schemaGeneration = 0;
	uint64_t m_namesGeneration = 0;
	godot::String m_playerName;
	int SlotOfNetId( uint32_t netId ) const;
	godot::String ResolveNameFields( int64_t net_id, const godot::String& format ) const;
};

} // namespace cb::gd
