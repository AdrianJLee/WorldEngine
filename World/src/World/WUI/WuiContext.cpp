#include "wldpch.h"
#include "World/WUI/WuiContext.h"

#include <chrono>

namespace World::Wui
{
	WuiContext::WuiContext()
	{
		m_StyleStack.push_back({});
	}

	void WuiContext::BeginFrame(const WuiInputState& input)
	{
		m_Input = input;
		m_ViewportSize = input.ViewportSize;
		m_TextInputActive = false;
		m_Cursor = WuiCursor::Arrow;
		m_Commands.clear();
		m_OverlayCommands.clear();
		m_OverlayDepth = 0;
		m_HoverBlockers.clear();
		++m_Frame;
	}

	// 命中测试:矩形包含 + 不在上层遮挡区内。调用方(IsHovered/IsClicked/DropTarget/
	// 弹窗外部点击)统一走这里,保证"上层浮动面板优先"一条规则。
	bool WuiContext::HitTest(const WuiRect& rect, glm::vec2 point) const
	{
		if (!World::Wui::HitTest(rect, point))
			return false;
		for (const WuiRect& blocker : m_HoverBlockers)
			if (blocker.Contains(point))
				return false;
		return true;
	}

	void WuiContext::EndFrame()
	{
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
		if (std::find(m_OpenPopups.begin(), m_OpenPopups.end(), id) == m_OpenPopups.end())
		{
			m_OpenPopups.push_back(id);
			RecordOp("popup", "open", std::to_string(id), "");
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
		if (HitTest(ignoreRect, m_Input.MousePos))
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
