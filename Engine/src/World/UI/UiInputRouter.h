#pragma once

// 游戏 UI 框架(GameUI)— 输入消费 / 焦点 / 手柄导航(工作包 M3)。
//
// 契约:`contract.ui-runtime` §5(硬):
//   * `UiInputRouter` 在 UI 帧消费输入;UI 消费过的指针,玩法同帧不再收到
//     —— 测试要能**双向**对照(命中可交互节点 = 消费,空白 = 不消费);
//   * 焦点顺序 = 绘制顺序(`UiScreen::Nodes()` 顺序);
//   * `Tab`/`Shift+Tab` + 方向键/手柄导航;文本输入是独立焦点域;
//   * 不可见(矩形在内容矩形外)/ 禁用 = 不可点。
//
// 边界(硬):不写 ECS、不发绘制命令、不改文档;只产出「消费标记 + 命令列表 + 焦点变化」。
// 命令出口由调用方提供(`UiCommandQueue`),router 只 append,不替宿主清帧。
//
// 命中唯一实现仍是 `UiScreen::HitTest`;本层在命中链上向上找**最近的交互祖先**
// (例:Button 里的 Label 被点到 ⇒ 目标是 Button),不另起一套命中。

#include "World/Core/Export.h"
#include "World/UI/UiNavigator.h"
#include "World/UI/UiScreen.h"
#include "World/WUI/WuiContext.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace World::UI
{
	// 一次 UI 命令/事件(节点 `On.{Event}` 的落点)。
	struct UiInputCommand
	{
		std::string NodeId;
		std::string Event;    // "Click" / ...
		std::string Command;  // 文档 `On.{Event}` 的命令;未声明 = 空
		// M13(仅追加,不动既有字段):事件携带的值。
		//   * Slider:Change/Commit = 当前数值(格式同 `UiPainter` 的 "%.3f");
		//   * TextField:Commit = 编辑后的文本(UTF-8);Change 未使用。
		std::string Value;
	};

	// 命令队列:宿主每帧清一次,router/绑定/脚本共用同一出口。
	class UiCommandQueue
	{
	public:
		void Clear() { m_Commands.clear(); }
		void Push(UiInputCommand command) { m_Commands.push_back(std::move(command)); }
		const std::vector<UiInputCommand>& Commands() const { return m_Commands; }
		std::size_t Size() const { return m_Commands.size(); }
		bool Empty() const { return m_Commands.empty(); }

	private:
		std::vector<UiInputCommand> m_Commands;
	};

	// 焦点域:导航焦点 vs 文本编辑焦点(文本域独立,方向键归文本编辑器)。
	enum class UiFocusDomain : uint8_t
	{
		Navigation = 0,
		Text = 1,
	};

	struct UiFocusChange
	{
		bool Changed = false;
		std::string From;
		std::string To;
	};

	// 一帧的路由结果。
	struct UiInputFrame
	{
		bool PointerConsumed = false;   // 指针被 UI 吃掉 ⇒ 玩法不得收到
		bool WheelConsumed = false;     // 滚轮落在滚动容器上
		bool KeyboardConsumed = false;  // Tab/方向键/文本输入/激活被 UI 吃掉
		bool ModalBarrier = false;      // 模态层吃掉了"模态外"的指针(不穿透)
		std::string HitId;              // 命中的可交互节点(不可点 = 空)
		std::string WheelTargetId;      // 吃了滚轮的节点
		UiFocusChange Focus;
		std::size_t CommandsEmitted = 0;
	};

	class WLD_API UiInputRouter
	{
	public:
		// ---- 命令出口 ----
		void SetCommandQueue(UiCommandQueue* queue) { m_Queue = queue; }
		UiCommandQueue* CommandQueue() const { return m_Queue; }

		// 模态/覆盖/调试层打开时,模态外的指针是否被吃掉(不穿透到玩法)。默认开。
		void SetModalBarrierEnabled(bool enabled) { m_ModalBarrier = enabled; }
		bool ModalBarrierEnabled() const { return m_ModalBarrier; }

		// ---- 入口 ----
		// 核心:返回本帧**是否消费了指针**(指针 = 鼠标/触摸)。
		bool Update(const UiScreen& screen, const Wui::WuiInputState& input);
		// M13:可写屏幕重载 —— 滚轮命中滚动容器时,落到 `UiScreen::SetScrollOffset`
		// (偏移是运行态;`SetScrollOffset` 是唯一钳位实现)。只读重载不落地滚动。
		bool Update(UiScreen& screen, const Wui::WuiInputState& input);
		// 便捷重载:临时指定命令队列(等同 SetCommandQueue + Update)。
		bool Update(const UiScreen& screen, const Wui::WuiInputState& input, UiCommandQueue& queue);
		bool Update(UiScreen& screen, const Wui::WuiInputState& input, UiCommandQueue& queue);
		// 导航感知:模态/覆盖/调试层打开时只路由最高层(下层页面不可点)。
		bool Update(const UiNavigator& navigator, const Wui::WuiInputState& input);

		const UiInputFrame& LastFrame() const { return m_Frame; }

		// ---- M13:跨帧交互状态(只读;状态由 router 持有,不每帧重建)----
		bool IsDragging() const { return m_Dragging; }
		const std::string& DraggingId() const { return m_DragNodeId; }
		bool IsEditing() const { return m_Editing; }
		const std::string& EditingNodeId() const { return m_EditingNodeId; }
		const std::string& EditingText() const { return m_EditingText; }
		// 宿主预置文本初值(如从绑定来的当前值)。优先于节点属性;编辑中调用立即生效。
		void SetEditingText(std::string_view nodeId, std::string text);

		// ---- 焦点(顺序 = 绘制顺序;Tab/Shift+Tab 环绕)----
		const std::string& FocusedId() const { return m_FocusedId; }
		UiFocusDomain FocusDomain() const { return m_Domain; }
		// 节点不存在/不可聚焦 = false,焦点不变。
		bool SetFocus(const UiScreen& screen, std::string_view nodeId);
		bool SetFocus(const UiScreen& screen, std::string_view nodeId, UiFocusDomain domain);
		void ClearFocus();
		// 可聚焦节点 id,按 `UiScreen::Nodes()` 顺序(即绘制顺序)。
		std::vector<std::string> FocusOrder(const UiScreen& screen) const;

		// ---- 判定(公开给宿主与测试;与路由同一口径)----
		static bool IsInteractiveType(const UiNodeInstance& node);
		static bool IsNodeEnabled(const UiNodeInstance& node);
		static bool IsNodeVisible(const UiNodeInstance& node, const UiViewport& viewport);
		static bool IsFocusable(const UiNodeInstance& node, const UiViewport& viewport);
		// 滚动容器:M3 的属性约定(`scrollable`/`scroll` 真值,或 `overflow: scroll|auto`)。
		static bool IsScrollContainer(const UiNodeInstance& node);

		static const char* ClickEventName() { return "Click"; }
		static const char* ChangeEventName() { return "Change"; }
		static const char* CommitEventName() { return "Commit"; }

	private:
		enum class NavDirection
		{
			Up,
			Down,
			Left,
			Right,
		};

		// scrollTarget = 可写屏幕(只用于滚轮偏移落地);只读路径传 nullptr。
		bool Route(const UiScreen& screen, const Wui::WuiInputState& input, UiScreen* scrollTarget);
		void HandleKeyboard(const UiScreen& screen, const Wui::WuiInputState& input);
		void HandleEditingKeyboard(const UiScreen& screen, const Wui::WuiInputState& input);
		const UiNodeInstance* ResolveTarget(const UiScreen& screen, const UiNodeInstance* hit) const;
		const UiNodeInstance* ResolveScrollContainer(const UiScreen& screen, const UiNodeInstance* hit) const;
		void EmitClick(const UiNodeInstance& node);
		void EmitNodeEvent(const UiNodeInstance& node, std::string_view event, std::string value);
		float SliderValueFromPointer(const UiNodeInstance& node, glm::vec2 designPoint) const;
		void BeginEditing(const UiNodeInstance& node);
		void CommitEditing(const UiScreen& screen);
		void CancelEditing();
		static bool IsSliderNode(const UiNodeInstance& node);
		static bool IsTextFieldNode(const UiNodeInstance& node);
		bool MoveFocusLinear(const UiScreen& screen, int delta);
		bool MoveFocusGeometric(const UiScreen& screen, NavDirection direction);
		void SetFocusedInternal(std::string nodeId, UiFocusDomain domain);
		void ValidateFocus(const UiScreen& screen);

		UiCommandQueue* m_Queue = nullptr;
		UiInputFrame m_Frame;
		std::string m_FocusedId;
		UiFocusDomain m_Domain = UiFocusDomain::Navigation;
		bool m_ModalBarrier = true;

		// M13:滑条拖拽 / 文本框编辑(跨帧;在 m_Frame 之外).
		bool m_Dragging = false;
		std::string m_DragNodeId;
		bool m_Editing = false;
		std::string m_EditingNodeId;
		std::string m_EditingText;
		std::string m_EditingOriginal;
		// 宿主经 `SetEditingText` 预置的初值(按稳定 Id);编辑开始时优先使用。
		std::unordered_map<std::string, std::string> m_TextPresets;
	};
}
