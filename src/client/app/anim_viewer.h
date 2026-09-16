#pragma once

// Offline animation preview: every clip side by side, plus an idle -> walk -> run speed sweep.
// Runs without a server; use it to check converted assets (cb_client --anim-viewer).

#include "anim_set.h"

#include <memory>
#include <string>

namespace cb::present
{

struct AnimViewerOptions
{
	double autoSeconds = 0.0; // > 0: close after this long (screenshot first, if set)
	std::string screenshot;
	float yaw = 0.35f; // initial model rotation (radians)
};

// Expects an open raylib window.
int RunAnimViewer( std::shared_ptr<const anim::AnimSet> set, const AnimViewerOptions& options );

} // namespace cb::present
