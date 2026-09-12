#include "wldpch.h"
#include "World/WUI/WuiImGuiBackend.h"
#include "World/Core/KeyCodes.h"

#include <imgui.h>

namespace
{
	// ImGuiKey 的非 ASCII 特殊键与引擎 KeyCodes(继承 GLFW)的映射。
	uint32_t MapKey(ImGuiKey key)
	{
		if (key >= ImGuiKey_Tab && key <= ImGuiKey_Menu)
		{
			switch (key)
			{
				case ImGuiKey_Tab: return World::KeyCodes::Tab;
				case ImGuiKey_LeftArrow: return World::KeyCodes::Left;
				case ImGuiKey_RightArrow: return World::KeyCodes::Right;
				case ImGuiKey_UpArrow: return World::KeyCodes::Up;
				case ImGuiKey_DownArrow: return World::KeyCodes::Down;
				case ImGuiKey_PageUp: return World::KeyCodes::PageUp;
				case ImGuiKey_PageDown: return World::KeyCodes::PageDown;
				case ImGuiKey_Home: return World::KeyCodes::Home;
				case ImGuiKey_End: return World::KeyCodes::End;
				case ImGuiKey_Insert: return World::KeyCodes::Insert;
				case ImGuiKey_Delete: return World::KeyCodes::Delete;
				case ImGuiKey_Backspace: return World::KeyCodes::Backspace;
				case ImGuiKey_Enter: return World::KeyCodes::Enter;
				case ImGuiKey_Escape: return World::KeyCodes::Escape;
				default: return 0;
			}
		}
		// ASCII 区间与 GLFW/KeyCodes 一致。
		if (key >= ImGuiKey_Space && key <= ImGuiKey_GraveAccent)
			return static_cast<uint32_t>(key);
		if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
			return static_cast<uint32_t>(key);
		if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
			return static_cast<uint32_t>(key);
		return 0;
	}
}

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
			input.MouseReleased[i] = io.MouseReleased[i];
		}
		input.Wheel = io.MouseWheel;
		input.WantKeyboard = io.WantCaptureKeyboard;
		input.Ctrl = io.KeyCtrl;
		input.Shift = io.KeyShift;
		input.Alt = io.KeyAlt;
		input.KeyDown.clear();
		for (int key = 0; key < ImGuiKey_COUNT; ++key)
			if (io.KeysDown[key])
			{
				const uint32_t mapped = MapKey(static_cast<ImGuiKey>(key));
				if (mapped)
					input.KeyDown.push_back(mapped);
			}
		input.TextInput.clear();
		for (unsigned int character : io.InputQueueCharacters)
			if (character)
				input.TextInput.push_back(character);
		input.ViewportSize = { io.DisplaySize.x, io.DisplaySize.y };
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
				case WuiDrawKind::Image:
					drawList->AddImage(reinterpret_cast<ImTextureID>(command.Image), min, max, { command.Uv.X, command.Uv.Y }, { command.Uv.X + command.Uv.W, command.Uv.Y + command.Uv.H }, ImGui::GetColorU32(ImVec4(command.Color.R, command.Color.G, command.Color.B, command.Color.A)));
					break;
				case WuiDrawKind::ClipPush:
					drawList->PushClipRect(min, max);
					break;
				case WuiDrawKind::ClipPop:
					drawList->PopClipRect();
					break;
			}
		}
	}

	void WuiImGuiBackend::EndFrame()
	{
	}
}
