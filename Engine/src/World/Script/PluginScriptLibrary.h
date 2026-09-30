#pragma once

// ============================================================================
// T4:插件脚本函数库的**唯一账本**。
//
// 为什么要有这一层:
//   * 插件经 WeHostApi::RegisterScriptFunction 注册的是"命名空间.函数名"形式的全局 Luau
//     函数;运行时绑定(VM 全局表)与存根渲染(WorldEngineAPI.luau)必须消费**同一份**
//     描述,否则"脚本能调的函数"和"存根宣告的函数"会漂移(与 BindServices.h 的
//     "唯一描述表"同一口径);
//   * 存根渲染顺序必须是**确定性**的:插件块按命名空间升序,同一命名空间内函数按
//     (插件 id, 函数名)升序 —— 与插件装载顺序无关;被禁用/被拒绝的插件不入账本,
//     因此不参与渲染;零插件时渲染输出与入库夹具逐字节一致。
//
// 生命周期:
//   * Register = 校验(名字形态 / 保留命名空间 / 签名格式 / 重名)+ 记账 +
//     (ScriptEngine 已初始化时)立即绑定;绑定失败 = 干净拒绝(不入账本);
//   * Unregister = 幂等;解绑成员、命名空间清空后移除全局表;归属别的插件 = false;
//   * UnregisterAll = 卸载 / Register 返回 false / Register 抛异常 / 管理器析构的兜底回收;
//   * OnVmShutdown 由 ScriptEngine 调用:只清"已绑定"状态,账本保留(下一次 Init 重建)。
// ============================================================================

#include "World/Core/Export.h"
#include "World/Plugins/WePluginApi.h"

#include <cstddef>
#include <string>
#include <vector>

namespace World
{
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	// 一条已注册插件函数的可观测快照(插件管理器 / 诊断 / 单测)。
	struct PluginScriptFunctionInfo
	{
		std::string PluginId;   // 归属插件(清单 id)
		std::string Name;       // 完整点分名("hello.ping")
		std::string Namespace;  // 全局表名("hello")
		std::string Member;     // 表里的函数名("ping")
		std::string Signature;  // 注册时的存根签名原文(可空)
		std::string Doc;        // 单行说明(可空)
	};

	class WLD_API PluginScriptLibrary
	{
	public:
		// 注册(宿主侧入口;PluginManager 经 WeHostApi 桥调用)。
		// 失败 = false + 可读 error,不留半注册;重复 Name(本插件内或跨插件)= 拒绝。
		static bool Register(const std::string& pluginId,
			const Plugins::WeScriptFunctionDesc& desc, std::string* error = nullptr);

		// 注销(幂等):本插件没注册过的名字 → true;名字归别的插件 → false + error。
		static bool Unregister(const std::string& pluginId, const std::string& name,
			std::string* error = nullptr);
		// 兜底回收一个插件的全部函数(卸载 / 回滚 / 析构);返回移除条数。
		static std::size_t UnregisterAll(const std::string& pluginId);

		static std::size_t Count();
		static bool IsRegistered(const std::string& pluginId, const std::string& name);
		// 名字的归属插件 id;未注册 = 空串。
		static std::string OwnerOf(const std::string& name);
		// 一条快照;未命中 = false(不改 *out)。
		static bool Describe(const std::string& pluginId, const std::string& name,
			PluginScriptFunctionInfo* out);
		// 全部条目快照(按 (pluginId, name) 升序;面板 / 诊断 / 单测)。
		static std::vector<PluginScriptFunctionInfo> Snapshot();

		// 存根渲染输入:按命名空间分组的只读描述(内部存储;返回的指针有效期 =
		// 下一次 Register/Unregister/UnregisterAll 之前)。
		static const std::vector<const ScriptServiceBinding*>& StubTables();

		// ---- VM 生命周期钩子(ScriptEngine 调用)----
		// 把当前账本里的全部函数绑进给定绑定上下文(Init / 重建时;**确定性绑定顺序**)。
		// 单条与引擎内建全局冲突 = 记 ERROR 并跳过该条(不让引擎启动失败;真正的拒绝
		// 发生在注册期 —— 只有"VM 之前注册"的边角情况会走到这里)。
		static bool BindAll(ScriptBindingContext& bindings, std::string* error = nullptr);
		// VM 关闭:账本保留,清绑定状态(下次 Init 重新绑定)。
		static void OnVmShutdown();
	};
}
