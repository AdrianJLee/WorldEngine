#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace World::Editor
{
	// PROJ-2/T1:项目启动器的"最近项目"存储(本机态 `local/projects.json`)与项目根校验。
	//
	// 口径(方案 v1 / 派工单):
	//   * 文件形态 `{ "recent": [ { "path": "...", "name": "...", "lastOpened": "<ISO8601>" } ] }`;
	//   * 最多 10 条;同路径去重(**大小写不敏感**,新记录置顶、更新时间);
	//   * 原子写(`projects.json.tmp-<随机>` → rename/MoveFileEx 替换);任何失败只报告,不崩;
	//   * 文件缺失/损坏 → 空列表(记一次警告);坏条目跳过而不是整表丢弃;
	//   * `Valid` = 目录存在 + `project.we.yaml` 存在且能被 ProjectManifest 解析(展示期判定,只读)。
	struct RecentProjectEntry
	{
		std::string Path;           // 项目根(存 UTF-8 文本,原样保留用户输入)
		std::string Name;           // 显示名:目录名优先,空则清单 id
		std::string LastOpened;     // ISO8601(UTC,秒精度)
		bool Valid = false;         // LoadRecent 逐条判定;AddRecent/RemoveRecent 不写它
		std::string InvalidReason;  // Valid=false 时的可读原因(面向 UI 的英文 canonical)
	};

	class ProjectLauncher
	{
	public:
		// 本机态存储路径 = WLD_LOCAL_DIR/projects.json(WLD_LOCAL_DIR 带尾分隔符)。
		static std::filesystem::path StorePath();

		// 读最近列表(顺序 = 最近在前)。缺失/损坏 = 空列表;逐条填 Valid/InvalidReason。
		static std::vector<RecentProjectEntry> LoadRecent();

		// 置顶写入一条:同路径(大小写不敏感)去重、刷新 lastOpened、截到 10 条;原子落盘。
		static bool AddRecent(const std::filesystem::path& projectRoot, std::string* error = nullptr);

		// 移除一条(大小写不敏感匹配);未命中 = true(幂等)。落盘失败 = false + 原因。
		static bool RemoveRecent(const std::filesystem::path& projectRoot, std::string* error = nullptr);

		// 项目根校验(只读):目录存在 + 清单存在且可解析。reason = 可读原因(空 = 通过)。
		static bool IsValidProjectRoot(const std::filesystem::path& root, std::string* reason = nullptr);

		// 两个路径是否指向同一位置(绝对 + 词法规范化 + 大小写不敏感;不要求路径存在)。
		static bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right);

		// 展示名:目录名;空则清单 id;都空 = "project"。
		static std::string DisplayName(const std::filesystem::path& projectRoot);

		// ISO8601(UTC):"<yyyy-MM-ddTHH:mm:ssZ>"。
		static std::string NowIso8601();

	private:
		// 只读文件、不判定有效性(AddRecent/RemoveRecent 的内部排序/去重用它)。
		static std::vector<RecentProjectEntry> LoadRaw();
		static bool SaveRecent(const std::vector<RecentProjectEntry>& entries, std::string* error);
	};
}
