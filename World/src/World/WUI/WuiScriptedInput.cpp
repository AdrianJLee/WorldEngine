#include "wldpch.h"
#include "WuiScriptedInput.h"

#include "World/Core/KeyCodes.h"

#include <string_view>

namespace World::Wui
{
	namespace
	{
		// UTF-8 → 码点(WUI 的 TextInput 就是 UTF-32 码点流)。非法/截断字节按各自单字节
		// 处理,不猜测编码;不做字素簇合并(与 WuiCodeEditor/TextField 的入参口径一致)。
		std::vector<uint32_t> DecodeUtf8(std::string_view text)
		{
			std::vector<uint32_t> codepoints;
			codepoints.reserve(text.size());
			for (size_t i = 0; i < text.size();)
			{
				const unsigned char lead = static_cast<unsigned char>(text[i]);
				uint32_t codepoint = lead;
				size_t length = 1;
				if (lead >= 0xF0)
				{
					codepoint = lead & 0x07u;
					length = 4;
				}
				else if (lead >= 0xE0)
				{
					codepoint = lead & 0x0Fu;
					length = 3;
				}
				else if (lead >= 0xC0)
				{
					codepoint = lead & 0x1Fu;
					length = 2;
				}
				if (i + length > text.size())
					length = 1;   // 截断序列:首字节按单字节码点,后续字节各自处理
				if (length > 1)
				{
					bool valid = true;
					for (size_t k = 1; k < length; ++k)
					{
						const unsigned char continuation = static_cast<unsigned char>(text[i + k]);
						if ((continuation & 0xC0u) != 0x80u)
						{
							valid = false;
							break;
						}
						codepoint = (codepoint << 6) | (continuation & 0x3Fu);
					}
					if (!valid)
					{
						codepoint = lead;
						length = 1;
					}
				}
				codepoints.push_back(codepoint);
				i += length;
			}
			return codepoints;
		}
	}

	WuiScriptedInput& WuiScriptedInput::Get()
	{
		static WuiScriptedInput instance;
		return instance;
	}

	void WuiScriptedInput::QueueClick(const std::string& windowKey, glm::vec2 position)
	{
		Pending& pending = m_Pending[windowKey];
		pending.Position = position;
		pending.Phase = 0;
		pending.FramesLeft = 2;   // press 帧 + release 帧
		// 同一窗口同时只有一份待注入输入:新的点击丢弃上一份还没注入完的文本。
		// (ui.type = QueueClick + QueueType,调用顺序保证文本挂在本次点击之后。)
		pending.TextFrames.clear();
		pending.NextTextFrame = 0;
	}

	void WuiScriptedInput::QueueType(const std::string& windowKey, std::string text)
	{
		Pending& pending = m_Pending[windowKey];
		// 文本挂在本次待注入输入的后半段:没有待注入点击时(纯文本窗口)下一帧就开始写。
		pending.TextFrames.clear();
		pending.NextTextFrame = 0;
		size_t lineStart = 0;
		for (;;)
		{
			const size_t newline = text.find('\n', lineStart);
			const size_t lineEnd = newline == std::string::npos ? text.size() : newline;
			std::string_view line(text.data() + lineStart, lineEnd - lineStart);
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);   // CRLF:'\r' 并入换行,不写进 TextInput
			pending.TextFrames.push_back(DecodeUtf8(line));
			if (newline == std::string::npos)
				break;
			lineStart = newline + 1;
		}
	}

	bool WuiScriptedInput::HasPending() const
	{
		for (const auto& entry : m_Pending)
			if (entry.second.FramesLeft > 0 || entry.second.NextTextFrame < entry.second.TextFrames.size())
				return true;
		return false;
	}

	void WuiScriptedInput::Apply(const std::string& windowKey, WuiInputState& input)
	{
		auto entry = m_Pending.find(windowKey);
		if (entry == m_Pending.end())
			return;
		Pending& pending = entry->second;
		if (pending.FramesLeft > 0)
		{
			// 点击阶段:第 1 帧 press、第 2 帧 release(与鼠标操作一致的帧序列)。
			// 注入期间把键盘焦点交给 WUI,否则菜单/下拉的"点外关闭"逻辑会先把它关掉。
			input.MousePos = pending.Position;
			input.WantKeyboard = true;
			if (pending.Phase == 0)
			{
				input.MouseDown[0] = true;
				input.MouseClicked[0] = true;
			}
			else
			{
				input.MouseDown[0] = false;
				input.MouseReleased[0] = true;
			}
			++pending.Phase;
			--pending.FramesLeft;
			if (pending.FramesLeft <= 0 && pending.NextTextFrame >= pending.TextFrames.size())
				m_Pending.erase(entry);
			return;   // 文本在点击完成后的下一帧才注入,不和点击同帧
		}
		if (pending.NextTextFrame < pending.TextFrames.size())
		{
			// 文本阶段:每帧一段;第 2 段起先注入 Enter 键再写该行码点,多行内容因此
			// 走控件自己的换行路径(与真实键入一致),而不是把 '\n' 当字符塞给控件。
			input.WantKeyboard = true;
			if (pending.NextTextFrame > 0)
				input.KeyDown.push_back(KeyCodes::Enter);
			const std::vector<uint32_t>& codepoints = pending.TextFrames[pending.NextTextFrame];
			input.TextInput.insert(input.TextInput.end(), codepoints.begin(), codepoints.end());
			++pending.NextTextFrame;
			if (pending.NextTextFrame >= pending.TextFrames.size())
				m_Pending.erase(entry);
			return;
		}
		m_Pending.erase(entry);   // 没有剩余事件:不留下空壳
	}
}
