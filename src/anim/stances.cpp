#include "stances.h"

#include "joint_math.h"

#include "ozz/animation/runtime/skeleton.h"

#include <cstdlib>
#include <sstream>

namespace cb::anim
{

std::vector<float> MaskWeights( const AnimSet& set, const std::string& roots, std::string& warnings )
{
	const auto& skeleton = set.Skeleton();
	const int joints = skeleton.num_joints();
	auto parents = skeleton.joint_parents();
	std::vector<float> weights( size_t( joints ), 0.0f );
	std::istringstream in( roots );
	std::string entry;
	while ( in >> entry )
	{
		size_t colon = entry.find( ':' );
		std::string name = entry.substr( 0, colon );
		float weight = colon == std::string::npos ? 1.0f : float( std::atof( entry.c_str() + colon + 1 ) );
		int root = FindJoint( set, name.c_str() );
		if ( root < 0 )
		{
			warnings += "mask joint '" + name + "' is not in the skeleton; ";
			continue;
		}
		// The root and everything below it (ozz orders parents first).
		std::vector<bool> below( size_t( joints ), false );
		below[size_t( root )] = true;
		weights[size_t( root )] = weight;
		for ( int j = root + 1; j < joints; ++j )
		{
			int parent = parents[size_t( j )];
			if ( parent >= 0 && below[size_t( parent )] )
			{
				below[size_t( j )] = true;
				weights[size_t( j )] = weight;
			}
		}
	}
	return weights;
}

std::shared_ptr<const StanceTable> BuildStanceTable( const AnimSet& set, const std::vector<std::string>& layers,
													 const std::vector<std::string>& stances, std::string& warnings )
{
	auto table = std::make_shared<StanceTable>();
	const int joints = set.Skeleton().num_joints();
	const int soaJoints = set.Skeleton().num_soa_joints();

	for ( const std::string& layer : layers )
	{
		std::string roots = set.Mask( layer );
		std::vector<float> weights;
		if ( roots.empty() && layer == "full" )
		{
			weights.assign( size_t( joints ), 1.0f );
		}
		else if ( roots.empty() )
		{
			warnings += "this character has no mask for layer '" + layer + "'; ";
		}
		else
		{
			weights = MaskWeights( set, roots, warnings );
		}
		ozz::vector<ozz::math::SimdFloat4> packed;
		if ( weights.empty() == false )
		{
			packed.resize( size_t( soaJoints ) );
			for ( int i = 0; i < soaJoints; ++i )
			{
				float lane[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				for ( int k = 0; k < 4; ++k )
				{
					int j = i * 4 + k;
					lane[k] = j < joints ? weights[size_t( j )] : 0.0f;
				}
				packed[size_t( i )] = ozz::math::simd_float4::Load( lane[0], lane[1], lane[2], lane[3] );
			}
		}
		table->masks.push_back( std::move( packed ) );
	}

	for ( const std::string& name : stances )
	{
		StanceTable::Stance stance;
		stance.name = name;
		bool any = false;
		for ( int c = 0; c < ClipCount; ++c )
		{
			stance.clips[size_t( c )] = set.StanceClip( name + "_" + ClipName( Clip( c ) ) );
			any |= stance.clips[size_t( c )] != nullptr;
		}
		stance.single = set.StanceClip( name );
		any |= stance.single != nullptr;
		if ( any == false )
		{
			warnings += "this character has no clips for stance '" + name + "'; ";
		}
		table->stances.push_back( stance );
	}
	return table;
}

} // namespace cb::anim
