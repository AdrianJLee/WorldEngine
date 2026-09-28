#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	class WorldContext;

	namespace Editor
	{
		// PROJ-1/T1:标准项目骨架生成器(纯逻辑 + 落盘;File ▸ New Project… 的内核)。
		//
		// 用户口径(2026-09-28):新项目必须是**标准、干净**的骨架,**不含**默认项目
		// (projects/default)里的任何示例。因此这里**从不**复制 projects/default:
		//   * 目录结构  ← 仓库模板 templates/project/**(逐文件复制,1:1;模板目录里
		//                 不要放"仅维护者可见"的说明文件 —— 它们会被复制进项目);
		//   * project.we.yaml ← 引擎 writer(ProjectManifest::Save,与解析同源,不手写 YAML);
		//   * assets/scenes/Main.wd ← 引擎场景序列化(SceneSerializer,不手写场景);
		//   * .vscode / .luau-lsp(Luau LSP 脚手架)← 引擎既有 EnsureScriptEditorScaffold
		//     (与编辑器启动期为当前项目生成的那两份逐字节同源)。
		//
		// 落盘纪律:先写 <目标>.tmp-<随机>,全部成功后再整体 rename 到 <目标>;
		// 任一步失败 = 删掉临时目录,目标位置不留半成品;逐文件不覆盖既有文件。
		class ProjectScaffolder
		{
		public:
			// 模板根 = <checkout>/templates/project(WLD_REPO_ROOT 锚定,与进程 CWD 无关)。
			static std::filesystem::path TemplateRoot();

			// "新建位置"的默认值:优先用户文档目录(%USERPROFILE%/Documents),否则当前工作目录。
			static std::filesystem::path DefaultProjectLocation();

			// 项目名校验(目录名口径:名称就是 <位置>/<名称> 的那个目录名)。
			// 空 / 含 \ / : * ? " < > | / 控制字符 / 结尾的 . 或空格 / Windows 保留设备名
			// → 可读原因;空串 = 通过。
			static std::string ValidateProjectName(const std::string& name);
			// 落点校验(只读):位置必须是已存在的目录;目标目录必须不存在或为空。
			static std::string ValidateLocation(const std::filesystem::path& location, const std::string& name);
			// 模板自检(只读):templates/project 目录与其必需条目是否齐全;空串 = 通过。
			// 向导的"模板缺失"行内错误与 Create 的前置守卫共用这一条。
			static std::string ValidateTemplate();

			// 项目清单 id = "com.<小写名称>"(小写 + 只保留 [a-z0-9._-],其余字符换成 '.';空段合并)。
			// 注意:不放 "example" 字样 —— 验收要求新项目里 `rg -i "example|stress"` 零命中
			// (示例内容只属于默认项目)。名称先按 ValidateProjectName 校验。
			static std::string ManifestId(const std::string& name);

			struct Result
			{
				bool Ok = false;
				std::string Error;                   // Ok=false 时可读原因
				std::filesystem::path ProjectRoot;   // Ok=true = <位置>/<名称>(绝对)
				std::vector<std::string> Files;      // Ok=true = 写出的相对路径(generic,已排序)
				// PROJ-3/T1(P2b):项目根里的启动入口(相对文件名,已排序):
				// `WeEdit.exe`/`WePlay.exe`(从构建目录复制,优先)或 `open-editor.cmd`/`run-game.cmd`
				// (exe 缺失时的兜底)。两者都写不成 = 空(创建本身仍然成功)。
				std::vector<std::string> EntryPoints;
			};

			// 生成骨架。context 只用于构造 Scene 走引擎的场景序列化(写 Main.wd)。
			// includeStarterScene(PROJ-2/T1,调用方默认传 true):
			//   true  ⇒ Main.wd = 最小可运行场景:一台 Camera3D(Primary,位置 [0,1,5])+ 一盏
			//           方向光 —— 启动 Runtime 就有可看的画面;**不含**任何示例资产/材质/脚本
			//           (新项目里 `rg -i "example|stress"` 必须仍 0 命中);
			//   false ⇒ 保持旧行为(空场景,启动后是空画面)。
			// 目标目录已存在且为空时:改名阶段先移除那个空目录,再整体 rename 进来。
			static Result Create(const std::filesystem::path& location, const std::string& name,
				WorldContext& context, bool includeStarterScene);
		};
	}
}
