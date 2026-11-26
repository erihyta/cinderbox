// cb_sheet: the sheet compiler on the command line (M285; docs/sheets.md, Tools).
//
//   cb_sheet check FILE...      reads each sheet; its mistakes as FILE:LINE: why. Exit code 1 if any
//   cb_sheet fmt [--check] FILE...
//                               writes each version 1 sheet in the one style (--check: says which
//                               are not, and changes nothing)
//   cb_sheet test FILE.cbtest...
//                               runs each sheet test (M291: sheet_test.h) on the real server,
//                               headless; what failed as FILE:LINE: why. Exit code 1 if any did
//
// A motion block's nodes (a find's shape, a marker) are the scene's: check compiles a sheet's motions
// against the parts its owner's bake recorded (motions/*.cfg naming it, M294), as the server does.

#include "sheet_parts.h"
#include "sheet_rules.h"
#include "sheet_test.h"
#include "sheet_v1.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

bool Read( const std::string& path, std::string& text )
{
	std::ifstream in( path, std::ios::binary );
	if ( in.good() == false )
	{
		std::fprintf( stderr, "%s: cannot be read\n", path.c_str() );
		return false;
	}
	std::stringstream all;
	all << in.rdbuf();
	text = all.str();
	return true;
}

bool Write( const std::string& path, const std::string& text )
{
	std::ofstream out( path, std::ios::binary | std::ios::trunc );
	out << text;
	if ( out.good() == false )
	{
		std::fprintf( stderr, "%s: cannot be written\n", path.c_str() );
		return false;
	}
	return true;
}

// (M294) The motions of every owner whose parts name the sheet `path` (in `project`, as the parts file
// names it), compiled as the server compiles them; their mistakes into `errors` ("line N: why" for
// this sheet's lines, the owner's file for the rest).
void MotionMistakes( const std::filesystem::path& project, const std::string& path, cb::sheet::Strings& errors )
{
	namespace fs = std::filesystem;
	std::error_code ignored;
	for ( fs::directory_iterator it( project / "motions", ignored ), end; it != end; it.increment( ignored ) )
	{
		std::string partsText, text, why;
		cb::sheet::OwnerParts owner;
		if ( it->path().extension() != ".cfg" || Read( it->path().string(), partsText ) == false || cb::sheet::ReadParts( partsText, owner, why ) == false )
		{
			continue;
		}
		const bool names = std::any_of( owner.sheets.begin(), owner.sheets.end(), [&]( const cb::sheet::OwnerParts::Sheet& s ) { return s.path == path; } );
		if ( names == false )
		{
			continue;
		}
		auto read = [&project]( const std::string& one, std::string& out ) { return Read( ( project / one ).string(), out ); };
		if ( cb::sheet::MotionsFromParts( owner, read, text, why ) == false )
		{
			const std::string own = path + " line ";
			errors.push_back( why.rfind( own, 0 ) == 0 ? "line " + why.substr( own.size() )
													   : "line 1: the motions of " + fs::relative( it->path(), project ).generic_string() + ": " + why );
		}
	}
}

int Usage()
{
	std::fprintf( stderr, "cb_sheet check FILE...\ncb_sheet fmt [--check] FILE...\ncb_sheet test FILE.cbtest...\n" );
	return 2;
}

} // namespace

int main( int argc, char** argv )
{
	if ( argc < 3 )
	{
		return Usage();
	}
	// (A move's values end at the next movement parameter: the simulation's names, as the editor and
	// the server read them.)
	cb::sheet::UseSimulationMoveParameters();
	const std::string command = argv[1];
	bool onlyCheck = false;
	std::vector<std::string> files;
	for ( int i = 2; i < argc; ++i )
	{
		const std::string arg = argv[i];
		if ( arg == "--check" && command == "fmt" )
		{
			onlyCheck = true;
		}
		else
		{
			files.push_back( arg );
		}
	}
	if ( command != "check" && command != "fmt" && command != "test" )
	{
		return Usage();
	}
	int failed = 0;
	if ( command == "test" )
	{
		for ( const std::string& file : files )
		{
			std::vector<cb::SheetTestResult> results;
			std::string error;
			if ( cb::RunSheetTests( file, cb::SheetTestRoot( file ), results, error ) == false )
			{
				std::printf( "%s\n", error.c_str() );
				++failed;
				continue;
			}
			for ( const cb::SheetTestResult& r : results )
			{
				std::printf( "%s  %s: %s\n", r.passed ? "ok  " : "FAIL", file.c_str(), r.name.c_str() );
				for ( const std::string& why : r.failures )
				{
					std::printf( "      %s\n", why.c_str() );
				}
				failed += r.passed ? 0 : 1;
			}
		}
		return failed == 0 ? 0 : 1;
	}
	for ( const std::string& file : files )
	{
		std::string text;
		if ( Read( file, text ) == false )
		{
			++failed;
			continue;
		}
		// "uses res://..." (M289): from the folder above it with a project.godot.
		namespace fs = std::filesystem;
		fs::path project = fs::absolute( fs::path( file ) ).parent_path();
		{
			while ( project.has_parent_path() && project != project.parent_path() && fs::exists( project / "project.godot" ) == false )
			{
				project = project.parent_path();
			}
			cb::sheet::SetSheetLoader( [project]( const std::string& path, std::string& out ) {
				return path.rfind( "res://", 0 ) == 0 && Read( ( project / path.substr( 6 ) ).string(), out );
			} );
		}
		if ( command == "check" )
		{
			cb::sheet::Strings errors;
			cb::sheet::ParseRules( text, errors ); // (a sheet not of version 1 is said at its line 1)
			// (M294) Its motions, as the server compiles them: against the parts its owner's bake recorded
			// (motions/*.cfg naming it), so what only the bake said is said here too.
			if ( errors.empty() )
			{
				MotionMistakes( project, fs::relative( fs::absolute( fs::path( file ) ), project ).generic_string(), errors );
			}
			for ( const std::string& error : errors )
			{
				// ("line N: why" as FILE:N: why)
				const size_t colon = error.find( ": " );
				const bool numbered = error.rfind( "line ", 0 ) == 0 && colon != std::string::npos;
				std::printf( "%s:%s: %s\n", file.c_str(), numbered ? error.substr( 5, colon - 5 ).c_str() : "1",
							 numbered ? error.substr( colon + 2 ).c_str() : error.c_str() );
			}
			failed += errors.empty() ? 0 : 1;
			continue;
		}
		std::string out;
		std::string why;
		const bool ok = cb::sheet::FormatVersion1( text, out, why );
		if ( ok == false )
		{
			std::printf( "%s: %s\n", file.c_str(), why.c_str() );
			++failed;
			continue;
		}
		if ( out == text )
		{
			continue;
		}
		if ( onlyCheck )
		{
			std::printf( "%s: not in the one style (cb_sheet fmt)\n", file.c_str() );
			++failed;
			continue;
		}
		if ( Write( file, out ) == false )
		{
			++failed;
			continue;
		}
		std::printf( "%s: formatted\n", file.c_str() );
	}
	return failed == 0 ? 0 : 1;
}
