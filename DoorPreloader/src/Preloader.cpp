#include "Preloader.h"

#include "Index.h"
#include "Settings.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace
{
	// ---------------------------------------------------------------- destinations

	struct Place
	{
		bool         interior = false;
		RE::FormID   id = 0;  // interior cell, or worldspace
		std::int32_t x = 0, y = 0;

		bool operator==(const Place&) const = default;
	};

	struct Target
	{
		Place                                  place;
		std::string                            label;
		std::vector<std::string>               models;
		std::atomic<std::size_t>               next{ 0 };    // next model to load
		std::atomic<std::size_t>               loaded{ 0 };
		std::atomic<std::size_t>               failed{ 0 };
		std::atomic<bool>                      cancelled{ false };
		std::atomic<bool>                      reported{ false };
		std::int64_t                           startedMs = 0;
		std::int64_t                           wantedMs = 0;  // main thread only
		std::mutex                             lock;
		std::vector<RE::NiPointer<RE::NiNode>> nodes;         // keeps the loaded models (and their textures) in memory
	};
	using TargetPtr = std::shared_ptr<Target>;

	std::vector<TargetPtr> g_targets;  // main thread only

	std::mutex              g_queueLock;
	std::condition_variable g_queueSignal;
	std::deque<TargetPtr>   g_queue;
	std::atomic<bool>       g_loadingScreen{ false };
	std::atomic<bool>       g_pumpQueued{ false };

	// ---------------------------------------------------------------- loading one model

	// Model paths are stored relative to the Meshes folder. Whether the model database wants that prefix
	// is settled by the first model that loads: 0 = not known yet, 1 = as stored, 2 = with "meshes\".
	std::atomic<int> g_pathForm{ 0 };

	bool Demand(const std::string& a_path, RE::NiPointer<RE::NiNode>& a_out)
	{
		const RE::BSModelDB::DBTraits::ArgsType args{};
		return RE::BSModelDB::Demand(a_path.c_str(), a_out, args) == RE::BSResource::ErrorCode::kNone && a_out;
	}

	bool LoadModel(const std::string& a_model, RE::NiPointer<RE::NiNode>& a_out)
	{
		const std::string prefixed = "meshes\\" + a_model;
		const int         form = g_pathForm.load();
		if (form == 1) {
			return Demand(a_model, a_out);
		}
		if (form == 2) {
			return Demand(prefixed, a_out);
		}
		if (Demand(prefixed, a_out)) {
			if (g_pathForm.exchange(2) == 0) {
				logger::info("model paths are requested with the meshes\\ prefix");
			}
			return true;
		}
		if (Demand(a_model, a_out)) {
			if (g_pathForm.exchange(1) == 0) {
				logger::info("model paths are requested as stored");
			}
			return true;
		}
		return false;
	}

	// Loads the target's next model. Returns false when the target has nothing left to do.
	bool Step(Target& a_target)
	{
		if (a_target.cancelled.load()) {
			return false;
		}
		const std::size_t i = a_target.next.fetch_add(1);
		if (i >= a_target.models.size()) {
			return false;
		}
		RE::NiPointer<RE::NiNode> node;
		if (LoadModel(a_target.models[i], node)) {
			std::scoped_lock guard(a_target.lock);
			if (!a_target.cancelled.load()) {
				a_target.nodes.push_back(std::move(node));
			}
			a_target.loaded.fetch_add(1);
		} else {
			a_target.failed.fetch_add(1);
		}
		if (a_target.loaded.load() + a_target.failed.load() >= a_target.models.size() && !a_target.reported.exchange(true)) {
			logger::info("ready: {} - {} models in {} ms ({} not found)", a_target.label, a_target.loaded.load(), NowMs() - a_target.startedMs,
				a_target.failed.load());
		}
		return true;
	}

	// Havok keeps its allocators in thread-local storage, and loading a model's collision data needs them.
	// The game sets them up at the start of each of its own threads; a thread it did not create has to do
	// the same before it may load models (without this the first collision shape crashes).
	class HavokThread
	{
	public:
		HavokThread()
		{
			using Construct_t = void*(void*);
			using System_t = void*();
			using Init_t = std::int32_t(void*);
			const REL::Relocation<Construct_t> construct{ REL::ID(57052) };  // hkMemoryRouter::hkMemoryRouter
			const REL::Relocation<System_t>    system{ REL::ID(58064) };     // hkMemorySystem::getInstance
			const REL::Relocation<Init_t>      init{ REL::ID(57711) };       // hkBaseSystem::initThread
			construct(_router);
			if (void* memory = system()) {
				// hkMemorySystem::threadInit(router, name, FLAG_ALL), virtual slot 3
				using ThreadInit_t = void(void*, void*, const char*, std::int32_t);
				auto** table = *static_cast<void***>(memory);
				reinterpret_cast<ThreadInit_t*>(table[3])(memory, _router, "DoorPreloader", 3);
				init(_router);
			}
		}

	private:
		alignas(16) std::byte _router[0x80]{};  // hkMemoryRouter is 0x78 bytes
	};

	// Stutter brake. The game's own frame time is watched: a slow-moving average says what is normal here, and
	// while the latest frames run clearly longer than that, loading pauses. The average only learns from frames
	// drawn while nothing is being loaded, so the loader's own cost cannot become the new normal.
	std::atomic<float>        g_normalFrame{ 0.0f };   // seconds
	std::atomic<std::uint32_t> g_brakes{ 0 };

	bool FramesAreSuffering()
	{
		const float guard = Settings::Get().stutterGuard;
		if (guard <= 0.0f) {
			return false;
		}
		const float frame = RE::GetSecondsSinceLastFrame();
		const float normal = g_normalFrame.load(std::memory_order_relaxed);
		if (frame <= 0.0f || frame > 0.25f || normal <= 0.0f) {
			return false;   // paused, a hitch that is not ours to judge, or nothing learnt yet
		}
		if (frame > normal * guard + 0.0015f) {
			g_brakes.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		return false;
	}

	// Main thread, from the scan: learn the normal frame time while the loader is idle.
	void LearnFrameTime(bool a_loaderIdle)
	{
		const float frame = RE::GetSecondsSinceLastFrame();
		if (!a_loaderIdle || frame <= 0.0f || frame > 0.1f) {
			return;
		}
		const float normal = g_normalFrame.load(std::memory_order_relaxed);
		g_normalFrame.store(normal <= 0.0f ? frame : normal * 0.8f + frame * 0.2f, std::memory_order_relaxed);
	}

	void Worker()
	{
		HavokThread havok;  // this function never returns, so it lives as long as the thread
		for (;;) {
			TargetPtr target;
			{
				std::unique_lock guard(g_queueLock);
				g_queueSignal.wait(guard, [] { return !g_queue.empty(); });
				target = g_queue.front();
			}
			// Stand back while the game itself is loading: it needs the disk and the loader more.
			if (g_loadingScreen.load()) {
				std::this_thread::sleep_for(50ms);
				continue;
			}
			// And stand back whenever the game's frames get longer than they were before the work started.
			if (FramesAreSuffering()) {
				std::this_thread::sleep_for(40ms);
				continue;
			}
			if (!Step(*target)) {
				std::scoped_lock guard(g_queueLock);
				std::erase(g_queue, target);
			}
		}
	}

	// iLoaderThreads = 0: a slice of loading on the game's own thread.
	void Pump()
	{
		g_pumpQueued.store(false);
		if (g_loadingScreen.load()) {
			return;
		}
		const auto budget = std::chrono::microseconds(static_cast<std::int64_t>(Settings::Get().mainThreadBudgetMs * 1000.0f));
		const auto start = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - start < budget) {
			TargetPtr target;
			{
				std::scoped_lock guard(g_queueLock);
				if (g_queue.empty()) {
					return;
				}
				target = g_queue.front();
			}
			if (!Step(*target)) {
				std::scoped_lock guard(g_queueLock);
				std::erase(g_queue, target);
			}
		}
	}

	// ---------------------------------------------------------------- what a destination needs

	std::string Lower(std::string_view a_text)
	{
		std::string out(a_text);
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	// What a placed character needs: skeleton, head, skin and the armour of the default outfit.
	template <class Add>
	void CharacterModels(RE::TESNPC* a_npc, Add& a_add)
	{
		auto* race = a_npc->GetRace();
		if (!race) {
			return;
		}
		const auto sex = a_npc->IsFemale() ? RE::SEXES::kFemale : RE::SEXES::kMale;
		a_add(race->skeletonModels[sex].GetModel());

		if (race->data.flags.any(RE::RACE_DATA::Flag::kFaceGenHead)) {
			// pre-built head: Meshes\Actors\Character\FaceGenData\FaceGeom\<plugin>\<form id>.nif
			if (const auto* file = a_npc->GetFile(0)) {
				const auto head = fmt::format("actors\\character\\facegendata\\facegeom\\{}\\{:08X}.nif", file->GetFilename(), a_npc->GetLocalFormID());
				a_add(head.c_str());
			}
		}

		auto armour = [&](RE::TESObjectARMO* a_armour) {
			if (!a_armour) {
				return;
			}
			for (auto* addon : a_armour->armorAddons) {
				if (addon && addon->IsValidRace(race)) {
					a_add(addon->bipedModels[sex].GetModel());
				}
			}
		};
		armour(a_npc->skin ? a_npc->skin : race->skin);
		if (a_npc->defaultOutfit) {
			for (auto* item : a_npc->defaultOutfit->outfitItems) {
				armour(item ? item->As<RE::TESObjectARMO>() : nullptr);
			}
		}
	}

	std::vector<std::string> ModelsFor(const Place& a_place)
	{
		const auto&             cfg = Settings::Get();
		std::vector<RE::FormID> bases;
		if (a_place.interior) {
			if (const auto* list = Index::Interior(a_place.id)) {
				bases = *list;
			}
		} else {
			Index::Exterior(a_place.id, a_place.x, a_place.y, cfg.exteriorRadius, bases);
		}
		std::vector<std::string>        out;
		std::unordered_set<std::string> seen;
		for (const RE::FormID id : bases) {
			if (out.size() >= cfg.maxModels) {
				break;
			}
			auto* form = RE::TESForm::LookupByID(id);
			if (!form) {
				continue;
			}
			auto add = [&](const char* a_path) {
				if (!a_path || !*a_path) {
					return;
				}
				std::string lower = Lower(a_path);
				if (lower.starts_with("meshes\\")) {
					lower.erase(0, 7);
				}
				if (lower.ends_with(".nif") && seen.insert(lower).second) {
					out.push_back(std::move(lower));
				}
			};
			if (auto* npc = form->As<RE::TESNPC>()) {
				if (cfg.characters) {
					CharacterModels(npc, add);
				}
			} else if (auto* model = skyrim_cast<RE::TESModel*>(form)) {
				add(model->GetModel());
			}
		}
		return out;
	}

	std::string LabelFor(const Place& a_place)
	{
		auto*       form = RE::TESForm::LookupByID(a_place.id);
		const char* name = form ? form->GetName() : "";
		if (a_place.interior) {
			return fmt::format("{} [{:08X}]", name && *name ? name : "interior", a_place.id);
		}
		return fmt::format("{} [{:08X}] cell {},{}", name && *name ? name : "worldspace", a_place.id, a_place.x, a_place.y);
	}

	// ---------------------------------------------------------------- the scan (main thread)

	void ReportArrival();

	void Release(const TargetPtr& a_target)
	{
		a_target->cancelled.store(true);
		{
			std::scoped_lock guard(g_queueLock);
			std::erase(g_queue, a_target);
		}
		std::scoped_lock guard(a_target->lock);
		a_target->nodes.clear();
	}

	bool PlaceOf(RE::TESObjectREFR* a_door, Place& a_out)
	{
		auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
		if (!teleport || !teleport->teleportData) {
			return false;
		}
		const auto linked = teleport->teleportData->linkedDoor.get();
		if (!linked) {
			return false;
		}
		if (auto* cell = linked->GetParentCell(); cell && cell->IsInteriorCell()) {
			a_out = { true, cell->GetFormID(), 0, 0 };
			return true;
		}
		auto* world = linked->GetWorldspace();
		if (!world) {
			return false;
		}
		// the door's teleport data holds the spot where the player arrives
		const auto& arrive = teleport->teleportData->position;
		a_out = { false, world->GetFormID(), static_cast<std::int32_t>(std::floor(arrive.x / 4096.0f)),
			static_cast<std::int32_t>(std::floor(arrive.y / 4096.0f)) };
		return true;
	}

	void Scan()
	{
		const auto&        cfg = Settings::Get();
		const std::int64_t now = NowMs();
		auto*              player = RE::PlayerCharacter::GetSingleton();
		auto*              tes = RE::TES::GetSingleton();
		auto*              here = player ? player->GetParentCell() : nullptr;
		if (!Index::Ready() || !tes || !here || !player->Is3DLoaded() || g_loadingScreen.load()) {
			return;
		}
		const auto* hereWorld = player->GetWorldspace();

		// Arrival somewhere new (works whether or not a loading screen was shown).
		{
			static RE::FormID lastCell = 0, lastWorld = 0;
			const RE::FormID  world = hereWorld ? hereWorld->GetFormID() : 0;
			const bool        moved = here->IsInteriorCell() ? here->GetFormID() != lastCell : (lastCell != 0 || world != lastWorld);
			if (moved && (lastCell != 0 || lastWorld != 0) && cfg.logLoadTimes) {
				ReportArrival();
			}
			lastCell = here->IsInteriorCell() ? here->GetFormID() : 0;
			lastWorld = world;
		}

		struct Found
		{
			Place place;
			float distance;   // real distance to the door
			float score;      // distance, stretched for doors that are not ahead of the player
		};
		{
			bool idle;
			{
				std::scoped_lock guard(g_queueLock);
				idle = g_queue.empty();
			}
			LearnFrameTime(idle);
		}
		const float yaw = player->GetAngleZ();
		const float lookX = std::sin(yaw), lookY = std::cos(yaw);
		std::vector<Found> found;
		const auto         origin = player->GetPosition();
		tes->ForEachReferenceInRange(player, cfg.distance, [&](RE::TESObjectREFR* a_ref) {
			Place place;
			if (a_ref && !a_ref->IsDisabled() && PlaceOf(a_ref, place)) {
				const bool alreadyHere = place.interior ? (here->IsInteriorCell() && here->GetFormID() == place.id) :
				                                          (hereWorld && !here->IsInteriorCell() && hereWorld->GetFormID() == place.id);
				if (!alreadyHere) {
					const auto  doorPos = a_ref->GetPosition();
					const float distance = origin.GetDistance(doorPos);
					// A door straight ahead counts at its real distance; one behind you counts as up to three times as far.
					// Within a few steps the direction no longer matters.
					float ahead = 1.0f;
					if (cfg.preferAhead && distance > 250.0f) {
						const float dx = doorPos.x - origin.x, dy = doorPos.y - origin.y;
						const float flat = std::sqrt(dx * dx + dy * dy);
						const float facing = flat > 1.0f ? (dx * lookX + dy * lookY) / flat : 1.0f;   // 1 ahead, -1 behind
						ahead = 2.0f - facing;                                                      // 1 .. 3
					}
					const float score = distance * ahead;
					if (score > cfg.distance) {
						return RE::BSContainer::ForEachResult::kContinue;   // too far once the direction is counted
					}
					auto it = std::ranges::find_if(found, [&](const Found& f) { return f.place == place; });
					if (it == found.end()) {
						found.push_back({ place, distance, score });
					} else if (score < it->score) {
						it->distance = distance;
						it->score = score;
					}
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		std::ranges::sort(found, {}, &Found::score);
		if (found.size() > static_cast<std::size_t>(cfg.maxTargets)) {
			found.resize(static_cast<std::size_t>(cfg.maxTargets));
		}

		for (const auto& f : found) {
			auto it = std::ranges::find_if(g_targets, [&](const TargetPtr& t) { return t->place == f.place; });
			if (it != g_targets.end()) {
				(*it)->wantedMs = now;
				continue;
			}
			auto target = std::make_shared<Target>();
			target->place = f.place;
			target->label = LabelFor(f.place);
			target->models = ModelsFor(f.place);
			target->startedMs = target->wantedMs = now;
			g_targets.push_back(target);
			if (target->models.empty()) {
				target->reported.store(true);
				logger::info("nothing known about {}", target->label);
				continue;
			}
			logger::info("preparing {} ({} models, door {:.0f} units away)", target->label, target->models.size(), f.distance);
			{
				std::scoped_lock guard(g_queueLock);
				g_queue.push_back(target);
			}
			g_queueSignal.notify_all();
		}

		// let go of places nobody is near any more
		std::erase_if(g_targets, [&](const TargetPtr& t) {
			if (now - t->wantedMs <= static_cast<std::int64_t>(cfg.keepSeconds) * 1000) {
				return false;
			}
			Release(t);
			logger::info("released {}", t->label);
			return true;
		});
	}

	// ---------------------------------------------------------------- loading screens

	std::int64_t g_loadStartMs = 0;

	void OnLoadFinished()
	{
		if (!Settings::Get().logLoadTimes || g_loadStartMs == 0) {
			return;
		}
		logger::info("loading screen was up for {} ms", NowMs() - g_loadStartMs);
		g_loadStartMs = 0;
	}

	void ReportArrival()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (!cell) {
			return;
		}
		const auto* world = player->GetWorldspace();
		const auto  pos = player->GetPosition();
		const Place arrived = cell->IsInteriorCell() ? Place{ true, cell->GetFormID(), 0, 0 } :
		                                               Place{ false, world ? world->GetFormID() : 0,
														   static_cast<std::int32_t>(std::floor(pos.x / 4096.0f)),
														   static_cast<std::int32_t>(std::floor(pos.y / 4096.0f)) };
		const Target* match = nullptr;
		for (const auto& t : g_targets) {
			const bool same = arrived.interior ? t->place == arrived :
			                                     (!t->place.interior && t->place.id == arrived.id && std::abs(t->place.x - arrived.x) <= 1 &&
														 std::abs(t->place.y - arrived.y) <= 1);
			if (same) {
				match = t.get();
				break;
			}
		}
		if (match) {
			logger::info("arrived in {} - prepared {}/{} models", LabelFor(arrived), match->loaded.load(), match->models.size());
		} else {
			logger::info("arrived in {} - not prepared", LabelFor(arrived));
		}
	}

	class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
				g_loadingScreen.store(a_event->opening);
				if (a_event->opening) {
					g_loadStartMs = NowMs();
				} else {
					OnLoadFinished();
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// Wakes the main thread: the scan at its interval, and loading slices when no loader thread is used.
	void Ticker()
	{
		const auto&  cfg = Settings::Get();
		std::int64_t lastScan = 0;
		for (;;) {
			std::this_thread::sleep_for(cfg.loaderThreads > 0 ? std::chrono::milliseconds(cfg.scanMs) : 16ms);
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks) {
				continue;
			}
			const std::int64_t now = NowMs();
			if (now - lastScan >= cfg.scanMs) {
				lastScan = now;
				tasks->AddTask([] { Scan(); });
			}
			if (cfg.loaderThreads == 0 && !g_pumpQueued.exchange(true)) {
				tasks->AddTask([] { Pump(); });
			}
		}
	}
}

namespace
{
	// How long a door really takes: from the moment the player uses a load door to the moment the player
	// stands in another cell. Measured the same way with or without a loading screen.
	std::atomic<bool> g_timing{ false };

	void TimeThisDoor(RE::FormID a_fromCell)
	{
		if (g_timing.exchange(true)) {
			return;
		}
		std::thread([a_fromCell] {
			const auto start = NowMs();
			for (;;) {
				std::this_thread::sleep_for(4ms);
				const auto elapsed = NowMs() - start;
				auto*      player = RE::PlayerCharacter::GetSingleton();
				auto*      cell = player ? player->GetParentCell() : nullptr;   // read only
				if (cell && cell->GetFormID() != a_fromCell && cell->IsAttached()) {
					const auto took = elapsed;
					if (auto* tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([took] {
							logger::info("door to arrival: {} ms (stutter brake used {} times so far this session)", took, g_brakes.load());
						});
					}
					break;
				}
				if (elapsed > 90000) {
					break;   // the door did not lead anywhere
				}
			}
			g_timing.store(false);
		}).detach();
	}

	struct DoorActivate
	{
		static bool thunk(RE::TESObjectDOOR* a_door, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator, std::uint8_t a_arg3,
			RE::TESBoundObject* a_object, std::int32_t a_count)
		{
			if (a_target && a_activator && a_activator->IsPlayerRef() && Settings::Get().logLoadTimes &&
				a_target->extraList.HasType<RE::ExtraTeleport>() && !a_target->IsLocked()) {
				if (auto* cell = a_activator->GetParentCell()) {
					TimeThisDoor(cell->GetFormID());
				}
			}
			return func(a_door, a_target, a_activator, a_arg3, a_object, a_count);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace Preloader
{
	void Start()
	{
		if (Settings::Get().logLoadTimes) {
			REL::Relocation<std::uintptr_t> door{ RE::VTABLE_TESObjectDOOR[0] };
			DoorActivate::func = door.write_vfunc(0x37, DoorActivate::thunk);
		}
		static MenuWatcher watcher;
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&watcher);
		}
		int threads = Settings::Get().loaderThreads;
		if (threads > 0 && !REL::Module::IsAE()) {
			// the thread set-up above is only known for the Anniversary Edition executables
			logger::info("not an Anniversary Edition runtime: loading on the game thread instead of loader threads");
			threads = 0;
			Settings::Get().loaderThreads = 0;
		}
		for (int i = 0; i < threads; ++i) {
			std::thread(Worker).detach();
		}
		std::thread(Ticker).detach();
		logger::info("watching doors within {:.0f} units ({})", Settings::Get().distance,
			threads > 0 ? fmt::format("{} loader thread(s)", threads) : "loading on the game thread"s);
	}
}
