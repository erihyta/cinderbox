#pragma once

// Plays a server recording (cb_server --record) in the client: pause, speed, seek, follow a player.

#include "anim_set.h"

#include <memory>
#include <string>

namespace cb::present
{

struct ReplayViewerOptions
{
	std::string path;
	double autoSeconds = 0.0; // > 0: close after this long (screenshot first, if set)
	std::string screenshot;
	double startSeconds = 0.0; // seek here first
};

// Expects an open raylib window. Returns non-zero if the file cannot be played or a checksum fails.
int RunReplayViewer( std::shared_ptr<const anim::AnimSet> animSet, const ReplayViewerOptions& options );

} // namespace cb::present
