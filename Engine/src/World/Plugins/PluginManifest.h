#pragma once

#include "World/Plugins/PluginTypes.h"
#include "World/Plugins/WePluginApi.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace World::Plugins
{
	// 插件产物扩展名(单一定义:清单解析与"开发构建根"查找共用,见 PluginManager::Discover)。
#if defined(_WIN32)
	constexpr const char* kPluginLibraryExtension = ".dll";
#else
	constexpr const char* kPluginLibraryExtension = ".so";
#endif

	// PLUG-T5:`contributes:` 的一条贡献声明(打包期引用索引的一条记录)。
	//
	// Extensions 只在 AssetType / Importer 两个面上有意义:内容里的引用以**文件扩展名**
	// 形式出现(如 `.whello`),cook 据此把内容文件映射回插件。规范化口径:小写、带前导点;
	// 声明时可省略前导点(解析期补上)。Components / ScriptNamespace 面不允许写扩展名。
	struct PluginContribution
	{
		PluginContributionFace Face = PluginContributionFace::Component;
		std::string Id;
		std::vector<std::string> Extensions;
	};

	// `plugin.we.yaml` 的解析结果 + 由位置推导出的定位信息(方案 §4.1)。
	//
	// 契约要点(与 tools/agents/tasks/20260930-1100-plugin-framework/plan.md v2.1 同步):
	//   * id 必需且非空;其余字段可缺省(缺省值见字段注释);
	//   * abi 缺省 = WE_PLUGIN_ABI_VERSION,写了就必须**等值**(ABI 演进 = 干净拒绝);
	//   * engine 缺省 = 不限制;当前只接受 ">=X.Y" 形态(最低引擎版本);
	//   * scope 由**位置**推导;清单若写了 `scope:` 且与位置不符 = 拒绝加载;
	//   * ship 只接受 auto|always|never(语义在 T5 落地)。
	struct PluginManifest
	{
		std::string Id;
		std::string Name;              // 缺省 = id
		std::string Version = "1.0.0"; // 仅诊断/依赖用(semver)
		std::string Publisher;         // 预留(不强制)
		std::string Signature;         // 预留(不强制)

		uint32_t Abi = WE_PLUGIN_ABI_VERSION;
		std::string Entry = WE_PLUGIN_QUERY_SYMBOL;

		// 最低引擎版本约束(空 = 不限制)。T1 只做形态校验与记录;与宿主版本比较依赖
		// "引擎版本号"的单一事实源(见 PLUG-T1 报告的遗留项)。
		std::string Engine;
		uint32_t EngineMinMajor = 0;
		uint32_t EngineMinMinor = 0;

		std::vector<std::string> Depends;    // 加载顺序 + 缺依赖拒绝
		std::vector<std::string> Provides;   // 能力声明(与 WePlugin::Provides 比对,差异 = 警告)
		std::vector<std::string> Overrides;  // 预留:显式覆盖低优先提供者(T3/T5 用)
		// PLUG-T5:打包期引用索引(见 PluginContribution 的说明);缺省 = 空。
		std::vector<PluginContribution> Contributions;
		PluginShipPolicy Ship = PluginShipPolicy::Auto;

		// 位置即 scope:Load() 时按所在根写入;清单里写了 scope: 且与位置不符 = 拒绝。
		PluginScope Scope = PluginScope::Engine;
		bool HasScopeField = false;

		// 发现期填充的定位信息(方案 §4.1:<root>/<name>/plugin.we.yaml + bin/<name>.dll)。
		std::filesystem::path Root;          // <...>/plugins/<name>
		std::filesystem::path ManifestPath;  // <Root>/plugin.we.yaml
		std::filesystem::path LibraryPath;   // <Root>/bin/<目录名><平台扩展名>

		// 解析 + 校验 manifestPath;locationScope = 插件包所在根推出的 scope。
		// 失败返回 false 并给可读 error(带清单位置);已解析出的字段会尽量保留在 out
		// 里(上层诊断仍能显示 id)。
		static bool Load(const std::filesystem::path& manifestPath, PluginScope locationScope,
			PluginManifest* out, std::string* error);
	};
}
