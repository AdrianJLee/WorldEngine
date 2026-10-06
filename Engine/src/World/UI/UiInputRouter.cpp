#include "wldpch.h"
#include "World/UI/UiInputRouter.h"

#include "World/Core/KeyCodes.h"
#include "World/UI/UiNodeRegistry.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace World::UI
{
	namespace
	{
		const std::string* FindProp(const UiNodeInstance& node, std::string_view name)
		{
			if (node.Source == nullptr)
				return nullptr;
			const UiProp* prop = node.Source->FindProp(name);
			return prop != nullptr ? &prop->Value : nullptr;
		}

		// 与 `UiPainter` 同一值编码协议(true/1/yes/on 为真)。
		bool BoolProp(const UiNodeInstance& node, std::string_view name, bool fallback)
		{
			const std::string* value = FindProp(node, name);
			if (value == nullptr || value->empty())
				return fallback;
			return *value == "1" || *value == "true" || *value == "True" ||
				*value == "yes" || *value == "on";
		}

		bool WasTriggered(const Wui::WuiInputState& input, uint32_t key)
		{
			return std::find(input.KeyPressed.begin(), input.KeyPressed.end(), key) != input.KeyPressed.end() ||
				std::find(input.KeyRepeated.begin(), input.KeyRepeated.end(), key) != input.KeyRepeated.end();
		}

		bool HasPointerEvent(const Wui::WuiInputState& input)
		{
			for (int button = 0; button < 3; ++button)
			{
				if (input.MouseDown[button] || input.MouseClicked[button] ||
					input.MouseReleased[button] || input.MouseDoubleClicked[button])
				{
					return true;
				}
			}
			return false;
		}

		glm::vec2 NodeCenter(const UiNodeInstance& node)
		{
			return glm::vec2 { node.Rect.X + node.Rect.W * 0.5f, node.Rect.Y + node.Rect.H * 0.5f };
		}
	}

	// ---- 判定 ----

	bool UiInputRouter::IsInteractiveType(const UiNodeInstance& node)
	{
		const UiNodeTypeDesc* type = UiNodeRegistry::Find(node.Type);
		return type != nullptr && type->Interactive;
	}

	bool UiInputRouter::IsNodeEnabled(const UiNodeInstance& node)
	{
		if (BoolProp(node, "disabled", false))
			return false;
		if (FindProp(node, "enabled") != nullptr && !BoolProp(node, "enabled", true))
			return false;
		return true;
	}

	bool UiInputRouter::IsNodeVisible(const UiNodeInstance& node, const UiViewport& viewport)
	{
		const Wui::WuiRect& rect = node.Rect;
		if (!(rect.W > 0.0f) || !(rect.H > 0.0f))
			return false;
		if (FindProp(node, "visible") != nullptr && !BoolProp(node, "visible", true))
			return false;
		// 可见 = 与视口内容矩形有实际重叠(矩形完全在内容矩形外 = 不可见)。
		const Wui::WuiRect& content = viewport.ContentRect;
		const float rectRight = rect.X + rect.W;
		const float rectBottom = rect.Y + rect.H;
		const float contentRight = content.X + content.W;
		const float contentBottom = content.Y + content.H;
		return rect.X < contentRight && rectRight > content.X &&
			rect.Y < contentBottom && rectBottom > content.Y;
	}

	bool UiInputRouter::IsFocusable(const UiNodeInstance& node, const UiViewport& viewport)
	{
		return IsInteractiveType(node) && IsNodeEnabled(node) && IsNodeVisible(node, viewport);
	}

	bool UiInputRouter::IsScrollContainer(const UiNodeInstance& node)
	{
		if (BoolProp(node, "scrollable", false) || BoolProp(node, "scroll", false))
			return true;
		const std::string* overflow = FindProp(node, "overflow");
		return overflow != nullptr && (*overflow == "scroll" || *overflow == "auto" || *overflow == "Scroll");
	}

	// ---- 焦点 ----

	std::vector<std::string> UiInputRouter::FocusOrder(const UiScreen& screen) const
	{
		std::vector<std::string> order;
		for (const UiNodeInstance& node : screen.Nodes())
		{
			if (IsFocusable(node, screen.Viewport()))
				order.push_back(node.Id);
		}
		return order;
	}

	bool UiInputRouter::SetFocus(const UiScreen& screen, std::string_view nodeId)
	{
		return SetFocus(screen, nodeId, UiFocusDomain::Navigation);
	}

	bool UiInputRouter::SetFocus(const UiScreen& screen, std::string_view nodeId, UiFocusDomain domain)
	{
		const UiNodeInstance* node = screen.Find(nodeId);
		if (node == nullptr)
			return false;
		if (domain == UiFocusDomain::Navigation && !IsFocusable(*node, screen.Viewport()))
			return false;
		SetFocusedInternal(std::string(nodeId), domain);
		return true;
	}

	void UiInputRouter::ClearFocus()
	{
		m_FocusedId.clear();
		m_Domain = UiFocusDomain::Navigation;
	}

	void UiInputRouter::SetFocusedInternal(std::string nodeId, UiFocusDomain domain)
	{
		m_FocusedId = std::move(nodeId);
		m_Domain = domain;
	}

	void UiInputRouter::ValidateFocus(const UiScreen& screen)
	{
		if (m_FocusedId.empty())
			return;
		if (m_Domain == UiFocusDomain::Text)
			return;   // 文本域由显式入口管理生命周期
		const UiNodeInstance* node = screen.Find(m_FocusedId);
		if (node == nullptr || !IsFocusable(*node, screen.Viewport()))
			m_FocusedId.clear();
	}

	bool UiInputRouter::MoveFocusLinear(const UiScreen& screen, int delta)
	{
		const std::vector<std::string> order = FocusOrder(screen);
		if (order.empty())
			return false;
		const long long count = static_cast<long long>(order.size());
		long long index = 0;
		bool found = false;
		for (long long i = 0; i < count; ++i)
		{
			if (order[static_cast<std::size_t>(i)] == m_FocusedId)
			{
				index = i;
				found = true;
				break;
			}
		}
		if (!found)
			index = delta >= 0 ? 0 : count - 1;
		else
			index = ((index + delta) % count + count) % count;   // 环绕
		SetFocusedInternal(order[static_cast<std::size_t>(index)], UiFocusDomain::Navigation);
		return true;
	}

	bool UiInputRouter::MoveFocusGeometric(const UiScreen& screen, NavDirection direction)
	{
		const std::vector<std::string> order = FocusOrder(screen);
		if (order.empty())
			return false;
		const UiNodeInstance* current = m_FocusedId.empty() ? nullptr : screen.Find(m_FocusedId);
		if (current == nullptr || !IsFocusable(*current, screen.Viewport()))
		{
			SetFocusedInternal(order.front(), UiFocusDomain::Navigation);
			return true;
		}

		// 几何规则(确定性):候选必须落在该方向的半平面(主轴增量 > epsilon);
		// 打分 = 主轴距离 + 2 × 垂直偏离;取最小,平手保绘制顺序靠前者。
		const glm::vec2 origin = NodeCenter(*current);
		const UiNodeInstance* best = nullptr;
		float bestScore = 0.0f;
		for (const std::string& id : order)
		{
			if (id == m_FocusedId)
				continue;
			const UiNodeInstance* candidate = screen.Find(id);
			if (candidate == nullptr)
				continue;
			const glm::vec2 center = NodeCenter(*candidate);
			const float dx = center.x - origin.x;
			const float dy = center.y - origin.y;
			float primary = 0.0f;
			float perpendicular = 0.0f;
			switch (direction)
			{
			case NavDirection::Right: primary = dx; perpendicular = std::fabs(dy); break;
			case NavDirection::Left: primary = -dx; perpendicular = std::fabs(dy); break;
			case NavDirection::Down: primary = dy; perpendicular = std::fabs(dx); break;
			case NavDirection::Up: primary = -dy; perpendicular = std::fabs(dx); break;
			}
			if (primary <= 0.001f)
				continue;
			const float score = primary + 2.0f * perpendicular;
			if (best == nullptr || score < bestScore - 0.0001f)
			{
				best = candidate;
				bestScore = score;
			}
		}
		if (best == nullptr)
			return false;   // 该方向没有候选:按键仍算被 UI 处理(由调用方决定)
		SetFocusedInternal(best->Id, UiFocusDomain::Navigation);
		return true;
	}

	// ---- 命中 ----

	const UiNodeInstance* UiInputRouter::ResolveTarget(const UiScreen& screen, const UiNodeInstance* hit) const
	{
		const UiNodeInstance* current = hit;
		while (current != nullptr)
		{
			if (IsInteractiveType(*current))
			{
				// 最近的交互祖先被禁用/不可见 ⇒ 整条链不可点(不穿透到更上层)。
				return IsNodeEnabled(*current) && IsNodeVisible(*current, screen.Viewport())
					? current : nullptr;
			}
			current = current->Parent >= 0
				? &screen.Nodes()[static_cast<std::size_t>(current->Parent)] : nullptr;
		}
		return nullptr;
	}

	const UiNodeInstance* UiInputRouter::ResolveScrollContainer(const UiScreen& screen, const UiNodeInstance* hit) const
	{
		const UiNodeInstance* current = hit;
		while (current != nullptr)
		{
			if (IsScrollContainer(*current) && IsNodeEnabled(*current) &&
				IsNodeVisible(*current, screen.Viewport()))
			{
				return current;
			}
			current = current->Parent >= 0
				? &screen.Nodes()[static_cast<std::size_t>(current->Parent)] : nullptr;
		}
		return nullptr;
	}

	void UiInputRouter::EmitClick(const UiNodeInstance& node)
	{
		UiInputCommand command;
		command.NodeId = node.Id;
		command.Event = ClickEventName();
		if (node.Source != nullptr)
		{
			for (const UiCommandDecl& decl : node.Source->On)
			{
				if (decl.Event == command.Event)
				{
					command.Command = decl.Command;
					break;
				}
			}
		}
		if (m_Queue != nullptr)
		{
			m_Queue->Push(std::move(command));
			++m_Frame.CommandsEmitted;
		}
	}

	// ---- 键盘 / 焦点导航 ----

	void UiInputRouter::HandleKeyboard(const UiScreen& screen, const Wui::WuiInputState& input)
	{
		const bool tab = WasTriggered(input, World::Tab);
		const bool activate = WasTriggered(input, World::Enter) ||
			WasTriggered(input, World::KPEnter) || WasTriggered(input, World::Space);

		if (tab)
		{
			if (m_Domain == UiFocusDomain::Text)
				m_Domain = UiFocusDomain::Navigation;   // Tab 退出文本域
			MoveFocusLinear(screen, input.Shift ? -1 : 1);
			m_Frame.KeyboardConsumed = true;
			return;   // 一帧只做一件事
		}

		if (m_Domain == UiFocusDomain::Text)
		{
			if (!input.TextInput.empty())
				m_Frame.KeyboardConsumed = true;
			return;   // 方向键/回车归文本编辑器(独立焦点域)
		}

		// 方向键/手柄 = UI 导航键:无论是否找到候选,都由 UI 消费,不能漏给玩法。
		bool directionPressed = false;
		if (WasTriggered(input, World::Right))
		{
			directionPressed = true;
			MoveFocusGeometric(screen, NavDirection::Right);
		}
		else if (WasTriggered(input, World::Left))
		{
			directionPressed = true;
			MoveFocusGeometric(screen, NavDirection::Left);
		}
		else if (WasTriggered(input, World::Up))
		{
			directionPressed = true;
			MoveFocusGeometric(screen, NavDirection::Up);
		}
		else if (WasTriggered(input, World::Down))
		{
			directionPressed = true;
			MoveFocusGeometric(screen, NavDirection::Down);
		}
		if (directionPressed)
			m_Frame.KeyboardConsumed = true;

		if (activate)
		{
			const UiNodeInstance* focused = m_FocusedId.empty() ? nullptr : screen.Find(m_FocusedId);
			if (focused != nullptr && IsFocusable(*focused, screen.Viewport()))
			{
				EmitClick(*focused);
				m_Frame.KeyboardConsumed = true;
			}
		}
	}

	// ---- 路由 ----

	bool UiInputRouter::Route(const UiScreen& screen, const Wui::WuiInputState& input)
	{
		m_Frame = UiInputFrame {};
		m_Frame.Focus.From = m_FocusedId;
		m_Frame.Focus.To = m_FocusedId;

		ValidateFocus(screen);   // 页面切换/禁用后,旧焦点可能已失效

		const UiNodeInstance* hit = screen.HitTest(input.MousePos);

		if (HasPointerEvent(input))
		{
			const UiNodeInstance* target = ResolveTarget(screen, hit);
			if (target != nullptr)
			{
				m_Frame.PointerConsumed = true;
				m_Frame.HitId = target->Id;
				if (input.MouseClicked[0] || input.MouseDoubleClicked[0])
					EmitClick(*target);
				if (IsFocusable(*target, screen.Viewport()))
					SetFocusedInternal(target->Id, UiFocusDomain::Navigation);
			}
		}

		if (input.Wheel != 0.0f)
		{
			const UiNodeInstance* scroller = ResolveScrollContainer(screen, hit);
			if (scroller != nullptr)
			{
				m_Frame.WheelConsumed = true;
				m_Frame.WheelTargetId = scroller->Id;
			}
		}

		HandleKeyboard(screen, input);

		m_Frame.Focus.To = m_FocusedId;
		m_Frame.Focus.Changed = m_Frame.Focus.From != m_Frame.Focus.To;
		return m_Frame.PointerConsumed;
	}

	bool UiInputRouter::Update(const UiScreen& screen, const Wui::WuiInputState& input)
	{
		return Route(screen, input);
	}

	bool UiInputRouter::Update(const UiScreen& screen, const Wui::WuiInputState& input, UiCommandQueue& queue)
	{
		m_Queue = &queue;
		return Route(screen, input);
	}

	bool UiInputRouter::Update(const UiNavigator& navigator, const Wui::WuiInputState& input)
	{
		const UiScreen* screen = navigator.TopInteractiveScreen();
		const bool blocking = navigator.TopLayer() != UiLayer::Page;
		if (screen == nullptr)
		{
			m_Frame = UiInputFrame {};
			m_Frame.Focus.From = m_FocusedId;
			m_Frame.Focus.To = m_FocusedId;
			return false;
		}
		const bool consumed = Route(*screen, input);
		// 模态/覆盖/调试层打开:最高层之外的指针一律不穿透(即使没落在控件上)。
		if (!consumed && blocking && m_ModalBarrier && HasPointerEvent(input))
		{
			m_Frame.PointerConsumed = true;
			m_Frame.ModalBarrier = true;
			return true;
		}
		return consumed;
	}
}
