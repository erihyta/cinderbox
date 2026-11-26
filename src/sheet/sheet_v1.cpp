#include "sheet_v1.h"

#include "sheet_dump.h"
#include "sheet_text.h"
#include "sheet_tree.h"

namespace cb::sheet
{

namespace
{

SheetLoader& LoaderSlot()
{
	static SheetLoader loader;
	return loader;
}

} // namespace

int SheetVersion( const std::string& text )
{
	for ( const std::string& line : LinesOf( text ) )
	{
		const std::string code = CodeOf( line );
		if ( code.empty() )
		{
			continue;
		}
		const std::vector<Word> w = WordsOf( code );
		if ( w[0].text != "cbsheet" )
		{
			return 0;
		}
		return w.size() == 2 && w[1].text == "1" ? 1 : -1;
	}
	return 0;
}

std::string NotVersion1( const std::string& text )
{
	bool any = false;
	for ( const std::string& line : LinesOf( text ) )
	{
		any = any || CodeOf( line ).empty() == false;
	}
	const int version = SheetVersion( text );
	return any == false || version == 1 ? std::string()
		   : version == 0				? std::string( "a sheet of version 0 (the spelling before M285) is no longer read: a sheet's first line is cbsheet 1" )
										: std::string( "this game reads sheets of version 1 (cbsheet 1)" );
}

void SetSheetLoader( SheetLoader loader )
{
	LoaderSlot() = std::move( loader );
}

const SheetLoader& TheSheetLoader()
{
	return LoaderSlot();
}

std::vector<Declaration> ReadDeclarations( const std::string& text, Strings& errors )
{
	if ( SheetVersion( text ) != 1 )
	{
		return {};
	}
	// With what it uses (M289): its lines said as this sheet's.
	const Source source = Compose( text, errors );
	return DeclarationsOf( source, errors );
}

std::string Meaning( const std::string& text )
{
	Strings errors;
	const std::vector<Rule> rules = ParseRules( text, errors );
	return DumpMeaning( rules, errors );
}

bool FormatVersion1( const std::string& text, std::string& formatted, std::string& why )
{
	if ( SheetVersion( text ) != 1 )
	{
		why = "it is not a version 1 sheet: its first line is cbsheet 1";
		return false;
	}
	formatted.clear();
	bool blank = false;
	bool any = false;
	for ( const std::string& line : LinesOf( text ) )
	{
		const std::string body = Strip( line );
		if ( body.empty() )
		{
			blank = any;
			continue;
		}
		if ( blank )
		{
			formatted += "\n";
		}
		blank = false;
		any = true;
		formatted += std::string( size_t( ( Column( line ) + 3 ) / 4 ), '\t' ) + body + "\n";
	}
	if ( Meaning( formatted ) != Meaning( text ) )
	{
		why = "formatted, it would mean something else (its indentation is not in fours?)";
		return false;
	}
	return true;
}

} // namespace cb::sheet
