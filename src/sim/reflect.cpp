#include "reflect.h"

#include "map.h"

#include <algorithm>

namespace cb
{

namespace
{

std::vector<ComponentDef> BuildRegistry()
{
	std::vector<ComponentDef> registry;

	ComponentDef shape;
	shape.name = "Shape";
	shape.doc = "Collision shape. Box uses size, Sphere uses radius, Capsule uses radius and height.";
	shape.fields = {
		{ "kind", FieldType::Enum, { 0.0f, 0.0f, 0.0f }, 0.0f, 2.0f, "Box,Sphere,Capsule", "" },
		{ "size", FieldType::Vec3, { 1.0f, 1.0f, 1.0f }, 0.01f, 1000.0f, nullptr, "Full size of a box, in metres" },
		{ "radius", FieldType::Float, { 0.5f, 0.0f, 0.0f }, 0.01f, 500.0f, nullptr, "Sphere and capsule radius" },
		{ "height", FieldType::Float, { 2.0f, 0.0f, 0.0f }, 0.02f, 1000.0f, nullptr, "Total capsule height" },
	};
	registry.push_back( shape );

	ComponentDef body;
	body.name = "Body";
	body.doc = "Box3D rigid body settings. Without it an entity is a dynamic body.";
	body.fields = {
		{ "type", FieldType::Enum, { 2.0f, 0.0f, 0.0f }, 0.0f, 2.0f, "Static,Kinematic,Dynamic", "" },
		{ "gravity_scale", FieldType::Float, { 1.0f, 0.0f, 0.0f }, -10.0f, 10.0f, nullptr, "" },
		{ "linear_damping", FieldType::Float, { 0.0f, 0.0f, 0.0f }, 0.0f, 100.0f, nullptr, "" },
		{ "angular_damping", FieldType::Float, { 0.0f, 0.0f, 0.0f }, 0.0f, 100.0f, nullptr, "" },
	};
	registry.push_back( body );

	ComponentDef material;
	material.name = "Material";
	material.doc = "Surface properties of the collision shape.";
	material.fields = {
		{ "density", FieldType::Float, { 1.0f, 0.0f, 0.0f }, 0.001f, 1000.0f, nullptr, "kg/m^3" },
		{ "friction", FieldType::Float, { 0.6f, 0.0f, 0.0f }, 0.0f, 10.0f, nullptr, "" },
		{ "restitution", FieldType::Float, { 0.0f, 0.0f, 0.0f }, 0.0f, 1.0f, nullptr, "Bounciness" },
	};
	registry.push_back( material );

	ComponentDef velocity;
	velocity.name = "Velocity";
	velocity.doc = "Velocity the entity starts with.";
	velocity.fields = {
		{ "linear", FieldType::Vec3, { 0.0f, 0.0f, 0.0f }, -200.0f, 200.0f, nullptr, "metres per second" },
		{ "angular", FieldType::Vec3, { 0.0f, 0.0f, 0.0f }, -100.0f, 100.0f, nullptr, "radians per second" },
	};
	registry.push_back( velocity );

	ComponentDef prop;
	prop.name = "Prop";
	prop.doc = "Makes the entity a prop: it counts against the per-player and global caps and can expire.";
	prop.fields = {
		{ "lifetime_seconds", FieldType::Float, { 0.0f, 0.0f, 0.0f }, 0.0f, 3600.0f, nullptr, "0 keeps it forever" },
	};
	registry.push_back( prop );

	return registry;
}

} // namespace

const FieldDef* ComponentDef::Field( uint32_t fieldId ) const
{
	for ( const FieldDef& f : fields )
	{
		if ( f.Id() == fieldId )
		{
			return &f;
		}
	}
	return nullptr;
}

const std::vector<ComponentDef>& AuthorableComponents()
{
	static const std::vector<ComponentDef> registry = BuildRegistry();
	return registry;
}

const ComponentDef* FindComponent( uint32_t componentId )
{
	for ( const ComponentDef& c : AuthorableComponents() )
	{
		if ( c.Id() == componentId )
		{
			return &c;
		}
	}
	return nullptr;
}

const ComponentDef* FindComponent( const std::string& name )
{
	return FindComponent( Fnv32( name.c_str() ) );
}

const AuthoredComponent* EntityTemplate::Find( uint32_t componentId ) const
{
	for ( const AuthoredComponent& c : components )
	{
		if ( c.id == componentId )
		{
			return &c;
		}
	}
	return nullptr;
}

namespace
{

const AuthoredField* FindField( const EntityTemplate& t, uint32_t componentId, uint32_t fieldId )
{
	const AuthoredComponent* component = t.Find( componentId );
	if ( component == nullptr )
	{
		return nullptr;
	}
	for ( const AuthoredField& f : component->fields )
	{
		if ( f.id == fieldId )
		{
			return &f;
		}
	}
	return nullptr;
}

} // namespace

bool TemplateHas( const EntityTemplate& t, uint32_t componentId )
{
	return t.Find( componentId ) != nullptr;
}

float TemplateFloat( const EntityTemplate& t, uint32_t componentId, uint32_t fieldId, float fallback )
{
	const AuthoredField* f = FindField( t, componentId, fieldId );
	return f != nullptr ? MapDequantize( f->raw[0], kMapPositionScale ) : fallback;
}

int32_t TemplateInt( const EntityTemplate& t, uint32_t componentId, uint32_t fieldId, int32_t fallback )
{
	const AuthoredField* f = FindField( t, componentId, fieldId );
	return f != nullptr ? f->raw[0] : fallback;
}

bool TemplateBool( const EntityTemplate& t, uint32_t componentId, uint32_t fieldId, bool fallback )
{
	const AuthoredField* f = FindField( t, componentId, fieldId );
	return f != nullptr ? f->raw[0] != 0 : fallback;
}

b3Vec3 TemplateVec3( const EntityTemplate& t, uint32_t componentId, uint32_t fieldId, b3Vec3 fallback )
{
	const AuthoredField* f = FindField( t, componentId, fieldId );
	if ( f == nullptr )
	{
		return fallback;
	}
	return b3Vec3{ MapDequantize( f->raw[0], kMapPositionScale ), MapDequantize( f->raw[1], kMapPositionScale ),
				   MapDequantize( f->raw[2], kMapPositionScale ) };
}

void SanitizeTemplate( EntityTemplate& t )
{
	std::vector<AuthoredComponent> kept;
	for ( AuthoredComponent& component : t.components )
	{
		const ComponentDef* def = FindComponent( component.id );
		if ( def == nullptr )
		{
			continue; // a component this build does not know
		}

		std::vector<AuthoredField> fields;
		for ( AuthoredField& field : component.fields )
		{
			const FieldDef* fieldDef = def->Field( field.id );
			if ( fieldDef == nullptr )
			{
				continue;
			}
			int count = fieldDef->type == FieldType::Vec3 ? 3 : 1;
			for ( int i = 0; i < count; ++i )
			{
				if ( fieldDef->type == FieldType::Float || fieldDef->type == FieldType::Vec3 )
				{
					float value = MapDequantize( field.raw[i], kMapPositionScale );
					value = std::clamp( value, fieldDef->min, fieldDef->max );
					field.raw[i] = MapQuantize( value, kMapPositionScale );
				}
				else
				{
					field.raw[i] = std::clamp( field.raw[i], int32_t( fieldDef->min ), int32_t( fieldDef->max ) );
				}
			}
			for ( int i = count; i < 3; ++i )
			{
				field.raw[i] = 0;
			}
			fields.push_back( field );
		}
		component.fields = std::move( fields );
		kept.push_back( std::move( component ) );
	}
	t.components = std::move( kept );
}

} // namespace cb
