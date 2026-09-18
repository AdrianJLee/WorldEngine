#include "wldpch.h"
#include "World/WUI/WuiCodeEditor.h"

#include "World/Core/KeyCodes.h"

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
		const WuiColor kTokenColors[7] = {
			{ 0.82f, 0.84f, 0.87f, 1.0f },
			{ 0.45f, 0.62f, 0.95f, 1.0f },
			{ 0.62f, 0.78f, 0.45f, 1.0f },
			{ 0.45f, 0.50f, 0.45f, 1.0f },
			{ 0.85f, 0.70f, 0.45f, 1.0f },
			{ 0.70f, 0.60f, 0.95f, 1.0f },
			{ 0.75f, 0.75f, 0.80f, 1.0f },
		};

		struct WuiCodeEditorState
		{
			float ScrollY = 0.0f;
			bool MouseSelecting = false;
			bool DraggingThumb = false;
			float ThumbGrabOffset = 0.0f;
			bool FollowCaret = true;
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
		if (focused && input.MouseClicked[0] && !ctx.IsHovered(rect))
		{
			ctx.SetFocus(0);
			ctx.SetTextInputActive(false);
			focused = false;
		}

		// ---- 滚轮(悬停文本区):只滚动,不动 caret ----
		if (ctx.IsHovered(textRect) && input.Wheel != 0.0f)
			state.ScrollY -= input.Wheel * lineHeight * 3.0f;

		// ---- 键盘(仅聚焦时) ----
		if (focused)
		{
			ctx.SetTextInputActive(true);
			if (ctx.IsHovered(textRect))
				ctx.SetCursor(WuiCursor::IBeam);
			const bool ctrl = input.Ctrl;
			const bool shift = input.Shift;
			const int pageLines = std::max(1, static_cast<int>(rect.H / lineHeight) - 1);
			bool caretAction = false;

			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				// Escape 失焦:后续输入回到引擎全局快捷键。
				ctx.SetFocus(0);
				ctx.SetTextInputActive(false);
				focused = false;
			}
			else
			{
				using Motion = WuiTextBuffer::Motion;
				if (ctrl)
				{
					if (ctx.IsKeyPressed(KeyCodes::S) && !readOnly)
						result.SaveRequested = true;
					if (ctx.IsKeyPressed(KeyCodes::A))
						buffer.SelectAll();
					if (ctx.IsKeyPressed(KeyCodes::C) && options.GetClipboard)
					{
						std::string selection;
						if (buffer.Copy(selection) && options.SetClipboard)
							options.SetClipboard(selection);
					}
					if (!readOnly && ctx.IsKeyPressed(KeyCodes::X))
					{
						std::string selection;
						if (buffer.Cut(selection) && options.SetClipboard)
							options.SetClipboard(selection);
						caretAction = true;
					}
					if (!readOnly && ctx.IsKeyPressed(KeyCodes::V) && options.GetClipboard)
					{
						std::string clip;
						if (options.GetClipboard(clip))
							buffer.Paste(clip);
						caretAction = true;
					}
					if (ctx.IsKeyPressed(KeyCodes::Z))
					{
						shift ? buffer.Redo() : buffer.Undo();
						caretAction = true;
					}
					if (ctx.IsKeyPressed(KeyCodes::Y))
					{
						buffer.Redo();
						caretAction = true;
					}
				}
				// 编辑键;ReadOnly 只导航/选择/复制。
				if (!readOnly && !ctrl)
				{
					if (ctx.IsKeyPressed(KeyCodes::Enter))
					{
						buffer.InsertNewline();
						caretAction = true;
					}
					if (ctx.IsKeyPressed(KeyCodes::Backspace))
					{
						buffer.Backspace();
						caretAction = true;
					}
					if (ctx.IsKeyPressed(KeyCodes::Delete))
					{
						buffer.DeleteForward();
						caretAction = true;
					}
					if (ctx.IsKeyPressed(KeyCodes::Tab))
					{
						buffer.IndentSelection(shift);
						caretAction = true;
					}
					for (uint32_t codepoint : input.TextInput)
					{
						// Enter/Tab/Escape 的 char 事件走按键分支,这里只收可见字符。
						if (codepoint < 0x20 || codepoint == 0x7F)
							continue;
						std::string encoded;
						EncodeUtf8(encoded, codepoint);
						buffer.InsertAtCaret(encoded);
						caretAction = true;
					}
				}
				// 导航键(ReadOnly 同样可用)。
				if (ctx.IsKeyPressed(KeyCodes::Left))
				{
					buffer.MoveCaret(ctrl ? Motion::WordLeft : Motion::Left, shift);
					caretAction = true;
				}
				if (ctx.IsKeyPressed(KeyCodes::Right))
				{
					buffer.MoveCaret(ctrl ? Motion::WordRight : Motion::Right, shift);
					caretAction = true;
				}
				if (ctx.IsKeyPressed(KeyCodes::Up))
				{
					buffer.MoveCaret(Motion::Up, shift);
					caretAction = true;
				}
				if (ctx.IsKeyPressed(KeyCodes::Down))
				{
					buffer.MoveCaret(Motion::Down, shift);
					caretAction = true;
				}
				if (ctx.IsKeyPressed(KeyCodes::Home))
				{
					buffer.MoveCaret(Motion::LineStart, shift);
					caretAction = true;
				}
				if (ctx.IsKeyPressed(KeyCodes::End))
				{
					buffer.MoveCaret(Motion::LineEnd, shift);
					caretAction = true;
				}
				if (ctx.IsKeyPressed(KeyCodes::PageUp))
				{
					buffer.MoveCaret(Motion::PageUp, shift, pageLines);
					caretAction = true;
				}
				if (ctx.IsKeyPressed(KeyCodes::PageDown))
				{
					buffer.MoveCaret(Motion::PageDown, shift, pageLines);
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

		const int caretLine = focused ? buffer.LineOfOffset(buffer.Caret()) : -1;
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
			if (line == caretLine)
				ctx.Commands().push_back({ WuiDrawKind::Rect, { textRect.X, lineY, textRect.W, lineHeight }, kCurrentLine, 0.0f });

			// 行号栏
			const std::string number = std::to_string(line + 1);
			const float numberWidth = ctx.MeasureTextWidth(number, fontSize, WuiFontFamily::Monospace);
			pushText(rect.X + gutterWidth - kGutterPaddingRight - numberWidth, lineY + textPadY,
				line == caretLine ? kGutterTextCurrent : kGutterText, number, -1, -1, -1);

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
				if (focused && line == caretLine && !caretDrawn && caretOffset >= absStart && caretOffset <= absEnd)
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
				if (tokenStart > cursor)
					drawSegment(cursor, tokenStart, WuiCodeTokenKind::Default);
				drawSegment(tokenStart, tokenEnd, token.Kind);
				cursor = std::max(cursor, tokenEnd);
			}
			drawSegment(cursor, lineView.size(), WuiCodeTokenKind::Default);
			// 空行 / caret 在行尾:补一个空文本命令,让后端按真实度量画 caret。
			if (focused && line == caretLine && !caretDrawn)
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
