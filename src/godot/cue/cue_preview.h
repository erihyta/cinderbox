#pragma once

// Cue Preview: an editor bottom panel that plays the edited scene's reactions with no game running.
//
// The edited scene is copied (unsaved edits included) onto a small stage under a CbDirector, with
// two stand-in players (player_0, player_1: a body and RightHand / LeftHand / Head sockets):
//   Held item   the scene is player_0/RightHand/Item (so ^^ is player_0)
//   Character   the scene is player_0
//   World       the scene sits under the director, like a vfx/reactions*.tscn
// Then: fire a cue (its name, $at, $other, value, strength; the point is $other's chest), set any
// state name the scene's conditions read on any entity, pick the local player. Screen effects flash
// and shake the preview. The stage is only a copy: nothing here touches the edited scene.

#include "cue_director.h"
#include "cue_info.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/h_split_container.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/option_button.hpp>
#include <godot_cpp/classes/spin_box.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <vector>

namespace cb::gd
{

class CbCuePreviewDock : public godot::HSplitContainer
{
	GDCLASS( CbCuePreviewDock, godot::HSplitContainer )

public:
	void Build();
	void SetEdited( godot::Node* root );
	void _process( double delta ) override;

protected:
	static void _bind_methods()
	{
	}

private:
	enum Mode
	{
		MODE_AUTO = 0,
		MODE_ITEM = 1,
		MODE_CHARACTER = 2,
		MODE_WORLD = 3,
	};
	enum Who
	{
		WHO_PLAYER0 = 0,
		WHO_PLAYER1 = 1,
		WHO_ITEM = 2,
		WHO_WORLD = 3,
		WHO_NONE = 4,
	};

	void Rebuild();
	int DetectMode( godot::Node* root ) const;
	godot::Node3D* StandIn( const godot::String& name, const godot::Vector3& at, const godot::Color& color );
	godot::Node* EntityFor( int who ) const;
	void CollectNames();
	void RefreshState();
	void OnReload();
	void OnCuePicked( int index );
	void OnFire();
	void OnStateEntity( int index );
	void OnStateValue( double value, godot::String name );
	void OnLocal( int index );
	void OnScreenEffect( double shake, double shakeTime, godot::Color flash, double flashTime );

	godot::ObjectID m_edited;
	godot::SubViewport* m_viewport = nullptr;
	godot::Node3D* m_stage = nullptr;
	godot::Camera3D* m_camera = nullptr;
	godot::Transform3D m_cameraHome;
	godot::ColorRect* m_flash = nullptr;
	CbDirector* m_director = nullptr;
	godot::ObjectID m_player0;
	godot::ObjectID m_player1;
	godot::ObjectID m_item;
	godot::Dictionary m_states[4]; // what the controls set, per Who (kept across reloads)

	godot::Label* m_title = nullptr;
	godot::OptionButton* m_mode = nullptr;
	godot::LineEdit* m_cue = nullptr;
	godot::OptionButton* m_cueNames = nullptr;
	godot::OptionButton* m_at = nullptr;
	godot::OptionButton* m_other = nullptr;
	godot::SpinBox* m_value = nullptr;
	godot::SpinBox* m_strength = nullptr;
	godot::OptionButton* m_stateEntity = nullptr;
	godot::VBoxContainer* m_stateRows = nullptr;
	godot::OptionButton* m_local = nullptr;
	godot::Label* m_log = nullptr;
	std::vector<godot::String> m_names; // state names the scene's conditions read
	std::vector<godot::String> m_cues;	// cue names its reactions listen for

	double m_shake = 0.0;
	double m_shakeDecay = 1.0;
	double m_flashAlpha = 0.0;
	double m_flashDecay = 1.0;
	godot::Color m_flashColor;
};

class CbCuePreviewPlugin : public godot::EditorPlugin
{
	GDCLASS( CbCuePreviewPlugin, godot::EditorPlugin )

public:
	void _enter_tree() override;
	void _exit_tree() override;

protected:
	static void _bind_methods()
	{
	}

private:
	void OnSceneChanged( godot::Node* root );
	CbCuePreviewDock* m_dock = nullptr;
	godot::Ref<CbReactionInspector> m_inspector;
};

} // namespace cb::gd
