#include "Index.h"

#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>

namespace
{
	constexpr std::uint32_t kCacheVersion = 2;

	using Table32 = std::unordered_map<RE::FormID, std::vector<RE::FormID>>;
	using Table64 = std::unordered_map<std::uint64_t, std::vector<RE::FormID>>;

	Table32           g_interiors;
	Table64           g_exteriors;
	std::atomic<bool> g_ready{ false };

	std::uint64_t ExteriorKey(RE::FormID a_world, std::int32_t a_x, std::int32_t a_y)
	{
		return (static_cast<std::uint64_t>(a_world) << 32) | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(a_x)) << 16) |
		       static_cast<std::uint16_t>(a_y);
	}

	// ---------------------------------------------------------------- the load order, as plain data

	struct Plugin
	{
		std::string   name;      // file name as loaded
		std::wstring  path;      // where to read it
		bool          light = false;
		std::uint32_t index = 0; // load index (full) or light index
	};

	std::string Lower(std::string_view a_text)
	{
		std::string out(a_text);
		std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	std::vector<Plugin> CollectPlugins()
	{
		std::vector<Plugin> out;
		auto*               data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return out;
		}
		auto add = [&](RE::TESFile* a_file, bool a_light) {
			if (!a_file) {
				return;
			}
			Plugin p;
			p.name = a_file->fileName;
			// The game hands the name over in the system code page; convert it without the exceptions std::filesystem
			// throws for names that code page cannot express.
			const int wide = ::MultiByteToWideChar(CP_ACP, 0, p.name.c_str(), -1, nullptr, 0);
			if (wide <= 1) {
				return;
			}
			std::wstring name(static_cast<std::size_t>(wide - 1), L'\0');
			::MultiByteToWideChar(CP_ACP, 0, p.name.c_str(), -1, name.data(), wide);
			p.path = L"Data\\" + name;
			p.light = a_light;
			p.index = a_light ? a_file->GetSmallFileCompileIndex() : a_file->GetCompileIndex();
			out.push_back(std::move(p));
		};
		for (auto* file : data->compiledFileCollection.files) {
			add(file, false);
		}
		for (auto* file : data->compiledFileCollection.smallFiles) {
			add(file, true);
		}
		return out;
	}

	std::uint64_t Signature(const std::vector<Plugin>& a_plugins)
	{
		std::uint64_t h = 1469598103934665603ull;
		auto          mix = [&](const void* a_data, std::size_t a_size) {
            const auto* bytes = static_cast<const std::uint8_t*>(a_data);
            for (std::size_t i = 0; i < a_size; ++i) {
                h = (h ^ bytes[i]) * 1099511628211ull;
            }
		};
		mix(&kCacheVersion, sizeof(kCacheVersion));
		for (const auto& p : a_plugins) {
			const std::string name = Lower(p.name);
			mix(name.data(), name.size());
			mix(&p.light, sizeof(p.light));
			mix(&p.index, sizeof(p.index));
			std::uint64_t size = 0;
			std::int64_t  time = 0;
			WIN32_FILE_ATTRIBUTE_DATA info{};
			if (::GetFileAttributesExW(p.path.c_str(), GetFileExInfoStandard, &info)) {
				size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
				time = (static_cast<std::int64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
			}
			mix(&size, sizeof(size));
			mix(&time, sizeof(time));
		}
		return h;
	}

	// ---------------------------------------------------------------- read-only view of a file

	class MappedFile
	{
	public:
		explicit MappedFile(const std::wstring& a_path)
		{
			_file = ::CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (_file == INVALID_HANDLE_VALUE) {
				return;
			}
			LARGE_INTEGER size{};
			if (!::GetFileSizeEx(_file, &size) || size.QuadPart < 24) {
				return;
			}
			_mapping = ::CreateFileMappingW(_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (!_mapping) {
				return;
			}
			_data = static_cast<const std::uint8_t*>(::MapViewOfFile(_mapping, FILE_MAP_READ, 0, 0, 0));
			_size = _data ? static_cast<std::size_t>(size.QuadPart) : 0;
		}
		~MappedFile()
		{
			if (_data) {
				::UnmapViewOfFile(_data);
			}
			if (_mapping) {
				::CloseHandle(_mapping);
			}
			if (_file != INVALID_HANDLE_VALUE) {
				::CloseHandle(_file);
			}
		}
		MappedFile(const MappedFile&) = delete;
		MappedFile& operator=(const MappedFile&) = delete;

		const std::uint8_t* data() const { return _data; }
		std::size_t         size() const { return _size; }

	private:
		HANDLE              _file = INVALID_HANDLE_VALUE;
		HANDLE              _mapping = nullptr;
		const std::uint8_t* _data = nullptr;
		std::size_t         _size = 0;
	};

	// ---------------------------------------------------------------- plugin file walker

	constexpr std::uint32_t Tag(const char (&a)[5])
	{
		return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a[0])) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a[1])) << 8) |
		       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a[2])) << 16) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a[3])) << 24);
	}

	template <class T>
	T Read(const std::uint8_t* a_at)
	{
		T v;
		std::memcpy(&v, a_at, sizeof(T));
		return v;
	}

	constexpr std::size_t   kHeader = 24;
	constexpr std::uint32_t kCompressed = 0x00040000;
	constexpr std::uint32_t kDeleted = 0x00000020;

	struct Walker
	{
		// Runtime form id prefix for each master index of the file being read; ~0 = that master is not loaded.
		struct Owner
		{
			bool          loaded = false;
			bool          light = false;
			std::uint32_t index = 0;
		};
		std::vector<Owner> owners;
		Table32&           interiors;
		Table64&           exteriors;
		std::size_t        refs = 0;

		RE::FormID Resolve(std::uint32_t a_local) const
		{
			const std::size_t slot = std::min<std::size_t>(a_local >> 24, owners.size() - 1);
			const Owner&      o = owners[slot];
			if (!o.loaded) {
				return 0;
			}
			return o.light ? (0xFE000000u | (o.index << 12) | (a_local & 0xFFFu)) : ((o.index << 24) | (a_local & 0xFFFFFFu));
		}

		struct Context
		{
			bool       exterior = false;
			bool       children = false;
			RE::FormID world = 0;
			RE::FormID cell = 0;
		};

		void Reference(const std::uint8_t* a_body, std::size_t a_size, const Context& a_ctx)
		{
			std::uint32_t base = 0;
			float         x = 0.0f, y = 0.0f;
			bool          placed = false;
			std::size_t   at = 0;
			while (at + 6 <= a_size) {
				const auto tag = Read<std::uint32_t>(a_body + at);
				const auto len = Read<std::uint16_t>(a_body + at + 4);
				at += 6;
				if (at + len > a_size) {
					break;
				}
				if (tag == Tag("NAME") && len >= 4) {
					base = Read<std::uint32_t>(a_body + at);
				} else if (tag == Tag("DATA") && len >= 12) {
					x = Read<float>(a_body + at);
					y = Read<float>(a_body + at + 4);
					placed = true;
				}
				at += len;
			}
			const RE::FormID id = base ? Resolve(base) : 0;
			if (!id) {
				return;
			}
			++refs;
			if (!a_ctx.exterior) {
				if (a_ctx.cell) {
					interiors[a_ctx.cell].push_back(id);
				}
			} else if (a_ctx.world && placed && std::isfinite(x) && std::isfinite(y)) {
				const auto gx = static_cast<std::int32_t>(std::floor(x / 4096.0f));
				const auto gy = static_cast<std::int32_t>(std::floor(y / 4096.0f));
				if (std::abs(gx) < 0x7FFF && std::abs(gy) < 0x7FFF) {
					exteriors[ExteriorKey(a_ctx.world, gx, gy)].push_back(id);
				}
			}
		}

		void Walk(const std::uint8_t* a_at, const std::uint8_t* a_end, Context a_ctx, int a_depth)
		{
			while (a_at + kHeader <= a_end) {
				const auto type = Read<std::uint32_t>(a_at);
				const auto size = Read<std::uint32_t>(a_at + 4);
				if (type == Tag("GRUP")) {
					if (size < kHeader || size > static_cast<std::size_t>(a_end - a_at)) {
						return;  // damaged file: stop here rather than read past the end
					}
					const auto label = Read<std::uint32_t>(a_at + 8);
					const auto kind = Read<std::int32_t>(a_at + 12);
					Context    ctx = a_ctx;
					bool       enter = a_depth < 16;
					switch (kind) {
					case 0:  // top group, named after its record type
						enter = enter && (label == Tag("CELL") || label == Tag("WRLD"));
						ctx.exterior = label == Tag("WRLD");
						break;
					case 1:  // everything under one worldspace
						ctx.world = Resolve(label);
						break;
					case 6:  // everything under one cell
						ctx.cell = Resolve(label);
						break;
					case 8:  // persistent references
					case 9:  // temporary references
						ctx.cell = Resolve(label);
						ctx.children = true;
						break;
					default:
						break;
					}
					if (enter) {
						Walk(a_at + kHeader, a_at + size, ctx, a_depth + 1);
					}
					a_at += size;
					continue;
				}
				if (size > static_cast<std::size_t>(a_end - a_at) - kHeader) {
					return;
				}
				if ((type == Tag("REFR") || type == Tag("ACHR")) && a_ctx.children) {  // placed objects and placed characters
					const auto flags = Read<std::uint32_t>(a_at + 8);
					if (!(flags & (kCompressed | kDeleted))) {
						Reference(a_at + kHeader, size, a_ctx);
					}
				}
				a_at += kHeader + size;
			}
		}
	};

	// Master file names from the plugin's header record.
	std::vector<std::string> ReadMasters(const std::uint8_t* a_data, std::size_t a_size)
	{
		std::vector<std::string> out;
		if (a_size < kHeader || Read<std::uint32_t>(a_data) != Tag("TES4")) {
			return out;
		}
		const std::size_t bodySize = std::min<std::size_t>(Read<std::uint32_t>(a_data + 4), a_size - kHeader);
		const auto*       body = a_data + kHeader;
		std::size_t       at = 0;
		while (at + 6 <= bodySize) {
			const auto tag = Read<std::uint32_t>(body + at);
			const auto len = Read<std::uint16_t>(body + at + 4);
			at += 6;
			if (at + len > bodySize) {
				break;
			}
			if (tag == Tag("MAST") && len > 0) {
				out.emplace_back(reinterpret_cast<const char*>(body + at), strnlen(reinterpret_cast<const char*>(body + at), len));
			}
			at += len;
		}
		return out;
	}

	void Parse(const std::vector<Plugin>& a_plugins)
	{
		std::unordered_map<std::string, const Plugin*> byName;
		for (const auto& p : a_plugins) {
			byName.emplace(Lower(p.name), &p);
		}
		std::size_t totalRefs = 0;
		for (const auto& plugin : a_plugins) {
			MappedFile file(plugin.path);
			if (!file.data()) {
				logger::warn("could not read {}", plugin.name);
				continue;
			}
			Walker walker{ {}, g_interiors, g_exteriors };
			for (const auto& master : ReadMasters(file.data(), file.size())) {
				Walker::Owner owner;
				if (auto it = byName.find(Lower(master)); it != byName.end()) {
					owner = { true, it->second->light, it->second->index };
				}
				walker.owners.push_back(owner);
			}
			walker.owners.push_back({ true, plugin.light, plugin.index });  // the file's own records

			const std::size_t first = kHeader + std::min<std::size_t>(Read<std::uint32_t>(file.data() + 4), file.size() - kHeader);
			walker.Walk(file.data() + first, file.data() + file.size(), {}, 0);
			totalRefs += walker.refs;
		}
		auto tidy = [](auto& a_table) {
			for (auto& [key, list] : a_table) {
				std::ranges::sort(list);
				list.erase(std::unique(list.begin(), list.end()), list.end());
				list.shrink_to_fit();
			}
		};
		tidy(g_interiors);
		tidy(g_exteriors);
		logger::info("read {} placed objects from {} plugins", totalRefs, a_plugins.size());
	}

	// ---------------------------------------------------------------- cache on disk

	std::filesystem::path CachePath()
	{
		auto dir = SKSE::log::log_directory();
		return dir ? *dir / "DoorPreloader.cache" : std::filesystem::path("DoorPreloader.cache");
	}

	template <class T>
	void Put(std::ostream& a_out, const T& a_value)
	{
		a_out.write(reinterpret_cast<const char*>(&a_value), sizeof(T));
	}

	template <class T>
	bool Get(std::istream& a_in, T& a_value)
	{
		return static_cast<bool>(a_in.read(reinterpret_cast<char*>(&a_value), sizeof(T)));
	}

	template <class Table>
	void SaveTable(std::ostream& a_out, const Table& a_table)
	{
		Put(a_out, static_cast<std::uint32_t>(a_table.size()));
		for (const auto& [key, list] : a_table) {
			Put(a_out, key);
			Put(a_out, static_cast<std::uint32_t>(list.size()));
			a_out.write(reinterpret_cast<const char*>(list.data()), static_cast<std::streamsize>(list.size() * sizeof(RE::FormID)));
		}
	}

	template <class Table>
	bool LoadTable(std::istream& a_in, Table& a_table)
	{
		std::uint32_t count = 0;
		if (!Get(a_in, count) || count > 4'000'000) {
			return false;
		}
		a_table.reserve(count);
		for (std::uint32_t i = 0; i < count; ++i) {
			typename Table::key_type key{};
			std::uint32_t            n = 0;
			if (!Get(a_in, key) || !Get(a_in, n) || n > 1'000'000) {
				return false;
			}
			std::vector<RE::FormID> list(n);
			if (n && !a_in.read(reinterpret_cast<char*>(list.data()), static_cast<std::streamsize>(n * sizeof(RE::FormID)))) {
				return false;
			}
			a_table.emplace(key, std::move(list));
		}
		return true;
	}

	constexpr std::uint32_t kMagic = 0x58495348;  // "HSIX"

	bool LoadCache(std::uint64_t a_signature)
	{
		std::ifstream in(CachePath(), std::ios::binary);
		std::uint32_t magic = 0, version = 0;
		std::uint64_t signature = 0;
		if (!in || !Get(in, magic) || !Get(in, version) || !Get(in, signature) || magic != kMagic || version != kCacheVersion ||
			signature != a_signature) {
			return false;
		}
		if (!LoadTable(in, g_interiors) || !LoadTable(in, g_exteriors)) {
			g_interiors.clear();
			g_exteriors.clear();
			return false;
		}
		return true;
	}

	void SaveCache(std::uint64_t a_signature)
	{
		std::ofstream out(CachePath(), std::ios::binary | std::ios::trunc);
		if (!out) {
			logger::warn("could not write the index cache");
			return;
		}
		Put(out, kMagic);
		Put(out, kCacheVersion);
		Put(out, a_signature);
		SaveTable(out, g_interiors);
		SaveTable(out, g_exteriors);
	}
}

namespace Index
{
	void BuildAsync()
	{
		auto plugins = CollectPlugins();
		std::thread([plugins = std::move(plugins)] {
			const auto start = NowMs();
			const auto signature = Signature(plugins);
			if (LoadCache(signature)) {
				logger::info("index loaded from disk in {} ms: {} interior cells, {} exterior cells", NowMs() - start, g_interiors.size(),
					g_exteriors.size());
			} else {
				Parse(plugins);
				SaveCache(signature);
				logger::info("index built for this load order in {} ms: {} interior cells, {} exterior cells", NowMs() - start,
					g_interiors.size(), g_exteriors.size());
			}
			g_ready.store(true);
		}).detach();
	}

	bool Ready()
	{
		return g_ready.load();
	}

	const std::vector<RE::FormID>* Interior(RE::FormID a_cell)
	{
		const auto it = g_interiors.find(a_cell);
		return it != g_interiors.end() ? &it->second : nullptr;
	}

	void Exterior(RE::FormID a_world, std::int32_t a_x, std::int32_t a_y, std::int32_t a_radius, std::vector<RE::FormID>& a_out)
	{
		// nearest cells first, so a model limit cuts the far ones
		for (std::int32_t ring = 0; ring <= a_radius; ++ring) {
			for (std::int32_t dy = -ring; dy <= ring; ++dy) {
				for (std::int32_t dx = -ring; dx <= ring; ++dx) {
					if (std::max(std::abs(dx), std::abs(dy)) != ring) {
						continue;
					}
					const auto it = g_exteriors.find(ExteriorKey(a_world, a_x + dx, a_y + dy));
					if (it != g_exteriors.end()) {
						a_out.insert(a_out.end(), it->second.begin(), it->second.end());
					}
				}
			}
		}
	}
}
