#include "mover.h"

#include "anim_controller.h"
#include "detmath.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <algorithm>
#include <cfloat>

namespace cb::mover
{

namespace
{

// Tuning that is not a parameter. Changing any of these changes simulation results.
constexpr float kMinSpeed = 0.01f;
constexpr float kPogoHertz = 5.0f;
constexpr float kPogoDamping = 0.7f;
constexpr float kInputScale = 1.0f / 127.0f;
constexpr int kMoverIterations = 5;
constexpr int kMaxPlanes = 8;

bool SameShape( b3ShapeId a, b3ShapeId b )
{
	return a.index1 == b.index1 && a.world0 == b.world0 && a.generation == b.generation;
}

bool SameBody( b3BodyId a, b3BodyId b )
{
	return a.index1 == b.index1 && a.world0 == b.world0 && a.generation == b.generation;
}

// The most a loose body may push the character out of itself in one solve, in metres, before its
// share of the two weights is taken (see CollectPlanes).
constexpr float kLoosePush = 0.5f;

struct MoverContext
{
	b3ShapeId self;
	b3BodyId selfBody; // (none of its shapes: a driven prop has several)
	float mass; // the character's, kg
	b3Pos origin;
	int count;
	b3CollisionPlane planes[kMaxPlanes];
	b3Pos points[kMaxPlanes];
	b3ShapeId shapes[kMaxPlanes];
};

bool MoverFilter( b3ShapeId shapeId, void* context )
{
	auto* ctx = static_cast<MoverContext*>( context );
	return SameShape( shapeId, ctx->self ) == false && SameBody( b3Shape_GetBody( shapeId ), ctx->selfBody ) == false;
}

bool CollectPlanes( b3ShapeId shapeId, const b3PlaneResult* results, int count, void* context )
{
	auto* ctx = static_cast<MoverContext*>( context );
	if ( SameShape( shapeId, ctx->self ) || SameBody( b3Shape_GetBody( shapeId ), ctx->selfBody ) )
	{
		return true;
	}

	// Everything a character meets stops it: what it would shove (a body pushed by characters)
	// these queries never see; its pusher does (M246), and Move stands on it from above (kTopWeight).
	// How far a shape may push the character out of itself (it only reads the body: nothing is
	// written): the world, and what is much heavier than the character, all the way; something loose, by its share of the two weights, so a thrown
	// grenade (0.4 kg against 80) does not shove the player it flies into, while a falling crate of
	// the player's own weight still does by half.
	float pushLimit = FLT_MAX;
	b3BodyId other = b3Shape_GetBody( shapeId );
	if ( b3Body_GetType( other ) == b3_dynamicBody )
	{
		float weight = b3Body_GetMass( other );
		float share = weight / ( weight + ctx->mass );
		pushLimit = share >= 0.9f ? FLT_MAX : kLoosePush * share;
	}
	for ( int i = 0; i < count && ctx->count < kMaxPlanes; ++i )
	{
		ctx->planes[ctx->count] = { results[i].plane, pushLimit, 0.0f, true };
		ctx->points[ctx->count] = b3OffsetPos( ctx->origin, results[i].point );
		ctx->shapes[ctx->count] = shapeId;
		ctx->count += 1;
	}
	return true;
}

// What a query sees of the bodies pushed by characters (their shapes meet no player's query), and
// of the ground that is not pushed by characters.
constexpr b3QueryFilter kPushableFilter = { CatPusher, CatPushable, 0, nullptr };
constexpr b3QueryFilter kGroundFilter = { CatPlayer, CatStatic | CatProp | CatDebris, 0, nullptr };

// The ground is found by a sphere of this share of the capsule's radius, cast down from the bottom
// sphere's centre: a gap narrower than it (between planks) and a ledge under part of it still hold
// the character up. On a slope it stands a little higher above the ground under its middle than on
// flat ground (it touches uphill first: Unreal's capsule does the same; feet IK hides it).
constexpr float kProbeWidth = 0.9f;

// A body pushed by characters is walked through (its pusher shoves it: M246), but its top is stood
// on, as a solid body's is, when it weighs at least this share of the character (a crate holds a
// player up; a ball of a ball pit, 1 kg against 80, does not) and the sphere does not start in it
// (one the capsule is in is walked through).
constexpr float kTopWeight = 1.0f / 16.0f;

// What holds a character up: how far under the bottom sphere's centre, and its surface's normal.
struct Ground
{
	bool hit = false;
	float distance = 0.0f;
	b3Vec3 normal = { 0.0f, 1.0f, 0.0f };
};

struct SphereContext
{
	bool pushable; // (then: its top only, and only one that holds the character up)
	float weight;
	bool hit;
	float fraction;
	b3Pos point;
	b3Vec3 normal;
};

float FindUnder( b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t, int, int, void* context )
{
	auto* ctx = static_cast<SphereContext*>( context );
	if ( ctx->pushable )
	{
		// (One it starts in is one the capsule is in: walked through, not stood on.)
		b3BodyId other = b3Shape_GetBody( shapeId );
		if ( fraction <= 0.0f || b3Body_GetType( other ) != b3_dynamicBody || b3Body_GetMass( other ) < ctx->weight )
		{
			return -1.0f;
		}
	}
	if ( ctx->hit == false || fraction < ctx->fraction )
	{
		ctx->hit = true;
		ctx->fraction = fraction;
		ctx->point = point;
		ctx->normal = normal;
	}
	return fraction;
}

// The ground under `from`, `length` at most (to the sphere's lowest point and on): the level's and
// solid bodies', and the tops of bodies pushed by characters that weigh `weight` or more.
Ground SphereGround( b3WorldId world, b3Pos from, float length, float radius, float weight )
{
	const b3Vec3 centre = { 0.0f, 0.0f, 0.0f };
	const b3ShapeProxy ball = { &centre, 1, radius };
	const float travel = length - radius;
	const b3Vec3 down = { 0.0f, -travel, 0.0f };
	SphereContext solid = { false, 0.0f, false, 1.0f, {}, {} };
	SphereContext loose = { true, weight, false, 1.0f, {}, {} };
	b3World_CastShape( world, from, &ball, down, kGroundFilter, FindUnder, &solid );
	b3World_CastShape( world, from, &ball, down, kPushableFilter, FindUnder, &loose );
	const SphereContext* best = loose.hit && ( solid.hit == false || loose.fraction < solid.fraction ) ? &loose : solid.hit ? &solid : nullptr;
	if ( best == nullptr )
	{
		return {};
	}
	Ground g = { true, 0.0f, best->normal };
	// The surface's own normal where the sphere touches it: on an edge the sphere's normal leans out
	// over the edge, and the slope would let it slide off ground that is level. A short ray down,
	// just past the touch (away from the sphere's centre: onto the surface the edge belongs to).
	float off = 0.0f;
	const b3Vec3 out = b3GetLengthAndNormalize( &off, b3Vec3{ best->point.x - from.x, 0.0f, best->point.z - from.z } );
	const float nudge = off > 0.001f ? 0.02f : 0.0f;
	const b3Pos above = { best->point.x + out.x * nudge, from.y, best->point.z + out.z * nudge };
	const float reach = from.y - best->point.y + 0.05f;
	if ( reach > 0.0f )
	{
		const b3RayResult surface = b3World_CastRayClosest( world, above, { 0.0f, -reach, 0.0f }, best->pushable ? kPushableFilter : kGroundFilter );
		if ( surface.hit )
		{
			g.normal = surface.normal;
		}
	}
	// How far the ground is under the middle, as a ray would find it: on a slope the sphere touches
	// uphill, its centre radius / cos(slope) over the plane, not radius. (So the character stands as
	// high on a slope as it did with a ray; past the steepest that holds it up it slides anyway.)
	g.distance = best->fraction * travel + radius / std::max( g.normal.y, 0.5f );
	return g;
}

} // namespace

void Move( const Body& body, const MoveParams& params, float dt, uint32_t tick, const PlayerInput& in, uint8_t pressed, Character& c,
		   Transform& t )
{
	// Camera-relative wish direction
	float camYaw = detmath::YawToRadians( in.cameraYaw );
	b3Vec3 forward = detmath::YawForward( camYaw );
	b3Vec3 right = detmath::YawRight( camYaw );
	float throttleForward = float( std::clamp<int>( in.moveForward, -127, 127 ) ) * kInputScale;
	float throttleRight = float( std::clamp<int>( in.moveRight, -127, 127 ) ) * kInputScale;
	// Flight: forward is where the camera looks, up and down too.
	if ( params[MoveParam::MoveFrame] >= 0.5f )
	{
		b3CosSin pitch = detmath::CosSin( float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f ) );
		forward = { forward.x * pitch.cosine, pitch.sine, forward.z * pitch.cosine };
	}

	if ( c.grounded )
	{
		c.sprinting = ( in.buttons & BtnSprint ) ? 1 : 0;
	}

	// Jump (edge triggered)
	if ( ( pressed & BtnJump ) && c.grounded )
	{
		c.velocity.y = params[MoveParam::JumpSpeed];
		c.grounded = 0;
		c.lastJumpTick = tick;
	}

	// Ground friction (horizontal only)
	b3Vec3 v = c.velocity;
	float speed = b3Length( b3Vec3{ v.x, 0.0f, v.z } );
	if ( speed < kMinSpeed )
	{
		v.x = 0.0f;
		v.z = 0.0f;
	}
	else if ( c.grounded )
	{
		float stopSpeed = params[MoveParam::StopSpeed];
		float control = speed < stopSpeed ? stopSpeed : speed;
		float newSpeed = std::max( 0.0f, speed - control * params[MoveParam::Friction] * dt );
		float ratio = newSpeed / speed;
		v.x *= ratio;
		v.z *= ratio;
	}

	// Air friction (every direction): what stops a flight when the keys are let go.
	if ( float airFriction = params[MoveParam::AirFriction]; airFriction > 0.0f && c.grounded == 0 )
	{
		float airSpeed = b3Length( v );
		if ( airSpeed < kMinSpeed )
		{
			v = { 0.0f, 0.0f, 0.0f };
		}
		else
		{
			float stopSpeed = params[MoveParam::StopSpeed];
			float control = airSpeed < stopSpeed ? stopSpeed : airSpeed;
			v = b3MulSV( std::max( 0.0f, airSpeed - control * airFriction * dt ) / airSpeed, v );
		}
	}

	float maxSpeed = c.sprinting ? params[MoveParam::SprintSpeed] : params[MoveParam::WalkSpeed];
	b3Vec3 desired = b3Add( b3MulSV( maxSpeed * throttleForward, forward ), b3MulSV( maxSpeed * throttleRight, right ) );
	float desiredSpeed = 0.0f;
	b3Vec3 desiredDir = b3GetLengthAndNormalize( &desiredSpeed, desired );
	if ( desiredSpeed > maxSpeed )
	{
		desiredSpeed = maxSpeed;
	}

	if ( c.grounded )
	{
		v.y = 0.0f;
	}

	float airControl = c.grounded ? 1.0f : params[MoveParam::AirControl];
	float currentSpeed = b3Dot( v, desiredDir );
	float addSpeed = desiredSpeed - currentSpeed;
	if ( addSpeed > 0.0f )
	{
		float accel = std::min( addSpeed, airControl * params[MoveParam::Accelerate] * maxSpeed * dt );
		v = b3MulAdd( v, accel, desiredDir );
	}

	v.y -= params[MoveParam::Gravity] * dt;
	if ( float maxFall = params[MoveParam::MaxFall]; maxFall > 0.0f && v.y < -maxFall )
	{
		v.y = -maxFall;
	}

	// Pogo spring keeps the capsule hovering above the ground, which smooths steps and slopes. The
	// ground is found by a sphere cast down from the bottom sphere's centre (kProbeWidth).
	b3Capsule capsule = { { 0.0f, -body.halfHeight, 0.0f }, { 0.0f, body.halfHeight, 0.0f }, body.radius };
	float pogoRest = 3.0f * body.radius;
	float probeLength = pogoRest + body.radius;
	b3Pos probeOrigin = b3Add( t.position, capsule.center1 );
	// (The ground is the physics world's: no character stands on another.)
	const b3WorldId ground = b3World_IsValid( body.scenery ) ? body.scenery : body.world;
	const Ground under = SphereGround( ground, probeOrigin, probeLength, kProbeWidth * body.radius, kTopWeight * params[MoveParam::Mass] );

	// Ground steeper than max_slope does not hold the character up: it slides down it, under gravity,
	// along the slope the mover's planes give it, and cannot walk or jump up it.
	const float maxSlope = params[MoveParam::MaxSlope];
	const bool tooSteep = under.hit && maxSlope < 90.0f && under.normal.y < detmath::CosSin( maxSlope * ( detmath::kPi / 180.0f ) ).cosine;

	bool wasGrounded = c.grounded != 0;
	if ( under.hit == false || v.y > 0.0f || tooSteep )
	{
		c.grounded = 0;
		c.pogoVelocity = 0.0f;
	}
	else
	{
		c.grounded = 1;
		float current = under.distance;
		float omega = 2.0f * detmath::kPi * kPogoHertz;
		float omegaH = omega * dt;
		c.pogoVelocity = ( c.pogoVelocity - omega * omegaH * ( current - pogoRest ) ) /
						 ( 1.0f + 2.0f * kPogoDamping * omegaH + omegaH * omegaH );
	}

	// Airborne by parameter (on a hook, in flight): the ground still carries the capsule where it is
	// under it, so it does not sink, but the character is in the air for everything that asks: no
	// ground friction, no jump, the in-air animation. It lands when the parameter is gone.
	if ( c.grounded != 0 && params[MoveParam::Airborne] >= 0.5f )
	{
		v.y = 0.0f;
		c.grounded = 0;
	}

	if ( c.grounded )
	{
		c.groundTicks = wasGrounded ? c.groundTicks + 1 : 0;
		c.airTicks = 0;
	}
	else
	{
		c.airTicks = wasGrounded ? 0 : c.airTicks + 1;
		c.groundTicks = 0;
	}

	// Move and slide
	b3Vec3 startPosition = t.position;
	b3Vec3 target = b3Add( t.position, b3MulSV( dt, b3Add( v, b3Vec3{ 0.0f, c.pogoVelocity, 0.0f } ) ) );
	b3QueryFilter moverFilter = { CatPlayer, ~uint64_t( 0 ), 0, nullptr };

	MoverContext ctx;
	ctx.self = body.shape;
	ctx.selfBody = body.body;
	ctx.mass = params[MoveParam::Mass];
	ctx.count = 0;
	for ( int iteration = 0; iteration < kMoverIterations; ++iteration )
	{
		ctx.count = 0;
		ctx.origin = t.position;
		b3World_CollideMover( body.world, t.position, &capsule, moverFilter, CollectPlanes, &ctx );
		if ( b3World_IsValid( body.scenery ) )
		{
			b3World_CollideMover( body.scenery, t.position, &capsule, moverFilter, CollectPlanes, &ctx );
		}

		b3Vec3 targetDelta = b3Sub( target, t.position );
		b3PlaneSolverResult solved = b3SolvePlanes( targetDelta, ctx.planes, ctx.count );
		float fraction = b3World_CastMover( body.world, t.position, &capsule, solved.delta, moverFilter, MoverFilter, &ctx );
		if ( b3World_IsValid( body.scenery ) )
		{
			fraction = std::min( fraction, b3World_CastMover( body.scenery, t.position, &capsule, solved.delta, moverFilter, MoverFilter, &ctx ) );
		}
		b3Vec3 delta = b3MulSV( fraction, solved.delta );
		t.position = b3Add( t.position, delta );

		if ( b3LengthSquared( delta ) < 0.0001f )
		{
			break;
		}
	}

	// What it touches stops it: a character never shoves a body itself (its pusher does, in the
	// physics step, the ones pushed by characters).
	b3CollisionPlane solid[kMaxPlanes];
	int solidCount = 0;
	for ( int i = 0; i < ctx.count; ++i )
	{
		solid[solidCount++] = ctx.planes[i];
	}
	v = b3ClipVector( v, solid, solidCount );
	c.velocity = v;

	// Face where the camera looks (a mod chose it), or turn toward the direction of travel.
	b3Vec3 moved = b3Sub( t.position, startPosition );
	float horizontalSq = moved.x * moved.x + moved.z * moved.z;
	if ( FacesCamera( c, in ) )
	{
		c.facingYaw = detmath::WrapAngle( detmath::YawToRadians( in.cameraYaw ) );
	}
	else if ( horizontalSq > ( 0.2f * dt ) * ( 0.2f * dt ) && desiredSpeed > 0.0f )
	{
		float targetYaw = detmath::Atan2( moved.x, moved.z );
		float diff = detmath::WrapAngle( targetYaw - c.facingYaw );
		float maxTurn = params[MoveParam::TurnRate] * dt;
		diff = std::clamp( diff, -maxTurn, maxTurn );
		c.facingYaw = detmath::WrapAngle( c.facingYaw + diff );
	}
	t.rotation = detmath::YawRotation( c.facingYaw );

	// Drive the kinematic body (what reads its velocity sees it move; it shoves nothing: M246).
	b3Body_SetTargetTransform( body.body, { b3Add( t.position, body.offset ), t.rotation }, dt, true );
}

} // namespace cb::mover
