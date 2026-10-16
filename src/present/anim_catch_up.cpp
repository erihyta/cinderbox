#include "anim_catch_up.h"

#include "pose.h"

#include <algorithm>

namespace cb::present
{

AnimState CatchUpAnimState( const AnimState& shown, const AnimGraph& graph, const AnimGraphPacks& packs, AnimCatchUp& memory,
							float seconds, float tickSeconds )
{
	AnimState out = shown;
	if ( memory.layers.size() < shown.graph.size() )
	{
		memory.layers.resize( shown.graph.size() );
	}
	std::vector<anim::ActiveClip> clips = anim::ActiveClips( shown, graph, packs );
	for ( size_t l = 0; l < shown.graph.size(); ++l )
	{
		const AnimGraphLayerState& L = shown.graph[l];
		AnimCatchUp::Layer& m = memory.layers[l];
		bool fresh = m.known == false || L.state != m.state || L.source != m.source || L.stateTime + 1e-4f < m.stateTime;
		if ( fresh )
		{
			// The play it is leaving, in case a correction comes straight back to it.
			AnimCatchUp::Layer was = m;
			bool resumed = m.left && L.state == m.leftState && L.source == m.leftSource && L.stateTime + 1e-4f >= m.leftStateTime &&
						   L.stateTime - m.leftStateTime < 0.25f;
			if ( resumed )
			{
				m.lag = std::max( 0.0f, m.leftLag - m.leftPace * ( L.stateTime - m.leftStateTime ) );
				m.pace = m.leftPace;
			}
			else
			{
				// A clip that does not loop, played forward from its own clock, first seen late.
				const anim::ActiveClip* clip = nullptr;
				for ( const anim::ActiveClip& c : clips )
				{
					clip = c.channel == int( l ) ? &c : clip;
				}
				bool plain = clip != nullptr && clip->loops == false && std::abs( clip->time - L.time ) < 1e-4f;
				bool late = L.stateTime > 1.5f * tickSeconds && L.stateTime <= kMaxLateStart && L.time <= L.stateTime + 1e-4f;
				m.lag = m.known && plain && late ? L.time : 0.0f;
				m.pace = m.lag / kCatchUpSeconds;
				memory.started += m.lag > 0.0f ? 1 : 0;
			}
			if ( was.known )
			{
				m.left = true;
				m.leftState = was.state;
				m.leftSource = was.source;
				m.leftStateTime = was.stateTime;
				m.leftLag = was.lag;
				m.leftPace = was.pace;
			}
		}
		else
		{
			m.lag = std::max( 0.0f, m.lag - m.pace * std::max( seconds, 0.0f ) );
		}
		m.known = true;
		m.state = L.state;
		m.source = L.source;
		m.stateTime = L.stateTime;
		if ( m.lag > 0.0f )
		{
			AnimGraphLayerState& o = out.graph[l];
			o.time = std::max( 0.0f, L.time - m.lag );
			o.stateTime = std::max( 0.0f, L.stateTime - m.lag );
			o.previousTime = std::max( 0.0f, L.previousTime - m.lag );
		}
	}
	return out;
}

} // namespace cb::present
