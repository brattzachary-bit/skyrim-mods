#include "Settings.h"

#include <SimpleIni.h>

void Settings::Load()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	const char* path = "Data\\SKSE\\Plugins\\DoorPreloader.ini";
	if (ini.LoadFile(path) < 0) {
		logger::warn("{} not found; using defaults", path);
		return;
	}
	enabled = ini.GetBoolValue("General", "bEnabled", enabled);
	distance = std::clamp(static_cast<float>(ini.GetDoubleValue("General", "fDistance", distance)), 200.0f, 10000.0f);
	scanMs = static_cast<std::uint32_t>(std::clamp(ini.GetLongValue("General", "uScanMs", scanMs), 100L, 5000L));
	maxTargets = static_cast<int>(std::clamp(ini.GetLongValue("General", "iMaxTargets", maxTargets), 1L, 8L));
	keepSeconds = static_cast<std::uint32_t>(std::clamp(ini.GetLongValue("General", "uKeepSeconds", keepSeconds), 5L, 600L));
	exteriorRadius = static_cast<int>(std::clamp(ini.GetLongValue("Loading", "iExteriorRadius", exteriorRadius), 0L, 3L));
	maxModels = static_cast<std::uint32_t>(std::clamp(ini.GetLongValue("Loading", "uMaxModels", maxModels), 10L, 20000L));
	loaderThreads = static_cast<int>(std::clamp(ini.GetLongValue("Loading", "iLoaderThreads", loaderThreads), 0L, 4L));
	mainThreadBudgetMs = std::clamp(static_cast<float>(ini.GetDoubleValue("Loading", "fMainThreadBudgetMs", mainThreadBudgetMs)), 0.2f, 16.0f);
	characters = ini.GetBoolValue("Loading", "bCharacters", characters);
	preferAhead = ini.GetBoolValue("General", "bPreferDoorsAhead", preferAhead);
	stutterGuard = std::clamp(static_cast<float>(ini.GetDoubleValue("Loading", "fStutterGuard", stutterGuard)), 0.0f, 5.0f);
	logLoadTimes = ini.GetBoolValue("Log", "bLoadTimes", logLoadTimes);
}
