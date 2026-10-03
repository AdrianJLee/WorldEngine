#include "wldpch.h"
#include "Project/ProjectLauncher.h"

#include "World/Asset/ProjectManifest.h"
#include "World/Core/Log.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLocalization.h"

#include <algorithm>
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

		// 路径"段数":根(盘符 / UNC 根)本身算 1 段,再加 relative_path 的每一段 ——
		// `E:\` = 1、`E:\proj` = 2、`\\server\share` = 1、`\\server\share\proj` = 2。
		// 与项目侧口径一致("路径段数 < 2"的目标一律拒绝,纯防御)。
		size_t PathSegmentCount(const std::filesystem::path& path)
		{
			size_t count = path.root_path().empty() ? 0 : 1;
			for (const std::filesystem::path& part : path.relative_path())
			{
				if (part != ".")
					++count;
			}
			return count;
		}

		// 引擎仓库内的"可丢弃根":这些第一段目录是构建产物 / 本机态 / 约定临时区,其下的
		// 项目目录允许永久删除。理由逐条与仓库口径对齐:
		//   build / tmp / local —— 本身就是 .gitignore 里的产物与本机状态目录;
		//   projects          —— projects/README.md 写明的"想把项目放在仓库里"时的默认容器;
		//   scratch / archive —— 仓库约定的临时/归档区。
		// 入参是相对仓库根的规范化键(小写、`/` 分隔,来自 NormalizedKey),因此比较天然大小写不敏感。
		bool IsDisposableRepoSubpath(const std::string& relativeKey)
		{
			if (relativeKey.empty())
				return false;
			const size_t slash = relativeKey.find('/');
			const std::string first = relativeKey.substr(0,
				slash == std::string::npos ? relativeKey.size() : slash);
			return first == "build" || first == "projects" || first == "tmp"
				|| first == "local" || first == "scratch" || first == "archive";
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

	std::string ProjectLauncher::DeleteProjectPermanently(const std::filesystem::path& projectRoot)
	{
		// 目标一律解析成"绝对 + 词法规范化"路径:破坏性删除绝不能跟着调用方的相对路径/CWD 走。
		std::error_code absoluteError;
		std::filesystem::path root = projectRoot;
		if (!root.is_absolute())
			root = std::filesystem::absolute(root, absoluteError);
		if (absoluteError)
			root = projectRoot;
		root = root.lexically_normal();
		const std::string rootText = root.empty() ? projectRoot.u8string() : root.u8string();

		// ① 必须存在且真的是目录(文件/符号链接目标/已失效路径一律拒绝)。
		std::error_code directoryError;
		if (root.empty() || !std::filesystem::is_directory(root, directoryError))
		{
			return Wui::TrFormat("modal.project_delete.error.not_directory",
				"The target is not an existing folder: {path}", { { "path", rootText } });
		}

		// ② 危险目标:盘根/UNC 根 + 浅路径(段数 < 2)。先于清单/当前项目判定 ——
		//    `E:\` 这类目标的拒绝理由必须明确(它们本来也不会含 project.we.yaml)。
		if (root == root.root_path())
		{
			return Wui::TrFormat("modal.project_delete.error.root",
				"Refusing to delete a drive/UNC root: {path}", { { "path", rootText } });
		}
		if (PathSegmentCount(root) < 2)
		{
			return Wui::TrFormat("modal.project_delete.error.too_shallow",
				"Refusing to delete a path this shallow: {path}", { { "path", rootText } });
		}

		// ③ 引擎仓库锚点:仓库根本身/它的任何祖先(删掉会把整个仓库带走)一律拒绝;仓库内
		//    只有"可丢弃根"下的目录放行(见 IsDisposableRepoSubpath),其余仓库内目录仍然拒绝
		//    —— 随仓库一起分发的项目/模板住在那里。用规范化键做前缀比较,大小写不敏感。
		const std::string key = NormalizedKey(root);
		const std::string repoKey = NormalizedKey(std::filesystem::path(std::string(WLD_REPO_ROOT)));
		if (key == repoKey || repoKey.rfind(key + "/", 0) == 0)
		{
			return Wui::TrFormat("modal.project_delete.error.repo_ancestor",
				"Refusing to delete the engine repository root or one of its parents: {path}",
				{ { "path", rootText } });
		}
		if (key.rfind(repoKey + "/", 0) == 0)
		{
			// 仓库根本身已在上面拦下 ⇒ 这里的相对键非空;第一段不在可丢弃根下才拒绝。
			const std::string relativeKey = key.substr(repoKey.size() + 1);
			if (!IsDisposableRepoSubpath(relativeKey))
			{
				return Wui::TrFormat("modal.project_delete.error.repo_inside",
					"Refusing to delete a folder inside the engine repository: {path}",
					{ { "path", rootText } });
			}
		}

		// ④ 不是项目就拒绝(与"打开项目"同一口径:缺清单 = 不是 WorldEngine 项目)。
		std::error_code manifestError;
		if (!std::filesystem::is_regular_file(root / kManifestFileName, manifestError))
		{
			return Wui::Tr("modal.project_delete.error.no_manifest",
				"Not a WorldEngine project: project.we.yaml is missing");
		}

		// ⑤ 编辑器形态下不能删当前打开的项目(启动器形态没有当前项目:ProjectDir() 指向
		//    local/launcher-stub 哨兵,不会命中真实目标)。
		if (SamePath(root, World::Paths::ProjectDir()))
		{
			return Wui::TrFormat("modal.project_delete.error.current_project",
				"Cannot delete the currently open project: {path}", { { "path", rootText } });
		}

		// ---- 以上校验全部通过,下面才开始写 ----
		// 永久删除(**不走**回收站/Shell/IFileOperation):remove_all 递归删目录树。
		std::error_code removeError;
		const std::uintmax_t removed = std::filesystem::remove_all(root, removeError);
		if (removeError || removed == static_cast<std::uintmax_t>(-1))
		{
			return Wui::TrFormat("modal.project_delete.error.remove_failed",
				"Permanent delete failed: {reason}",
				{ { "reason", removeError ? removeError.message() : std::string("unknown error") } });
		}
		WLD_CORE_INFO("[project] permanently deleted project folder '{0}' ({1} entries removed)",
			rootText, static_cast<unsigned long long>(removed));

		// 成功后从最近列表移除该条;列表落盘失败不回滚删除(目录已经没了),只记警告 ——
		// 残留的那条会在下次加载时被判为"路径失效"。失败路径**不**移除列表项。
		std::string listError;
		if (!RemoveRecent(root, &listError))
		{
			WLD_CORE_WARN("[project] permanent delete succeeded but updating the recent list failed: "
				"'{0}': {1}", rootText, listError);
		}
		return std::string();
	}
}
