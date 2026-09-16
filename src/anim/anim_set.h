#pragma once

// A skeleton and the six locomotion clips, either loaded from ozz files or generated in code.

#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/memory/unique_ptr.h"

#include <array>
#include <memory>
#include <string>

namespace cb::anim
{

enum Clip : int
{
	ClipIdle,
	ClipWalk,
	ClipRun,
	ClipJumpStart,
	ClipFall,
	ClipLand,
	ClipCount,
};

// Names used in anim.cfg and for the converted file names.
const char* ClipName( Clip clip );

class AnimSet
{
public:
	// Blocky placeholder: Mixamo joint names, procedural clips.
	static std::unique_ptr<AnimSet> CreateProcedural();

	// Loads `<dir>/anim.cfg` (see assets/anim/README.md). Returns null and sets `error` on failure.
	// Missing clips are allowed (that layer falls back to the rest pose) and reported in `warnings`.
	static std::unique_ptr<AnimSet> Load( const std::string& dir, std::string& error, std::string& warnings );

	// Writes skeleton, clips and anim.cfg to `dir` (used to test the file pipeline).
	bool Save( const std::string& dir ) const;

	const ozz::animation::Skeleton& Skeleton() const
	{
		return *m_skeleton;
	}
	// Null if the clip is not available.
	const ozz::animation::Animation* Get( Clip clip ) const
	{
		return m_clips[clip].get();
	}
	float Duration( Clip clip ) const
	{
		return m_clips[clip] ? m_clips[clip]->duration() : 1.0f;
	}

	// Uniform scale applied to model-space poses (e.g. 0.01 for centimetre rigs).
	float Scale() const
	{
		return m_scale;
	}
	// Keep the root joint from drifting horizontally (clips exported without "In Place").
	bool LockRootXZ() const
	{
		return m_lockRootXZ;
	}
	const std::string& Description() const
	{
		return m_description;
	}

private:
	ozz::unique_ptr<ozz::animation::Skeleton> m_skeleton;
	std::array<ozz::unique_ptr<ozz::animation::Animation>, ClipCount> m_clips;
	float m_scale = 1.0f;
	bool m_lockRootXZ = true;
	std::string m_description;
};

} // namespace cb::anim
