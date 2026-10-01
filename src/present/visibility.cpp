#include "visibility.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace cb::present
{

void KeepVisible( PresentationFrame& frame, const std::function<bool( const FrameEntity& )>& visible )
{
	std::unordered_set<uint32_t> hidden;
	for ( const FrameEntity& e : frame.entities )
	{
		if ( e.netId != frame.localNetId && visible( e ) == false )
		{
			hidden.insert( e.netId );
		}
	}
	if ( hidden.empty() )
	{
		return;
	}
	// What a hidden player holds is hidden with it.
	for ( const FrameEntity& e : frame.entities )
	{
		if ( e.kind == VisualKind::Item && e.holder != 0 && hidden.count( e.holder ) != 0 )
		{
			hidden.insert( e.netId );
		}
	}

	// Ragdoll parts go with their entity; the ones that stay are numbered again.
	std::vector<uint32_t> newIndex( frame.ragdolls.size(), UINT32_MAX );
	std::vector<FrameRagdoll> ragdolls;
	for ( size_t i = 0; i < frame.ragdolls.size(); ++i )
	{
		if ( hidden.count( frame.ragdolls[i].netId ) == 0 )
		{
			newIndex[i] = uint32_t( ragdolls.size() );
			ragdolls.push_back( frame.ragdolls[i] );
		}
	}
	frame.ragdolls.swap( ragdolls );
	frame.entities.erase( std::remove_if( frame.entities.begin(), frame.entities.end(), [&]( const FrameEntity& e ) { return hidden.count( e.netId ) != 0; } ),
						  frame.entities.end() );
	for ( FrameEntity& e : frame.entities )
	{
		if ( e.ragdoll != UINT32_MAX )
		{
			e.ragdoll = e.ragdoll < newIndex.size() ? newIndex[e.ragdoll] : UINT32_MAX;
		}
	}

	// The rings: a record about something hidden says nothing any more.
	auto names = [&]( uint32_t a, uint32_t b ) { return hidden.count( a ) != 0 || hidden.count( b ) != 0; };
	for ( ImpactRecord& impact : frame.impacts )
	{
		if ( names( impact.netIdA, impact.netIdB ) )
		{
			uint32_t tick = impact.tick;
			impact = ImpactRecord{};
			impact.tick = tick;
		}
	}
	for ( ModEventRecord& event : frame.modEvents )
	{
		if ( names( event.netIdA, event.netIdB ) )
		{
			uint32_t tick = event.tick;
			event = ModEventRecord{};
			event.type = 0xFFFF; // no event has this type
			event.tick = tick;
		}
	}
}

} // namespace cb::present
