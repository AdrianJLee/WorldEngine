#pragma once

// ============================================================================
// WorldEngine 插件 ABI(v1)—— 公共接口的唯一事实源。
//
// 纪律(与 World/Modules/WeModule.h 同一套,但**版本号独立**):
//   * 只含 C 类型与函数指针:不跨边界传 STL / 原始 C++ 对象;
//   * 每个结构以 `StructSize + AbiVersion` 开头,宿主与插件**双向**校验;
//   * **字段只增不改号**:尾部追加字段 = StructSize 增长(不升 AbiVersion);
//     语义变更 = 升 WE_PLUGIN_ABI_VERSION(旧插件被干净拒绝,不按新布局解释)。
//     ⇒ 判据是"**声明的大小覆盖对方要读的前缀**":宿主接受 `plugin.StructSize >= 已知 v1 前缀`
//       (插件用更新的头编译、尾部字段更多时合法);小于该前缀 = 干净拒绝。
//       宿主侧将来追加字段后,读新字段前必须逐项检查 `plugin.StructSize` 是否覆盖该字段。
//
// 与 Game 模块的关系:`WE_MODULE_ABI_VERSION`(Game 模块契约)保持独立演进;
// 插件加载器复用 ModuleManager 底层的 DynamicLibrary 与"等值门"思路,不复制第二套实现。
//
// 版本同步点(升级时必须一起改):本文件、`plugin.we.yaml` 的 `abi`、
// `Engine/src/World/Plugins/PluginManager.*`、`docs/dev/plugin-framework.md`。
// 方案:`tools/agents/tasks/20260930-1100-plugin-framework/plan.md`(v2.1)。
// ============================================================================

#include <cstdint>

namespace World
{
	class WorldContext;
}

namespace World::Plugins
{
	// 插件 ABI 版本(独立于 Game 模块的 WE_MODULE_ABI_VERSION)。
	constexpr uint32_t WE_PLUGIN_ABI_VERSION = 1;

	// 导出入口符号名(清单 `entry:` 可覆盖;默认即此名)。
	constexpr const char* WE_PLUGIN_QUERY_SYMBOL = "WePluginQuery";

	// 宿主日志等级(WeHostApi::Log 的 level 取值)。
	enum WePluginLogLevel : int
	{
		WePluginLogInfo = 0,
		WePluginLogWarn = 1,
		WePluginLogError = 2,
	};

	// 插件导出的 C++ 函数库条目(`provides: [cxx.exports]` 的载体)。
	// Function 的解释由调用方与被调用方按 Name+Version 约定,宿主不解析。
	struct WePluginExport
	{
		const char* Name = nullptr;
		uint32_t Version = 0;
		void* Function = nullptr;
	};

	// 宿主能力表(函数指针表;T1 只含最小集合,注册面在 T2 逐段**尾部追加**)。
	struct WeHostApi
	{
		uint32_t StructSize = sizeof(WeHostApi);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;
		// 宿主私有指针(插件只应原样回传给宿主提供的函数)。
		void* UserData = nullptr;
		// 日志(宿主负责加插件身份前缀与落盘;message 为 UTF-8,调用方不保留所有权)。
		void (*Log)(void* userData, int level, const char* message) = nullptr;
	};

	// 插件描述 + 生命周期回调 + 导出表。
	struct WePlugin
	{
		uint32_t StructSize = sizeof(WePlugin);
		uint32_t AbiVersion = WE_PLUGIN_ABI_VERSION;

		// 身份(必须与清单 `id` 一致;宿主以清单为准,不一致 = 拒绝加载)。
		const char* Id = nullptr;
		const char* Name = nullptr;
		const char* Version = nullptr;          // "1.0.0"(semver,仅诊断/依赖用)
		const char* Publisher = nullptr;        // 预留(不强制)
		const char* MinEngineVersion = nullptr; // 预留(与清单 `engine:` 比对)

		// 能力 id 列表(null 结尾的字符串数组;与清单 `provides:` 比对,缺声明 = 警告)。
		const char* const* Provides = nullptr;
		uint32_t ProvidesCount = 0;

		// C++ 函数库导出表(可为空 = 纯行为插件)。
		const WePluginExport* Exports = nullptr;
		uint32_t ExportCount = 0;

		// 生命周期契约:
		//   * Register 返回 true = 注册成功;返回 false = 插件**必须已自行清理**本次注册的
		//     任何部分状态(契约:false 之后宿主**不会**再调 Unregister,防二次释放);
		//   * Register 不得让异常跨过 ABI 边界(抛异常 = 契约违约:宿主按 best-effort 调
		//     Unregister 回滚后再拒绝该插件);
		//   * Unregister 必须销毁插件拥有的一切对象/回调/注册项,且与 Register 成功次数一一对应。
		bool (*Register)(WorldContext& context, const WeHostApi& host) = nullptr;
		void (*Unregister)(WorldContext& context) = nullptr;
	};

	// 入口签名:`WePluginQuery(hostAbiVersion)`。
	// 返回 nullptr = 宿主 ABI 不受支持(插件必须显式拒绝,而不是按旧布局解释)。
	using WePluginQueryFn = const WePlugin* (*)(uint32_t hostAbiVersion);
}
