#include "wldpch.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiJson.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace World::Wui
{
	namespace
	{
		// 一层语言包 = 一个目录 + 一个优先级(engine / editor / project / Mod… 数量不限)。
		// `Order` = 注册顺序:同优先级时先解析先注册的层,后注册的层覆盖它。
		struct LocalizationLayer
		{
			std::string Name;
			std::filesystem::path Directory;
			int Priority = 0;
			size_t Order = 0;
		};

		struct LocalizationState
		{
			std::string Language = "en";
			bool Loaded = false;
			uint32_t Generation = 1;
			bool TermHints = true;
			// 没有注册任何层时的默认目录(P4-UX1 的"游戏内容语言包"默认值)。
			std::filesystem::path Directory =
				std::filesystem::path(WLD_PROJECT_DIR) / "assets" / "localization";
			std::vector<LocalizationLayer> Layers;
			size_t NextLayerOrder = 0;
			std::unordered_map<std::string, std::string> Catalog;
			std::unordered_map<std::string, std::string> Sources;   // key → "层/文件.json"
			std::vector<std::string> Conflicts;                     // "层/文件.json:key"(层内重复,被忽略的那次)
			std::unordered_set<std::string> Missing;
		};

		LocalizationState& State()
		{
			static LocalizationState state = [] {
				LocalizationState out;
				if (const char* lang = std::getenv("WLD_LANG"))
					if (lang[0])
						out.Language = lang;
				if (const char* hints = std::getenv("WLD_UI_TERM_HINTS"))
					out.TermHints = !(hints[0] == '0' || hints[0] == '\0');
				return out;
			}();
			return state;
		}

		// 默认语言(英文 / `en-US` / 空)= 源码内联文案,不读目录(P4-UX1 口径)。
		bool IsInlineDefaultLanguage(const std::string& language)
		{
			return language.empty() || language == "en" || language == "en-US";
		}

		// 条目值:字符串,或 `{"text": "…"}` 对象 —— 其它字段忽略(S2 结构化条目的前向兼容)。
		bool EntryText(const JsonValue& value, std::string& out)
		{
			if (value.type == JsonValue::Type::String)
			{
				out = value.String;
				return true;
			}
			if (value.type == JsonValue::Type::Object)
			{
				if (const JsonValue* text = value.Find("text"))
					if (text->type == JsonValue::Type::String)
					{
						out = text->String;
						return true;
					}
			}
			return false;
		}

		bool IsMetadataKey(const std::string& key)
		{
			return key.empty() || key.front() == '$';   // $format/$layer/$language/$owns… 不作文案
		}

		// 相对键 = 文件相对 `<directory>/<language>` 的路径(POSIX 分隔符,逐字节序)。
		// 语言包按域分文件夹(`panels/settings.json`),子目录也参与排序,结果与平台无关。
		std::string RelativeKey(const std::filesystem::path& file, const std::filesystem::path& root)
		{
			return file.lexically_relative(root).generic_string();
		}

		// 扫描一层的 `<directory>/<language>/**/*.json`(递归;只认常规文件 + `.json`)。
		// 层内(含单文件内)重复键:按相对键序先出现者生效,后出现的记入 conflicts —— 跨层覆盖不算冲突。
		void ScanLayer(const LocalizationLayer& layer, const std::string& language,
			std::unordered_map<std::string, std::string>& catalog,
			std::unordered_map<std::string, std::string>& sources,
			std::vector<std::string>& conflicts)
		{
			const std::filesystem::path directory = layer.Directory / language;
			std::error_code ec;
			if (!std::filesystem::is_directory(directory, ec))
			{
				WLD_CORE_WARN("本地化层目录缺失: {0}(层 {1},语言 {2};回退内联默认文案)",
					directory.string(), layer.Name, language);
				return;
			}
			std::vector<std::pair<std::string, std::filesystem::path>> files;
			// 递归扫描:`<directory>/<language>/**/*.json`(子目录 = 分域,如 panels/ shell/ schema/)。
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(
					directory, std::filesystem::directory_options::skip_permission_denied, ec))
				if (entry.is_regular_file() && entry.path().extension() == ".json")
					files.emplace_back(RelativeKey(entry.path(), directory), entry.path());
			if (ec)
			{
				WLD_CORE_WARN("本地化层目录读取失败: {0}({1})", directory.string(), ec.message());
				return;
			}
			std::sort(files.begin(), files.end(), [](const auto& a, const auto& b)
			{
				return a.first < b.first;   // 相对路径逐字节序(与迭代顺序、子目录深度无关)
			});
			for (const auto& [relative, file] : files)
			{
				std::ifstream stream(file, std::ios::binary);
				if (!stream)
				{
					WLD_CORE_WARN("本地化语言包打不开: {0}", file.string());
					continue;
				}
				std::ostringstream buffer;
				buffer << stream.rdbuf();
				std::string error;
				const std::optional<JsonValue> root = JsonValue::Parse(buffer.str(), &error);
				if (!root || root->type != JsonValue::Type::Object)
				{
					WLD_CORE_WARN("本地化语言包解析失败({0}): {1}", file.string(), error);
					continue;
				}
				const std::string label = layer.Name + "/" + relative;
				size_t count = 0;
				for (const auto& [key, value] : root->Object)
				{
					if (IsMetadataKey(key))
						continue;
					std::string text;
					if (!EntryText(value, text))
						continue;   // 非文本形态忽略(前向兼容)
					if (catalog.count(key))
					{
						conflicts.push_back(label + ":" + key);   // 层内重复:先出现者生效
						continue;
					}
					catalog.emplace(key, std::move(text));
					sources[key] = label;
					++count;
				}
				WLD_CORE_INFO("本地化文件已加载: {0}({1} 条)", file.string(), count);
			}
		}

		// 按 priority 升序 + 注册顺序叠加全部层(高优先级覆盖低优先级);重建 Catalog/Sources/Conflicts。
		void LoadAllLayers(LocalizationState& state)
		{
			state.Catalog.clear();
			state.Sources.clear();
			state.Conflicts.clear();

			std::vector<const LocalizationLayer*> ordered;
			ordered.reserve(state.Layers.size());
			for (const LocalizationLayer& layer : state.Layers)
				ordered.push_back(&layer);
			std::sort(ordered.begin(), ordered.end(), [](const LocalizationLayer* a, const LocalizationLayer* b)
			{
				if (a->Priority != b->Priority)
					return a->Priority < b->Priority;
				return a->Order < b->Order;
			});

			for (const LocalizationLayer* layer : ordered)
			{
				std::unordered_map<std::string, std::string> layerCatalog;
				std::unordered_map<std::string, std::string> layerSources;
				ScanLayer(*layer, state.Language, layerCatalog, layerSources, state.Conflicts);
				for (const auto& [key, text] : layerCatalog)
				{
					state.Catalog[key] = text;             // 后解析的层覆盖先解析的层
					state.Sources[key] = layerSources[key];
				}
			}
			state.Missing.clear();
			++state.Generation;
			WLD_CORE_INFO("本地化目录已加载: {0} 个层/{1} 条(语言 {2};层内重复 {3})",
				ordered.size(), state.Catalog.size(), state.Language, state.Conflicts.size());
		}

		void EnsureCatalogLoaded(LocalizationState& state)
		{
			if (state.Loaded)
				return;
			state.Loaded = true;   // 只尝试一次,失败也不反复读盘
			if (IsInlineDefaultLanguage(state.Language))
				return;
			if (state.Layers.empty())
			{
				WLD_CORE_WARN("本地化未注册任何层(语言 {0};回退内联默认文案)", state.Language);
				return;
			}
			LoadAllLayers(state);
		}

		// 层集合或语言变了:清空已加载内容,下一处 Tr 重扫(Generation++ 供 UI 缓存失效)。
		void InvalidateCatalog(LocalizationState& state)
		{
			state.Loaded = false;
			state.Catalog.clear();
			state.Sources.clear();
			state.Conflicts.clear();
			++state.Generation;
		}
	}

	std::string Tr(std::string_view key, std::string_view fallback)
	{
		LocalizationState& state = State();
		EnsureCatalogLoaded(state);
		if (state.Catalog.empty())
			return std::string(fallback);
		const auto found = state.Catalog.find(std::string(key));
		if (found != state.Catalog.end())
			return found->second;
		state.Missing.insert(std::string(key));
		return std::string(fallback);
	}

	LocalizedLabel TrLabel(std::string_view key, std::string_view englishTerm)
	{
		LocalizedLabel label;
		label.Text = Tr(key, englishTerm);
		// 主文案已经就是英文时不重复;中文等其它语言下按需带英文术语。
		const std::string& language = GetLanguage();
		const bool englishUi = language.empty() || language.rfind("en", 0) == 0;
		if (!englishUi && ShowTermHints() && !englishTerm.empty() && label.Text != englishTerm)
			label.Term = std::string(englishTerm);
		return label;
	}

	bool ShowTermHints()
	{
		return State().TermHints;
	}

	void SetShowTermHints(bool enabled)
	{
		State().TermHints = enabled;
		++State().Generation;
	}

	bool LoadLocalizationCatalog(const std::filesystem::path& path)
	{
		LocalizationState& state = State();
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			WLD_CORE_WARN("本地化目录打不开: {0}", path.string());
			return false;
		}
		std::ostringstream buffer;
		buffer << file.rdbuf();
		std::string error;
		const std::optional<JsonValue> root = JsonValue::Parse(buffer.str(), &error);
		if (!root || root->type != JsonValue::Type::Object)
		{
			WLD_CORE_WARN("本地化目录解析失败({0}): {1}", path.string(), error);
			return false;
		}
		// 单文件入口(测试/兼容):等价于"只有这一个文件"的目录,替换现有一切。
		state.Catalog.clear();
		state.Sources.clear();
		state.Conflicts.clear();
		const std::string label = "file/" + path.filename().string();
		size_t count = 0;
		for (const auto& [key, value] : root->Object)
		{
			if (IsMetadataKey(key))
				continue;
			std::string text;
			if (!EntryText(value, text))
				continue;
			if (state.Catalog.count(key))
			{
				state.Conflicts.push_back(label + ":" + key);
				continue;
			}
			state.Catalog.emplace(key, std::move(text));
			state.Sources[key] = label;
			++count;
		}
		state.Missing.clear();
		++state.Generation;
		WLD_CORE_INFO("本地化目录已加载: {0}({1} 条,语言 {2})", path.string(), count, state.Language);
		return true;
	}

	void SetLanguage(const std::string& code)
	{
		LocalizationState& state = State();
		if (state.Language == code)
			return;
		state.Language = code;
		InvalidateCatalog(state);
		EnsureCatalogLoaded(state);
	}

	void SetLocalizationDirectory(const std::filesystem::path& directory)
	{
		// 兼容壳:清空后只注册一层(name = "default", priority = 0),行为与旧版单目录等价。
		ClearLocalizationLayers();
		RegisterLocalizationLayer("default", directory, 0);
	}

	const std::filesystem::path& GetLocalizationDirectory()
	{
		LocalizationState& state = State();
		return state.Layers.empty() ? state.Directory : state.Layers.front().Directory;
	}

	void RegisterLocalizationLayer(std::string_view name, const std::filesystem::path& directory, int priority)
	{
		LocalizationState& state = State();
		LocalizationLayer layer;
		layer.Name = std::string(name);
		layer.Directory = directory;
		layer.Priority = priority;
		layer.Order = state.NextLayerOrder++;
		state.Layers.push_back(std::move(layer));
		InvalidateCatalog(state);
		WLD_CORE_INFO("本地化层已注册: {0} → {1}(priority {2})", std::string(name), directory.string(), priority);
	}

	void ClearLocalizationLayers()
	{
		LocalizationState& state = State();
		state.Layers.clear();
		state.NextLayerOrder = 0;
		InvalidateCatalog(state);
	}

	void ReloadLocalization()
	{
		LocalizationState& state = State();
		InvalidateCatalog(state);
		EnsureCatalogLoaded(state);   // 立即重扫(英文内联语言下按口径不读盘)
	}

	std::string LocalizationSource(std::string_view key)
	{
		LocalizationState& state = State();
		EnsureCatalogLoaded(state);
		const auto found = state.Sources.find(std::string(key));
		return found == state.Sources.end() ? std::string() : found->second;
	}

	std::vector<std::string> LocalizationConflicts()
	{
		LocalizationState& state = State();
		EnsureCatalogLoaded(state);
		return state.Conflicts;
	}

	const std::string& GetLanguage()
	{
		return State().Language;
	}

	uint32_t LocalizationGeneration()
	{
		return State().Generation;
	}

	std::vector<std::string> MissingLocalizationKeys()
	{
		const LocalizationState& state = State();
		return { state.Missing.begin(), state.Missing.end() };
	}
}
