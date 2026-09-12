#include "wldpch.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Core/KeyCodes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace World::Wui
{
	namespace
	{
		void AppendUtf8(std::string& buffer, uint32_t codepoint)
		{
			if (codepoint < 0x80)
				buffer.push_back(static_cast<char>(codepoint));
			else if (codepoint < 0x800)
			{
				buffer.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
				buffer.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else if (codepoint < 0x10000)
			{
				buffer.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
				buffer.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				buffer.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else
			{
				buffer.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
				buffer.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
				buffer.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				buffer.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
		}

		void PopUtf8(std::string& buffer)
		{
			if (buffer.empty())
				return;
			buffer.pop_back();
			while (!buffer.empty() && (static_cast<unsigned char>(buffer.back()) & 0xC0) == 0x80)
				buffer.pop_back();
		}

	}

	void DrawPanelSurface(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme)
	{
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.PanelBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });
	}

	void Panel(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiTheme& theme)
	{
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.PanelBg, 4.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 4.0f, 1.0f });
		const WuiRect header { rect.X, rect.Y, rect.W, 26.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, header, theme.PanelHeader, 4.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { header.X + 10.0f, header.Y + 4.0f, 0, 0 }, theme.Text, 0, 1.0f, title, 15.0f, false });
	}

	void Label(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const WuiColor& color, float fontSize)
	{
		ctx.Commands().push_back({ WuiDrawKind::Text, { pos.x, pos.y, 0, 0 }, color, 0, 1.0f, text, fontSize, false });
	}

	bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme)
	{
		const bool hovered = ctx.IsHovered(rect);
		const bool pressed = ctx.Input().MouseDown[0] && hovered;
		const WuiColor fill = hovered ? theme.ButtonHover : theme.ButtonBg;
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, fill, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, pressed ? theme.Accent : theme.Text, 0, 1.0f, label, 15.0f, false });
		(void)id;
		return hovered && ctx.Input().MouseClicked[0];
	}

	bool Toggle(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme)
	{
		bool& value = ctx.Persist<bool>(id, false);
		if (ctx.IsClicked(rect))
			value = !value;

		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, label, 15.0f, false });
		return value;
	}

	bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool& value, const WuiTheme& theme)
	{
		if (ctx.IsClicked(rect))
			value = !value;
		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, label, 15.0f, false });
		(void)id;
		return value;
	}

	void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme)
	{
		const float range = std::max(0.0001f, max - min);
		if (ctx.Input().MouseDown[0] && ctx.IsHovered(rect))
		{
			const float fraction = (ctx.Input().MousePos.x - rect.X) / std::max(1.0f, rect.W);
			value = min + std::max(0.0f, std::min(1.0f, fraction)) * range;
		}

		const float trackH = 4.0f;
		const float trackY = rect.Y + rect.H * 0.5f - trackH * 0.5f;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, trackY, rect.W, trackH }, theme.ButtonBg, 2.0f });
		const float fraction = (value - min) / range;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, trackY, rect.W * fraction, trackH }, theme.Accent, 2.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X + rect.W * fraction - 4.0f, rect.Y + (rect.H - 12.0f) * 0.5f, 8.0f, 12.0f }, theme.Text, 2.0f });
		(void)id;
	}

	namespace
	{
		struct NumericDragState
		{
			bool Editing = false;
			bool Dragging = false;
			std::string Buffer;
			float DragStartX = 0;
		};
	}

	namespace
	{
		struct WuiEditState { int Cursor = -1; int SelStart = -1; int SelEnd = -1; };
		struct WuiNumericState { bool Pressed = false; bool Dragging = false; bool Editing = false; float PressX = 0; double PressValue = 0; std::string Buffer; int Cursor = -1; int SelStart = -1; int SelEnd = -1; };

		int Utf8Count(const std::string& text)
		{
			int count = 0;
			for (size_t i = 0; i < text.size();)
			{
				const unsigned char c = static_cast<unsigned char>(text[i]);
				i += (c & 0x80) ? ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4)) : 1;
				++count;
			}
			return count;
		}

		size_t Utf8Offset(const std::string& text, int cursor)
		{
			if (cursor < 0) return text.size();
			size_t offset = 0;
			int index = 0;
			while (offset < text.size() && index < cursor)
			{
				const unsigned char c = static_cast<unsigned char>(text[offset]);
				offset += (c & 0x80) ? ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4)) : 1;
				++index;
			}
			return offset;
		}

		// 由像素位置估算光标字符下标:ASCII 半角按 0.52 倍字号,其余按全角宽度。
		int CursorAtX(const std::string& text, float x, float fontSize)
		{
			if (x <= 0)
				return 0;
			float accumulated = 0;
			int index = 0;
			for (size_t i = 0; i < text.size();)
			{
				const unsigned char c = static_cast<unsigned char>(text[i]);
				const int length = (c & 0x80) ? ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4)) : 1;
				const float width = (c < 0x80) ? fontSize * 0.52f : fontSize * 0.95f;
				if (x < accumulated + width * 0.5f)
					return index;
				accumulated += width;
				i += length;
				++index;
			}
			return index;
		}

		void InsertUtf8At(std::string& text, size_t offset, uint32_t codepoint)
		{
			std::string encoded;
			AppendUtf8(encoded, codepoint);
			text.insert(offset, encoded);
		}

		void EraseBefore(std::string& text, int& cursor)
		{
			const size_t offset = Utf8Offset(text, cursor);
			if (offset == 0) return;
			size_t start = offset - 1;
			while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
			text.erase(start, offset - start);
			--cursor;
		}

		void EraseAt(std::string& text, int cursor)
		{
			const size_t offset = Utf8Offset(text, cursor);
			if (offset >= text.size()) return;
			size_t end = offset + 1;
			while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
			text.erase(offset, end - offset);
		}

		bool EditUpdate(WuiContext& ctx, std::string& buffer, int& cursor, int& selStart, int& selEnd, bool& submitted, bool& cancelled)
		{
			submitted = false;
			cancelled = false;
			if (cursor < 0) cursor = Utf8Count(buffer);
			for (uint32_t codepoint : ctx.Input().TextInput)
			{
				if (selStart >= 0 && selEnd > selStart)
				{
					buffer.erase(Utf8Offset(buffer, selStart), Utf8Offset(buffer, selEnd) - Utf8Offset(buffer, selStart));
					cursor = selStart;
				}
				selStart = -1;
				selEnd = -1;
				InsertUtf8At(buffer, Utf8Offset(buffer, cursor), codepoint);
				++cursor;
			}
			const int count = Utf8Count(buffer);
			if (cursor > count) cursor = count;
			if (ctx.IsKeyPressed(KeyCodes::Left) && cursor > 0) { --cursor; selStart = -1; selEnd = -1; }
			if (ctx.IsKeyPressed(KeyCodes::Right) && cursor < count) { ++cursor; selStart = -1; selEnd = -1; }
			if (ctx.IsKeyPressed(KeyCodes::Home)) { cursor = 0; selStart = -1; selEnd = -1; }
			if (ctx.IsKeyPressed(KeyCodes::End)) { cursor = count; selStart = -1; selEnd = -1; }
			if (ctx.IsKeyPressed(KeyCodes::Backspace))
			{
				if (selStart >= 0 && selEnd > selStart)
				{
					buffer.erase(Utf8Offset(buffer, selStart), Utf8Offset(buffer, selEnd) - Utf8Offset(buffer, selStart));
					cursor = selStart;
					selStart = -1;
					selEnd = -1;
				}
				else if (cursor > 0)
					EraseBefore(buffer, cursor);
			}
			if (ctx.IsKeyPressed(KeyCodes::Delete))
			{
				if (selStart >= 0 && selEnd > selStart)
				{
					buffer.erase(Utf8Offset(buffer, selStart), Utf8Offset(buffer, selEnd) - Utf8Offset(buffer, selStart));
					cursor = selStart;
					selStart = -1;
					selEnd = -1;
				}
				else if (cursor < Utf8Count(buffer))
					EraseAt(buffer, cursor);
			}
			if (ctx.IsKeyPressed(KeyCodes::Enter)) submitted = true;
			if (ctx.IsKeyPressed(KeyCodes::Escape)) cancelled = true;
			return submitted || cancelled;
		}

		std::string WithCursor(const std::string& text, int cursor)
		{
			const size_t offset = Utf8Offset(text, cursor);
			return text.substr(0, offset) + "|" + text.substr(offset);
		}

		void PushTextFieldCommand(WuiContext& ctx, const WuiRect& rect, const std::string& text, const WuiTheme& theme, bool focused, bool hovered, int selStart, int selEnd)
		{
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonBg, 3.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, focused ? theme.Accent : theme.Border, 3.0f, focused ? 1.5f : 1.0f });
			WuiDrawCommand command { WuiDrawKind::Text, { rect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, text, 15.0f, false };
			command.TextSelStart = (focused && selStart >= 0 && selEnd > selStart) ? static_cast<int>(Utf8Offset(text, selStart)) : -1;
			command.TextSelEnd = (focused && selStart >= 0 && selEnd > selStart) ? static_cast<int>(Utf8Offset(text, selEnd)) : -1;
			ctx.Commands().push_back(std::move(command));
			(void)hovered;
		}
	}

	bool DragFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float speed, float min, float max, const WuiTheme& theme)
	{
		WuiNumericState& state = ctx.Persist<WuiNumericState>(id, {});
		bool changed = false;
		const bool hovered = ctx.IsHovered(rect);
		const float lo = min < max ? min : -1e30f;
		const float hi = min < max ? max : 1e30f;

		if (state.Editing)
		{
			bool submitted = false, cancelled = false;
			if (EditUpdate(ctx, state.Buffer, state.Cursor, state.SelStart, state.SelEnd, submitted, cancelled))
			{
				if (submitted)
				{
					char* end = nullptr;
					const float parsed = std::strtof(state.Buffer.c_str(), &end);
					if (end && *end == 0) { value = std::max(lo, std::min(hi, parsed)); changed = true; }
				}
				state.Editing = false;
				state.Cursor = -1;
				state.SelStart = -1;
				state.SelEnd = -1;
			}
			else if (ctx.Input().MouseClicked[0] && !hovered)
			{
				char* end = nullptr;
				const float parsed = std::strtof(state.Buffer.c_str(), &end);
				if (end && *end == 0) { value = std::max(lo, std::min(hi, parsed)); changed = true; }
				state.Editing = false;
				state.Cursor = -1;
				state.SelStart = -1;
				state.SelEnd = -1;
			}
			ctx.SetCursor(WuiCursor::IBeam);
		}
		else
		{
			if (ctx.Input().MouseClicked[0] && hovered)
			{
				state.Pressed = true;
				state.PressX = ctx.Input().MousePos.x;
				state.PressValue = value;
			}
			if (state.Pressed)
			{
				const float dx = ctx.Input().MousePos.x - state.PressX;
				if (std::fabs(dx) > 1.5f)
				{
					state.Dragging = true;
					ctx.SetFocus(id);
				}
				if (state.Dragging)
				{
					value = std::max(lo, std::min(hi, static_cast<float>(state.PressValue + dx * speed)));
					changed = true;
					ctx.SetCursor(WuiCursor::ResizeEW);
				}
				if (ctx.Input().MouseReleased[0])
				{
					if (!state.Dragging)
					{
						state.Editing = true;
						char buffer[32];
						std::snprintf(buffer, sizeof(buffer), "%.3f", value);
						state.Buffer = buffer;
						state.Cursor = -1;
						state.SelStart = 0;
						state.SelEnd = Utf8Count(state.Buffer);
						ctx.SetFocus(id);
						ctx.SetTextInputActive(true);
					}
					state.Pressed = false;
					state.Dragging = false;
				}
			}
			else if (hovered)
				ctx.SetCursor(WuiCursor::ResizeEW);
		}

		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, state.Editing ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, (state.Editing || hovered || state.Dragging) ? theme.Accent : theme.Border, 3.0f, 1.0f });
		std::string text;
		if (state.Editing) text = (state.SelStart >= 0 && state.SelEnd > state.SelStart) ? state.Buffer : WithCursor(state.Buffer, state.Cursor);
		else { char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%.3f", value); text = buffer; }
		WuiDrawCommand command { WuiDrawKind::Text, { rect.X + 5.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, text, 14.0f, false };
		if (state.Editing && state.SelStart >= 0 && state.SelEnd > state.SelStart)
		{
			command.TextSelStart = static_cast<int>(Utf8Offset(state.Buffer, state.SelStart));
			command.TextSelEnd = static_cast<int>(Utf8Offset(state.Buffer, state.SelEnd));
		}
		ctx.Commands().push_back(std::move(command));
		return changed;
	}

	bool DragInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme)
	{
		WuiNumericState& state = ctx.Persist<WuiNumericState>(id, {});
		bool changed = false;
		const bool hovered = ctx.IsHovered(rect);
		const int64_t lo = min < max ? min : INT64_MIN;
		const int64_t hi = min < max ? max : INT64_MAX;

		if (state.Editing)
		{
			bool submitted = false, cancelled = false;
			if (EditUpdate(ctx, state.Buffer, state.Cursor, state.SelStart, state.SelEnd, submitted, cancelled))
			{
				if (submitted)
				{
					char* end = nullptr;
					const long long parsed = std::strtoll(state.Buffer.c_str(), &end, 10);
					if (end && *end == 0) { value = std::max(lo, std::min(hi, static_cast<int64_t>(parsed))); changed = true; }
				}
				state.Editing = false;
				state.Cursor = -1;
				state.SelStart = -1;
				state.SelEnd = -1;
			}
			else if (ctx.Input().MouseClicked[0] && !hovered)
			{
				char* end = nullptr;
				const long long parsed = std::strtoll(state.Buffer.c_str(), &end, 10);
				if (end && *end == 0) { value = std::max(lo, std::min(hi, static_cast<int64_t>(parsed))); changed = true; }
				state.Editing = false;
				state.Cursor = -1;
				state.SelStart = -1;
				state.SelEnd = -1;
			}
			ctx.SetCursor(WuiCursor::IBeam);
		}
		else
		{
			if (ctx.Input().MouseClicked[0] && hovered)
			{
				state.Pressed = true;
				state.PressX = ctx.Input().MousePos.x;
				state.PressValue = static_cast<double>(value);
			}
			if (state.Pressed)
			{
				const float dx = ctx.Input().MousePos.x - state.PressX;
				if (std::fabs(dx) > 1.5f)
				{
					state.Dragging = true;
					ctx.SetFocus(id);
				}
				if (state.Dragging)
				{
					value = std::max(lo, std::min(hi, static_cast<int64_t>(state.PressValue + static_cast<double>(dx))));
					changed = true;
					ctx.SetCursor(WuiCursor::ResizeEW);
				}
				if (ctx.Input().MouseReleased[0])
				{
					if (!state.Dragging)
					{
						state.Editing = true;
						state.Buffer = std::to_string(value);
						state.Cursor = -1;
						state.SelStart = 0;
						state.SelEnd = Utf8Count(state.Buffer);
						ctx.SetFocus(id);
						ctx.SetTextInputActive(true);
					}
					state.Pressed = false;
					state.Dragging = false;
				}
			}
			else if (hovered)
				ctx.SetCursor(WuiCursor::ResizeEW);
		}

		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, state.Editing ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, (state.Editing || hovered || state.Dragging) ? theme.Accent : theme.Border, 3.0f, 1.0f });
		std::string text;
		if (state.Editing) text = (state.SelStart >= 0 && state.SelEnd > state.SelStart) ? state.Buffer : WithCursor(state.Buffer, state.Cursor);
		else text = std::to_string(value);
		WuiDrawCommand command { WuiDrawKind::Text, { rect.X + 5.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, text, 14.0f, false };
		if (state.Editing && state.SelStart >= 0 && state.SelEnd > state.SelStart)
		{
			command.TextSelStart = static_cast<int>(Utf8Offset(state.Buffer, state.SelStart));
			command.TextSelEnd = static_cast<int>(Utf8Offset(state.Buffer, state.SelEnd));
		}
		ctx.Commands().push_back(std::move(command));
		return changed;
	}

	bool TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, bool* cancelledOut)
	{
		WuiEditState& state = ctx.Persist<WuiEditState>(id, {});
		if (ctx.IsClicked(rect))
		{
			ctx.SetFocus(id);
			// 点击定位光标,不再整段全选;避免一输入就整体覆盖。
			state.Cursor = CursorAtX(buffer, ctx.Input().MousePos.x - (rect.X + 6.0f), 15.0f);
			state.SelStart = -1;
			state.SelEnd = -1;
		}
		const bool focused = ctx.Focus() == id;
		if (focused) ctx.SetTextInputActive(true);
		bool submitted = false;
		if (focused)
		{
			bool cancelled = false;
			if (EditUpdate(ctx, buffer, state.Cursor, state.SelStart, state.SelEnd, submitted, cancelled))
				ctx.SetFocus(0);
			else if (ctx.Input().MouseClicked[0] && !ctx.IsHovered(rect))
				ctx.SetFocus(0);
			if (cancelledOut)
				*cancelledOut = cancelled;
			ctx.SetCursor(WuiCursor::IBeam);
		}
		else if (cancelledOut)
			*cancelledOut = false;
		else if (ctx.IsHovered(rect))
			ctx.SetCursor(WuiCursor::IBeam);
		const bool hasSelection = focused && state.SelStart >= 0 && state.SelEnd > state.SelStart;
		const std::string text = hasSelection ? buffer : (focused ? WithCursor(buffer, state.Cursor) : buffer);
		WuiDrawCommand command { WuiDrawKind::Text, { rect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, text, 15.0f, false };
		if (hasSelection)
		{
			command.TextSelStart = static_cast<int>(Utf8Offset(buffer, state.SelStart));
			command.TextSelEnd = static_cast<int>(Utf8Offset(buffer, state.SelEnd));
		}
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, focused ? theme.Accent : theme.Border, 3.0f, focused ? 1.5f : 1.0f });
		ctx.Commands().push_back(std::move(command));
		return submitted;
	}
	void Image(WuiContext& ctx, const WuiRect& rect, uint64_t textureId, const WuiRect& uv, const WuiTheme& theme)
	{
		ctx.Commands().push_back({ WuiDrawKind::Image, rect, theme.Text, 0, 1.0f, "", 15.0f, false, textureId, uv });
	}

	bool Combo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		const std::vector<std::string>& options, int& selected, const WuiTheme& theme)
	{
		const bool hovered = ctx.IsHovered(rect);
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, hovered ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });
		const std::string text = label.empty() ? options[selected] : label + ": " + options[selected];
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, text, 15.0f, false });

		if (ctx.IsClicked(rect))
		{
			if (ctx.IsPopupOpen(id))
				ctx.ClosePopup(id);
			else
				ctx.OpenPopup(id);
		}

		bool changed = false;
		if (ctx.IsPopupOpen(id))
		{
			ctx.PushOverlay();
			const float itemH = 22.0f;
			const WuiRect panel { rect.X, rect.Y + rect.H + 2.0f, rect.W, itemH * options.size() + 8.0f };
			DrawPanelSurface(ctx, panel, theme);
			for (size_t i = 0; i < options.size(); ++i)
			{
				const WuiRect item { panel.X + 4.0f, panel.Y + 4.0f + itemH * static_cast<float>(i), panel.W - 8.0f, itemH };
				if (ctx.IsHovered(item))
					ctx.Commands().push_back({ WuiDrawKind::Rect, item, theme.ButtonHover, 2.0f });
				if (ctx.IsClicked(item))
				{
					selected = static_cast<int>(i);
					changed = true;
					ctx.ClosePopup(id);
				}
				ctx.Commands().push_back({ WuiDrawKind::Text, { item.X + 6.0f, item.Y + 3.0f, 0, 0 }, theme.Text, 0, 1.0f, options[i], 15.0f, false });
			}
			ctx.ClosePopupsOnOutsideClick({ id }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(id);
			ctx.PopOverlay();
		}
		return changed;
	}

	bool TreeNode(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool leaf, const WuiTheme& theme)
	{
		bool& open = ctx.Persist<bool>(id, false);
		if (ctx.IsClicked(rect))
			open = !leaf && !open;
		const std::string marker = leaf ? "  " : (open ? "- " : "+ ");
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 4.0f, rect.Y + 2.0f, 0, 0 }, theme.TextMuted, 0, 1.0f, marker, 14.0f, false });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 22.0f, rect.Y + 2.0f, 0, 0 }, theme.Text, 0, 1.0f, label, 14.0f, false });
		return open;
	}

	bool BeginMenuBar(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme)
	{
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.PanelHeader, 0.0f });
		return true;
	}

	void EndMenuBar(WuiContext& ctx)
	{
		(void)ctx;
	}

	bool BeginMenu(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme)
	{
		const bool open = ctx.IsPopupOpen(id);
		if (ctx.IsHovered(rect) || open)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonHover, 0.0f });
		if (ctx.IsClicked(rect))
		{
			if (open)
				ctx.ClosePopup(id);
			else
			{
				ctx.CloseAllPopups();
				ctx.OpenPopup(id);
			}
		}
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, label, 15.0f, false });
		return ctx.IsPopupOpen(id);
	}

	void EndMenu(WuiContext& ctx, WuiId id, const WuiRect& panel, const WuiTheme& theme)
	{
		ctx.ClosePopupsOnOutsideClick({ id }, panel);
		if (ctx.IsKeyPressed(KeyCodes::Escape))
			ctx.ClosePopup(id);
	}

	bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool enabled, const WuiTheme& theme)
	{
		if (ctx.IsHovered(rect) && enabled)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonHover, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, enabled ? theme.Text : theme.TextMuted, 0, 1.0f, label, 15.0f, false });
		(void)id;
		return enabled && ctx.IsClicked(rect);
	}

	bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool checked, bool enabled, const WuiTheme& theme)
	{
		if (ctx.IsHovered(rect) && enabled)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonHover, 0.0f });
		// 复选风格(如 Window 菜单的可见性开关)显示 [x]/[ ];普通动作项走上面的重载。
		const std::string text = std::string(checked ? "[x] " : "[ ] ") + label;
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, enabled ? theme.Text : theme.TextMuted, 0, 1.0f, text, 15.0f, false });
		(void)id;
		return enabled && ctx.IsClicked(rect);
	}

	bool BeginModal(WuiContext& ctx, WuiId id, const std::string& title, const glm::vec2& size, WuiRect* panel, const WuiTheme& theme)
	{
		if (ctx.Modal() != id)
			return false;
		ctx.PushOverlay();
		const glm::vec2 viewport = ctx.ViewportSize();
		const WuiRect centered { (viewport.x - size.x) * 0.5f, (viewport.y - size.y) * 0.5f, size.x, size.y };
		if (panel)
			*panel = centered;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { 0, 0, viewport.x, viewport.y }, { 0, 0, 0, 0.5f }, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, centered, theme.PanelBg, 5.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, centered, theme.Border, 5.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { centered.X + 14.0f, centered.Y + 10.0f, 0, 0 }, theme.Text, 0, 1.0f, title, 16.0f, true });
		return true;
	}

	void EndModal(WuiContext& ctx, WuiId id)
	{
		(void)ctx;
		(void)id;
		ctx.PopOverlay();
	}

	bool BeginScrollArea(WuiContext& ctx, const WuiRect& viewport, float contentHeight, float& scrollY, const WuiTheme& theme)
	{
		if (ctx.IsHovered(viewport))
			scrollY -= ctx.Input().Wheel * 40.0f;
		scrollY = std::max(0.0f, std::min(scrollY, std::max(0.0f, contentHeight - viewport.H)));
		ctx.Commands().push_back({ WuiDrawKind::ClipPush, viewport, theme.PanelBg });
		return true;
	}

	void EndScrollArea(WuiContext& ctx)
	{
		ctx.Commands().push_back({ WuiDrawKind::ClipPop });
	}

	WuiRect TableCell(const WuiRect& table, const std::vector<float>& columns, size_t row, size_t column, float rowHeight)
	{
		float x = table.X;
		for (size_t i = 0; i < column && i < columns.size(); ++i)
			x += columns[i];
		const float width = column < columns.size() ? columns[column] : table.W;
		return { x, table.Y + rowHeight * static_cast<float>(row), width, rowHeight };
	}
}
