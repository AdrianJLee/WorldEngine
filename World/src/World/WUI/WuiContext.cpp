#include "wldpch.h"
#include "World/WUI/WuiContext.h"
#include "World/Core/KeyCodes.h"

#include <chrono>

namespace World::Wui
{
	namespace
	{
		struct TextMeasureHookState
		{
			// W9 review:多 owner(每个 WuiRhiBackend 一个)。后注册者优先;注销后自动回退到
			// 仍存活的上一个,避免"任一 backend 析构就把度量清成估算"造成命中/光标错位。
			std::vector<std::pair<void*, WuiTextMeasureFn>> Entries;
		};

		TextMeasureHookState& MeasureHook()
		{
			static TextMeasureHookState hook;
			return hook;
		}

		// 无钩子时的回退度量:ASCII 0.6em,其余 1.0em(与旧 CursorAtX 的启发式一致,
		// 但按码点解码,不会把多字节串算成多个 ASCII)。
		float FallbackAdvance(uint32_t codepoint, float fontSize)
		{
			return fontSize * (codepoint < 0x80 ? 0.6f : 1.0f);
		}

		template <typename Fn>
		void ForEachCodepoint(std::string_view text, Fn&& fn)
		{
			size_t i = 0;
			while (i < text.size())
			{
				const unsigned char c = static_cast<unsigned char>(text[i]);
				uint32_t cp = c;
				size_t length = 1;
				if (c >= 0xF0) { length = 4; cp = c & 0x07u; }
				else if (c >= 0xE0) { length = 3; cp = c & 0x0Fu; }
				else if (c >= 0xC0) { length = 2; cp = c & 0x1Fu; }
				if (i + length > text.size())
				{
					cp = 0xFFFD;
					length = 1;
				}
				for (size_t k = 1; k < length; ++k)
					cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
				fn(cp);
				i += length;
			}
		}

		bool SameOverlayRect(const WuiOverlayRect& a, const WuiOverlayRect& b)
		{
			return a.Depth == b.Depth && a.Rect.X == b.Rect.X && a.Rect.Y == b.Rect.Y
				&& a.Rect.W == b.Rect.W && a.Rect.H == b.Rect.H;
		}

		// P4-U28:验收期调试计数(只在 WLD_TRACE_UI=1 时输出,每帧最多一条)。探针用它证明
		// 两件事:①弹层关闭帧的 release 被遮挡区挡住(没有落到下层);②关闭后"多挡一帧"
		// 确实发生、而且只发生一帧(两帧后彻底清除)。
		void TraceU28Block(const WuiInputState& input, uint64_t frame, const WuiOverlayRect& blocker)
		{
			if (!std::getenv("WLD_TRACE_UI"))
				return;
			static uint64_t lastLiveFrame = 0;
			static uint64_t lastDelayedFrame = 0;
			const char* kind = blocker.Delayed ? "delayed" : "live";
			if (blocker.Delayed)
			{
				if (lastDelayedFrame == frame)
					return;
				lastDelayedFrame = frame;
			}
			else
			{
				const bool released = input.MouseReleased[0] || input.MouseReleased[1] || input.MouseReleased[2];
				if (!released || lastLiveFrame == frame)
					return;
				lastLiveFrame = frame;
			}
			WLD_CORE_INFO("[wui-u28] {0} overlay blocked hit on frame {1}: rect=({2},{3},{4},{5}) depth={6}",
				kind, frame, static_cast<int>(blocker.Rect.X), static_cast<int>(blocker.Rect.Y),
				static_cast<int>(blocker.Rect.W), static_cast<int>(blocker.Rect.H), blocker.Depth);
		}
	}

	void SetTextMeasureHook(void* owner, WuiTextMeasureFn fn)
	{
		auto& entries = MeasureHook().Entries;
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[&](const auto& entry) { return entry.first == owner; }), entries.end());
		entries.emplace_back(owner, std::move(fn));
	}

	void ClearTextMeasureHook(void* owner)
	{
		auto& entries = MeasureHook().Entries;
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[&](const auto& entry) { return entry.first == owner; }), entries.end());
	}

	float MeasureTextWithHook(std::string_view utf8, float fontSize, WuiFontFamily family)
	{
		const auto& entries = MeasureHook().Entries;
		if (!entries.empty() && entries.back().second)
			return entries.back().second(utf8, fontSize, family);
		float width = 0;
		ForEachCodepoint(utf8, [&](uint32_t cp) { width += FallbackAdvance(cp, fontSize); });
		return width;
	}

	WuiTextFocus& WuiTextFocus::Get()
	{
		static WuiTextFocus instance;
		return instance;
	}

	void WuiTextFocus::BeginContextFrame(const void* context)
	{
		m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
			[&](const Item& item) { return item.Context == context; }), m_Entries.end());
	}

	void WuiTextFocus::Set(const void* context, WuiId id, std::string window, std::string panel)
	{
		BeginContextFrame(context);
		if (id == 0)
			return;
		Item item;
		item.Context = context;
		item.Info.Id = id;
		item.Info.Window = std::move(window);
		item.Info.Panel = std::move(panel);
		m_Entries.push_back(std::move(item));
	}

	void WuiTextFocus::Clear()
	{
		m_Entries.clear();
	}

	const std::string& WuiTextFocus::Window() const
	{
		static const std::string empty;
		return m_Entries.empty() ? empty : m_Entries.back().Info.Window;
	}

	const std::string& WuiTextFocus::Panel() const
	{
		static const std::string empty;
		return m_Entries.empty() ? empty : m_Entries.back().Info.Panel;
	}

	WuiContext::WuiContext()
	{
		m_StyleStack.push_back({});
	}

	WuiContext::~WuiContext()
	{
		// 宿主销毁(窗口关闭)后不再有 BeginFrame 来重建登记,必须在这里摘掉。
		WuiTextFocus::Get().BeginContextFrame(this);
	}

	void WuiContext::BeginFrame(const WuiInputState& input)
	{
		m_Input = input;
		m_ViewportSize = input.ViewportSize;
		// 文本控件是否仍持焦点:登记每帧重建,所以判定必须赶在 BeginContextFrame 清掉本上下文的
		// 登记**之前**取值 —— 否则本窗口自己的文本焦点已经被抹掉,一按 Tab 就会被焦点表抢走。
		const bool textFocusActive = m_TextInputActive || WuiTextFocus::Get().Active();
		m_TextInputActive = false;
		// 文本焦点每帧重建:上一帧的登记先失效,本帧聚焦的文本控件再登记。
		WuiTextFocus::Get().BeginContextFrame(this);
		m_Cursor = WuiCursor::Arrow;
		m_Commands.clear();
		m_OverlayCommands.clear();
		m_OverlayDepth = 0;
		// P4-U7:点击消费标记每帧归零;上一帧登记的覆盖层矩形转为"只挡更浅层"的遮挡区。
		m_PointerConsumedClick[0] = m_PointerConsumedClick[1] = m_PointerConsumedClick[2] = false;
		// P4-U28:弹层关闭那一帧(上一帧还登记着)继续遮挡,并且**多挡一帧**(上一帧刚消失的
		// 矩形再挡一帧)后才彻底清除 —— 避免 close 帧的 release 直接落到下层,又不留幽灵遮挡。
		m_UnderlayBlockers.clear();
		for (const WuiOverlayRect& rect : m_OverlayRects)
			m_UnderlayBlockers.push_back(rect);
		for (const WuiOverlayRect& rect : m_OverlayRectsLast)
		{
			const bool stillRegistered = std::any_of(m_OverlayRects.begin(), m_OverlayRects.end(),
				[&](const WuiOverlayRect& other) { return SameOverlayRect(other, rect); });
			if (stillRegistered)
				continue;
			WuiOverlayRect delayed = rect;
			delayed.Delayed = true;
			m_UnderlayBlockers.push_back(delayed);
		}
		m_OverlayRectsLast = std::move(m_OverlayRects);
		m_OverlayRects.clear();
		m_HoverBlockers.clear();
		m_Tooltip.clear();
		// P4-U28:按下归属。新的一次按下重新归属;没有按下/抬起标记(事件丢失)时不留幽灵。
		for (int button = 0; button < 3; ++button)
		{
			if (m_Input.MouseClicked[button] || (!m_Input.MouseDown[button] && !m_Input.MouseReleased[button]))
				m_ClickOwners[button] = WuiClickOwner {};
		}
		// 焦点顺序表每帧重建:上一帧的表挪到 Prev(Tab 顺序与"消失即失焦"都基于它)。
		m_FocusablesPrev = std::move(m_Focusables);
		m_Focusables.clear();
		NavigateFocus(m_Input, textFocusActive);
		++m_Frame;
	}

	void WuiContext::RegisterFocusable(WuiId id, const WuiRect& rect)
	{
		if (id == 0)
			return;
		// 同一 id 在一帧里只保留一条:重复绘制时以最后一次登记的矩形为准,顺序仍按首次登记的位置。
		for (WuiFocusable& entry : m_Focusables)
			if (entry.Id == id)
			{
				entry.Rect = rect;
				return;
			}
		m_Focusables.push_back(WuiFocusable { id, rect });
	}

	void WuiContext::PushClipRect(const WuiRect& rect)
	{
		m_ClipStack.push_back(rect);
	}

	void WuiContext::PopClipRect()
	{
		if (!m_ClipStack.empty())
			m_ClipStack.pop_back();
	}

	// 与**最内层**裁剪区相交才算可见:滚动区里被滚出视口的控件不画焦点环。
	// 没有裁剪区(不在滚动区里)时一律可见。
	bool WuiContext::ClipAllows(const WuiRect& rect) const
	{
		if (m_ClipStack.empty())
			return true;
		const WuiRect& clip = m_ClipStack.back();
		return rect.X + rect.W > clip.X && rect.X < clip.X + clip.W
			&& rect.Y + rect.H > clip.Y && rect.Y < clip.Y + clip.H;
	}

	void WuiContext::NavigateFocus(const WuiInputState& input, bool textFocusActive)
	{
		// 文本控件正在编辑:Tab(代码编辑器里是缩进 / 接受补全候选)与 Escape 都归它,焦点表不抢。
		if (textFocusActive)
			return;
		bool forward = false;
		bool backward = false;
		for (uint32_t key : input.KeyPressed)
		{
			if (key != KeyCodes::Tab)
				continue;
			// 只用 KeyPressed(本帧新按下):按住 Tab 不会因 KeyDown/KeyRepeated 连跳到表尾。
			if (input.Shift)
				backward = true;
			else
				forward = true;
		}
		// Escape 清焦点,但只清"由焦点顺序表拥有"的焦点(上一帧登记过的控件)。焦点在代码编辑器/
		// 视口这类自管 id 上时不动它:它们的 Escape 语义(关补全浮层但保留焦点等)由自己处理。
		const bool escapePressed = std::find(input.KeyPressed.begin(), input.KeyPressed.end(), KeyCodes::Escape)
			!= input.KeyPressed.end();
		const bool focusOwnedByTable = std::any_of(m_FocusablesPrev.begin(), m_FocusablesPrev.end(),
			[&](const WuiFocusable& entry) { return entry.Id == m_Focus; });
		if (!forward && !backward)
		{
			if (escapePressed && focusOwnedByTable)
				SetFocus(0);
			return;
		}
		if (m_FocusablesPrev.empty())
			return;
		std::vector<WuiId> order;
		order.reserve(m_FocusablesPrev.size());
		for (const WuiFocusable& entry : m_FocusablesPrev)
			order.push_back(entry.Id);
		// NextFocus(WuiCore):线性遍历、末尾回卷;当前焦点不在表里时正向取第一个、反向取最后一个。
		if (const std::optional<WuiId> next = NextFocus(order, m_Focus, backward))
			SetFocus(*next);
	}

	void WuiContext::SetTextInputActive(bool active)
	{
		m_TextInputActive = active;
		if (active && m_Focus != 0)
			WuiTextFocus::Get().Set(this, m_Focus, m_WindowKey, m_PanelId);
		else if (!active)
			WuiTextFocus::Get().BeginContextFrame(this);
	}

	float WuiContext::MeasureTextWidth(std::string_view utf8, float fontSize, WuiFontFamily family) const
	{
		return World::Wui::MeasureTextWithHook(utf8, fontSize, family);
	}

	// 命中测试:矩形包含 + 不在上层遮挡区内。调用方(IsHovered/IsClicked/DropTarget/
	// 弹窗外部点击)统一走这里,保证"上层浮动面板优先"一条规则。
	bool WuiContext::HitTest(const WuiRect& rect, glm::vec2 point) const
	{
		if (!World::Wui::HitTest(rect, point))
			return false;
		// P4-U7:上一帧的覆盖层矩形(弹出菜单/下拉/模态外框)= 本帧的"下层遮挡区"。
		// P4-U28:改成按深度遮挡 —— 只挡**比它浅**的绘制层(depth 0 的常驻面板最浅,
		// 模态内容 depth 1,叠在模态里的弹层 depth 2…),弹层自己与更深的子菜单照常命中。
		// 旧口径"只挡 depth 0"在模态内部会漏挡(模态内容也是覆盖层深度),M3 报告的
		// "点选项那一下穿透到 Parent 下拉"就是这条漏挡。
		for (const WuiOverlayRect& overlay : m_UnderlayBlockers)
		{
			if (!overlay.Rect.Contains(point))
				continue;
			const int blockingDepth = std::max(overlay.Depth, 1);
			if (m_OverlayDepth < blockingDepth)
			{
				TraceU28Block(m_Input, m_Frame, overlay);
				return false;
			}
		}
		for (const WuiRect& blocker : m_HoverBlockers)
			if (blocker.Contains(point))
				return false;
		return true;
	}

	void WuiContext::RecordClickOwner(int button, WuiId id, const WuiRect& rect) const
	{
		if (button < 0 || button > 2)
			return;
		WuiClickOwner& owner = m_ClickOwners[button];
		if (owner.Valid)
			return;   // 一次按下只归第一个命中它的控件
		owner.Id = id;
		owner.Rect = rect;
		owner.Valid = true;
	}

	void WuiContext::EndFrame()
	{
		// P4-U28:release 帧核对完按下归属就让它失效;按住没松开的按钮继续保留归属。
		for (int button = 0; button < 3; ++button)
			if (!m_Input.MouseDown[button])
				m_ClickOwners[button] = WuiClickOwner {};
		// 待拖 → 真拖:不依赖源控件仍处于悬停状态(鼠标可离开源标签/图标)。
		if (m_DragPending && !m_Dragging)
		{
			if (!m_Input.MouseDown[0])
			{
				m_DragPending = false;
				m_DragPayload.clear();
				m_DragId = 0;
			}
			else if (glm::length(m_Input.MousePos - m_DragPressPos) > 4.0f)
			{
				m_Dragging = true;
				RecordOp("drag", "active", m_DragPayload, "");
			}
		}
		if (m_Dragging && m_Input.MouseReleased[0])
		{
			m_DropAccepted = m_DropArmed;
			RecordOp("drag", "release", m_DragPayload, m_DropArmed ? "armed" : "no-target");
			if (std::getenv("WLD_TRACE_UI"))
				WLD_CORE_INFO("[wui-drag] release consumed: payload={0}", m_DragPayload);
			m_Dragging = false;
			m_DragId = 0;
			m_DropArmed = false;
			m_DragPending = false;
			// m_DragPayload 保留到 AcceptDrop 消费后再清空。
		}
		else
		{
			// 诊断:按钮已经不在按下,却既没有 down 也没有 release 标记(说明释放事件
			// 没有传进本上下文),此时拖拽状态会挂死。
			if (m_Dragging && !m_Input.MouseDown[0] && !m_Input.MouseReleased[0])
			{
				static int traced = 0;
				if (std::getenv("WLD_TRACE_UI") && traced < 6)
				{
					++traced;
					WLD_CORE_WARN("[wui-drag] STUCK: dragging with no down/up flag, payload={0}", m_DragPayload);
				}
			}
			m_DropAccepted = false;
			if (m_DragPending && m_Input.MouseReleased[0])
			{
				m_DragPending = false;
				m_DragPayload.clear();
				m_DragId = 0;
			}
		}
		// 焦点顺序表的"消失即失焦":上一帧登记过、本帧没有再登记的控件(面板关闭、控件隐藏、
		// 条件绘制分支不再走)不留幽灵焦点。自管焦点的 id(代码编辑器/视口)从不在表里,不受影响。
		if (m_Focus != 0)
		{
			const bool wasRegistered = std::any_of(m_FocusablesPrev.begin(), m_FocusablesPrev.end(),
				[&](const WuiFocusable& entry) { return entry.Id == m_Focus; });
			const bool stillRegistered = std::any_of(m_Focusables.begin(), m_Focusables.end(),
				[&](const WuiFocusable& entry) { return entry.Id == m_Focus; });
			if (wasRegistered && !stillRegistered)
				SetFocus(0);
		}
	}

	void WuiContext::RecordOp(std::string category, std::string action, std::string target, std::string detail)
	{
		const double timeMs = static_cast<double>(
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		m_Ops.Record(m_Frame, timeMs, std::move(category), std::move(action), std::move(target), std::move(detail));
	}

	void WuiContext::PushStyle(const WuiStyle& style)
	{
		m_StyleStack.push_back(CurrentStyle().Overlay(style));
	}

	void WuiContext::PopStyle()
	{
		if (m_StyleStack.size() > 1)
			m_StyleStack.pop_back();
	}

	WuiStyle WuiContext::CurrentStyle() const
	{
		return m_StyleStack.back();
	}

	void WuiContext::OpenPopup(WuiId id)
	{
		const bool wasOpen = std::find(m_OpenPopups.begin(), m_OpenPopups.end(), id) != m_OpenPopups.end();
		if (!wasOpen)
		{
			m_OpenPopups.push_back(id);
			RecordOp("popup", "open", std::to_string(id), "");
			// P4-U7:打开弹层的这一下点击**只归它** —— 同帧后面绘制的控件不再看到这次点击。
			// 只在"从未打开 → 打开"的跃迁上消费,弹层开着时重复调用不会吞掉后续点击。
			// 左键(点击式菜单/下拉)与右键(右键菜单)都要消费:右键菜单正是右键打开的。
			ConsumePointerClick(0);
			ConsumePointerClick(1);
		}
		m_PopupOpenFrame[id] = m_Frame;
	}

	void WuiContext::ClosePopup(WuiId id)
	{
		auto it = std::find(m_OpenPopups.begin(), m_OpenPopups.end(), id);
		if (it != m_OpenPopups.end())
		{
			m_OpenPopups.erase(it);
			RecordOp("popup", "close", std::to_string(id), "");
		}
		m_PopupOpenFrame.erase(id);
	}

	bool WuiContext::IsPopupOpen(WuiId id) const
	{
		return std::find(m_OpenPopups.begin(), m_OpenPopups.end(), id) != m_OpenPopups.end();
	}

	bool WuiContext::ClosePopupsOnOutsideClick(const std::vector<WuiId>& popups, const WuiRect& ignoreRect)
	{
		if (!m_Input.MouseClicked[0] && !m_Input.MouseClicked[1])
			return false;
		// P4-U7:这里必须用**原始**矩形判定 —— 弹层自己算"内/外",不能被本帧的捕获层/
		// 下层遮挡区挡住(否则"点在弹层内部"会被判成外部点击而误关)。
		if (HitTestRaw(ignoreRect, m_Input.MousePos))
			return false;
		for (WuiId popup : popups)
		{
			// 弹出开启当帧的点击(即打开菜单的那一下)不算外部点击。
			auto frameIt = m_PopupOpenFrame.find(popup);
			if (frameIt != m_PopupOpenFrame.end() && frameIt->second == m_Frame)
				continue;
			ClosePopup(popup);
		}
		return true;
	}

	void WuiContext::BeginDrag(WuiId id, const std::string& payload)
	{
		if (m_Dragging || !m_Input.MouseDown[0])
			return;
		if (!m_DragPending)
		{
			m_DragPending = true;
			m_DragPressPos = m_Input.MousePos;
			m_DragId = id;
			m_DragPayload = payload;
			m_DropArmed = false;
			RecordOp("drag", "press", m_DragPayload, "");
			return;
		}
		if (glm::length(m_Input.MousePos - m_DragPressPos) > 4.0f)
		{
			m_Dragging = true;
			RecordOp("drag", "active", m_DragPayload, "");
		}
	}

	bool WuiContext::IsDragActive(std::string* payload) const
	{
		if (m_Dragging && payload)
			*payload = m_DragPayload;
		return m_Dragging;
	}

	void WuiContext::EndDrag()
	{
		m_Dragging = false;
		m_DragPending = false;
		m_DropArmed = false;
		m_DropAccepted = false;
		m_DragPayload.clear();
		m_DragId = 0;
	}

	bool WuiContext::DropTarget(const WuiRect& rect)
	{
		return DropTarget(rect, {});
	}

	bool WuiContext::DropTarget(const WuiRect& rect, const std::string& payloadPrefix)
	{
		if (!m_Dragging)
			return false;
		if (!payloadPrefix.empty() && m_DragPayload.rfind(payloadPrefix, 0) != 0)
			return false;
		if (HitTest(rect, m_Input.MousePos))
			m_DropArmed = true;
		return false;
	}

	bool WuiContext::AcceptDrop(std::string* payload)
	{
		return AcceptDrop(payload, {});
	}

	bool WuiContext::AcceptDrop(std::string* payload, const std::string& payloadPrefix)
	{
		if (!m_DropAccepted)
			return false;
		if (!payloadPrefix.empty() && m_DragPayload.rfind(payloadPrefix, 0) != 0)
			return false; // 类型不匹配,留给其他消费者
		if (payload)
			*payload = m_DragPayload;
		m_DropAccepted = false;
		RecordOp("drag", "accept", payload ? *payload : m_DragPayload, "");
		m_DragPayload.clear();
		return true;
	}
}
