#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace std::literals;

namespace logger = SKSE::log;

inline std::int64_t NowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
