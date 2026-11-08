#pragma once

// Where a frame's time goes in the game's own nodes (M259): a tool's figures, read and reset by
// CinderboxClient::get_frame_times. Main thread only.

#include <chrono>
#include <cstdint>

namespace cb::godot_prof
{

enum Part
{
	Skeletons, // the pose put on each character's skeleton (CbPoseModifier)
	Lists,	   // the HUD's lists (CbList)
	Labels,	   // the HUD's labels and bindings (CbFieldLabel, CbFieldBinding, CbPromptLabel, CbKey)
	Icons,	   // item icons (CbItemIcon)
	PartCount
};

struct Parts
{
	double ms[PartCount] = {};
	uint64_t count[PartCount] = {};
};

inline Parts& TheParts()
{
	static Parts parts;
	return parts;
}

// Times the scope into a part.
class Timed
{
public:
	explicit Timed( Part part )
		: m_part( part )
		, m_from( std::chrono::steady_clock::now() )
	{
	}
	~Timed()
	{
		Parts& parts = TheParts();
		parts.ms[m_part] += std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - m_from ).count();
		parts.count[m_part] += 1;
	}

private:
	Part m_part;
	std::chrono::steady_clock::time_point m_from;
};

} // namespace cb::godot_prof
