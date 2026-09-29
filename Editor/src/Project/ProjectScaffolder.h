#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	class WorldContext;

	namespace Editor
	{
		// PROJ-1/T1 + PROJ-7/T2:标准项目骨架生成器(纯逻辑 + 落盘;File ▸ New Project… 的内核)。
		//
		// 用户口径(2026-09-28):新建项目时**可选项目模板**,模板由引擎自带
		// (`<checkout>/templates/project-<id>/**`):一个干净的标准骨架(empty)+ 一个带
		// 示例内容的模板(example)。因此这里**只**复制所选模板目录,不复制任何具体项目:
		//   * 目录结构  ← 所选模板目录(逐文件复制,1:1;模板目录里不要放"仅维护者可见"的
		//                 说明文件 —— 它们会被复制进项目);
		//   * project.we.yaml ← 引擎 writer(ProjectManifest::Save,与解析同源,不手写 YAML);
		//   * 启动场景:模板声明了 defaultScene(相对内容根 assets/)⇒ 用模板自带的那份;
		//     模板没声明(empty)⇒ assets/scenes/Main.wd 走引擎场景序列化
		//     (includeStarterScene:相机 + 方向光 / 空场景);
		//   * .vscode / .luau-lsp(Luau LSP 脚手架)← 引擎既有 EnsureScriptEditorScaffold
		//     (与编辑器启动期为当前项目生成的那两份逐字节同源)。
		//
		// 落盘纪律:先写 <目标>.tmp-<随机>,全部成功后再整体 rename 到 <目标>;
		// 任一步失败 = 删掉临时目录,目标位置不留半成品;逐文件不覆盖既有文件。
		class ProjectScaffolder
		{
		public:
			// 模板库里的一个模板 = templates/project-<id>/template.json 的投影。
			// Valid=false 的条目仍然会出现在 ListTemplates() 里(向导照样列出来),选中它时
			// 用 Error 画可读的行内错误 —— 模板坏了不能"静默消失",否则用户/自动化只看到
			// "少了一个模板"。
			struct TemplateInfo
			{
				std::string Id;            // 目录名 project-<id> 的 <id>(稳定标识;向导 a11y id = project.new.template.<id>)
				std::string Name;          // 显示名(向导里画在分段按钮上)
				std::string Description;   // 一行说明(悬停提示 + 选中行说明)
				std::string DefaultScene;  // 模板自带的启动场景(相对内容根 assets/,例 "scenes/3DTest.wd");空 = 模板不声明
				bool HasSamples = false;   // 模板带示例内容(assets/scripts/examples/ 存在)⇒ 向导给"会带入示例…"提示
				int Order = 100;           // 排序键(小的在前);template.json 缺省 100
				bool Valid = true;         // false = template.json 缺失/非法(Error = 可读原因)
				std::string Error;         // Valid=false 时的可读原因
				std::filesystem::path Directory;   // 模板目录(绝对)
			};

			// 模板根 = <checkout>/templates(WLD_REPO_ROOT 锚定,与进程 CWD 无关)。
			static std::filesystem::path TemplateRoot();
			// 某个模板的目录 = <模板根>/project-<id>。id 只允许一段安全 ASCII 名字
			// (字母/数字/'.'/'_'/'-',首字符字母或数字);不合法时返回空路径(不做任何路径拼接)。
			static std::filesystem::path TemplateDirectory(const std::string& templateId);
			// 扫描模板库(只读,非递归):每个 templates/project-* 目录读一次 template.json,
			// 按 (order, id) 排序返回。目录在但 template.json 缺失/读不出来 ⇒ Valid=false。
			static std::vector<TemplateInfo> ListTemplates();

			// "新建位置"的默认值:优先用户文档目录(%USERPROFILE%/Documents),否则当前工作目录。
			static std::filesystem::path DefaultProjectLocation();

			// 项目名校验(目录名口径:名称就是 <位置>/<名称> 的那个目录名)。
			// 空 / 含 \ / : * ? " < > | / 控制字符 / 结尾的 . 或空格 / Windows 保留设备名
			// → 可读原因;空串 = 通过。
			static std::string ValidateProjectName(const std::string& name);
			// 落点校验(只读):位置必须是已存在的目录;目标目录必须不存在或为空。
			static std::string ValidateLocation(const std::filesystem::path& location, const std::string& name);
			// 按所选模板自检(只读):模板目录、template.json、id 与目录名一致、必需条目齐全、
			// 声明的 defaultScene 存在;空串 = 通过。传空 id(一个模板都没有)⇒ 报模板根缺失。
			// 向导的行内错误与 Create 的前置守卫共用这一条。
			static std::string ValidateTemplate(const std::string& templateId);

			// 项目清单 id = "com.<小写名称>"(小写 + 只保留 [a-z0-9._-],其余字符换成 '.';空段合并)。
			// 注意:不放 "example" 字样 —— 验收要求新项目里 `rg -i "example|stress"` 零命中
			// (示例内容只属于示例模板,不属于任何一个标准项目)。名称先按 ValidateProjectName 校验。
			static std::string ManifestId(const std::string& name);

			struct Result
			{
				bool Ok = false;
				std::string Error;                   // Ok=false 时可读原因
				std::filesystem::path ProjectRoot;   // Ok=true = <位置>/<名称>(绝对)
				std::vector<std::string> Files;      // Ok=true = 写出的相对路径(generic,已排序)
				// PROJ-3/T1(P2b):项目根里的启动入口(相对文件名,已排序):
				// PROJ-4/T1(P3)起名字跟项目名:`<项目名>-Edit.exe`/`<项目名>-Play.exe`
				// (从构建目录的 WeEdit/WePlay 复制,优先)或 `<项目名>-Edit.cmd`/`<项目名>-Play.cmd`
				// (exe 缺失时的兜底)。两者都写不成 = 空(创建本身仍然成功)。
				std::vector<std::string> EntryPoints;
			};

			// PROJ-11/T1:给**已经存在**的项目补齐"自带构建/启动入口"(用户 2026-09-29:
			// 「项目怎么编译这些 c++ 以及如何启动呢」)。只动项目根下的入口文件:
			//   * 缺 CMakeLists.txt  ⇒ 从 templates/project-<best>/CMakeLists.txt 复制;
			//   * 缺 build.cmd       ⇒ 从同一模板复制;
			//   * `<项目名>-Edit.exe` / `<项目名>-Play.exe` 用构建目录的 WeEdit/WePlay
			//     **刷新**(存在就覆盖 —— 这是本动作的明确语义);
			//   * 同时补/刷新 `<项目名>-Edit.cmd` / `<项目名>-Play.cmd`:内容记录生成时的
			//     引擎根(WLD_REPO_ROOT)。它既是 exe 缺失时的兜底,也是项目 build.cmd
			//     在没有 WE_ROOT 环境变量、项目又不在引擎树内时解析引擎根的唯一来源
			//     (模板 CMakeLists/build.cmd 的解析顺序见 templates/project-*/build.cmd)。
			//   * `.gitignore` 只补缺行。
			// **不覆盖**已存在的 CMakeLists.txt / build.cmd(记入 Skipped),**不碰**
			// src/** 与 assets/**。模板选择:项目根的 template.json(向导会复制)优先,
			// 否则按示例内容标记判断,最后回落 empty。
			struct BuildEntryResult
			{
				bool Ok = false;
				std::string Error;                   // Ok=false 时可读原因
				std::filesystem::path ProjectRoot;   // 解析后的绝对项目根
				std::string TemplateId;              // 实际采用的模板 id(empty/example)
				std::vector<std::string> Created;    // 新建的文件(相对项目根,已排序)
				std::vector<std::string> Skipped;    // 已存在、按要求未改写的文件
				std::vector<std::string> Refreshed;  // 被刷新/写入的启动器(相对项目根,已排序)
				std::vector<std::string> Warnings;   // 非致命失败(启动器/忽略文件写不成)
			};
			static BuildEntryResult EnsureBuildEntryPoints(const std::filesystem::path& projectRoot);

			// 生成骨架。context 只用于构造 Scene 走引擎的场景序列化(写 Main.wd)。
			// templateId(默认 "empty",PROJ-7/T2):templates/project-<id> 里的模板;不存在/
			//   缺 template.json/缺必需条目 ⇒ 可读错误,不写任何东西。
			// includeStarterScene(PROJ-2/T1,调用方默认传 true):
			//   只对**没有 defaultScene 的模板**(empty)生效:
			//     true  ⇒ Main.wd = 最小可运行场景:一台 Camera3D(Primary,位置 [0,1,5])+ 一盏
			//             方向光 —— 启动 Runtime 就有可看的画面;**不含**任何示例资产/材质/脚本;
			//     false ⇒ 旧行为(空场景,启动后是空画面)。
			//   模板声明了 defaultScene(example)时:清单 start_scene 用模板自带的场景,
			//   **不生成** Main.wd,这个参数被忽略(不产生用不到的第 5 个场景)。
			// 目标目录已存在且为空时:改名阶段先移除那个空目录,再整体 rename 进来。
			static Result Create(const std::filesystem::path& location, const std::string& name,
				WorldContext& context, bool includeStarterScene, const std::string& templateId = "empty");
		};
	}
}
