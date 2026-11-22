// cb_sheet: the sheet compiler on the command line (M285; docs/sheets.md, Tools).
//
//   cb_sheet check FILE...      reads each sheet; its mistakes as FILE:LINE: why. Exit code 1 if any
//   cb_sheet fmt [--check] FILE...
//                               writes each version 1 sheet in the one style (--check: says which
//                               are not, and changes nothing)
//   cb_sheet convert FILE...    writes each version 0 sheet as version 1, in place; one that would
//                               mean something else is not written, and says where
//
// A motion block's nodes (a find's shape, a marker) are the scene's: only the editor and the bake,
// which have the scene, check them.

#include "sheet_rules.h"
#include "sheet_v1.h"

#include <cstdio>
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

int Usage()
{
	std::fprintf( stderr, "cb_sheet check FILE...\ncb_sheet fmt [--check] FILE...\ncb_sheet convert FILE...\n" );
	return 2;
}

} // namespace

int main( int argc, char** argv )
{
	if ( argc < 3 )
	{
		return Usage();
	}
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
	if ( command != "check" && command != "fmt" && command != "convert" )
	{
		return Usage();
	}
	int failed = 0;
	for ( const std::string& file : files )
	{
		std::string text;
		if ( Read( file, text ) == false )
		{
			++failed;
			continue;
		}
		if ( command == "check" )
		{
			cb::sheet::Strings errors;
			cb::sheet::ParseRules( text, errors );
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
		const bool ok = command == "fmt" ? cb::sheet::FormatVersion1( text, out, why ) : cb::sheet::ConvertToVersion1( text, out, why );
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
		std::printf( "%s: %s\n", file.c_str(), command == "fmt" ? "formatted" : "converted to version 1" );
	}
	return failed == 0 ? 0 : 1;
}
