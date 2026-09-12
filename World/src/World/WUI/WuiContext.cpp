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
		m_Commands.clear();
	}

	void WuiContext::EndFrame()
	{
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
}
