#include "wldpch.h"
#include "ProjectLauncher.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Log.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLocalization.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

namespace World::Editor
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr const char* kManifestFileName = "project.we.yaml";
		// PROJ-4/T1(P2):最近列表上限 10 → 30,与启动器的显示上限(EditorShell 的
		// std::min(size, 30) + 滚动区)一致 —— 否则存储端先把第 11 条截掉,显示上限形同虚设。
		constexpr size_t kMaxRecentProjects = 30;

		std::string RandomToken()
		{
			const uint64_t now = static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			std::mt19937_64 generator(std::random_device{}() ^ now);
			char buffer[9] = {};
			std::snprintf(buffer, sizeof(buffer), "%08x",
				static_cast<unsigned int>(generator() & 0xffffffffu));
			return buffer;
		}

		// 去重/比较用的路径键:绝对 + 词法规范化 + 小写(Windows 大小写不敏感;
		// 不做 canonical() —— 路径可以已失效,键只需稳定)。
		std::string NormalizedKey(const std::filesystem::path& path)
		{
			std::error_code ec;
			std::filesystem::path absolute = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
			if (ec)
				absolute = path;
			std::string key = absolute.lexically_normal().generic_u8string();
			for (char& character : key)
			{
				const unsigned char c = static_cast<unsigned char>(character);
				if (c >= 'A' && c <= 'Z')
					character = static_cast<char>(c - 'A' + 'a');
			}
			return key;
		}

		bool ReadTextFile(const std::filesystem::path& file, std::string* out)
		{
			std::ifstream stream(file, std::ios::binary);
			if (!stream)
				return false;
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			*out = buffer.str();
			return true;
		}
	}

	std::filesystem::path ProjectLauncher::StorePath()
	{
		// WLD_LOCAL_DIR 带尾分隔符("<repo>/local/"),与 EditorPreferences 同一锚点。
		return std::filesystem::path(std::string(WLD_LOCAL_DIR) + "projects.json");
	}

	std::string ProjectLauncher::NowIso8601()
	{
		const std::time_t now = std::time(nullptr);
		std::tm utc {};
#ifdef WLD_PLATFORM_WINDOWS
		gmtime_s(&utc, &now);
#else
		gmtime_r(&now, &utc);
#endif
		char buffer[32] = {};
		if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
			return "1970-01-01T00:00:00Z";
		return buffer;
	}

	std::string ProjectLauncher::DisplayName(const std::filesystem::path& projectRoot)
	{
		const std::string directoryName = projectRoot.filename().u8string();
		if (!directoryName.empty())
			return directoryName;
		Asset::ProjectManifest manifest;
		std::string error;
		if (Asset::ProjectManifest::Load(projectRoot / kManifestFileName, &manifest, &error) &&
			!manifest.Id.empty())
			return manifest.Id;
		return "project";
	}

	bool ProjectLauncher::IsValidProjectRoot(const std::filesystem::path& root, std::string* reason)
	{
		if (root.empty())
		{
			if (reason)
				*reason = Wui::Tr("modal.launcher.error.not_directory", "The folder does not exist");
			return false;
		}
		std::error_code ec;
		if (!std::filesystem::is_directory(root, ec))
		{
			if (reason)
				*reason = Wui::Tr("modal.launcher.error.not_directory", "The folder does not exist");
			return false;
		}
		const std::filesystem::path manifestPath = root / kManifestFileName;
		std::error_code manifestError;
		if (!std::filesystem::is_regular_file(manifestPath, manifestError))
		{
			if (reason)
				*reason = Wui::Tr("modal.launcher.error.no_manifest",
					"Not a WorldEngine project: project.we.yaml is missing");
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string parseError;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &parseError))
		{
			if (reason)
				*reason = Wui::TrFormat("modal.launcher.error.manifest_invalid",
					"project.we.yaml could not be read: {reason}", { { "reason", parseError } });
			return false;
		}
		return true;
	}

	bool ProjectLauncher::SamePath(const std::filesystem::path& left, const std::filesystem::path& right)
	{
		if (left.empty() || right.empty())
			return left.empty() && right.empty();
		return NormalizedKey(left) == NormalizedKey(right);
	}

	std::vector<RecentProjectEntry> ProjectLauncher::LoadRaw()
	{
		std::vector<RecentProjectEntry> entries;
		const std::filesystem::path file = StorePath();
		std::string text;
		if (!ReadTextFile(file, &text))
			return entries;   // 首次运行/被删:空列表

		std::string parseError;
		const std::optional<Wui::JsonValue> root = Wui::JsonValue::Parse(text, &parseError);
		if (!root || root->type != Wui::JsonValue::Type::Object)
		{
			WLD_CORE_WARN("[project] recent projects file is not valid JSON, treating it as empty: {0} ({1})",
				file.u8string(), parseError);
			return entries;
		}
		const Wui::JsonValue* recent = root->Find("recent");
		if (!recent || recent->type != Wui::JsonValue::Type::Array)
			return entries;
		for (const Wui::JsonValue& item : recent->Array)
		{
			if (item.type != Wui::JsonValue::Type::Object)
				continue;   // 坏条目跳过,不整表丢弃
			RecentProjectEntry entry;
			entry.Path = item.Find("path") ? item.Find("path")->AsString() : std::string();
			entry.Name = item.Find("name") ? item.Find("name")->AsString() : std::string();
			entry.LastOpened = item.Find("lastOpened") ? item.Find("lastOpened")->AsString() : std::string();
			if (entry.Path.empty())
				continue;
			if (entry.Name.empty())
				entry.Name = DisplayName(std::filesystem::u8path(entry.Path));
			entries.push_back(std::move(entry));
		}
		return entries;
	}

	std::vector<RecentProjectEntry> ProjectLauncher::LoadRecent()
	{
		std::vector<RecentProjectEntry> entries = LoadRaw();
		for (RecentProjectEntry& entry : entries)
		{
			std::string reason;
			entry.Valid = IsValidProjectRoot(std::filesystem::u8path(entry.Path), &reason);
			entry.InvalidReason = entry.Valid ? std::string() : std::move(reason);
		}
		return entries;
	}

	bool ProjectLauncher::SaveRecent(const std::vector<RecentProjectEntry>& entries, std::string* error)
	{
		const std::filesystem::path file = StorePath();
		std::error_code ec;
		if (!file.parent_path().empty())
			std::filesystem::create_directories(file.parent_path(), ec);

		Wui::JsonValue recent;
		recent.type = Wui::JsonValue::Type::Array;
		for (const RecentProjectEntry& entry : entries)
		{
			Wui::JsonValue item;
			item.type = Wui::JsonValue::Type::Object;
			item.Object.emplace_back("path", Wui::JsonValue::MakeString(entry.Path));
			item.Object.emplace_back("name", Wui::JsonValue::MakeString(entry.Name));
			item.Object.emplace_back("lastOpened", Wui::JsonValue::MakeString(entry.LastOpened));
			recent.Array.push_back(std::move(item));
		}
		Wui::JsonValue root;
		root.type = Wui::JsonValue::Type::Object;
		root.Object.emplace_back("recent", std::move(recent));

		// 原子写:先写同目录临时文件,再整体替换(缺目录/半截文件都不会出现在目标路径上)。
		const std::filesystem::path temporary =
			file.parent_path() / (file.filename().u8string() + ".tmp-" + RandomToken());
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				if (error)
					*error = "cannot write " + temporary.u8string();
				return false;
			}
			stream << root.Dump() << "\n";
			stream.flush();
			if (!stream)
			{
				stream.close();
				std::error_code ignored;
				std::filesystem::remove(temporary, ignored);
				if (error)
					*error = "failed to write " + temporary.u8string();
				return false;
			}
		}
#ifdef WLD_PLATFORM_WINDOWS
		if (!MoveFileExW(temporary.wstring().c_str(), file.wstring().c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const DWORD lastError = GetLastError();
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			if (error)
				*error = "cannot replace " + file.u8string() + " (Win32 error "
					+ std::to_string(static_cast<unsigned long>(lastError)) + ")";
			return false;
		}
#else
		std::filesystem::rename(temporary, file, ec);
		if (ec)
		{
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			if (error)
				*error = "cannot replace " + file.u8string() + " (" + ec.message() + ")";
			return false;
		}
#endif
		return true;
	}

	bool ProjectLauncher::AddRecent(const std::filesystem::path& projectRoot, std::string* error)
	{
		if (projectRoot.empty())
		{
			if (error)
				*error = "project root is empty";
			return false;
		}
		std::error_code ec;
		const std::filesystem::path root = projectRoot.is_absolute()
			? projectRoot.lexically_normal()
			: std::filesystem::absolute(projectRoot, ec).lexically_normal();
		if (ec)
		{
			if (error)
				*error = "cannot resolve " + projectRoot.u8string();
			return false;
		}

		std::vector<RecentProjectEntry> entries = LoadRaw();
		const std::string key = NormalizedKey(root);
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[&key](const RecentProjectEntry& entry)
			{
				return NormalizedKey(std::filesystem::u8path(entry.Path)) == key;
			}), entries.end());

		RecentProjectEntry entry;
		entry.Path = root.u8string();
		entry.Name = DisplayName(root);
		entry.LastOpened = NowIso8601();
		entries.insert(entries.begin(), std::move(entry));
		if (entries.size() > kMaxRecentProjects)
			entries.resize(kMaxRecentProjects);
		return SaveRecent(entries, error);
	}

	bool ProjectLauncher::RemoveRecent(const std::filesystem::path& projectRoot, std::string* error)
	{
		std::vector<RecentProjectEntry> entries = LoadRaw();
		const std::string key = NormalizedKey(projectRoot);
		const size_t before = entries.size();
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[&key](const RecentProjectEntry& entry)
			{
				return NormalizedKey(std::filesystem::u8path(entry.Path)) == key;
			}), entries.end());
		if (entries.size() == before)
			return true;   // 幂等:本来就不在表里
		return SaveRecent(entries, error);
	}
}
