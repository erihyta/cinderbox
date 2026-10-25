#include "scripts.h"

#include "../mirror.h"

#include "ragdoll.h"

#include <algorithm>

namespace cb::present::scripts
{

namespace
{

// How long a ragdoll takes to take over from the pose its player was last drawn in.
constexpr float kBlendSeconds = 0.15f;

b3Quat Nlerp( b3Quat a, b3Quat b, float t )
{
	if ( b3DotQuat( a, b ) < 0.0f )
	{
		b = b3NegateQuat( b );
	}
	return b3NLerp( a, b, t );
}

} // namespace

void RegisterRagdolls( flecs::world& world )
{
	// Parts interpolated between the last two ticks, then a skeleton pose hung off them.
	world.system<RagdollAnim, const Visual>( "EvaluateRagdollPose" ).each( []( flecs::iter& it, size_t, RagdollAnim& ra, const Visual& v ) {
		// Its player's character's rig.
		const std::shared_ptr<const RagdollRig>& rig = it.world().get<AnimLibrary>().Of( v.character ).ragdoll;
		if ( rig == nullptr )
		{
			return;
		}
		float alpha = it.world().get<FrameTiming>().tickAlpha;
		Transform parts[kRagdollParts];
		for ( int i = 0; i < kRagdollParts; ++i )
		{
			parts[i].position = b3Lerp( ra.previous[i].position, ra.current[i].position, alpha );
			parts[i].rotation = Nlerp( ra.previous[i].rotation, ra.current[i].rotation, alpha );
		}
		Transform frame = RagdollFrame( parts[ragdoll::Pelvis], ra.yaw );

		ra.age += it.delta_time();
		if ( ra.hasStart && ra.age < kBlendSeconds )
		{
			Models target;
			RagdollModels( *rig, parts, frame, target );
			BlendModels( ra.start, target, std::clamp( ra.age / kBlendSeconds, 0.0f, 1.0f ), ra.models );
			return;
		}
		RagdollModels( *rig, parts, frame, ra.models );
	} );
}

} // namespace cb::present::scripts
