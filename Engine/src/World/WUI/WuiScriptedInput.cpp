#include "wldpch.h"
#include "World/WUI/WuiScriptedInput.h"

#include "World/Core/KeyCodes.h"

#include <algorithm>
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

	void WuiScriptedInput::QueueClick(const std::string& windowKey, glm::vec2 position, int button,
	bool ctrl, bool shift)
	{
		Pending& pending = m_Pending[windowKey];
		pending.Position = position;
		pending.Button = std::clamp(button, 0, 2);
	// 修饰键随这次点击一起注入(两帧都保持),点击结束自然消失。
	pending.ClickCtrl = ctrl;
	pending.ClickShift = shift;
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
			if (entry.second.FramesLeft > 0 || entry.second.KeyPhase > 0
				|| entry.second.WheelFrames > 0
				|| entry.second.NextTextFrame < entry.second.TextFrames.size())
				return true;
		return false;
	}

	void WuiScriptedInput::QueueKey(const std::string& windowKey, uint32_t keyCode, bool ctrl, bool shift,
		int holdFrames)
	{
		Pending& pending = m_Pending[windowKey];
		pending.Key = keyCode;
		pending.KeyPhase = 1;
		pending.KeyHoldFrames = std::max(0, holdFrames);
		pending.KeyCtrl = ctrl;
		pending.KeyShift = shift;
	}

	void WuiScriptedInput::QueueWheel(const std::string& windowKey, glm::vec2 position, float wheel)
	{
		Pending& pending = m_Pending[windowKey];
		pending.Position = position;
		pending.Wheel = wheel;
		pending.WheelFrames = 1;
	}

	void WuiScriptedInput::BeginFrame()
{
	// 新的一帧:上一帧的快照作废(相位已在上一帧推进过,这里不推进)。
	++m_Serial;
	m_FrameSnapshots.clear();
}

// 把本帧待注入的输入写进 `input`(同一帧可被多个消费者各调一次)。语义见头文件。
void WuiScriptedInput::Apply(const std::string& windowKey, WuiInputState& input)
{
	// 没调用过 BeginFrame 的调用方(逐帧 Apply 的单元测试)保持旧语义:一次调用推进一帧。
	if (m_Serial == 0)
	{
		bool pointerPosition = false;
		AdvancePhase(windowKey, input, pointerPosition);
		return;
	}
	// 本帧第一次读 ⇒ 推进相位并把结果记成快照;同帧后续消费者只读同一份。
	auto snapshot = m_FrameSnapshots.find(windowKey);
	if (snapshot == m_FrameSnapshots.end())
	{
		FrameInjection injection;
		AdvancePhase(windowKey, injection.State, injection.PointerPosition);
		snapshot = m_FrameSnapshots.emplace(windowKey, std::move(injection)).first;
	}
	MergeInjection(snapshot->second, input);
}

// 把一次注入并入调用方状态:指针位置**覆盖**(注入优先),按钮边沿取并集,
// 按键/文本追加,修饰键与 WantKeyboard 取或 —— 不碰平台轮询写进去的其它字段。
void WuiScriptedInput::MergeInjection(const FrameInjection& injection, WuiInputState& input)
{
	const WuiInputState& source = injection.State;
	if (injection.PointerPosition)
		input.MousePos = source.MousePos;
	for (int button = 0; button < 3; ++button)
	{
		input.MouseDown[button] = input.MouseDown[button] || source.MouseDown[button];
		input.MouseClicked[button] = input.MouseClicked[button] || source.MouseClicked[button];
		input.MouseReleased[button] = input.MouseReleased[button] || source.MouseReleased[button];
	}
	input.Wheel += source.Wheel;
	input.Ctrl = input.Ctrl || source.Ctrl;
	input.Shift = input.Shift || source.Shift;
	input.WantKeyboard = input.WantKeyboard || source.WantKeyboard;
	input.KeyDown.insert(input.KeyDown.end(), source.KeyDown.begin(), source.KeyDown.end());
	input.KeyPressed.insert(input.KeyPressed.end(), source.KeyPressed.begin(), source.KeyPressed.end());
	input.TextInput.insert(input.TextInput.end(), source.TextInput.begin(), source.TextInput.end());
}

// 推进注入相位一帧(旧的 Apply 语义:一次调用 = 一帧,写进 `out`)。
void WuiScriptedInput::AdvancePhase(const std::string& windowKey, WuiInputState& input, bool& outPointerPosition)
	{
		auto entry = m_Pending.find(windowKey);
		if (entry == m_Pending.end())
			return;
		Pending& pending = entry->second;
		if (pending.WheelFrames > 0)
		{
			// 滚轮单独占一帧(位置 + Wheel),不与点击/文本同帧:控件读到的就是普通滚轮输入。
			input.MousePos = pending.Position;
			outPointerPosition = true;   // 位置由注入给出(合并时覆盖平台轮询)
			input.Wheel = pending.Wheel;
			input.WantKeyboard = true;
			if (std::getenv("WLD_TRACE_UI"))
				WLD_CORE_INFO("[dev] scripted wheel {0} at ({1},{2}) window={3}", pending.Wheel,
					static_cast<int>(pending.Position.x), static_cast<int>(pending.Position.y), windowKey);
			--pending.WheelFrames;
			if (pending.WheelFrames <= 0 && pending.FramesLeft <= 0 && pending.KeyPhase == 0
				&& pending.NextTextFrame >= pending.TextFrames.size())
				m_Pending.erase(entry);
			return;
		}
		if (pending.KeyPhase > 0)
		{
			input.WantKeyboard = true;
			// 组合键:按下的那一帧与释放的那一帧都保持修饰键按下(与真实"Ctrl↓ → A↓ → A↑ → Ctrl↑"一致)。
			input.Ctrl = pending.KeyCtrl;
			input.Shift = pending.KeyShift;
			if (pending.KeyPhase == 1)
			{
				input.KeyDown.push_back(pending.Key);
				input.KeyPressed.push_back(pending.Key);
				pending.KeyPhase = pending.KeyHoldFrames > 0 ? 3 : 2;
			}
			else if (pending.KeyPhase == 3)
			{
				// 按住:保持 KeyDown(控件侧"按住每帧触发"的路径照旧能读到),不发 KeyPressed /
				// KeyRepeated —— 一次真人敲键横跨几帧,但通常还没到系统重复延迟。
				input.KeyDown.push_back(pending.Key);
				if (--pending.KeyHoldFrames <= 0)
					pending.KeyPhase = 2;
			}
			else
			{
				pending.KeyPhase = 0;
				pending.Key = 0;
				pending.KeyHoldFrames = 0;
				pending.KeyCtrl = false;
				pending.KeyShift = false;
			}
			return;   // 按键注入独立占一帧:不与点击/文本同帧,时序更接近真实键盘
		}
		if (pending.FramesLeft > 0)
		{
			// 点击阶段:第 1 帧 press、第 2 帧 release(与鼠标操作一致的帧序列)。
			// 注入期间把键盘焦点交给 WUI,否则菜单/下拉的"点外关闭"逻辑会先把它关掉。
			// 按键由注入时指定的 button 决定(0 = 左键,1 = 右键):写进对应的下标,
			// 控件侧走的就是普通鼠标路径。
			const int button = std::clamp(pending.Button, 0, 2);
			// 修饰键:与真人按住 Ctrl/Shift 点一下同一口径(面板据此走"加选 / 等比"分支)。
			input.Ctrl = pending.ClickCtrl;
			input.Shift = pending.ClickShift;
			input.MousePos = pending.Position;
			outPointerPosition = true;   // 位置由注入给出(合并时覆盖平台轮询)
			input.WantKeyboard = true;
			if (pending.Phase == 0)
			{
				input.MouseDown[button] = true;
				input.MouseClicked[button] = true;
			}
			else
			{
				input.MouseDown[button] = false;
				input.MouseReleased[button] = true;
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
			{
				input.KeyDown.push_back(KeyCodes::Enter);
				// 沿:代码编辑器等"一次按键=一次动作"的控件按 KeyPressed 消费(电平会跨帧重复)。
				input.KeyPressed.push_back(KeyCodes::Enter);
			}
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
