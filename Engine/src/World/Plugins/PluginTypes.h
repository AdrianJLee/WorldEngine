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
}
