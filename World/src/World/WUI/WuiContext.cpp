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
		m_Commands.clear();
	}

	void WuiContext::EndFrame()
	{
		if (m_Dragging && m_Input.MouseReleased[0])
		{
			m_DropAccepted = m_DropArmed;
			m_Dragging = false;
			m_DragPayload.clear();
			m_DragId = 0;
			m_DropArmed = false;
		}
		else if (!m_Dragging)
		{
			m_DropAccepted = false;
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
		if (m_Input.MouseDown[0] && !m_Dragging)
		{
			m_Dragging = true;
			m_DragId = id;
			m_DragPayload = payload;
			m_DropArmed = false;
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
		m_DropArmed = false;
		m_DropAccepted = false;
		m_DragPayload.clear();
		m_DragId = 0;
	}

	bool WuiContext::DropTarget(const WuiRect& rect)
	{
		if (!m_Dragging)
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
		return true;
	}
}
