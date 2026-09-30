#pragma once

#include "World/Plugins/PluginManifest.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// PLUG-T5:插件打包(cook 侧)—— 随包闭包 + 引用完整性硬门 + 产物拷贝。
//
// 工业标准口径(方案 §7 / 派工单 PLUG-T5):
//   * **随包 = 项目清单显式启用 + `depends` 传递闭包**;项目插件默认随包(`ship: never` 可排除);
//     内容扫描**不用于自动塞包**,只做"引用了包外插件 ⇒ cook 失败"的硬门;
//   * 硬门需要"内容里的 id 属于哪个插件"的索引 —— 来源是每个插件自己清单里的
//     `contributes:`(静态声明,见 PluginContribution),因此 cook **不加载插件 DLL**
//     (打包是确定性步骤,不执行插件代码);
//   * 唯一例外:`plugins.tolerate_missing: [id]` ⇒ 该插件的引用缺件降级为 ERROR 日志 + 摘要计数
//     (绝不静默)。
//
// 事实源:`plugin.we.yaml`(`PluginManifest::Load`)+ `project.we.yaml` 的 `plugins:` 块
// (`World::Asset::ProjectManifest::Plugins`)。本文件只做只读扫描 + 拷贝,不改清单。

namespace World::Plugins
{
	// 发现期的一个插件包(清单 + 解析到的产物路径;不加载 DLL)。
	struct PluginPackage
	{
		PluginManifest Manifest;
		// 实际产物路径:包自带 `<Root>/bin/<目录名><扩展名>` 优先,否则按 devBinaryRoots
		// 里的同名产物解析(与 PluginManager::Discover 同一口径)。
		std::filesystem::path LibraryPath;
		bool LibraryFound = false;
	};

	// 发现两个插件根(不加载 DLL)。清单不合法 / 同根重复 id 的包记入 diagnostics 并跳过;
	// 跨根同 id = 项目插件覆盖引擎插件(方案 §1 优先级),同样记入 diagnostics。
	// 根不存在 = 0 个包(不是错误)。
	bool DiscoverPluginPackages(const std::filesystem::path& enginePluginsRoot,
		const std::filesystem::path& projectPluginsRoot,
		const std::vector<std::filesystem::path>& devBinaryRoots,
		std::vector<PluginPackage>* out, std::vector<std::string>* diagnostics);

	// 一次打包的输入(全部来自清单与内容根,不含任何运行时对象)。
	struct PluginPackRequest
	{
		std::filesystem::path EnginePluginsRoot;    // <引擎根>/plugins
		std::filesystem::path ProjectPluginsRoot;   // <项目根>/plugins
		std::vector<std::filesystem::path> DevBinaryRoots;   // 开发构建的插件产物根(可选)
		std::filesystem::path ContentRoot;          // 引用扫描根(不存在 = 0 命中)
		std::filesystem::path PublishDir;           // 产物落点;空 = 不拷贝
		std::vector<std::string> Enabled;           // project.we.yaml 的 plugins.enabled
		std::vector<std::string> TolerateMissing;   // project.we.yaml 的 plugins.tolerate_missing
		// false = 只算闭包 + 跑硬门,不拷产物(=`--check` 路径);
		// PublishDir 为空时同样不拷贝。
		bool CopyLibraries = true;
	};

	// 一次打包的结果。SummaryLine 是固定格式的 cook 摘要行(探针断言点):
	//   `[plugins] shipped=<id,id|none> skipped=<id,id|none> missing-references=<n>`
	struct PluginPackResult
	{
		bool Ok = false;
		std::string Error;                      // 首条失败原因(可读)
		std::vector<std::string> Shipped;       // 随包插件 id(依赖拓扑序)
		std::vector<std::string> Skipped;       // 发现但未随包的 id(含 ship: never)
		// "引用者 → 引用面 → 建议"三条诊断(硬门失败时逐条列出;被 tolerate 的降级项也在内)。
		std::vector<std::string> Diagnostics;
		// 发现期告警(重复 id / 覆盖 / 清单坏)。
		std::vector<std::string> Warnings;
		size_t MissingReferences = 0;           // 引用缺件条数(>0 时 Ok 仅当全部被 tolerate)
		size_t CopiedLibraries = 0;             // 实际拷进 <publish>/bin/plugins/ 的 DLL 数
		// 逐引用面的扫描命中数(0 = 该面没有可扫描的数据 —— 不是"跳过"的静默):
		//   ① 组件 ② 资产类型 ③ 导入器 ④ 脚本命名空间 ⑤ 渲染钩子(当前没有注册面 ⇒ 恒 0)
		size_t ComponentReferences = 0;
		size_t AssetTypeReferences = 0;
		size_t ImporterReferences = 0;
		size_t ScriptReferences = 0;
		size_t RenderHookReferences = 0;
		std::string SummaryLine;
	};

	// 完整打包:发现 → 闭包 → 引用硬门 → 拷贝 DLL → 摘要行(细节都写核心日志)。
	PluginPackResult PackagePlugins(const PluginPackRequest& request);
}
