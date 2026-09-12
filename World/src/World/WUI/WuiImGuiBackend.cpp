#include "wldpch.h"
#include "World/WUI/WuiImGuiBackend.h"

#include <imgui.h>

namespace World::Wui
{
	void WuiImGuiBackend::SetFonts(ImFont* regular, ImFont* bold, ImFont* cjk)
	{
		m_Regular = regular;
		m_Bold = bold;
		m_Cjk = cjk;
	}

	bool WuiImGuiBackend::BeginFrame(WuiInputState& input)
	{
		const ImGuiIO& io = ImGui::GetIO();
		input.MousePos = { io.MousePos.x, io.MousePos.y };
		for (int i = 0; i < 3; ++i)
		{
			input.MouseDown[i] = io.MouseDown[i];
			input.MouseClicked[i] = io.MouseClicked[i];
		}
		input.Wheel = io.MouseWheel;
		input.WantKeyboard = io.WantCaptureKeyboard;
		return true;
	}

	void WuiImGuiBackend::Render(const std::vector<WuiDrawCommand>& commands)
	{
		ImDrawList* drawList = ImGui::GetForegroundDrawList();
		for (const WuiDrawCommand& command : commands)
		{
			const ImVec2 min { command.Rect.X, command.Rect.Y };
			const ImVec2 max { command.Rect.X + command.Rect.W, command.Rect.Y + command.Rect.H };
			switch (command.Kind)
			{
				case WuiDrawKind::Rect:
					drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(command.Color.R, command.Color.G, command.Color.B, command.Color.A)), command.Rounding);
					break;
				case WuiDrawKind::RectOutline:
					drawList->AddRect(min, max, ImGui::GetColorU32(ImVec4(command.Color.R, command.Color.G, command.Color.B, command.Color.A)), command.Rounding, 0, command.Thickness);
					break;
				case WuiDrawKind::Text:
				{
					ImFont* font = m_Regular ? m_Regular : ImGui::GetFont();
					bool hasNonAscii = false;
					for (unsigned char c : command.Text)
						if (c > 127) { hasNonAscii = true; break; }
					if (hasNonAscii && m_Cjk)
						font = m_Cjk;
					else if (command.Bold && m_Bold)
						font = m_Bold;
					drawList->AddText(font, command.FontSize, min, ImGui::GetColorU32(ImVec4(command.Color.R, command.Color.G, command.Color.B, command.Color.A)), command.Text.c_str());
					break;
				}
			}
		}
	}

	void WuiImGuiBackend::EndFrame()
	{
	}
}
