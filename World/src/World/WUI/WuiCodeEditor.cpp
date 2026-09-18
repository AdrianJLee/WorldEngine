#include "wldpch.h"
#include "World/WUI/WuiCodeEditor.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"

#include <algorithm>
#include <cmath>

namespace World::Wui
{
	namespace
	{
		constexpr float kScrollbarWidth = 10.0f;
		constexpr float kGutterPaddingLeft = 6.0f;
		constexpr float kGutterPaddingRight = 8.0f;
		constexpr float kThumbMinHeight = 24.0f;

		const WuiColor kBackground { 0.10f, 0.105f, 0.11f, 1.0f };
		const WuiColor kGutterBackground { 0.13f, 0.135f, 0.14f, 1.0f };
		const WuiColor kGutterText { 0.44f, 0.46f, 0.50f, 1.0f };
		const WuiColor kGutterTextCurrent { 0.78f, 0.81f, 0.85f, 1.0f };
		const WuiColor kCurrentLine { 1.0f, 1.0f, 1.0f, 0.05f };
		const WuiColor kCaretColor { 0.90f, 0.92f, 0.95f, 1.0f };
		const WuiColor kScrollbarTrack { 0.15f, 0.155f, 0.16f, 1.0f };
		const WuiColor kScrollbarThumb { 0.33f, 0.35f, 0.38f, 1.0f };
		const WuiColor kSuggestBackground { 0.145f, 0.152f, 0.165f, 0.985f };
		const WuiColor kSuggestBorder { 0.30f, 0.32f, 0.36f, 1.0f };
		const WuiColor kSuggestSelected { 0.20f, 0.30f, 0.46f, 1.0f };
		const WuiColor kSuggestName { 0.86f, 0.89f, 0.93f, 1.0f };
		const WuiColor kSuggestType { 0.55f, 0.58f, 0.63f, 1.0f };
		const WuiColor kSuggestDoc { 0.66f, 0.69f, 0.74f, 1.0f };
		const WuiColor kTokenColors[7] = {
			{ 0.82f, 0.84f, 0.87f, 1.0f },
			{ 0.45f, 0.62f, 0.95f, 1.0f },
			{ 0.62f, 0.78f, 0.45f, 1.0f },
			{ 0.45f, 0.50f, 0.45f, 1.0f },
			{ 0.85f, 0.70f, 0.45f, 1.0f },
			{ 0.70f, 0.60f, 0.95f, 1.0f },
			{ 0.75f, 0.75f, 0.80f, 1.0f },
		};

		// W9.5 补全浮层:最多 12 行可滚动 + 底部一行 Doc。
		constexpr int kSuggestMaxRows = 12;
		constexpr float kSuggestRowHeight = 22.0f;
		constexpr float kSuggestWidth = 320.0f;
		constexpr float kSuggestFontSize = 13.0f;
		constexpr float kSuggestDocFontSize = 12.0f;

		struct WuiCodeEditorState
		{
			float ScrollY = 0.0f;
			bool MouseSelecting = false;
			bool DraggingThumb = false;
			float ThumbGrabOffset = 0.0f;
			bool FollowCaret = true;
			// ---- W9.5 补全浮层(状态必须跨帧:打开/选中/滚动)----
			bool PopupVisible = false;
			std::size_t ReplaceStart = 0;   // 前缀起点(buffer 字节偏移)
			std::size_t CaretAtOpen = 0;    // 弹出时的 caret(buffer 字节偏移)
			std::vector<World::LuauCompletionItem> PopupItems;
			std::string PopupLinePrefix;    // 打开浮层时的 linePrefix(无障碍状态用)
			float PopupAnchorX = 0.0f;      // 浮层锚点(caret 左上角),打开时固定避免逐帧漂移
			float PopupAnchorY = 0.0f;
			int PopupSelected = 0;
			int PopupScroll = 0;
			bool PopupVisibleLastFrame = false;
			WuiRect PopupBounds {};
		};

		size_t CodepointLength(unsigned char lead)
		{
			if (lead >= 0xF0)
				return 4;
			if (lead >= 0xE0)
				return 3;
			if (lead >= 0xC0)
				return 2;
			return 1;
		}

		uint32_t DecodeCodepoint(std::string_view text, size_t offset, size_t& length)
		{
			const unsigned char lead = static_cast<unsigned char>(text[offset]);
			length = CodepointLength(lead);
			if (offset + length > text.size())
			{
				length = 1;
				return 0xFFFD;
			}
			uint32_t codepoint = 0;
			switch (length)
			{
				case 1: codepoint = lead; break;
				case 2: codepoint = lead & 0x1Fu; break;
				case 3: codepoint = lead & 0x0Fu; break;
				default: codepoint = lead & 0x07u; break;
			}
			for (size_t i = 1; i < length; ++i)
				codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[offset + i]) & 0x3Fu);
			return codepoint;
		}

		void EncodeUtf8(std::string& out, uint32_t codepoint)
		{
			if (codepoint < 0x80)
				out.push_back(static_cast<char>(codepoint));
			else if (codepoint < 0x800)
			{
				out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else if (codepoint < 0x10000)
			{
				out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
		}

		bool IsWordCodepoint(uint32_t codepoint)
		{
			return codepoint >= 0x80
				|| (codepoint >= '0' && codepoint <= '9')
				|| (codepoint >= 'A' && codepoint <= 'Z')
				|| (codepoint >= 'a' && codepoint <= 'z')
				|| codepoint == '_';
		}

		// 补全前缀标识符字符(与 Luau 标识符一致的 ASCII 子集)。
		bool IsIdentByte(char c)
		{
			return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
				|| (c >= 'a' && c <= 'z') || c == '_';
		}

		// 接受补全时的插入形态:方法/函数插 name() 且 caret 落括号内;其余插 name。
		bool InsertAsCall(const World::LuauCompletionItem& item)
		{
			if (item.Kind == World::LuauCompletionItem::KindType::Method)
				return true;
			return item.Type.find("fun") != std::string::npos
				|| item.Name.find('(') != std::string::npos;
		}

		// 浮层里的名字去掉 "name(...)" 写法,只留标识符(插入时按需补 "()")。
		std::string CompletionInsertName(std::string name)
		{
			const std::size_t paren = name.find('(');
			if (paren != std::string::npos)
				name.erase(paren);
			return name;
		}

		// 前缀不足时截断到字节预算(UTF-8 边界;不做精确像素裁剪)。
		std::string TruncateBytes(const std::string& text, std::size_t maxBytes)
		{
			if (text.size() <= maxBytes)
				return text;
			std::size_t cut = maxBytes;
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
				--cut;
			return text.substr(0, cut) + "…";
		}

		// 真实度量的像素 → 行内字节偏移(命中测试,替代旧的 CursorAtX 启发式)。
		size_t OffsetAtX(const WuiContext& ctx, std::string_view line, float x, float fontSize)
		{
			float pen = 0.0f;
			size_t offset = 0;
			while (offset < line.size())
			{
				const size_t length = std::min(CodepointLength(static_cast<unsigned char>(line[offset])), line.size() - offset);
				const float width = ctx.MeasureTextWidth(line.substr(offset, length), fontSize, WuiFontFamily::Monospace);
				if (width > 0.0f && x < pen + width * 0.5f)
					return offset;
				pen += width;
				offset += length;
			}
			return line.size();
		}

		void SelectWordAt(WuiTextBuffer& buffer, const std::string& text, size_t offset)
		{
			offset = std::min(offset, text.size());
			size_t start = offset;
			while (start > 0)
			{
				const size_t prev = WuiTextBuffer::PrevCodepointBoundary(text, start);
				size_t length = 1;
				const uint32_t codepoint = DecodeCodepoint(text, prev, length);
				if (!IsWordCodepoint(codepoint))
					break;
				start = prev;
			}
			size_t end = offset;
			while (end < text.size())
			{
				size_t length = 1;
				const uint32_t codepoint = DecodeCodepoint(text, end, length);
				if (!IsWordCodepoint(codepoint))
					break;
				end += length;
			}
			if (end <= start)
				return;
			buffer.SetCaret(start, false);
			buffer.SetCaret(end, true);
		}

		std::string_view LineView(const WuiTextBuffer& buffer, int line, size_t& lineStart)
		{
			const auto [start, end] = buffer.LineRange(line);
			lineStart = start;
			std::string_view view(buffer.Text().data() + start, end - start);
			if (!view.empty() && view.back() == '\r')
				view.remove_suffix(1);
			return view;
		}

		// 前缀起点:caret 往回走连续标识符字符(不含 '.'/':');紧邻非标识符时返回 caret
		// (空前缀,如刚敲下 '.')。
		std::size_t CompletionReplaceStart(std::string_view text, std::size_t caret)
		{
			caret = std::min(caret, text.size());
			std::size_t start = caret;
			while (start > 0 && IsIdentByte(text[start - 1]))
				--start;
			return start;
		}
	}

	WuiCodeEditorResult CodeEditor(WuiContext& ctx, WuiId id, const WuiRect& rect,
		WuiTextBuffer& buffer, const WuiCodeEditorOptions& options)
	{
		WuiCodeEditorResult result;
		if (rect.W <= 0.0f || rect.H <= 0.0f)
			return result;

		WuiCodeEditorState& state = ctx.Persist<WuiCodeEditorState>(id, {});
		const float fontSize = options.FontSize > 0.0f ? options.FontSize : 14.0f;
		const float lineHeight = options.LineHeight > 0.0f ? options.LineHeight : 20.0f;
		const uint64_t revisionAtStart = buffer.Revision();
		const int lineCount = std::max(1, buffer.LineCount());
		const float digitWidth = ctx.MeasureTextWidth("0", fontSize, WuiFontFamily::Monospace);
		const float gutterWidth = options.GutterWidth > 0.0f
			? options.GutterWidth
			: kGutterPaddingLeft + digitWidth * static_cast<float>(std::to_string(lineCount).size()) + kGutterPaddingRight;
		const float scrollbarWidth = rect.W > gutterWidth + kScrollbarWidth ? kScrollbarWidth : 0.0f;
		const WuiRect textRect { rect.X + gutterWidth, rect.Y,
			std::max(0.0f, rect.W - gutterWidth - scrollbarWidth), rect.H };
		const WuiRect track { rect.X + rect.W - kScrollbarWidth, rect.Y, kScrollbarWidth, rect.H };
		const float contentHeight = static_cast<float>(lineCount) * lineHeight;
		const float maxScroll = std::max(0.0f, contentHeight - rect.H);
		const bool needScrollbar = scrollbarWidth > 0.0f && maxScroll > 0.0f;
		bool focused = ctx.Focus() == id;
		const bool readOnly = options.ReadOnly;
		const WuiInputState& input = ctx.Input();
		// 本帧开始时的浮层可见性(帧内会被"打字关闭/点外关闭"改写,刷新判断要用这个)。
		const bool popupVisibleAtFrameStart = state.PopupVisible;

		// 命中:行 = 鼠标 y 对应的行,列 = 该行内真实度量得到的字节偏移。
		const auto HitOffset = [&](float mouseY, float mouseX)
		{
			int line = static_cast<int>(std::floor((mouseY - textRect.Y + state.ScrollY) / lineHeight));
			line = std::max(0, std::min(line, lineCount - 1));
			size_t lineStart = 0;
			const std::string_view view = LineView(buffer, line, lineStart);
			const float x = std::max(0.0f, mouseX - textRect.X);
			return lineStart + OffsetAtX(ctx, view, x, fontSize);
		};

		// ---- 鼠标:点击定位 / 拖拽选择 / 双击选词 ----
		if (input.MouseClicked[0] && ctx.IsHovered(textRect))
		{
			ctx.SetFocus(id);
			focused = true;
			const size_t offset = HitOffset(input.MousePos.y, input.MousePos.x);
			if (input.MouseDoubleClicked[0])
			{
				SelectWordAt(buffer, buffer.Text(), offset);
				state.MouseSelecting = false;
			}
			else
			{
				buffer.SetCaret(offset, input.Shift);
				state.MouseSelecting = true;
			}
			state.FollowCaret = true;
		}
		if (state.MouseSelecting && input.MouseDown[0])
		{
			buffer.SetCaret(HitOffset(input.MousePos.y, input.MousePos.x), true);
			state.FollowCaret = true;
		}
		if (state.MouseSelecting && input.MouseReleased[0])
			state.MouseSelecting = false;
		// 浮层打开时点击编辑区空白(浮层矩形之外)= 关闭浮层;点击候选行稍后处理。
		if (state.PopupVisible && input.MouseClicked[0]
			&& !state.PopupBounds.Contains(input.MousePos))
			state.PopupVisible = false;
		if (focused && input.MouseClicked[0] && !ctx.IsHovered(rect))
		{
			ctx.SetFocus(0);
			ctx.SetTextInputActive(false);
			focused = false;
			state.PopupVisible = false;
		}

		// ---- 滚轮(悬停文本区):只滚动,不动 caret ----
		// Ctrl+滚轮留给宿主做字号缩放(见 ScriptEditorPanel):这里不滚动。
		// 浮层内滚轮由下面的浮层逻辑消费(滚动列表,不滚编辑器)。
		const bool wheelOverPopup = state.PopupVisible
			&& state.PopupBounds.Contains(input.MousePos);
		if (ctx.IsHovered(textRect) && input.Wheel != 0.0f && !input.Ctrl && !wheelOverPopup)
			state.ScrollY -= input.Wheel * lineHeight * 3.0f;

		// ---- W9.5 补全触发:本帧插入可见字符、TextInput 含 '.'/':'、或 Ctrl+Space 沿 ----
		struct CompletionRequest
		{
			bool Wanted = false;
			bool CtrlSpace = false;
		};
		CompletionRequest completionRequest;

		// ---- 键盘(仅聚焦时;浮层可见时下列键优先被浮层消费,caret 不动)----
		if (focused)
		{
			ctx.SetTextInputActive(true);
			if (ctx.IsHovered(textRect))
				ctx.SetCursor(WuiCursor::IBeam);

			// Esc 的双重语义:浮层可见时只关浮层、保持焦点;浮层不可见时才失焦。
			const bool escapeTriggered = ctx.WasKeyTriggered(KeyCodes::Escape);
			bool escapeHandledByPopup = false;
			bool popupConsumedKey = false;
			if (state.PopupVisible)
			{
				const int count = static_cast<int>(state.PopupItems.size());
				// 浮层可见时普通字符仍然照常输入(输入后重新过滤候选);这里只消费
				// 导航/接受/关闭键。Enter/Tab 的按键分支在下面统一处理。
				if (escapeTriggered)
				{
					// Esc 只关浮层:保持文本焦点(浮层不可见时 Esc 仍是失焦)。
					state.PopupVisible = false;
					escapeHandledByPopup = true;
				}
				else if (ctx.WasKeyTriggered(KeyCodes::Up))
				{
					popupConsumedKey = true;
					if (count > 0)
						state.PopupSelected = (state.PopupSelected - 1 + count) % count;
					state.PopupScroll = std::max(0, std::min(
						std::max(0, count - kSuggestMaxRows), state.PopupSelected - kSuggestMaxRows / 2));
				}
				else if (ctx.WasKeyTriggered(KeyCodes::Down))
				{
					popupConsumedKey = true;
					if (count > 0)
						state.PopupSelected = (state.PopupSelected + 1) % count;
					state.PopupScroll = std::max(0, std::min(
						std::max(0, count - kSuggestMaxRows), state.PopupSelected - kSuggestMaxRows / 2));
				}
				else if (ctx.WasKeyTriggered(KeyCodes::PageUp))
				{
					popupConsumedKey = true;
					state.PopupSelected = std::max(0, state.PopupSelected - kSuggestMaxRows);
					state.PopupScroll = std::max(0, std::min(
						std::max(0, count - kSuggestMaxRows), state.PopupSelected - kSuggestMaxRows / 2));
				}
				else if (ctx.WasKeyTriggered(KeyCodes::PageDown))
				{
					popupConsumedKey = true;
					if (count > 0)
						state.PopupSelected = std::min(count - 1, state.PopupSelected + kSuggestMaxRows);
					state.PopupScroll = std::max(0, std::min(
						std::max(0, count - kSuggestMaxRows), state.PopupSelected - kSuggestMaxRows / 2));
				}
			}
			// 接受:浮层可见时 Enter/Tab 优先给补全(不换行/不缩进)。
			bool acceptedCompletion = false;
			if (state.PopupVisible
				&& (ctx.WasKeyTriggered(KeyCodes::Enter) || ctx.WasKeyTriggered(KeyCodes::Tab)))
			{
				acceptedCompletion = true;
				const std::string& text = buffer.Text();
				const std::size_t caretNow = std::min(buffer.Caret(), text.size());
				const std::size_t replaceStart = std::min(state.ReplaceStart, caretNow);
				const int count = static_cast<int>(state.PopupItems.size());
				if (state.PopupSelected >= 0 && state.PopupSelected < count)
				{
					const World::LuauCompletionItem& item =
						state.PopupItems[static_cast<std::size_t>(state.PopupSelected)];
					std::string inserted = CompletionInsertName(item.Name);
					const bool call = InsertAsCall(item);
					if (call && caretNow >= text.size())
						inserted += "(";   // caret 在行尾:补 "(" 等待后续参数
					buffer.SetCaret(replaceStart, false);
					buffer.SetCaret(caretNow, true);
					buffer.InsertAtCaret(inserted);
					if (call)
					{
						const std::size_t afterName = replaceStart + inserted.size();
						if (afterName > text.size() || text[afterName] != ')')
						{
							const std::size_t caretAfterName = buffer.Caret();
							buffer.InsertAtCaret(")");
							buffer.SetCaret(caretAfterName, false);
						}
					}
				}
				state.PopupVisible = false;
				state.FollowCaret = true;
			}
			if (escapeTriggered && !escapeHandledByPopup && focused)
			{
				// Escape 失焦:后续输入回到引擎全局快捷键。
				ctx.SetFocus(0);
				ctx.SetTextInputActive(false);
				focused = false;
			}
			// 可见字符:始终输入(浮层打开时先关闭,随后由触发逻辑重新查询)。
			// 这保证普通打字永远不会被浮层吞掉(实测:多行注入含 '.' 时曾丢字符)。
			const bool popupConsuming = popupConsumedKey || acceptedCompletion;
			bool caretAction = false;
			// 普通编辑/导航键;被浮层消费的键(Up/Down/PageUp/PageDown/接受)不再落到 buffer。
			if (!popupConsuming)
			{
				const bool ctrl = input.Ctrl;
				const bool shift = input.Shift;
				const int pageLines = std::max(1, static_cast<int>(rect.H / lineHeight) - 1);
				if (ctrl)
				{
					if (ctx.WasKeyTriggered(KeyCodes::S) && !readOnly)
						result.SaveRequested = true;
					if (ctx.WasKeyTriggered(KeyCodes::A))
						buffer.SelectAll();
					if (ctx.WasKeyTriggered(KeyCodes::C) && options.GetClipboard)
					{
						std::string selection;
						if (buffer.Copy(selection) && options.SetClipboard)
							options.SetClipboard(selection);
					}
					if (!readOnly && ctx.WasKeyTriggered(KeyCodes::X))
					{
						std::string selection;
						if (buffer.Cut(selection) && options.SetClipboard)
							options.SetClipboard(selection);
						caretAction = true;
					}
					if (!readOnly && ctx.WasKeyTriggered(KeyCodes::V) && options.GetClipboard)
					{
						std::string clip;
						if (options.GetClipboard(clip))
							buffer.Paste(clip);
						caretAction = true;
					}
					if (!readOnly && ctx.WasKeyTriggered(KeyCodes::Z))
					{
						shift ? buffer.Redo() : buffer.Undo();
						caretAction = true;
					}
					if (!readOnly && ctx.WasKeyTriggered(KeyCodes::Y))
					{
						buffer.Redo();
						caretAction = true;
					}
				}
				// 编辑键;ReadOnly 只导航/选择/复制。
				if (!readOnly && !ctrl)
				{
					if (ctx.WasKeyTriggered(KeyCodes::Enter))
					{
						buffer.InsertNewline();
						caretAction = true;
					}
					if (ctx.WasKeyTriggered(KeyCodes::Backspace))
					{
						buffer.Backspace();
						caretAction = true;
					}
					if (ctx.WasKeyTriggered(KeyCodes::Delete))
					{
						buffer.DeleteForward();
						caretAction = true;
					}
					if (ctx.WasKeyTriggered(KeyCodes::Tab))
					{
						buffer.IndentSelection(shift);
						caretAction = true;
					}
				}
				// 导航键(ReadOnly 同样可用)。
				if (ctx.WasKeyTriggered(KeyCodes::Left))
				{
					buffer.MoveCaret(ctrl ? WuiTextBuffer::Motion::WordLeft : WuiTextBuffer::Motion::Left, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::Right))
				{
					buffer.MoveCaret(ctrl ? WuiTextBuffer::Motion::WordRight : WuiTextBuffer::Motion::Right, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::Up))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::Up, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::Down))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::Down, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::Home))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::LineStart, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::End))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::LineEnd, shift);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::PageUp))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::PageUp, shift, pageLines);
					caretAction = true;
				}
				if (ctx.WasKeyTriggered(KeyCodes::PageDown))
				{
					buffer.MoveCaret(WuiTextBuffer::Motion::PageDown, shift, pageLines);
					caretAction = true;
				}
			}
			if (caretAction)
				state.FollowCaret = true;
		}
		else if (ctx.IsHovered(textRect))
		{
			ctx.SetCursor(WuiCursor::IBeam);
		}

		// ---- 可见字符:始终插入(浮层打开时先关闭,随后按新前缀重新查询)----
		// 注意顺序:必须在补全查询**之前**插入并刷新 caret,否则 linePrefix 是插入前的行内容。
		bool keepPopupOpen = false;
		bool textHadSeparator = false;
		if (focused && !readOnly && !input.Ctrl)
		{
			bool typedVisible = false;
			for (uint32_t codepoint : input.TextInput)
			{
				// Enter/Tab/Escape 的 char 事件走按键分支,这里只收可见字符。
				if (codepoint < 0x20 || codepoint == 0x7F)
					continue;
				std::string encoded;
				EncodeUtf8(encoded, codepoint);
				buffer.InsertAtCaret(encoded);
				typedVisible = true;
				if (codepoint == '.' || codepoint == ':')
					textHadSeparator = true;
			}
			if (typedVisible)
			{
				// 浮层原本打开 → 输入后按新前缀刷新;原本关闭 → 输入字母不重新弹出
				// (只有下面的 '.'/':' 分支才自动弹)。
				keepPopupOpen = popupVisibleAtFrameStart;
				state.PopupVisible = false;
				state.FollowCaret = true;
			}
		}
		// 补全触发:Ctrl+Space 手动;'.'/':' 自动;普通打字只在浮层已打开时刷新候选。
		if (focused && input.Ctrl && ctx.WasKeyTriggered(KeyCodes::Space))
			completionRequest = { true, true };
		else if (focused && (textHadSeparator || keepPopupOpen))
			completionRequest = { true, false };

		// ---- W9.5 补全查询:字符串/注释内不弹(用高亮 token 判定,不数引号)----
		const int caretLine = buffer.LineOfOffset(buffer.Caret());
		size_t caretLineStart = 0;
		const std::string_view caretLineText = LineView(buffer, caretLine, caretLineStart);
		std::vector<WuiCodeToken> caretTokens;
		if (options.Highlight)
			options.Highlight(caretLineText, caretTokens);
		bool caretInStringOrComment = false;
		{
			const size_t localCaret = std::min(buffer.Caret(), caretLineStart + caretLineText.size())
				- caretLineStart;
			for (const WuiCodeToken& token : caretTokens)
			{
				if (token.Kind != WuiCodeTokenKind::String && token.Kind != WuiCodeTokenKind::Comment)
					continue;
				const size_t start = std::min<size_t>(token.StartByte, caretLineText.size());
				const size_t end = std::max(start, std::min<size_t>(token.EndByte, caretLineText.size()));
				if (localCaret > start && localCaret <= end)
				{
					caretInStringOrComment = true;
					break;
				}
			}
		}
		if (completionRequest.Wanted && focused && options.Completion && !readOnly && !caretInStringOrComment)
		{
			const std::size_t caretNow = std::min(buffer.Caret(), buffer.Text().size());
			const std::size_t replaceStart = CompletionReplaceStart(buffer.Text(), caretNow);
			std::string linePrefix;
			if (caretNow >= caretLineStart)
				linePrefix.assign(buffer.Text(), caretLineStart, caretNow - caretLineStart);
			std::vector<World::LuauCompletionItem> items;
			// 行首到现在的纯空白(空行/自动缩进)不查询:全量候选既没有信息量,又会让
			// 随后的 Enter 被当成"接受候选"(实测:注入多行文本时插入了 assert)。
			const bool meaningfulPrefix = !linePrefix.empty()
				&& (IsIdentByte(linePrefix.back()) || linePrefix.back() == '.' || linePrefix.back() == ':');
			if (meaningfulPrefix)
				options.Completion(linePrefix, items);
			if (!items.empty())
			{
				state.PopupVisible = true;
				state.ReplaceStart = replaceStart;
				state.CaretAtOpen = caretNow;
				state.PopupLinePrefix = linePrefix;
				state.PopupItems = std::move(items);
				state.PopupSelected = 0;
				state.PopupScroll = 0;
				// 锚点固定在打开/刷新时的 caret 位置:避免浮层逐帧漂移导致鼠标命中错位。
				{
					size_t anchorLineStart = 0;
					const std::string_view anchorLineText = LineView(buffer, caretLine, anchorLineStart);
					const std::size_t anchorLocal = std::min(caretNow,
						anchorLineStart + anchorLineText.size()) - anchorLineStart;
					state.PopupAnchorX = textRect.X + ctx.MeasureTextWidth(
						anchorLineText.substr(0, anchorLocal), fontSize, WuiFontFamily::Monospace);
					state.PopupAnchorY = textRect.Y + static_cast<float>(caretLine) * lineHeight
						- state.ScrollY;
				}
			}
			else
			{
				state.PopupVisible = false;   // 空候选不弹
				state.PopupLinePrefix.clear();
			}
		}
		// ReadOnly / 未聚焦 / 无 provider / 光标落在字符串·注释内:一律不弹。
		if (state.PopupVisible && (!focused || readOnly || !options.Completion || caretInStringOrComment))
			state.PopupVisible = false;

		// ---- caret 跟随:滚动到刚移动/编辑的 caret 行 ----
		if (state.FollowCaret && maxScroll > 0.0f)
		{
			const float caretTop = static_cast<float>(buffer.LineOfOffset(buffer.Caret())) * lineHeight;
			if (caretTop < state.ScrollY)
				state.ScrollY = caretTop;
			else if (caretTop + lineHeight > state.ScrollY + rect.H)
				state.ScrollY = caretTop + lineHeight - rect.H;
		}
		state.FollowCaret = false;
		state.ScrollY = std::max(0.0f, std::min(state.ScrollY, maxScroll));

		// ---- W9.5 补全浮层:位置(空间不足翻到上方)/鼠标 hover·点击/绘制/无障碍 ----
		{
			const int itemCount = static_cast<int>(state.PopupItems.size());
			if (state.PopupVisible && itemCount > 0)
			{
				// 锚点:打开/刷新时固定的 caret 位置(不是当前 caret,浮层不逐帧漂移)。
				const float caretX = state.PopupAnchorX;
				const float caretTop = state.PopupAnchorY;
				const bool showDoc = state.PopupSelected >= 0 && state.PopupSelected < itemCount
					&& !state.PopupItems[static_cast<std::size_t>(state.PopupSelected)].Doc.empty();
				const int rows = std::min(itemCount, kSuggestMaxRows);
				const float width = std::max(180.0f, std::min(kSuggestWidth, textRect.W));
				const float height = static_cast<float>(rows) * kSuggestRowHeight
					+ (showDoc ? 24.0f : 6.0f) + 6.0f;
				float x = std::min(caretX, textRect.X + textRect.W - width);
				x = std::max(textRect.X, x);
				float y = caretTop + lineHeight;   // caret 下方
				if (y + height > rect.Y + rect.H)
					y = caretTop - height;         // 空间不足 → 翻到上方
				if (y < rect.Y)
					y = std::max(rect.Y, rect.Y + rect.H - height);
				state.PopupBounds = { x, y, width, height };

				// 滚轮在浮层内滚动列表(不滚编辑器)。
				if (ctx.IsHovered(state.PopupBounds) && input.Wheel != 0.0f)
				{
					const int maxFirst = std::max(0, itemCount - kSuggestMaxRows);
					state.PopupSelected = std::max(0, std::min(itemCount - 1,
						state.PopupSelected - (input.Wheel > 0.0f ? 1 : -1)));
					state.PopupScroll = std::max(0, std::min(maxFirst,
						state.PopupSelected - kSuggestMaxRows / 2));
				}
				if (state.PopupScroll > itemCount - rows)
					state.PopupScroll = std::max(0, itemCount - rows);
				if (state.PopupScroll < 0)
					state.PopupScroll = 0;

				const float firstRowY = state.PopupBounds.Y + 3.0f;
				int hoveredItem = -1;
				for (int row = 0; row < rows; ++row)
				{
					const int itemIndex = state.PopupScroll + row;
					if (itemIndex >= itemCount)
						break;
					const WuiRect rowRect { state.PopupBounds.X,
						firstRowY + static_cast<float>(row) * kSuggestRowHeight,
						state.PopupBounds.W, kSuggestRowHeight };
					if (ctx.IsHovered(rowRect))
						hoveredItem = itemIndex;
				}
				if (hoveredItem >= 0)
				{
					state.PopupSelected = hoveredItem;
					ctx.SetCursor(WuiCursor::Hand);
					if (input.MouseClicked[0])
					{
						// 点击接受(替换前缀,caret 落插入内容之后)。用浮层打开时记录的区间,
						// 而不是当前 caret:点击可能先被文本命中测试重新定位 caret
						// (自动化 ui.invoke 点击节点中心时实测会发生)。
						const World::LuauCompletionItem& item =
							state.PopupItems[static_cast<std::size_t>(state.PopupSelected)];
						const std::string& text = buffer.Text();
						const std::size_t caretNow = std::min(state.CaretAtOpen, text.size());
						const std::size_t replaceStart = std::min(state.ReplaceStart, caretNow);
						std::string inserted = CompletionInsertName(item.Name);
						const bool call = InsertAsCall(item);
						if (call && caretNow >= text.size())
							inserted += "(";
						buffer.SetCaret(replaceStart, false);
						buffer.SetCaret(caretNow, true);
						buffer.InsertAtCaret(inserted);
						if (call)
						{
							const std::size_t afterName = replaceStart + inserted.size();
							if (afterName > text.size() || text[afterName] != ')')
							{
								const std::size_t caretAfterName = buffer.Caret();
								buffer.InsertAtCaret(")");
								buffer.SetCaret(caretAfterName, false);
							}
						}
						state.PopupVisible = false;
						state.FollowCaret = true;
					}
				}
				if (state.PopupVisible)
				{
					// 面板 + 行(名称左、类型右 muted、选中行高亮、底部一行 Doc)。
					ctx.Commands().push_back({ WuiDrawKind::Rect, state.PopupBounds, kSuggestBackground, 4.0f });
					ctx.Commands().push_back({ WuiDrawKind::RectOutline, state.PopupBounds, kSuggestBorder, 4.0f, 1.0f });
					for (int row = 0; row < rows; ++row)
					{
						const int itemIndex = state.PopupScroll + row;
						if (itemIndex >= itemCount)
							break;
						const World::LuauCompletionItem& item =
							state.PopupItems[static_cast<std::size_t>(itemIndex)];
						const WuiRect rowRect { state.PopupBounds.X + 3.0f,
							firstRowY + static_cast<float>(row) * kSuggestRowHeight,
							state.PopupBounds.W - 6.0f, kSuggestRowHeight };
						if (itemIndex == state.PopupSelected)
							ctx.Commands().push_back({ WuiDrawKind::Rect, rowRect, kSuggestSelected, 3.0f });
						const float textY = rowRect.Y + (kSuggestRowHeight - kSuggestFontSize) * 0.5f;
						{
							WuiDrawCommand nameCommand;
							nameCommand.Kind = WuiDrawKind::Text;
							nameCommand.Rect = { rowRect.X + 8.0f, textY, 0.0f, 0.0f };
							nameCommand.Color = kSuggestName;
							nameCommand.Text = TruncateBytes(item.Name, 34);
							nameCommand.FontSize = kSuggestFontSize;
							nameCommand.Family = WuiFontFamily::Monospace;
							ctx.Commands().push_back(std::move(nameCommand));
						}
						if (!item.Type.empty())
						{
							const float typeWidth = ctx.MeasureTextWidth(item.Type, kSuggestFontSize,
								WuiFontFamily::Monospace);
							WuiDrawCommand typeCommand;
							typeCommand.Kind = WuiDrawKind::Text;
							typeCommand.Rect = { rowRect.X + rowRect.W - 8.0f - typeWidth, textY, 0.0f, 0.0f };
							typeCommand.Color = kSuggestType;
							typeCommand.Text = TruncateBytes(item.Type, 24);
							typeCommand.FontSize = kSuggestFontSize;
							typeCommand.Family = WuiFontFamily::Monospace;
							ctx.Commands().push_back(std::move(typeCommand));
						}
					}
					if (showDoc)
					{
						const World::LuauCompletionItem& item =
							state.PopupItems[static_cast<std::size_t>(state.PopupSelected)];
						ctx.Commands().push_back({ WuiDrawKind::Rect,
							{ state.PopupBounds.X + 1.0f,
								state.PopupBounds.Y + state.PopupBounds.H - 25.0f,
								state.PopupBounds.W - 2.0f, 24.0f },
							{ 0.10f, 0.11f, 0.12f, 0.98f }, 0.0f });
						WuiDrawCommand docCommand;
						docCommand.Kind = WuiDrawKind::Text;
						docCommand.Rect = { state.PopupBounds.X + 8.0f,
							state.PopupBounds.Y + state.PopupBounds.H - 21.0f, 0.0f, 0.0f };
						docCommand.Color = kSuggestDoc;
						docCommand.Text = TruncateBytes(item.Doc, 60);
						docCommand.FontSize = kSuggestDocFontSize;
						docCommand.Family = WuiFontFamily::Ui;
						ctx.Commands().push_back(std::move(docCommand));
					}

					// 无障碍:仅浮层可见期间登记 <前缀>.<i>(Interactive,点击=接受)
					// 与 <前缀>.status(只读)。
					WuiAccessibility& accessibility = WuiAccessibility::Get();
					if (accessibility.Enabled())
					{
						const std::string prefix = options.CompletionIdPrefix.empty()
							? std::string("editor.suggest") : options.CompletionIdPrefix;
						for (int row = 0; row < rows; ++row)
						{
							const int itemIndex = state.PopupScroll + row;
							if (itemIndex >= itemCount)
								break;
							const World::LuauCompletionItem& item =
								state.PopupItems[static_cast<std::size_t>(itemIndex)];
							WuiAccessNode node;
							node.Id = HashId((prefix + "." + std::to_string(itemIndex)).c_str());
							node.Window = accessibility.CurrentWindow();
							node.Panel = accessibility.CurrentPanel();
							node.Kind = "suggest";
							node.Label = item.Name;
							node.Value = item.Type;
							node.Rect = { state.PopupBounds.X + 3.0f,
								firstRowY + static_cast<float>(row) * kSuggestRowHeight,
								state.PopupBounds.W - 6.0f, kSuggestRowHeight };
							node.Focused = itemIndex == state.PopupSelected;
							node.Interactive = true;
							accessibility.Register(node);
						}
						WuiAccessNode statusNode;
						statusNode.Id = HashId((prefix + ".status").c_str());
						statusNode.Window = accessibility.CurrentWindow();
						statusNode.Panel = accessibility.CurrentPanel();
						statusNode.Kind = "suggest";
						statusNode.Label = "completion status";
						statusNode.Value = "prefix '" + state.PopupLinePrefix + "', "
							+ std::to_string(itemCount) + " items, selected "
							+ (state.PopupSelected >= 0 && state.PopupSelected < itemCount
								? state.PopupItems[static_cast<std::size_t>(state.PopupSelected)].Name
								: std::string());
						statusNode.Rect = { state.PopupBounds.X, state.PopupBounds.Y,
							state.PopupBounds.W, state.PopupBounds.H };
						statusNode.Interactive = false;
						accessibility.Register(statusNode);
					}
				}
			}
			else
			{
				state.PopupVisible = false;
			}
			state.PopupVisibleLastFrame = state.PopupVisible;
		}

		// ---- 竖向滚动条:thumb 可拖拽,点击轨道翻页 ----
		float thumbHeight = 0.0f;
		float thumbY = 0.0f;
		if (needScrollbar)
		{
			thumbHeight = std::min(rect.H, std::max(kThumbMinHeight, rect.H * (rect.H / contentHeight)));
			float travel = std::max(1.0f, rect.H - thumbHeight);
			thumbY = rect.Y + (maxScroll > 0.0f ? state.ScrollY / maxScroll : 0.0f) * travel;
			const WuiRect thumb { track.X + 2.0f, thumbY, track.W - 4.0f, thumbHeight };
			if (input.MouseClicked[0] && ctx.IsHovered(track))
			{
				if (ctx.IsHovered(thumb))
				{
					state.DraggingThumb = true;
					state.ThumbGrabOffset = input.MousePos.y - thumbY;
				}
				else
				{
					state.ScrollY += (input.MousePos.y < thumbY ? -1.0f : 1.0f) * rect.H;
				}
			}
			if (state.DraggingThumb && input.MouseDown[0])
			{
				const float travelNow = std::max(1.0f, rect.H - thumbHeight);
				state.ScrollY = (input.MousePos.y - state.ThumbGrabOffset - rect.Y) / travelNow * maxScroll;
			}
			if (state.DraggingThumb && input.MouseReleased[0])
				state.DraggingThumb = false;
			state.ScrollY = std::max(0.0f, std::min(state.ScrollY, maxScroll));
			travel = std::max(1.0f, rect.H - thumbHeight);
			thumbY = rect.Y + (maxScroll > 0.0f ? state.ScrollY / maxScroll : 0.0f) * travel;
		}

		// ---- 绘制(只画可见行) ----
		const auto pushText = [&](float x, float y, const WuiColor& color, std::string text,
			int selStart, int selEnd, int caretByte)
		{
			WuiDrawCommand command;
			command.Kind = WuiDrawKind::Text;
			command.Rect = { x, y, 0.0f, 0.0f };
			command.Color = color;
			command.Text = std::move(text);
			command.FontSize = fontSize;
			command.Family = WuiFontFamily::Monospace;
			command.TextSelStart = selStart;
			command.TextSelEnd = selEnd;
			command.TextCaretByte = caretByte;
			ctx.Commands().push_back(std::move(command));
		};

		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, kBackground, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, rect.Y, gutterWidth, rect.H }, kGutterBackground, 0.0f });

		const int drawCaretLine = focused ? buffer.LineOfOffset(buffer.Caret()) : -1;
		const int firstVisible = std::max(0, static_cast<int>(std::floor(state.ScrollY / lineHeight)));
		const int lastVisible = std::min(lineCount - 1,
			static_cast<int>(std::floor((state.ScrollY + rect.H) / lineHeight)) + 1);
		const float textPadY = (lineHeight - fontSize) * 0.5f;

		ctx.Commands().push_back({ WuiDrawKind::ClipPush, textRect, kBackground });
		std::vector<WuiCodeToken> tokens;
		const auto [selStart, selEnd] = buffer.Selection();
		const size_t caretOffset = buffer.Caret();
		for (int line = firstVisible; line <= lastVisible; ++line)
		{
			const float lineY = textRect.Y + static_cast<float>(line) * lineHeight - state.ScrollY;
			size_t lineStart = 0;
			const std::string_view lineView = LineView(buffer, line, lineStart);
			if (line == drawCaretLine)
				ctx.Commands().push_back({ WuiDrawKind::Rect, { textRect.X, lineY, textRect.W, lineHeight }, kCurrentLine, 0.0f });

			// 行号栏
			const std::string number = std::to_string(line + 1);
			const float numberWidth = ctx.MeasureTextWidth(number, fontSize, WuiFontFamily::Monospace);
			pushText(rect.X + gutterWidth - kGutterPaddingRight - numberWidth, lineY + textPadY,
				line == drawCaretLine ? kGutterTextCurrent : kGutterText, number, -1, -1, -1);

			// 语法分段:token 之间按 Default 补齐(不丢字符,也不打乱像素推进)。
			tokens.clear();
			if (options.Highlight)
				options.Highlight(lineView, tokens);
			std::sort(tokens.begin(), tokens.end(),
				[](const WuiCodeToken& a, const WuiCodeToken& b) { return a.StartByte < b.StartByte; });
			float pen = 0.0f;
			bool caretDrawn = false;
			const auto drawSegment = [&](size_t start, size_t end, WuiCodeTokenKind kind)
			{
				if (end <= start)
					return;
				const std::string_view segment = lineView.substr(start, end - start);
				const size_t absStart = lineStart + start;
				const size_t absEnd = lineStart + end;
				int localSelStart = -1;
				int localSelEnd = -1;
				if (selEnd > selStart && selStart < absEnd && selEnd > absStart)
				{
					const size_t s = std::max(selStart, absStart) - absStart;
					const size_t e = std::min(selEnd, absEnd) - absStart;
					if (e > s)
					{
						localSelStart = static_cast<int>(s);
						localSelEnd = static_cast<int>(e);
					}
				}
				int caretByte = -1;
				if (focused && line == drawCaretLine && !caretDrawn && caretOffset >= absStart && caretOffset <= absEnd)
				{
					caretByte = static_cast<int>(caretOffset - absStart);
					caretDrawn = true;
				}
				pushText(textRect.X + pen, lineY + textPadY, kTokenColors[static_cast<size_t>(kind)],
					std::string(segment), localSelStart, localSelEnd, caretByte);
				pen += ctx.MeasureTextWidth(segment, fontSize, WuiFontFamily::Monospace);
			};
			size_t cursor = 0;
			for (const WuiCodeToken& token : tokens)
			{
				const size_t tokenStart = std::min<size_t>(token.StartByte, lineView.size());
				const size_t tokenEnd = std::max(tokenStart, std::min<size_t>(token.EndByte, lineView.size()));
				// W9 review:重叠/乱序 token 只画尚未消费的部分,避免同一段重复绘制。
				const size_t start = std::max(tokenStart, cursor);
				if (start > cursor)
					drawSegment(cursor, start, WuiCodeTokenKind::Default);
				if (start < tokenEnd)
					drawSegment(start, tokenEnd, token.Kind);
				cursor = std::max(cursor, tokenEnd);
			}
			drawSegment(cursor, lineView.size(), WuiCodeTokenKind::Default);
			// 空行 / caret 在行尾:补一个空文本命令,让后端按真实度量画 caret。
			if (focused && line == drawCaretLine && !caretDrawn)
				pushText(textRect.X + pen, lineY + textPadY, kCaretColor, std::string(), -1, -1, 0);
		}
		ctx.Commands().push_back({ WuiDrawKind::ClipPop });

		if (needScrollbar)
		{
			ctx.Commands().push_back({ WuiDrawKind::Rect, track, kScrollbarTrack, 0.0f });
			ctx.Commands().push_back({ WuiDrawKind::Rect,
				{ track.X + 2.0f, thumbY, track.W - 4.0f, thumbHeight }, kScrollbarThumb, 2.0f });
		}

		result.Changed = buffer.Revision() != revisionAtStart;
		return result;
	}
}
