#pragma once

// A parsed sheet as text, every value with its kind, in a fixed form: what the golden files
// (tests/golden/sheets) hold, so a change to the parser that changes what a shipped sheet means is
// seen in a test, and a converted sheet (step 6's version 1) can be compared with what it was.

#include "sheet_rules.h"

#include <string>
#include <vector>

namespace cb::sheet
{

std::string Dump( const std::vector<Rule>& rules, const Strings& errors );

// The same without line numbers (a rule's line and head, a statement's "line") and with every
// dictionary's keys in order: what two spellings of a sheet are compared by (sheet_v1.h, Meaning).
std::string DumpMeaning( const std::vector<Rule>& rules, const Strings& errors );

} // namespace cb::sheet
