#include "wldpch.h"
#include "World/UI/UiInputRouter.h"

#include "World/Core/KeyCodes.h"
#include "World/UI/UiNodeRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

		// 与 `UiPainter` 同一值编码协议(颜色/尺寸之外的纯数值属性)。
		float NumberProp(const UiNodeInstance& node, std::string_view name, float fallback)
		{
			const std::string* value = FindProp(node, name);
			if (value == nullptr || value->empty())
				return fallback;
			char* end = nullptr;
			const float parsed = std::strtof(value->c_str(), &end);
			return end != value->c_str() ? parsed : fallback;
		}

		std::string TextProp(const UiNodeInstance& node, std::string_view name, std::string fallback = std::string())
		{
			const std::string* value = FindProp(node, name);
			return value != nullptr ? *value : std::move(fallback);
		}

		// 与 `UiPainter::PaintSlider` 的无障碍 Value 同一格式(便于脚本/测试直接比对)。
		std::string FormatNumber(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3f", value);
			return std::string(buffer);
		}

		// UTF-32 码点 → UTF-8(非法码点忽略;编辑缓冲以 UTF-8 存,退格按码点删)。
		void AppendUtf8(std::string& text, uint32_t codepoint)
		{
			if (codepoint <= 0x7Fu)
			{
				text.push_back(static_cast<char>(codepoint));
			}
			else if (codepoint <= 0x7FFu)
			{
				text.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
				text.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
			}
			else if (codepoint <= 0xFFFFu)
			{
				text.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
				text.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
				text.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
			}
			else if (codepoint <= 0x10FFFFu)
			{
				text.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
				text.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
				text.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
				text.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
			}
		}

		// 删掉最后一个 UTF-8 码点(连同其续字节)。
		void PopUtf8(std::string& text)
		{
			if (text.empty())
				return;
			std::size_t index = text.size() - 1;
			while (index > 0 && (static_cast<unsigned char>(text[index]) & 0xC0u) == 0x80u)
				--index;
			text.erase(index);
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

	bool UiInputRouter::IsSliderNode(const UiNodeInstance& node)
	{
		const UiNodeTypeDesc* type = UiNodeRegistry::Find(node.Type);
		return type != nullptr && type->Type == "Slider";
	}

	bool UiInputRouter::IsTextFieldNode(const UiNodeInstance& node)
	{
		const UiNodeTypeDesc* type = UiNodeRegistry::Find(node.Type);
		return type != nullptr && type->Type == "TextField";
	}

	void UiInputRouter::EmitNodeEvent(const UiNodeInstance& node, std::string_view event, std::string value)
	{
		UiInputCommand command;
		command.NodeId = node.Id;
		command.Event = std::string(event);
		command.Value = std::move(value);
		if (node.Source != nullptr)
		{
			for (const UiCommandDecl& decl : node.Source->On)
			{
				if (std::string_view(decl.Event) == event)
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

	void UiInputRouter::EmitClick(const UiNodeInstance& node)
	{
		EmitNodeEvent(node, ClickEventName(), std::string());
	}

	// 指针在设计空间的位置 → 滑条值。与 `UiPainter::PaintSlider` 同一口径:
	// fraction = clamp((x - rect.X) / rect.W, 0, 1);value = min + fraction*(max-min);
	// 有 `step`(>0)时按 step 量化并再钳位到 [min,max]。
	float UiInputRouter::SliderValueFromPointer(const UiNodeInstance& node, glm::vec2 designPoint) const
	{
		const float minValue = NumberProp(node, "min", 0.0f);
		const float maxValue = NumberProp(node, "max", 1.0f);
		if (!(node.Rect.W > 0.0f))
			return minValue;

		const float fraction = std::clamp((designPoint.x - node.Rect.X) / node.Rect.W, 0.0f, 1.0f);
		float value = minValue + fraction * (maxValue - minValue);

		const float step = NumberProp(node, "step", 0.0f);
		if (step > 0.0f)
			value = minValue + std::round((value - minValue) / step) * step;

		const float low = std::min(minValue, maxValue);
		const float high = std::max(minValue, maxValue);
		return std::clamp(value, low, high);
	}

	void UiInputRouter::BeginEditing(const UiNodeInstance& node)
	{
		m_Editing = true;
		m_EditingNodeId = node.Id;
		// 初值:宿主预置(`SetEditingText`)优先;否则读节点属性(绘制用 `value`,
		// 派工单口径允许 `text` 作兼容),都没有 = 空。
		m_EditingOriginal = TextProp(node, "value", TextProp(node, "text"));
		const auto preset = m_TextPresets.find(node.Id);
		m_EditingText = preset != m_TextPresets.end() ? preset->second : m_EditingOriginal;
		SetFocusedInternal(node.Id, UiFocusDomain::Text);
	}

	void UiInputRouter::CommitEditing(const UiScreen& screen)
	{
		if (!m_Editing)
			return;
		const std::string nodeId = m_EditingNodeId;
		const std::string text = m_EditingText;
		m_Editing = false;
		m_EditingNodeId.clear();
		m_EditingText.clear();
		m_EditingOriginal.clear();
		m_Domain = UiFocusDomain::Navigation;
		if (const UiNodeInstance* node = screen.Find(nodeId); node != nullptr)
			EmitNodeEvent(*node, CommitEventName(), text);
	}

	void UiInputRouter::CancelEditing()
	{
		if (!m_Editing)
			return;
		// 放弃 = 回到原值;编辑缓冲保留"原值"供宿主/测试读取,但不产生 Commit。
		m_EditingText = m_EditingOriginal;
		m_Editing = false;
		m_EditingNodeId.clear();
		m_EditingOriginal.clear();
		m_Domain = UiFocusDomain::Navigation;
	}

	void UiInputRouter::SetEditingText(std::string_view nodeId, std::string text)
	{
		const std::string key(nodeId);
		m_TextPresets[key] = text;
		if (m_Editing && m_EditingNodeId == key)
			m_EditingText = std::move(text);
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

	// M13②:文本框编辑态的键盘语义(与"仅文本焦点域"区分:编辑态下 Backspace/Enter/Escape
	// 生效,方向键/Tab 不导航)。
	void UiInputRouter::HandleEditingKeyboard(const UiScreen& screen, const Wui::WuiInputState& input)
	{
		if (WasTriggered(input, World::Backspace))
		{
			PopUtf8(m_EditingText);
			m_Frame.KeyboardConsumed = true;
			return;
		}
		if (WasTriggered(input, World::Enter) || WasTriggered(input, World::KPEnter))
		{
			CommitEditing(screen);   // 值 = 编辑中的文本
			m_Frame.KeyboardConsumed = true;
			return;
		}
		if (WasTriggered(input, World::Escape))
		{
			CancelEditing();         // 放弃 = 回到原值,不产生 Commit
			m_Frame.KeyboardConsumed = true;
			return;
		}
		if (!input.TextInput.empty())
		{
			for (const uint32_t codepoint : input.TextInput)
				AppendUtf8(m_EditingText, codepoint);
			m_Frame.KeyboardConsumed = true;
			return;
		}
		// 编辑态:方向键保持"文本域语义"(不归 UI),Tab 被 UI 吃掉但**不导航**。
		if (WasTriggered(input, World::Tab))
			m_Frame.KeyboardConsumed = true;
	}

	// ---- 路由 ----

	bool UiInputRouter::Route(const UiScreen& screen, const Wui::WuiInputState& input, UiScreen* scrollTarget)
	{
		m_Frame = UiInputFrame {};
		m_Frame.Focus.From = m_FocusedId;
		m_Frame.Focus.To = m_FocusedId;

		ValidateFocus(screen);   // 页面切换/禁用后,旧焦点可能已失效

		// 编辑态挂在已消失的节点上(切页 / 热重载)⇒ 静默取消,不留悬空状态。
		if (m_Editing && screen.Find(m_EditingNodeId) == nullptr)
			CancelEditing();

		const UiNodeInstance* hit = screen.HitTest(input.MousePos);
		const UiNodeInstance* target = ResolveTarget(screen, hit);
		const bool pressed = input.MouseDown[0] || input.MouseClicked[0] || input.MouseDoubleClicked[0];
		const glm::vec2 designPoint = screen.Viewport().PhysicalToDesign(input.MousePos);

		// ---- M13①:滑条拖拽(跨帧;拖拽期间指针必须消费,玩法不得收到)----
		if (m_Dragging)
		{
			m_Frame.PointerConsumed = true;
			m_Frame.HitId = m_DragNodeId;
			const UiNodeInstance* dragNode = screen.Find(m_DragNodeId);
			const bool released = input.MouseReleased[0] || !input.MouseDown[0];
			if (dragNode == nullptr || !IsNodeEnabled(*dragNode) || !IsNodeVisible(*dragNode, screen.Viewport()))
			{
				m_Dragging = false;   // 节点失效:静默结束(不推 Commit)
				m_DragNodeId.clear();
			}
			else if (released)
			{
				EmitNodeEvent(*dragNode, CommitEventName(),
					FormatNumber(SliderValueFromPointer(*dragNode, designPoint)));
				m_Dragging = false;
				m_DragNodeId.clear();
			}
			else
			{
				// 按住期间每帧一条 Change(与绘制同一套 min/max/step)。
				EmitNodeEvent(*dragNode, ChangeEventName(),
					FormatNumber(SliderValueFromPointer(*dragNode, designPoint)));
			}
			m_Frame.Focus.To = m_FocusedId;
			m_Frame.Focus.Changed = m_Frame.Focus.From != m_Frame.Focus.To;
			return m_Frame.PointerConsumed;
		}

		if (HasPointerEvent(input))
		{
			// 按下别处:先提交正在编辑的文本框(重新按下同一个框除外)。
			if (m_Editing && pressed &&
				!(target != nullptr && IsTextFieldNode(*target) && target->Id == m_EditingNodeId))
			{
				CommitEditing(screen);
				// 结束编辑的那一次按下由 UI 吃掉:玩法不得因为"点空白退出输入"而同帧拿到这一下。
				m_Frame.PointerConsumed = true;
			}

			if (pressed && target != nullptr && IsTextFieldNode(*target) &&
				IsNodeEnabled(*target) && IsNodeVisible(*target, screen.Viewport()))
			{
				// M13②:点聚焦 → 文本焦点域 + 编辑态(不推 Click)。
				if (!(m_Editing && m_EditingNodeId == target->Id))
					BeginEditing(*target);
				m_Frame.PointerConsumed = true;
				m_Frame.HitId = target->Id;
			}
			else if (pressed && target != nullptr && IsSliderNode(*target) &&
				IsNodeEnabled(*target) && IsNodeVisible(*target, screen.Viewport()))
			{
				// M13①:按下落在滑条 = 进入拖拽,立即推一条 Change。
				m_Dragging = true;
				m_DragNodeId = target->Id;
				SetFocusedInternal(target->Id, UiFocusDomain::Navigation);
				m_Frame.PointerConsumed = true;
				m_Frame.HitId = target->Id;
				EmitNodeEvent(*target, ChangeEventName(),
					FormatNumber(SliderValueFromPointer(*target, designPoint)));
			}
			else if (target != nullptr)
			{
				m_Frame.PointerConsumed = true;
				m_Frame.HitId = target->Id;
				if (input.MouseClicked[0] || input.MouseDoubleClicked[0])
					EmitClick(*target);
				if (IsFocusable(*target, screen.Viewport()))
					SetFocusedInternal(target->Id, UiFocusDomain::Navigation);
			}
		}

		// ---- M13③:滚轮命中滚动容器 ⇒ 消费 + 偏移落地 ----
		if (input.Wheel != 0.0f)
		{
			const UiNodeInstance* scroller = ResolveScrollContainer(screen, hit);
			if (scroller != nullptr)
			{
				m_Frame.WheelConsumed = true;
				m_Frame.WheelTargetId = scroller->Id;
				if (scrollTarget != nullptr)
				{
					// 步长 = `scrollStep`(设计单位,默认 40);钳位是 `SetScrollOffset` 的唯一实现。
					const float step = NumberProp(*scroller, "scrollStep", 40.0f);
					glm::vec2 offset = screen.ScrollOffset(scroller->Id);
					offset.y += -input.Wheel * step;   // 滚轮向下(负)= 内容下移 ⇒ 偏移增
					scrollTarget->SetScrollOffset(scroller->Id, offset);
				}
			}
		}

		if (m_Editing)
			HandleEditingKeyboard(screen, input);
		else
			HandleKeyboard(screen, input);

		m_Frame.Focus.To = m_FocusedId;
		m_Frame.Focus.Changed = m_Frame.Focus.From != m_Frame.Focus.To;
		return m_Frame.PointerConsumed;
	}

	bool UiInputRouter::Update(const UiScreen& screen, const Wui::WuiInputState& input)
	{
		return Route(screen, input, nullptr);
	}

	bool UiInputRouter::Update(UiScreen& screen, const Wui::WuiInputState& input)
	{
		return Route(screen, input, &screen);
	}

	bool UiInputRouter::Update(const UiScreen& screen, const Wui::WuiInputState& input, UiCommandQueue& queue)
	{
		m_Queue = &queue;
		return Route(screen, input, nullptr);
	}

	bool UiInputRouter::Update(UiScreen& screen, const Wui::WuiInputState& input, UiCommandQueue& queue)
	{
		m_Queue = &queue;
		return Route(screen, input, &screen);
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
		const bool consumed = Route(*screen, input, nullptr);   // UiPage::Screen 是 const:不落地滚动偏移
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
