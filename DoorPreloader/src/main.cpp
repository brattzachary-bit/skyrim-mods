#include "Index.h"
#include "Preloader.h"
#include "Settings.h"

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .logLevel = spdlog::level::info });
	logger::info("{} v{}", SKSE::GetPluginName(), SKSE::GetPluginVersion().string());

	Settings::Get().Load();
	if (!Settings::Get().enabled) {
		logger::info("disabled in DoorPreloader.ini");
		return true;
	}
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Index::BuildAsync();
			Preloader::Start();
		}
	});
	return true;
}
