#include "cinderbox_companion.h"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <cmath>

using namespace godot;

namespace cb::gd
{

namespace
{

// Forward moves up to this long fire keys; longer jumps (a join, a reconnect) only apply values.
constexpr double kMaxFiringStep = 0.25;

} // namespace

void CbCompanionPlayer::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "setup", "library", "root" ), &CbCompanionPlayer::setup );
	ClassDB::bind_method( D_METHOD( "begin_frame" ), &CbCompanionPlayer::begin_frame );
	ClassDB::bind_method( D_METHOD( "play_at", "channel", "clip", "time", "loops" ), &CbCompanionPlayer::play_at );
	ClassDB::bind_method( D_METHOD( "end_frame" ), &CbCompanionPlayer::end_frame );
	ClassDB::bind_method( D_METHOD( "get_channel_clip", "channel" ), &CbCompanionPlayer::get_channel_clip );
}

void CbCompanionPlayer::setup( const Ref<AnimationLibrary>& library, Node* root )
{
	m_library = library;
	m_root = root != nullptr ? uint64_t( root->get_instance_id() ) : 0;
}

CbCompanionPlayer::Channel& CbCompanionPlayer::ChannelAt( int channel )
{
	if ( channel >= int( m_channels.size() ) )
	{
		m_channels.resize( size_t( channel + 1 ) );
	}
	Channel& c = m_channels[size_t( channel )];
	if ( c.player == nullptr )
	{
		c.player = memnew( AnimationPlayer );
		c.player->set_name( "Channel" + String::num_int64( channel ) );
		add_child( c.player );
		if ( Node* root = Object::cast_to<Node>( ObjectDB::get_instance( ObjectID( m_root ) ) ) )
		{
			c.player->set_root_node( c.player->get_path_to( root ) );
		}
		// Driven by play_at only, never by the frame clock.
		c.player->set_callback_mode_process( AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL );
		c.player->set_callback_mode_method( AnimationMixer::ANIMATION_CALLBACK_MODE_METHOD_IMMEDIATE );
		// Several channels share one library; in deterministic mode each would keep writing RESET
		// values to the tracks its current clip lacks, undoing what the other channels play.
		c.player->set_deterministic( false );
		if ( m_library.is_valid() )
		{
			c.player->add_animation_library( "", m_library );
		}
	}
	return c;
}

void CbCompanionPlayer::Reset( Channel& c )
{
	if ( c.player == nullptr || c.clip.is_empty() )
	{
		return;
	}
	if ( c.player->has_animation( "RESET" ) )
	{
		c.player->play( "RESET" );
		Seek( c, 0.0 );
	}
	else
	{
		Seek( c, 0.0 ); // no RESET: what the clip leaves behind goes back to how it starts
	}
	c.player->stop();
	c.clip = String();
	m_resetThisFrame = true;
}

void CbCompanionPlayer::Seek( Channel& c, double time )
{
	// update_only: apply values, but run no method, audio or animation-playback keys.
	c.player->seek( time, true, true );
}

void CbCompanionPlayer::begin_frame()
{
	for ( Channel& c : m_channels )
	{
		c.touched = false;
	}
}

void CbCompanionPlayer::play_at( int channel, const String& clip, double time, bool loops )
{
	if ( channel < 0 || channel > 16 || m_library.is_null() )
	{
		return;
	}
	Channel& c = ChannelAt( channel );
	c.touched = true;
	if ( m_library->has_animation( clip ) == false )
	{
		Reset( c ); // nothing of its own on this clip
		return;
	}
	if ( clip != c.clip )
	{
		// A new clip: its values at `time`; keys before `time` are history and do not fire.
		c.player->play( clip );
		Seek( c, time );
		c.clip = clip;
		c.time = time;
		c.high = time;
		return;
	}

	double length = m_library->get_animation( clip )->get_length();
	double step = time - c.time;
	bool wrapped = false;
	if ( loops && step < -0.5 * length )
	{
		step += length; // came round the loop: a new cycle
		wrapped = true;
	}
	if ( step > 0.0 && step < kMaxFiringStep )
	{
		if ( wrapped )
		{
			c.player->advance( step );
			c.high = time;
		}
		else if ( time > c.high )
		{
			// Fire only what lies past the furthest point reached, so a rollback that stepped back
			// and forward again does not fire the same key twice.
			if ( c.time < c.high )
			{
				Seek( c, c.high );
			}
			c.player->advance( time - c.high );
			c.high = time;
		}
		else
		{
			Seek( c, time );
		}
	}
	else if ( step != 0.0 )
	{
		Seek( c, time ); // back, or a long jump: values only
		if ( step > 0.0 )
		{
			c.high = std::max( c.high, time );
		}
	}
	c.time = time;
}

void CbCompanionPlayer::end_frame()
{
	for ( Channel& c : m_channels )
	{
		if ( c.touched == false )
		{
			Reset( c );
		}
	}
	if ( m_resetThisFrame )
	{
		// RESET covers every companion track; the channels still playing put their own values back.
		for ( Channel& c : m_channels )
		{
			if ( c.clip.is_empty() == false )
			{
				Seek( c, c.time );
			}
		}
		m_resetThisFrame = false;
	}
}

String CbCompanionPlayer::get_channel_clip( int channel ) const
{
	return channel >= 0 && channel < int( m_channels.size() ) ? m_channels[size_t( channel )].clip : String();
}

} // namespace cb::gd
