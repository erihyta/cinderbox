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
// Gameplay never depends on these: the simulation does not know Godot exists.

#include "anim_set.h"
#include "client_thread.h"
#include "mirror.h"

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
	void _exit_tree() override;

	// Scripting API
	void connect_to_server();
	void disconnect_from_server();
	bool is_running() const;
	void set_input( const godot::Vector2& move, double camera_yaw, bool jump, bool sprint, bool spawn_prop );
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
};

} // namespace cb::gd
