#include "view.h"

#include <chrono>

namespace cb::present
{

double ViewClock()
{
	static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	return std::chrono::duration<double>( std::chrono::steady_clock::now() - start ).count();
}

} // namespace cb::present
