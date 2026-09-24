#include "wldpch.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiJson.h"

#include <algorithm>
#include <cctype>
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

		// 条目(S2/S3,前向兼容):文本 + 复数变体(`plural` = 类别 → 文本,如 one/few/many/other)。
		// `context`/`status`/`maxLength` 等结构化字段给工具链/译者用,读取器忽略。
		struct LocalizationEntry
		{
			bool HasText = false;      // `"k": "text"` 或 `{"text": "…"}`(`""` 也算有文本)
			std::string Text;
			std::unordered_map<std::string, std::string> Plural;
		};

		// 输入文件戳(S2 热重载):路径 + 大小 + 最后写入时间。
		struct LocalizationFileStamp
		{
			std::filesystem::path Path;
			uintmax_t Size = 0;
			std::filesystem::file_time_type Modified{};
		};

		// 集合比较用(C++17 不自动生成):路径 + 大小 + mtime 全同才算"没变"。
		bool operator==(const LocalizationFileStamp& a, const LocalizationFileStamp& b)
		{
			return a.Path == b.Path && a.Size == b.Size && a.Modified == b.Modified;
		}

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
			std::unordered_map<std::string, LocalizationEntry> Catalog;
			std::unordered_map<std::string, std::string> Sources;   // key → "层/文件.json"
			std::vector<std::string> Conflicts;                     // "层/文件.json:key"(层内重复,被忽略的那次)
			std::unordered_set<std::string> Missing;
			std::vector<LocalizationFileStamp> Stamp;               // 本次加载的输入文件(S2 热重载比对基准)
		};

		// 编译产物(S2):`<layer>/<lang>/catalog.json` + `$format` 版本;条目与域文件同一套形态。
		constexpr const char* kCatalogFileName = "catalog.json";
		constexpr const char* kCatalogFormat = "wld-localization-catalog/1";

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

		std::string LowerAscii(std::string_view text)
		{
			std::string out(text);
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		// 主语言标签(`zh-CN` → `zh`);回退链与复数类别共用。
		std::string MainLanguageTag(const std::string& language)
		{
			std::string main = language;
			if (const size_t cut = main.find_first_of("-_"); cut != std::string::npos)
				main.resize(cut);
			return LowerAscii(main);
		}

		// 英文族:`en` / `en-US` / `en_GB`…(大小写不敏感)。
		bool IsEnglishLike(const std::string& language)
		{
			const std::string lower = LowerAscii(language);
			return lower == "en" || lower.rfind("en-", 0) == 0 || lower.rfind("en_", 0) == 0;
		}

		// 默认语言(英文族 / 空)= 源码内联文案,不读目录(P4-UX1 口径)。
		bool IsInlineDefaultLanguage(const std::string& language)
		{
			return language.empty() || IsEnglishLike(language);
		}

		// 语言回退链候选(S2):`zh-CN` → {`zh-CN`, `zh`}(去重);英文族/空 = 空表。
		std::vector<std::string> LanguageCandidates(const std::string& language)
		{
			std::vector<std::string> candidates;
			if (IsInlineDefaultLanguage(language))
				return candidates;
			candidates.push_back(language);
			const std::string main = MainLanguageTag(language);
			if (!main.empty() && main != LowerAscii(language))
				candidates.push_back(main);
			return candidates;
		}

		// 复数类别(S3,内部静态,不导出):en = one/other;ru = one/few/many;zh/ja/ko 与其它语言 = other。
		// 返回的类别名就是 `plural` 词典的键;词典缺该类别时由调用方回退 `other`。
		const char* PluralCategory(const std::string& language, long long count)
		{
			const std::string main = MainLanguageTag(language);
			if (main == "en")
				return count == 1 ? "one" : "other";
			if (main == "ru")
			{
				const long long n = count < 0 ? -count : count;
				const long long mod10 = n % 10;
				const long long mod100 = n % 100;
				if (mod10 == 1 && mod100 != 11)
					return "one";
				if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14))
					return "few";
				return "many";   // 0 / 5–9 / 11–14 → many
			}
			return "other";      // zh/ja/ko 等:中性(无复数变化)
		}

		// 条目值:字符串,或结构化对象(`{"text": …}` / `{"plural": {类别: 文本}}`);
		// 其它值形态(数字/数组/null)不进表(前向兼容)。
		bool ParseEntry(const JsonValue& value, LocalizationEntry& out)
		{
			out = LocalizationEntry{};
			if (value.type == JsonValue::Type::String)
			{
				out.HasText = true;
				out.Text = value.String;
				return true;
			}
			if (value.type != JsonValue::Type::Object)
				return false;
			if (const JsonValue* text = value.Find("text"))
				if (text->type == JsonValue::Type::String)
				{
					out.HasText = true;
					out.Text = text->String;
				}
			if (const JsonValue* plural = value.Find("plural"))
				if (plural->type == JsonValue::Type::Object)
					for (const auto& [category, variant] : plural->Object)
						if (variant.type == JsonValue::Type::String && !category.empty())
							out.Plural.emplace(category, variant.String);
			// 只有 `plural.other`(无 `text`)时,把它同时当条目文本 —— `Tr`/`TrFormat` 也能读到。
			if (!out.HasText)
				if (const auto other = out.Plural.find("other"); other != out.Plural.end())
				{
					out.HasText = true;
					out.Text = other->second;
				}
			return out.HasText || !out.Plural.empty();
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

		// 读一个 JSON 对象(域文件 / 编译产物 / 单文件入口共用);失败时 error 给出原因。
		bool ReadJsonObject(const std::filesystem::path& path, JsonValue& out, std::string& error)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				error = "文件打不开";
				return false;
			}
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			const std::optional<JsonValue> root = JsonValue::Parse(buffer.str(), &error);
			if (!root || root->type != JsonValue::Type::Object)
			{
				if (error.empty())
					error = "根节点不是 JSON 对象";
				return false;
			}
			out = *root;
			return true;
		}

		// 编译产物探测(S2):文件可解析且 `$format` = `wld-localization-catalog/1`。
		bool IsCompiledCatalog(const std::filesystem::path& path)
		{
			JsonValue root;
			std::string error;
			if (!ReadJsonObject(path, root, error))
				return false;
			const JsonValue* format = root.Find("$format");
			return format != nullptr && format->type == JsonValue::Type::String && format->String == kCatalogFormat;
		}

		std::string JoinCandidates(const std::vector<std::string>& candidates)
		{
			std::string out;
			for (const std::string& candidate : candidates)
			{
				if (!out.empty())
					out += ", ";
				out += candidate;
			}
			return out.empty() ? std::string("(无:英文族/空语言不读目录)") : out;
		}

		// 该层该语言要读的一个输入文件(相对键 → 路径;相对键 = 来源标签/排序键)。
		struct LocalizationInputFile
		{
			std::string Relative;
			std::filesystem::path Path;
		};

		// 一层在本语言下的输入:命中的候选目录 + 要读的文件(产物模式 = 只有 catalog.json)。
		struct LayerInputs
		{
			std::filesystem::path Directory;   // `<layer>/<candidate>`;空 = 该层没有该语言
			bool Artifact = false;             // true = 只读编译产物(跳过域文件递归扫描)
			std::vector<LocalizationInputFile> Files;
			std::string Warning;               // 收集期读盘错误(加载路径负责打日志;轮询路径不打)
		};

		// 收集该层该语言的输入文件(纯查询,不写日志 —— `LocalizationFilesChanged` 也用它):
		// 逐候选找第一个存在的 `<directory>/<candidate>`;命中后若 `<candidate>/catalog.json`
		// 是合法产物则只读它,否则递归扫 `<candidate>/**/*.json`(相对路径序)。
		LayerInputs CollectLayerInputs(const LocalizationLayer& layer, const std::string& language)
		{
			LayerInputs inputs;
			for (const std::string& candidate : LanguageCandidates(language))
			{
				const std::filesystem::path directory = layer.Directory / candidate;
				std::error_code exists;
				if (std::filesystem::is_directory(directory, exists))
				{
					inputs.Directory = directory;
					break;                        // 先命中的候选生效(该层不再看后续候选)
				}
			}
			if (inputs.Directory.empty())
				return inputs;

			const std::filesystem::path artifact = inputs.Directory / kCatalogFileName;
			std::error_code artifactError;
			if (std::filesystem::is_regular_file(artifact, artifactError) && IsCompiledCatalog(artifact))
			{
				inputs.Artifact = true;           // 产物优先:层内域文件不参与本次加载
				inputs.Files.push_back({ std::string(kCatalogFileName), artifact });
				return inputs;
			}

			std::vector<std::pair<std::string, std::filesystem::path>> files;
			std::error_code ec;
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(
					inputs.Directory, std::filesystem::directory_options::skip_permission_denied, ec))
				if (entry.is_regular_file() && entry.path().extension() == ".json")
					files.emplace_back(RelativeKey(entry.path(), inputs.Directory), entry.path());
			if (ec)
			{
				inputs.Warning = "本地化层目录读取失败: " + inputs.Directory.string() + "(" + ec.message() + ")";
				inputs.Files.clear();
				return inputs;
			}
			std::sort(files.begin(), files.end(), [](const auto& a, const auto& b)
			{
				return a.first < b.first;   // 相对路径逐字节序(与迭代顺序、子目录深度无关)
			});
			for (auto& [relative, file] : files)
				inputs.Files.push_back({ relative, std::move(file) });
			return inputs;
		}

		// 读一个输入文件(域文件或编译产物),条目并入层内表。
		// 层内(含单文件内)重复键:先出现者生效,后出现的记入 conflicts —— 跨层覆盖不算冲突。
		void LoadInputFile(const LocalizationLayer& layer, const LocalizationInputFile& input,
			std::unordered_map<std::string, LocalizationEntry>& catalog,
			std::unordered_map<std::string, std::string>& sources,
			std::vector<std::string>& conflicts)
		{
			JsonValue root;
			std::string error;
			if (!ReadJsonObject(input.Path, root, error))
			{
				WLD_CORE_WARN("本地化语言包读取失败({0}):{1}", input.Path.string(), error);
				return;
			}
			const std::string label = layer.Name + "/" + input.Relative;
			size_t count = 0;
			for (const auto& [key, value] : root.Object)
			{
				if (IsMetadataKey(key))
					continue;
				LocalizationEntry entry;
				if (!ParseEntry(value, entry))
					continue;   // 非文本形态忽略(前向兼容)
				if (catalog.count(key))
				{
					conflicts.push_back(label + ":" + key);   // 层内重复:先出现者生效
					continue;
				}
				catalog.emplace(key, std::move(entry));
				sources[key] = label;
				++count;
			}
			WLD_CORE_INFO("本地化文件已加载: {0}({1} 条)", input.Path.string(), count);
		}

		// 取文件的盘面戳(大小 + 最后写入时间);不存在/取不到 → false。
		bool StatFile(const std::filesystem::path& path, LocalizationFileStamp& out)
		{
			std::error_code ec;
			const uintmax_t size = std::filesystem::file_size(path, ec);
			if (ec)
				return false;
			const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, ec);
			if (ec)
				return false;
			out.Path = path;
			out.Size = size;
			out.Modified = modified;
			return true;
		}

		void SortStamp(std::vector<LocalizationFileStamp>& stamp)
		{
			std::sort(stamp.begin(), stamp.end(), [](const LocalizationFileStamp& a, const LocalizationFileStamp& b)
			{
				const std::string left = a.Path.generic_string();
				const std::string right = b.Path.generic_string();
				if (left != right)
					return left < right;
				if (a.Size != b.Size)
					return a.Size < b.Size;
				return a.Modified < b.Modified;
			});
		}

		// 当前盘面上的输入文件集合 = 按加载同一套规则(候选回退 / 产物优先 / 递归域文件)重算。
		// 与 `Stamp` 比较即可发现"改 / 增 / 删" —— 枚举不到的文件(已删除)自然从集合里消失。
		std::vector<LocalizationFileStamp> CurrentStamp(const LocalizationState& state)
		{
			std::vector<LocalizationFileStamp> stamp;
			for (const LocalizationLayer& layer : state.Layers)
			{
				const LayerInputs inputs = CollectLayerInputs(layer, state.Language);
				for (const LocalizationInputFile& input : inputs.Files)
				{
					LocalizationFileStamp file;
					if (StatFile(input.Path, file))
						stamp.push_back(std::move(file));
				}
			}
			SortStamp(stamp);
			return stamp;
		}

		// 按 priority 升序 + 注册顺序叠加全部层(高优先级覆盖低优先级);重建 Catalog/Sources/Conflicts/Stamp。
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
				const LayerInputs inputs = CollectLayerInputs(*layer, state.Language);
				if (inputs.Directory.empty())
				{
					WLD_CORE_WARN("本地化层目录缺失: {0}(层 {1},语言 {2};候选 {3};回退内联默认文案)",
						(layer->Directory / state.Language).string(), layer->Name, state.Language,
						JoinCandidates(LanguageCandidates(state.Language)));
					continue;
				}
				if (!inputs.Warning.empty())
					WLD_CORE_WARN("{0}", inputs.Warning);
				if (inputs.Artifact)
					WLD_CORE_WARN("本地化编译产物优先: {0}(层 {1};跳过域文件扫描)",
						inputs.Files.front().Path.string(), layer->Name);

				std::unordered_map<std::string, LocalizationEntry> layerCatalog;
				std::unordered_map<std::string, std::string> layerSources;
				for (const LocalizationInputFile& input : inputs.Files)
					LoadInputFile(*layer, input, layerCatalog, layerSources, state.Conflicts);
				for (auto& [key, entry] : layerCatalog)
				{
					state.Catalog[key] = std::move(entry);             // 后解析的层覆盖先解析的层
					state.Sources[key] = layerSources[key];
				}
			}
			state.Stamp = CurrentStamp(state);   // S2 热重载比对基准(与加载同一套解析规则)
			state.Missing.clear();
			++state.Generation;
			WLD_CORE_INFO("本地化目录已加载: {0} 个层/{1} 条(语言 {2};层内重复 {3};输入文件 {4})",
				ordered.size(), state.Catalog.size(), state.Language, state.Conflicts.size(), state.Stamp.size());
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

		// 层集合或语言变了:清空已加载内容(含热重载基准),下一处 Tr 重扫(Generation++ 供 UI 缓存失效)。
		void InvalidateCatalog(LocalizationState& state)
		{
			state.Loaded = false;
			state.Catalog.clear();
			state.Sources.clear();
			state.Conflicts.clear();
			state.Stamp.clear();
			++state.Generation;
		}

		// 占位符替换(S3):`{name}` → 值(未知占位符原样保留);`{{`/`}}` → `{`/`}`;其余字符原样。
		std::string FormatPlaceholders(std::string_view text,
			const std::vector<std::pair<std::string_view, std::string_view>>& args)
		{
			std::string out;
			out.reserve(text.size());
			for (size_t i = 0; i < text.size();)
			{
				const char c = text[i];
				if (c == '{')
				{
					if (i + 1 < text.size() && text[i + 1] == '{')
					{
						out.push_back('{');           // `{{` 转义
						i += 2;
						continue;
					}
					const size_t close = text.find('}', i + 1);
					// 占位符内部不允许再出现 `{`(嵌套写法按字面量透传)。
					if (close != std::string_view::npos && text.find('{', i + 1) > close)
					{
						const std::string_view name = text.substr(i + 1, close - i - 1);
						std::string_view value;
						bool replaced = false;
						for (const auto& arg : args)
						{
							if (arg.first != name)
								continue;
							value = arg.second;
							replaced = true;
							break;
						}
						if (replaced)
						{
							out.append(value);
							i = close + 1;
							continue;
						}
					}
					out.push_back('{');               // 未知/不完整 → 原样保留
					++i;
					continue;
				}
				if (c == '}' && i + 1 < text.size() && text[i + 1] == '}')
				{
					out.push_back('}');               // `}}` 转义
					i += 2;
					continue;
				}
				out.push_back(c);
				++i;
			}
			return out;
		}

		// 查表(含 EnsureCatalogLoaded);未命中返回 nullptr。
		const LocalizationEntry* FindEntry(LocalizationState& state, std::string_view key)
		{
			EnsureCatalogLoaded(state);
			const auto found = state.Catalog.find(std::string(key));
			return found == state.Catalog.end() ? nullptr : &found->second;
		}
	}

	std::string Tr(std::string_view key, std::string_view fallback)
	{
		LocalizationState& state = State();
		const LocalizationEntry* entry = FindEntry(state, key);
		if (entry != nullptr && entry->HasText)
			return entry->Text;
		if (entry == nullptr && !state.Catalog.empty())
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
		JsonValue root;
		std::string error;
		if (!ReadJsonObject(path, root, error))
		{
			WLD_CORE_WARN("本地化目录读取失败({0}):{1}", path.string(), error);
			return false;
		}
		// 单文件入口(测试/兼容):等价于"只有这一个文件"的目录,替换现有一切。
		state.Catalog.clear();
		state.Sources.clear();
		state.Conflicts.clear();
		const std::string label = "file/" + path.filename().string();
		size_t count = 0;
		for (const auto& [key, value] : root.Object)
		{
			if (IsMetadataKey(key))
				continue;
			LocalizationEntry entry;
			if (!ParseEntry(value, entry))
				continue;
			if (state.Catalog.count(key))
			{
				state.Conflicts.push_back(label + ":" + key);
				continue;
			}
			state.Catalog.emplace(key, std::move(entry));
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

	std::vector<std::string> LocalizationLanguageCandidates()
	{
		return LanguageCandidates(State().Language);
	}

	bool LocalizationFilesChanged()
	{
		LocalizationState& state = State();
		if (!state.Loaded)
			return false;                              // 未加载过 → 没有可比对的输入集合
		if (IsInlineDefaultLanguage(state.Language))
			return false;                              // 英文族/空:内联默认,不读盘
		return CurrentStamp(state) != state.Stamp;
	}

	std::string TrFormat(std::string_view key, std::string_view fallback,
		const std::vector<std::pair<std::string_view, std::string_view>>& args)
	{
		LocalizationState& state = State();
		const LocalizationEntry* entry = FindEntry(state, key);
		if (entry != nullptr && entry->HasText)
			return FormatPlaceholders(entry->Text, args);
		if (entry == nullptr && !state.Catalog.empty())
			state.Missing.insert(std::string(key));
		return FormatPlaceholders(fallback, args);
	}

	std::string TrPlural(std::string_view key, std::string_view fallback, long long count)
	{
		LocalizationState& state = State();
		const LocalizationEntry* entry = FindEntry(state, key);
		std::string text;
		if (entry != nullptr && !entry->Plural.empty())
		{
			const auto variant = entry->Plural.find(PluralCategory(state.Language, count));
			if (variant != entry->Plural.end())
				text = variant->second;
			else if (const auto other = entry->Plural.find("other"); other != entry->Plural.end())
				text = other->second;                  // 缺该类别 → 回退 other
		}
		if (text.empty() && entry != nullptr && entry->HasText)
			text = entry->Text;                        // 没有 `plural` → 文本 + `{count}`
		if (text.empty())
		{
			if (entry == nullptr && !state.Catalog.empty())
				state.Missing.insert(std::string(key));
			text = std::string(fallback);
		}
		const std::string number = std::to_string(count);
		const std::vector<std::pair<std::string_view, std::string_view>> args{ { "count", number } };
		return FormatPlaceholders(text, args);
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
