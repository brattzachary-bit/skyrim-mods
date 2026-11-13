#pragma once

struct Settings
{
	bool          enabled = true;
	float         distance = 1800.0f;
	std::uint32_t scanMs = 400;
	int           maxTargets = 2;
	std::uint32_t keepSeconds = 45;
	int           exteriorRadius = 2;
	std::uint32_t maxModels = 1500;
	int           loaderThreads = 1;
	float         mainThreadBudgetMs = 2.0f;
	bool          characters = true;
	bool          preferAhead = true;
	float         stutterGuard = 1.35f;
	bool          logLoadTimes = true;

	void Load();

	static Settings& Get()
	{
		static Settings s;
		return s;
	}
};
