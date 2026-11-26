#include "sheet_parts.h"

#include "move_params.h"
#include "sheet_rules.h"
#include "sheet_text.h"
#include "sheet_v1.h"

#include <algorithm>

namespace cb::sheet
{

namespace
{

std::string Three( const Float3& v )
{
	return "\t" + Num( v.x ) + "\t" + Num( v.y ) + "\t" + Num( v.z );
}

// A line's fields (tab between them).
std::vector<std::string> Fields( const std::string& line )
{
	return Split( line, "\t" );
}

bool Floats( const std::vector<std::string>& f, size_t from, size_t count, std::vector<float>& out )
{
	if ( f.size() < from + count )
	{
		return false;
	}
	out.clear();
	for ( size_t k = from; k < from + count; ++k )
	{
		if ( IsValidFloat( f[k] ) == false )
		{
			return false;
		}
		out.push_back( float( ToFloat( f[k] ) ) );
	}
	return true;
}

// Whether a sheet's text has motion blocks (its mistakes then refuse the bake: a motion it could not
// read would be one silently left out). As the editor's bake asked it before M294.
bool NamesMotions( const std::string& text )
{
	return BeginsWith( text, "motion " ) || Contains( text, "\nmotion " ) || Contains( text, "\nmotion\t" );
}

} // namespace

std::string ShapesKey( const std::string& path, bool own, Float3 moved )
{
	return path + ( own ? "\town" : "\troot" ) + Three( moved );
}

// --- Scenes ------------------------------------------------------------------------------------------

PartsScene::PartsScene( const Parts& parts )
	: m_parts( parts )
{
}

bool PartsScene::Place( const std::string& path, const char* what, Float3& zAxis, Float3& origin, std::string& why ) const
{
	auto it = m_parts.markers.find( path );
	if ( it == m_parts.markers.end() )
	{
		why = std::string( what ) + " " + path + ": not among the item's baked parts (bake it again)";
		return false;
	}
	zAxis = it->second.zAxis;
	origin = it->second.origin;
	return true;
}

bool PartsScene::PathSamples( const std::string& path, std::vector<Float3>& out, std::string& why ) const
{
	auto it = m_parts.paths.find( path );
	if ( it == m_parts.paths.end() )
	{
		why = "follow " + path + ": not among the item's baked parts (bake it again)";
		return false;
	}
	out = it->second;
	return true;
}

bool PartsScene::Shapes( const std::string& path, bool own, Float3 moved, std::vector<ShapePiece>& out, std::string& why ) const
{
	auto it = m_parts.shapes.find( ShapesKey( path, own, moved ) );
	if ( it == m_parts.shapes.end() )
	{
		why = "find: " + path + ": not among the item's baked parts (bake it again)";
		return false;
	}
	out = it->second;
	return true;
}

RecordingScene::RecordingScene( const Scene& inner, Parts& parts )
	: m_inner( inner )
	, m_parts( parts )
{
}

bool RecordingScene::Place( const std::string& path, const char* what, Float3& zAxis, Float3& origin, std::string& why ) const
{
	if ( m_inner.Place( path, what, zAxis, origin, why ) == false )
	{
		return false;
	}
	m_parts.markers[path] = Parts::Marker{ zAxis, origin };
	return true;
}

bool RecordingScene::PathSamples( const std::string& path, std::vector<Float3>& out, std::string& why ) const
{
	if ( m_inner.PathSamples( path, out, why ) == false )
	{
		return false;
	}
	m_parts.paths[path] = out;
	return true;
}

bool RecordingScene::Shapes( const std::string& path, bool own, Float3 moved, std::vector<ShapePiece>& out, std::string& why ) const
{
	if ( m_inner.Shapes( path, own, moved, out, why ) == false )
	{
		return false;
	}
	m_parts.shapes[ShapesKey( path, own, moved )] = out;
	return true;
}

// --- The file -----------------------------------------------------------------------------------------

std::string WriteParts( const OwnerParts& owner, const std::string& about )
{
	std::string out = "cinderbox_parts\t1\n# " + about + "\n";
	if ( owner.kind.empty() == false )
	{
		out += "kind\t" + owner.kind + "\n";
		out += "use\t" + std::to_string( owner.use ) + "\n";
	}
	for ( const OwnerParts::Sheet& sheet : owner.sheets )
	{
		out += "sheet\t" + sheet.path + "\n";
		for ( const auto& [path, marker] : sheet.parts.markers )
		{
			out += "marker\t" + path + Three( marker.zAxis ) + Three( marker.origin ) + "\n";
		}
		for ( const auto& [path, samples] : sheet.parts.paths )
		{
			out += "path\t" + path;
			for ( const Float3& p : samples )
			{
				out += Three( p );
			}
			out += "\n";
		}
		for ( const auto& [key, pieces] : sheet.parts.shapes )
		{
			out += "shapes\t" + key + "\t" + std::to_string( pieces.size() ) + "\n";
			for ( const ShapePiece& piece : pieces )
			{
				out += "piece\t" + Num( piece.radius );
				for ( const Float3& p : piece.points )
				{
					out += Three( p );
				}
				out += "\n";
			}
		}
	}
	return out;
}

bool ReadParts( const std::string& text, OwnerParts& out, std::string& error )
{
	out = OwnerParts{};
	std::vector<std::string> lines = Split( Replace( text, "\r", "" ), "\n" );
	if ( lines.empty() || lines[0] != "cinderbox_parts\t1" )
	{
		error = BeginsWith( text, "cinderbox_motions" ) ? "a motions file baked before M294 (its motions are compiled from its sheets now): bake the item again"
														: "not a parts file (cinderbox_parts 1)";
		return false;
	}
	std::vector<ShapePiece>* pieces = nullptr;
	size_t piecesLeft = 0;
	for ( size_t k = 1; k < lines.size(); ++k )
	{
		const std::string& line = lines[k];
		if ( line.empty() || line[0] == '#' )
		{
			continue;
		}
		const std::vector<std::string> f = Fields( line );
		const std::string& key = f[0];
		auto fail = [&]( const std::string& why ) {
			error = "line " + std::to_string( k + 1 ) + ": " + why;
			return false;
		};
		std::vector<float> v;
		if ( key != "piece" && piecesLeft > 0 )
		{
			return fail( "a shape's pieces end early" );
		}
		if ( key == "kind" && f.size() == 2 )
		{
			out.kind = f[1];
		}
		else if ( key == "use" && f.size() == 2 && IsValidInt( f[1] ) )
		{
			out.use = int( ToInt( f[1] ) );
		}
		else if ( key == "sheet" && f.size() == 2 && f[1].empty() == false )
		{
			out.sheets.push_back( OwnerParts::Sheet{ f[1], Parts{} } );
		}
		else if ( out.sheets.empty() )
		{
			return fail( "a part before the sheet it is of" );
		}
		else if ( key == "marker" && f.size() == 8 && Floats( f, 2, 6, v ) )
		{
			out.sheets.back().parts.markers[f[1]] = Parts::Marker{ { v[0], v[1], v[2] }, { v[3], v[4], v[5] } };
		}
		else if ( key == "path" && f.size() == 2 + 3 * size_t( kPathSamples ) && Floats( f, 2, 3 * size_t( kPathSamples ), v ) )
		{
			std::vector<Float3>& samples = out.sheets.back().parts.paths[f[1]];
			for ( size_t i = 0; i < v.size(); i += 3 )
			{
				samples.push_back( { v[i], v[i + 1], v[i + 2] } );
			}
		}
		else if ( key == "shapes" && f.size() == 7 && ( f[2] == "own" || f[2] == "root" ) && Floats( f, 3, 3, v ) && IsValidInt( f[6] ) && ToInt( f[6] ) > 0 )
		{
			pieces = &out.sheets.back().parts.shapes[ShapesKey( f[1], f[2] == "own", { v[0], v[1], v[2] } )];
			piecesLeft = size_t( ToInt( f[6] ) );
		}
		else if ( key == "piece" && piecesLeft > 0 && f.size() >= 5 && ( f.size() - 2 ) % 3 == 0 && Floats( f, 1, f.size() - 1, v ) )
		{
			ShapePiece piece;
			piece.radius = v[0];
			for ( size_t i = 1; i < v.size(); i += 3 )
			{
				piece.points.push_back( { v[i], v[i + 1], v[i + 2] } );
			}
			pieces->push_back( piece );
			piecesLeft -= 1;
		}
		else
		{
			return fail( "\"" + key + "\" cannot be read" );
		}
	}
	if ( piecesLeft > 0 )
	{
		error = "a shape's pieces end early";
		return false;
	}
	return true;
}

// --- Baking and compiling -------------------------------------------------------------------------------

bool BakeOwnerMotions( const std::vector<SheetSource>& sheets, const std::string& kind, int use, std::string& lines, std::vector<std::string>& names,
					   OwnerParts* record, std::string& error )
{
	if ( record != nullptr )
	{
		record->kind = kind;
		record->use = use;
	}
	for ( const SheetSource& source : sheets )
	{
		Strings problems;
		const std::vector<Rule> rules = ParseRules( source.text, problems );
		if ( NamesMotions( source.text ) && problems.empty() == false )
		{
			error = source.name + " " + problems[0];
			return false;
		}
		Parts parts;
		const RecordingScene recording( *source.scene, parts );
		const Scene& scene = record != nullptr ? static_cast<const Scene&>( recording ) : *source.scene;
		bool any = false;
		for ( const Rule& rule : rules )
		{
			if ( rule.type != "motion" )
			{
				continue;
			}
			std::string why;
			const std::string baked = BakeSheetMotion( rule.properties, kind, use, scene, why );
			if ( baked.empty() )
			{
				// (A statement's mistake says its line; the rest are the motion's first line's.)
				error = source.name + ( why.rfind( "line ", 0 ) == 0 ? " " + why : " line " + std::to_string( rule.line ) + ": " + why );
				return false;
			}
			const std::string name = rule.properties.Get( "name", "" ).AsString();
			if ( std::find( names.begin(), names.end(), name ) != names.end() )
			{
				error = source.name + " line " + std::to_string( rule.line ) + ": there is a motion named " + name + " already";
				return false;
			}
			names.push_back( name );
			lines += baked;
			any = true;
		}
		if ( record != nullptr && any )
		{
			record->sheets.push_back( OwnerParts::Sheet{ source.path, parts } );
		}
	}
	return true;
}

void UseSimulationMoveParameters()
{
	Strings parameters;
	for ( int p = 0; p < kMoveParams; ++p )
	{
		parameters.push_back( MoveParamInfoOf( p ).name );
	}
	SetMoveParameterNames( parameters );
}

bool MotionsFromParts( const OwnerParts& owner, const std::function<bool( const std::string& path, std::string& text )>& read, std::string& text,
					   std::string& error )
{
	UseSimulationMoveParameters();
	// "uses res://...": among the item's sheets, by their paths in it.
	const SheetLoader before = TheSheetLoader();
	SetSheetLoader( [&read]( const std::string& path, std::string& out ) { return read( path.rfind( "res://", 0 ) == 0 ? path.substr( 6 ) : path, out ); } );
	std::vector<std::string> texts;
	for ( const OwnerParts::Sheet& sheet : owner.sheets )
	{
		texts.emplace_back();
		if ( read( sheet.path, texts.back() ) == false )
		{
			SetSheetLoader( before );
			error = sheet.path + ": not in the item";
			return false;
		}
	}
	std::vector<PartsScene> scenes;
	scenes.reserve( owner.sheets.size() );
	std::vector<SheetSource> sources;
	for ( size_t i = 0; i < owner.sheets.size(); ++i )
	{
		scenes.emplace_back( owner.sheets[i].parts );
		sources.push_back( SheetSource{ owner.sheets[i].path, owner.sheets[i].path, texts[i], &scenes.back() } );
	}
	std::string lines;
	std::vector<std::string> names;
	const bool ok = BakeOwnerMotions( sources, owner.kind, owner.use, lines, names, nullptr, error );
	SetSheetLoader( before );
	if ( ok == false )
	{
		return false;
	}
	text = "cinderbox_motions\t3\n" + lines;
	return true;
}

} // namespace cb::sheet
