#include "wldpch.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiJson.h"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace World::Wui
{
	namespace
	{
		struct LocalizationState
		{
			std::string Language = "en";
			bool Loaded = false;
			uint32_t Generation = 1;
			bool TermHints = true;
			std::unordered_map<std::string, std::string> Catalog;
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

		void EnsureCatalogLoaded(LocalizationState& state)
		{
			if (state.Loaded)
				return;
			state.Loaded = true;   // 只尝试一次,失败也不反复读盘
			// 默认语言(英文)就是源码内联文案,不需要目录。
			if (state.Language.empty() || state.Language == "en" || state.Language == "en-US")
				return;
			const std::filesystem::path path =
				std::filesystem::path(WLD_GAME_DIR) / "assets" / "localization" / (state.Language + ".json");
			if (std::filesystem::exists(path))
				LoadLocalizationCatalog(path);
			else
				WLD_CORE_WARN("本地化目录缺失: {0}(回退内联默认文案)", path.string());
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
		state.Catalog.clear();
		for (const auto& [key, value] : root->Object)
			if (value.type == JsonValue::Type::String)
				state.Catalog[key] = value.String;
		state.Missing.clear();
		++state.Generation;
		WLD_CORE_INFO("本地化目录已加载: {0}({1} 条,语言 {2})", path.string(), state.Catalog.size(), state.Language);
		return true;
	}

	void SetLanguage(const std::string& code)
	{
		LocalizationState& state = State();
		if (state.Language == code)
			return;
		state.Language = code;
		state.Loaded = false;
		state.Catalog.clear();
		EnsureCatalogLoaded(state);
		++state.Generation;
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
