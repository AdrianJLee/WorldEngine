#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	namespace Editor
	{
		// PLUG-AUTH-1:插件骨架生成器(纯逻辑 + 落盘;File ▸ New Plugin… / 插件管理器
		// 「新建插件…」的内核)。
		//
		// 与 ProjectScaffolder 同一套口径:模板来自引擎自带
		// `<checkout>/templates/plugin-<id>/**`(逐文件复制 + 4 个占位符的字面替换),
		// 先写同卷临时目录、成功后整体改名;目标已存在 = 失败,任何失败都不留半成品。
		//
		// 与项目脚手架不同的一点:插件必须能被**发现期**接受,所以生成 plugin.we.yaml 后
		// 立刻用 World::Plugins::PluginManifest::Load 自校验 —— 校验不过就回滚临时目录。
		class PluginScaffolder
		{
		public:
			// 插件落点:引擎插件 = <引擎根>/plugins;项目插件 = <项目根>/plugins。
			// 位置即 scope(见 PluginManifest.h),清单里不写 scope: 字段。
			enum class PluginTarget
			{
				Engine = 0,
				Project,
			};

			// 模板库里的一个模板 = templates/plugin-<id>/template.json 的投影。
			// Valid=false 的条目仍然会出现在 ListTemplates() 里(向导照样列出来),
			// 选中它时用 Error 画可读的行内错误 —— 模板坏了不能"静默消失"。
			struct PluginTemplateInfo
			{
				std::string Id;            // 目录名 plugin-<id> 的 <id>(向导 a11y id = plugin.new.template.<id>)
				std::string Name;          // 显示名(向导里画在分段按钮上)
				std::string Description;   // 一行说明(悬停提示 + 选中行说明)
				std::string Requires;      // none | t2b | t3b | t4(template.json 声明;向导据此提示注册面未就绪)
				int Order = 100;           // 排序键(小的在前);template.json 缺省 100
				bool EngineOk = true;      // scopes 里是否允许 "engine"
				bool ProjectOk = true;     // scopes 里是否允许 "project"
				bool Valid = true;         // false = template.json 缺失/非法,或缺必需文件
				std::string Error;         // Valid=false 时的可读原因
				std::filesystem::path Directory;   // 模板目录(绝对)
			};

			struct PluginScaffoldRequest
			{
				PluginTarget Target = PluginTarget::Engine;
				std::string TemplateId;
				std::string Name;          // 目录名 = 插件短名
				std::string PluginId;      // 反域名形态(清单里的 id)
			};

			struct PluginScaffoldResult
			{
				bool Ok = false;
				std::string Error;                    // Ok=false 时可读原因
				std::filesystem::path PluginRoot;     // Ok=true = <pluginsRoot>/<name>(绝对)
				std::vector<std::string> Files;       // Ok=true = 写出的相对路径(generic,已排序)
			};

			// 模板根 = <checkout>/templates(与 ProjectScaffolder::TemplateRoot 同一条口径)。
			static std::filesystem::path TemplateRoot();
			// 某个模板的目录 = <模板根>/plugin-<id>;id 不合法时返回空路径(不做路径拼接)。
			static std::filesystem::path TemplateDirectory(const std::string& templateId);
			// 扫描模板库(只读,非递归):每个 templates/plugin-* 目录读一次 template.json,
			// 按 (order, id) 排序返回。template.json 缺失/非法或缺必需文件 ⇒ Valid=false。
			static std::vector<PluginTemplateInfo> ListTemplates();

			// 目标名("engine"/"project";向导 a11y value / 清单注释用)。
			static const char* TargetName(PluginTarget target);

			// 插件短名(目录名)校验:非空/无非法字符/非保留设备名/长度 ≤ 64 → 空串 = 通过。
			static std::string ValidatePluginName(const std::string& name);
			// 插件 ID 校验:反域名形态(小写字母/数字/'-'/'.';至少两段;每段以字母或数字
			// 开头结尾)—— 空串 = 通过。
			static std::string ValidatePluginId(const std::string& id);
			// 落点校验(只读):目标目录不存在,且 pluginsRoot 下没有任何已有插件的
			// `plugin.we.yaml` 声明了同一个 id(不覆盖、不撞 id)。空串 = 通过。
			static std::string ValidateTarget(const std::filesystem::path& pluginsRoot,
				const std::string& name, const std::string& pluginId);
			// 模板校验(只读):模板目录 / template.json / 必需文件 / 目标 scope 都在。
			static std::string ValidateTemplate(const std::string& templateId, PluginTarget target);

			// 生成骨架:校验 → 写同卷临时目录(逐文件复制 + 占位符字面替换)→
			// PluginManifest::Load 自校验 → 整体改名为 <pluginsRoot>/<name>。
			// 目标已存在 = 失败;任何失败都不留半成品。
			static PluginScaffoldResult Scaffold(const std::filesystem::path& pluginsRoot,
				const PluginScaffoldRequest& request);
		};
	}
}
