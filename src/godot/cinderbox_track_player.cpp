#include "cinderbox_track_player.h"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace cb::gd
{

namespace
{

// Forward moves up to this long fire keys; longer jumps (a join, a reconnect) only apply values.
constexpr double kMaxFiringStep = 0.25;
// A clip first seen up to this far in plays its tracks from the start (behind latency), faster than
// the pose by kCatchUpPace until they are level.
constexpr double kMaxLateStart = 0.5;
constexpr double kCatchUpPace = 2.0;

} // namespace

void CbTrackPlayer::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "setup", "library", "root" ), &CbTrackPlayer::setup );
	ClassDB::bind_method( D_METHOD( "begin_frame" ), &CbTrackPlayer::begin_frame );
	ClassDB::bind_method( D_METHOD( "play_at", "channel", "clip", "time", "loops", "restarted" ), &CbTrackPlayer::play_at, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "end_frame" ), &CbTrackPlayer::end_frame );
	ClassDB::bind_method( D_METHOD( "get_channel_clip", "channel" ), &CbTrackPlayer::get_channel_clip );
	ClassDB::bind_method( D_METHOD( "get_late_start_count" ), &CbTrackPlayer::get_late_start_count );
	ClassDB::bind_method( D_METHOD( "get_caught_up_count" ), &CbTrackPlayer::get_caught_up_count );
	ClassDB::bind_method( D_METHOD( "clear_caches" ), &CbTrackPlayer::clear_caches );
}

void CbTrackPlayer::setup( const Ref<AnimationLibrary>& library, Node* root )
{
	m_library = library;
	m_root = root != nullptr ? uint64_t( root->get_instance_id() ) : 0;
}

CbTrackPlayer::Channel& CbTrackPlayer::ChannelAt( int channel )
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

void CbTrackPlayer::Leave( Channel& c )
{
	if ( c.clip.is_empty() )
	{
		return;
	}
	if ( c.catching && c.time > c.track && c.time - c.track < 1.0 )
	{
		c.player->advance( c.time - c.track );
		c.high = std::max( c.high, c.time );
	}
	c.catching = false;
	c.leftClip = c.clip;
	c.leftTime = c.time;
}

void CbTrackPlayer::CatchUp( Channel& c, double time )
{
	double step = time - c.time;
	c.time = time;
	if ( step >= kMaxFiringStep )
	{
		// A long jump while catching up: values only, as any long jump.
		Seek( c, time );
		c.catching = false;
		c.high = time;
		return;
	}
	double target = std::min( time, c.track + kCatchUpPace * std::max( step, 0.0 ) );
	if ( target > c.track )
	{
		c.player->advance( target - c.track );
		c.track = target;
	}
	if ( c.track >= time )
	{
		// Level with the pose (or a correction stepped the pose back behind the tracks).
		c.catching = false;
		m_caughtUp += 1;
		c.high = c.track;
		if ( c.track != time )
		{
			Seek( c, time );
		}
	}
}

void CbTrackPlayer::Reset( Channel& c )
{
	if ( c.player == nullptr || c.clip.is_empty() )
	{
		return;
	}
	Leave( c );
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

void CbTrackPlayer::Seek( Channel& c, double time )
{
	// update_only: apply values, but run no method, audio or animation-playback keys.
	c.player->seek( time, true, true );
}

void CbTrackPlayer::begin_frame()
{
	for ( Channel& c : m_channels )
	{
		c.touched = false;
	}
}

void CbTrackPlayer::play_at( int channel, const String& clip, double time, bool loops, bool restarted )
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
	if ( clip != c.clip || ( restarted && time < c.time ) )
	{
		Leave( c );
		// The same play of a clip it just left (a correction went away and came straight back):
		// on from there, nothing fires twice.
		bool resumed = restarted == false && clip == c.leftClip && time >= c.leftTime && time - c.leftTime < kMaxFiringStep;
		c.player->play( clip );
		c.clip = clip;
		if ( resumed )
		{
			Seek( c, c.leftTime );
			c.time = c.leftTime;
			c.high = c.leftTime;
		}
		else if ( loops == false && time > 0.0 && time <= kMaxLateStart )
		{
			// First seen a little way in: the tracks start from the beginning and catch up.
			Seek( c, 0.0 );
			c.time = time;
			c.track = 0.0;
			c.high = 0.0;
			c.catching = true;
			m_lateStarts += 1;
			return;
		}
		else
		{
			// From its start, or far in (a join): its values at `time`; keys before it are history.
			Seek( c, time );
			c.time = time;
			c.high = time;
			return;
		}
	}
	if ( c.catching )
	{
		CatchUp( c, time );
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

void CbTrackPlayer::end_frame()
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
		// RESET covers every track; the channels still playing put their own values back.
		for ( Channel& c : m_channels )
		{
			if ( c.clip.is_empty() == false )
			{
				Seek( c, c.catching ? c.track : c.time );
			}
		}
		m_resetThisFrame = false;
	}
}

void CbTrackPlayer::clear_caches()
{
	for ( Channel& c : m_channels )
	{
		if ( c.player != nullptr )
		{
			c.player->clear_caches();
		}
	}
}

String CbTrackPlayer::get_channel_clip( int channel ) const
{
	return channel >= 0 && channel < int( m_channels.size() ) ? m_channels[size_t( channel )].clip : String();
}

} // namespace cb::gd
