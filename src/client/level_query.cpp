#include "level_query.h"

#include "reflect.h"
#include "simulation.h"

namespace cb
{

LevelQuery::LevelQuery() = default;
LevelQuery::~LevelQuery() = default;

void LevelQuery::SetLevel( const LevelLayout& layout, uint64_t hash )
{
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( hash == m_pendingHash )
	{
		return;
	}
	m_pending = layout;
	m_pendingHash = hash;
}

float LevelQuery::FreeDistance( b3Vec3 origin, b3Vec3 direction, float maxDistance, float radius )
{
	std::lock_guard<std::mutex> lock( m_mutex );
	if ( m_pendingHash == 0 )
	{
		return -1.0f;
	}
	if ( m_builtHash != m_pendingHash )
	{
		// What never moves: the statics, and the instances of static templates.
		LevelLayout statics = m_pending;
		statics.props.clear();
		std::vector<LevelInstance> kept;
		for ( const LevelInstance& instance : statics.instances )
		{
			const bool fixed = instance.templateIndex < statics.templates.size() &&
							   TemplateInt( statics.templates[instance.templateIndex], Fnv32( "Body" ), Fnv32( "type" ), 2 ) == 0;
			if ( fixed )
			{
				kept.push_back( instance );
			}
		}
		statics.instances = std::move( kept );
		statics.spawnTemplate = kNoTemplate;
		SimConfig config;
		config.physicsArenaMB = 64;
		m_level.reset();
		m_level = std::make_unique<Simulation>( config, statics );
		m_builtHash = m_pendingHash;
	}
	return m_level->StaticFreeDistance( origin, direction, maxDistance, radius );
}

} // namespace cb
