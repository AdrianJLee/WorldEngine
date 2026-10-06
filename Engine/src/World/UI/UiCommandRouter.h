#pragma once

// 游戏 UI 框架(GameUI)— UI 命令出口:命令队列 → 事件总线(工作包 M26)。
//
// 边界(硬,依据 `invariant.ui-presentation-only`):UI → 逻辑的**唯一出口是命令/事件**。
// 本层把 `UiInputRouter` 产出的每条命令(`节点 On.{Event}` 的落点)包成 `UiCommandEvent`,
// 用 `EventBus::EmitDeferred` 入队 —— 项目 Lua/C++ 系统订阅它即可响应;由会话在帧末
// `EventBus::DispatchPending` 统一派发(与既有事件同一条路径)。引擎**不发明玩法语义**。
//
// 内置只做**导航类**:`ui.close`(有模态先关模态,否则出栈)/ `ui.back`(同 `UiNavigator::Back`)。
// 只有调用方给了 `UiNavigator*` 才落地导航;引擎当前两个宿主都没有 `UiNavigator`
// ⇒ 命令只进总线(`Dispatch` 的 `navigator` 传空,报告里已记录)。
//
// 载荷口径:`EventBus::EmitDeferred` 按 `sizeof(T)` 字节复制(见 `EventBus.h`)⇒ `UiCommandEvent`
// 必须是**平凡可拷贝的定长结构**;节点 / 事件 / 命令超长按字节截断(容量见下面常量)。
// 注意:`UiInputCommand::Value`(M13 滑条/文本域的值)**不在**本事件载荷内(派工单口径
// 为 `{NodeId, Event, Command}`);需要值的项目应在后续工作包里扩展载荷。

#include "World/Core/Export.h"
#include "World/UI/UiInputRouter.h"
#include "World/UI/UiNavigator.h"

#include <cstddef>
#include <type_traits>

namespace World::Gameplay
{
	class EventBus;
}

namespace World::UI
{
	inline constexpr std::size_t kUiCommandEventNodeIdCapacity = 64;
	inline constexpr std::size_t kUiCommandEventEventCapacity = 32;
	inline constexpr std::size_t kUiCommandEventCommandCapacity = 128;

	// 一条 UI 命令的总线载荷(值盒装;`EventBus::Subscribe<UiCommandEvent>` 反序列化同形值)。
	struct UiCommandEvent
	{
		char NodeId[kUiCommandEventNodeIdCapacity] {};
		char Event[kUiCommandEventEventCapacity] {};
		char Command[kUiCommandEventCommandCapacity] {};
	};

	static_assert(std::is_trivially_copyable_v<UiCommandEvent>,
		"UiCommandEvent is copied byte-wise by EventBus::EmitDeferred");

	// 跨模块载荷(WorldRuntime.dll → Game.dll 的订阅者)⇒ 总尺寸 + 逐字段 offsetof 双断言
	// (docs/dev/performance-and-data-layout.md §4.3 门禁形态;现有 `PhysicsEvents.h` 的
	// EventBus 载荷尚未补,这里按新结构先落地)。
	static_assert(sizeof(UiCommandEvent) ==
		kUiCommandEventNodeIdCapacity + kUiCommandEventEventCapacity + kUiCommandEventCommandCapacity,
		"UiCommandEvent size drift");
	static_assert(offsetof(UiCommandEvent, NodeId) == 0, "UiCommandEvent.NodeId offset drift");
	static_assert(offsetof(UiCommandEvent, Event) == kUiCommandEventNodeIdCapacity,
		"UiCommandEvent.Event offset drift");
	static_assert(offsetof(UiCommandEvent, Command) ==
		kUiCommandEventNodeIdCapacity + kUiCommandEventEventCapacity,
		"UiCommandEvent.Command offset drift");

	struct UiCommandDispatchResult
	{
		std::size_t Dispatched = 0;    // 入队的总线事件数(每条命令一条,含内置导航命令)
		std::size_t Navigations = 0;   // 被内置导航额外消费的命令数(ui.close / ui.back)
	};

	class WLD_API UiCommandRouter
	{
	public:
		// 把 `queue` 里每条命令 `EmitDeferred` 到 `events`;`navigator` 非空时,内置导航命令
		// 额外落地。不修改 `queue`(清帧仍由宿主 `UiCommandQueue::Clear` 负责)。
		static UiCommandDispatchResult Dispatch(const UiCommandQueue& queue, Gameplay::EventBus& events,
			UiNavigator* navigator = nullptr);
	};
}
