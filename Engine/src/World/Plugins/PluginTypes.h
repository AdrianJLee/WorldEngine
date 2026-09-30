#pragma once

#include <cstdint>

namespace World::Plugins
{
	// 位置即 scope(方案 §1):两类插件机制同一套,只有"住哪/谁编/给谁用/怎么打包"不同。
	enum class PluginScope : uint8_t
	{
		Engine = 0,   // <引擎根>/plugins/**
		Project = 1,  // <项目根>/plugins/**
	};

	// 随包策略(方案 §7;T1 只解析 + 校验,打包闭包在 T5 落地)。
	enum class PluginShipPolicy : uint8_t
	{
		Auto = 0,    // 引擎插件默认:被引用才随包
		Always = 1,  // 项目插件默认
		Never = 2,   // 显式排除
	};

	// 单个插件的可观测状态(插件管理器面板/诊断/单测用;运行句柄不出现在这里)。
	enum class PluginState : uint8_t
	{
		Discovered = 0,  // 已发现,未加载
		Loaded = 1,      // 已 Register 成功
		Rejected = 2,    // 清单/契约校验或加载失败(诊断见 PluginEntry::Diagnostic)
		Unloaded = 3,    // 曾加载,已按契约 Unregister 并释放 DLL
	};

	inline const char* PluginScopeName(PluginScope scope)
	{
		switch (scope)
		{
			case PluginScope::Engine: return "engine";
			case PluginScope::Project: return "project";
		}
		return "unknown";
	}

	inline const char* PluginShipPolicyName(PluginShipPolicy ship)
	{
		switch (ship)
		{
			case PluginShipPolicy::Auto: return "auto";
			case PluginShipPolicy::Always: return "always";
			case PluginShipPolicy::Never: return "never";
		}
		return "unknown";
	}

	inline const char* PluginStateName(PluginState state)
	{
		switch (state)
		{
			case PluginState::Discovered: return "discovered";
			case PluginState::Loaded: return "loaded";
			case PluginState::Rejected: return "rejected";
			case PluginState::Unloaded: return "unloaded";
		}
		return "unknown";
	}

	// PLUG-T5:插件在**打包期**对宿主声明的"提供面"(`plugin.we.yaml` 的 `contributes:`)。
	//
	// 为什么需要静态声明:cook 要在**不加载插件 DLL** 的前提下回答"内容里的这个 id 是哪个
	// 插件提供的"(引用完整性硬门的唯一索引来源)。声明与运行时注册的实际 id 由
	// PluginManager::LoadRecord 在加载成功后比对,不一致只记 WARN(不阻断加载),
	// 但会让打包门禁漏报 —— 所以插件作者必须让两面保持一致。
	enum class PluginContributionFace : uint8_t
	{
		Component = 0,        // `.wd` 场景里的组件类型 id(WeComponentDesc::Id)
		AssetType = 1,        // 资产类型 id(WeAssetTypeDesc::Id;内容里以扩展名出现)
		Importer = 2,         // 导入器 id(WeAssetImporterDesc::Id;内容里以源扩展名出现)
		ScriptNamespace = 3,  // 插件脚本函数命名空间(WeScriptFunctionDesc::Name 的第一段)
	};

	inline const char* PluginContributionFaceName(PluginContributionFace face)
	{
		switch (face)
		{
			case PluginContributionFace::Component: return "component";
			case PluginContributionFace::AssetType: return "asset.type";
			case PluginContributionFace::Importer: return "asset.importer";
			case PluginContributionFace::ScriptNamespace: return "script.namespace";
		}
		return "unknown";
	}
}
