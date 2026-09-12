#include "wldpch.h"
#include "World/WUI/WuiContext.h"

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
			}
		}
		if (m_Dragging && m_Input.MouseReleased[0])
		{
			m_DropAccepted = m_DropArmed;
			m_Dragging = false;
			m_DragId = 0;
			m_DropArmed = false;
			m_DragPending = false;
			// m_DragPayload 保留到 AcceptDrop 消费后再清空。
		}
		else
		{
			m_DropAccepted = false;
			if (m_DragPending && m_Input.MouseReleased[0])
			{
				m_DragPending = false;
				m_DragPayload.clear();
				m_DragId = 0;
			}
		}
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
			m_OpenPopups.push_back(id);
	}

	void WuiContext::ClosePopup(WuiId id)
	{
		m_OpenPopups.erase(std::remove(m_OpenPopups.begin(), m_OpenPopups.end(), id), m_OpenPopups.end());
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
			ClosePopup(popup);
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
			return;
		}
		if (glm::length(m_Input.MousePos - m_DragPressPos) > 4.0f)
			m_Dragging = true;
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
		if (!m_DropAccepted)
			return false;
		if (payload)
			*payload = m_DragPayload;
		m_DropAccepted = false;
		m_DragPayload.clear();
		return true;
	}
}
