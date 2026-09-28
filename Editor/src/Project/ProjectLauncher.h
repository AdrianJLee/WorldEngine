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
	//   * 最多 30 条(PROJ-4/T1:10 → 30,与启动器显示上限一致);同路径去重
	//     (**大小写不敏感**,新记录置顶、更新时间);
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

		// 置顶写入一条:同路径(大小写不敏感)去重、刷新 lastOpened、截到 30 条;原子落盘。
		static bool AddRecent(const std::filesystem::path& projectRoot, std::string* error = nullptr);

		// 移除一条(大小写不敏感匹配);未命中 = true(幂等)。落盘失败 = false + 原因。
		static bool RemoveRecent(const std::filesystem::path& projectRoot, std::string* error = nullptr);

		// 项目根校验(只读):目录存在 + 清单存在且可解析。reason = 可读原因(空 = 通过)。
		static bool IsValidProjectRoot(const std::filesystem::path& root, std::string* reason = nullptr);

		// PROJ-5/T1:永久删除项目目录(**不可恢复**;明确不走回收站/Shell/IFileOperation 删除)。
		//
		// 返回空串 = 成功;否则 = 可直接显示给用户的可读失败原因。typedName = 用户在二次确认里
		// 逐字输入的**项目目录名**:去首尾空白后须与目录名大小写不敏感相等,不匹配一律拒绝
		// (UI 也会禁用确认按钮,这里是双保险)。
		//
		// 全部校验在**任何写操作之前**完成(顺序即理由优先级,危险目标先判 —— 否则 `E:\` 这种
		// 目录名为空的目标会先撞上"名字不匹配",拿不到明确的拒绝理由):
		//   ① 目标存在且是目录;
		//   ② 拒绝盘根/UNC 根与"路径段数 < 2"的浅路径;
		//   ③ 拒绝 WLD_REPO_ROOT 本身或它的任何祖先(删掉会把整个仓库带走),
		//      以及引擎仓库内的目录(如 projects/default 这类随仓库走的项目);
		//   ④ 含 project.we.yaml(不是项目就拒绝);
		//   ⑤ typedName 与目录名匹配;
		//   ⑥ 编辑器形态下拒绝当前打开的项目(启动器形态没有当前项目,ProjectDir() 指向
		//      local/launcher-stub 哨兵,不会命中真实目标)。
		// 删除用 std::filesystem::remove_all;成功后从最近列表移除该条,失败**不**动列表项。
		static std::string DeleteProjectPermanently(const std::filesystem::path& projectRoot,
			const std::string& typedName);

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
