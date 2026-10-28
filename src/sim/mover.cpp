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

	// Everything a character meets stops it: what it would shove (a body pushed by characters) its
	// queries never see; its pusher does (M246). How far a shape may push the character out of
	// itself (it only reads the body: nothing is written): the world, and what is much heavier than
	// the character, all the way; something loose, by its share of the two weights, so a thrown
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

	// Pogo spring keeps the capsule hovering above the ground, which smooths steps and slopes.
	b3Capsule capsule = { { 0.0f, -body.halfHeight, 0.0f }, { 0.0f, body.halfHeight, 0.0f }, body.radius };
	float pogoRest = 3.0f * body.radius;
	float rayLength = pogoRest + body.radius;
	b3Pos rayOrigin = b3Add( t.position, capsule.center1 );
	b3QueryFilter groundFilter = { CatPlayer, CatStatic | CatProp | CatDebris, 0, nullptr };
	b3RayResult ray = b3World_CastRayClosest( body.world, rayOrigin, { 0.0f, -rayLength, 0.0f }, groundFilter );

	// Ground steeper than max_slope does not hold the character up: it slides down it, under gravity,
	// along the slope the mover's planes give it, and cannot walk or jump up it.
	const float maxSlope = params[MoveParam::MaxSlope];
	const bool tooSteep = ray.hit && maxSlope < 90.0f && ray.normal.y < detmath::CosSin( maxSlope * ( detmath::kPi / 180.0f ) ).cosine;

	bool wasGrounded = c.grounded != 0;
	if ( ray.hit == false || v.y > 0.0f || tooSteep )
	{
		c.grounded = 0;
		c.pogoVelocity = 0.0f;
	}
	else
	{
		c.grounded = 1;
		float current = ray.fraction * rayLength;
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

		b3Vec3 targetDelta = b3Sub( target, t.position );
		b3PlaneSolverResult solved = b3SolvePlanes( targetDelta, ctx.planes, ctx.count );
		float fraction = b3World_CastMover( body.world, t.position, &capsule, solved.delta, moverFilter, MoverFilter, &ctx );
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
