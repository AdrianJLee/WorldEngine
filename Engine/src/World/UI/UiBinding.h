#pragma once

// 游戏 UI 框架(GameUI)— 数据绑定:源协议解析 + 注册制解析器 + 扁平绑定表(工作包 M6)。
//
// 契约:`contract.ui-runtime` §6(绑定)。
// 边界(硬,依据 `invariant.ui-presentation-only`):
//   * **只读**:绑定不写 ECS 结构、不改 `.wui` 文档、不发命令;UI → 逻辑一律走节点 `On.Command`。
//   * **解耦**:本层不 include `entt`/`Scene`/脚本运行时;`ecs:`/`script:`/`service:` 的求值
//     一律走调用方注册的解析器(`RegisterResolver`),引擎只负责"解析 + 调度 + 变更检测"。
//   * **变更检测**:调用方每帧给一个版本号(tick / 组件版本);版本不变 = 不重复求值。
//
// 求值结果是**属性覆盖值文本**(编码 = 属性文本协议),由调用方在绘制前覆盖进节点属性。

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace World::UI
{
	class WLD_API UiScreen;   // 只按引用使用;不把 UiDocument / 布局拉进本头

	// 源协议(字符串前缀即协议,与 `contract.ui-runtime` §6 一致)。
	enum class UiBindingScheme : uint8_t
	{
		Const = 0,   // const:<literal>
		Ecs,         // ecs:<Entity>/<Component>/<Field>(2 段 = <Component>/<Field>,查询首选实体)
		Script,      // script:<path>.<field>
		Service,     // service:<name>
		Unknown,     // 未登记的协议前缀(`Parse` 拒绝)
	};

	WLD_API const char* UiBindingSchemeName(UiBindingScheme scheme);
	WLD_API bool ParseUiBindingScheme(std::string_view name, UiBindingScheme& out);

	// 一条已解析的源。`Text` = 协议后的原文(不改写);`Parts` = 按协议语义切分的段:
	//   Const   → [literal]
	//   Ecs     → [entity, component, field] 或 [component, field]
	//   Script  → [path, field](按最后一个 '.' 切分,路径可含 '.')
	//   Service → [name]
	struct UiBindingSource
	{
		UiBindingScheme Scheme = UiBindingScheme::Unknown;
		std::string Text;
		std::vector<std::string> Parts;
	};

	// 解析器 = 按协议求值:源 → 覆盖值文本。返回 false + 可读 error = 本条不可求值
	// (调用方记一条 warning;不影响绘制、不影响其它绑定)。
	// `context` 由 `UiBindingTable::Refresh` 原样透传(ECS world / 脚本环境 / 服务注册表)。
	using UiBindingResolverFn = bool (*)(const UiBindingSource& source, void* context,
		std::string& out, std::string* error);

	// 扁平绑定表里的一条:节点稳定 Id + 目标属性 + 已解析源。
	struct UiBinding
	{
		std::string NodeId;
		std::string Target;
		UiBindingSource Source;

		// **只解析,不求值**。成功 = true;失败(空串 / 未知协议 / 协议后为空 / 段非法)
		// 写可读 error;`out` 仅在成功时被改写。
		static bool Parse(std::string_view source, UiBindingSource& out, std::string* error = nullptr);

		// 解析器注册表(注册制;`scheme` = 协议名,如 "ecs")。空名 / 空函数 = false;
		// 同名重复注册 = 覆盖(内置 "const" 可被调用方替换)。进程内共享,与表实例无关。
		static bool RegisterResolver(std::string_view scheme, UiBindingResolverFn fn);
		static UiBindingResolverFn FindResolver(std::string_view scheme);
	};

	// 求值输入:版本号驱动变更检测;`Context` 透传给解析器。
	struct UiBindingDataSource
	{
		uint64_t Version = 0;
		void* Context = nullptr;
	};

	struct UiBindingWarning
	{
		std::string NodeId;
		std::string Target;
		std::string Source;    // 原文(供 AI / 日志定位)
		std::string Message;
	};

	// 扁平绑定表:一个 `UiScreen` 的全部 `Bind:` 条目 + 按版本的求值缓存。
	class WLD_API UiBindingTable
	{
	public:
		// 把 `screen` 内所有节点的 `Bind:` 编成扁平表(文档遍历序 = 稳定顺序)。
		// 目标为空 / 源解析失败 → 记一条 warning 且该条不入表(不静默、不猜)。
		// 重复调用 = 从头重建(先清空)。
		void Attach(const UiScreen& screen);

		// 按变更检测求值:Version 与上次相同 → 不求值、返回 0(解析器零调用);
		// Version 变化 → 重算全部条目,返回实际求值条数。
		// 解析器缺失 / 求值失败 → 一条 warning,该条**保留上一值**(不影响绘制)。
		std::size_t Refresh(const UiBindingDataSource& dataSource);

		// 查询求值结果;无该条 / 尚未求值 → false(不返回伪默认值)。
		bool Value(std::string_view nodeId, std::string_view target, std::string& out) const;
		bool HasValue(std::string_view nodeId, std::string_view target) const;

		std::size_t Count() const { return m_Entries.size(); }
		const std::vector<UiBinding>& Entries() const { return m_Entries; }
		const std::vector<UiBindingWarning>& Warnings() const { return m_Warnings; }

		// 清空条目 / 缓存值 / 警告 / 版本记录(下次 Refresh 视为首次)。
		void Reset();

	private:
		std::vector<UiBinding> m_Entries;
		std::vector<std::string> m_Values;    // 与 m_Entries 同序
		std::vector<char> m_Resolved;         // 与 m_Entries 同序(0/1)
		std::vector<UiBindingWarning> m_Warnings;
		uint64_t m_Version = 0;
		bool m_HasVersion = false;
	};
}
