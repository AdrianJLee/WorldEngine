#include "wldpch.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/Core/KeyCodes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace World::Wui
{
	namespace
	{
		// 控件绘制时登记无障碍节点(AI 控制通道的 ui.tree / ui.invoke 数据源)。
		// 关闭控制通道时这一步只是往一个 vector 里追加,无额外分配以外的副作用。
		void RegisterAccessNode(WuiId id, const char* kind, const WuiRect& rect,
			const std::string& label, const std::string& value,
			bool enabled = true, bool interactive = true, bool focused = false)
		{
			if (id == 0)
				return;
			WuiAccessNode node;
			node.Id = id;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = interactive;
			node.Focused = focused;
			WuiAccessibility::Get().Register(node);
		}

		std::string FloatToText(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3f", value);
			return buffer;
		}

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

	void LabelWithTerm(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const std::string& term,
		const WuiColor& color, float fontSize, const WuiTheme& theme)
	{
		if (term.empty())
		{
			Label(ctx, pos, text, color, fontSize);
			return;
		}
		// P4-UX1:主文案后 6px 接小号术语(Caption + 次要色)。术语"退后一层"阅读,
		// 不换行、不加括号堆叠;右侧空间不够时直接省略(窄面板不硬塞)。
		const float textWidth = ctx.MeasureTextWidth(text, fontSize);
		const float termSize = theme.FontSizeCaption;
		const float termX = pos.x + textWidth + 6.0f;
		ctx.Commands().push_back({ WuiDrawKind::Text, { pos.x, pos.y, 0, 0 }, color, 0, 1.0f, text, fontSize, false });
		ctx.Commands().push_back({ WuiDrawKind::Text, { termX, pos.y + (fontSize - termSize) * 0.5f, 0, 0 },
			theme.TextMuted, 0, 1.0f, term, termSize, false });
	}

	bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme)
	{
		RegisterAccessNode(id, "button", rect, label, std::string());
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
		RegisterAccessNode(id, "toggle", rect, label, value ? "on" : "off");

		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, label, 15.0f, false });
		return value;
	}

	bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool& value, const WuiTheme& theme)
	{
		return Checkbox(ctx, id, rect, label, std::string(), value, theme);
	}

	bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const std::string& term,
		bool& value, const WuiTheme& theme)
	{
		// 返回"本帧是否被点击改值",与 Combo/DragInt/DragFloat 的约定一致(旧版返回 value,
		// 导致 `if (Checkbox(...))` 的调用点在**勾选时每帧触发、取消勾选时反而不触发**)。
		// 控件状态本身仍然写回 value 引用,并在无障碍节点里记录。
		bool changed = false;
		if (ctx.IsClicked(rect))
		{
			value = !value;
			changed = true;
		}
		// 无障碍节点带上英文术语:脚本/自动化在中文界面下也能按英文检索。
		RegisterAccessNode(id, "checkbox", rect,
			term.empty() ? label : (label + " (" + term + ")"), value ? "true" : "false");
		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		const float labelSize = 15.0f;
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - labelSize) * 0.5f, 0, 0 },
			theme.Text, 0, 1.0f, label, labelSize, false });
		if (!term.empty())
		{
			// 英文术语对照:Caption/次要色;右侧放不下就省略。
			const float termSize = theme.FontSizeCaption;
			const float termX = rect.X + 24.0f + ctx.MeasureTextWidth(label, labelSize) + 6.0f;
			if (termX + ctx.MeasureTextWidth(term, termSize) <= rect.X + rect.W)
				ctx.Commands().push_back({ WuiDrawKind::Text, { termX, rect.Y + (rect.H - termSize) * 0.5f, 0, 0 },
					theme.TextMuted, 0, 1.0f, term, termSize, false });
		}
		(void)id;
		return changed;
	}

	void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme)
	{
		RegisterAccessNode(id, "slider", rect, std::string(), FloatToText(value));
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
		struct WuiEditState { int Cursor = -1; int SelStart = -1; int SelEnd = -1; int DragAnchor = -1; bool MouseSelecting = false; };
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
			// Shift 组合键扩展选区:靠近哪一端就移动哪一端,归零则取消选区。
			const bool shift = ctx.Input().Shift;
			auto moveEdge = [&](int candidate)
			{
				if (selStart < 0 || selEnd <= selStart)
				{
					selStart = cursor;
					selEnd = cursor;
				}
				if (std::abs(candidate - selStart) <= std::abs(candidate - selEnd))
					selStart = candidate;
				else
					selEnd = candidate;
				cursor = candidate;
				if (selStart > selEnd) std::swap(selStart, selEnd);
				if (selStart == selEnd) { selStart = -1; selEnd = -1; }
			};
			if (ctx.IsKeyPressed(KeyCodes::Left) && cursor > 0)
			{
				if (shift) moveEdge(cursor - 1);
				else { --cursor; selStart = -1; selEnd = -1; }
			}
			if (ctx.IsKeyPressed(KeyCodes::Right) && cursor < count)
			{
				if (shift) moveEdge(cursor + 1);
				else { ++cursor; selStart = -1; selEnd = -1; }
			}
			if (ctx.IsKeyPressed(KeyCodes::Home))
			{
				if (shift) moveEdge(0);
				else { cursor = 0; selStart = -1; selEnd = -1; }
			}
			if (ctx.IsKeyPressed(KeyCodes::End))
			{
				if (shift) moveEdge(count);
				else { cursor = count; selStart = -1; selEnd = -1; }
			}
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
		RegisterAccessNode(id, "drag-float", rect, std::string(), FloatToText(value));
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
			// 编辑态也只在悬停该控件时显示 I 型光标:否则鼠标移到别处仍保持输入形状。
			if (hovered)
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
		if (state.Editing) text = state.Buffer;
		else { char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%.3f", value); text = buffer; }
		WuiDrawCommand command { WuiDrawKind::Text, { rect.X + 5.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, text, 14.0f, false };
		if (state.Editing && state.SelStart >= 0 && state.SelEnd > state.SelStart)
		{
			command.TextSelStart = static_cast<int>(Utf8Offset(state.Buffer, state.SelStart));
			command.TextSelEnd = static_cast<int>(Utf8Offset(state.Buffer, state.SelEnd));
		}
		else if (state.Editing)
			command.TextCursorByte = static_cast<int>(Utf8Offset(state.Buffer, state.Cursor));
		ctx.Commands().push_back(std::move(command));
		return changed;
	}

	bool DragInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme)
	{
		RegisterAccessNode(id, "drag-int", rect, std::string(), std::to_string(value));
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
			if (hovered)
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
		if (state.Editing) text = state.Buffer;
		else text = std::to_string(value);
		WuiDrawCommand command { WuiDrawKind::Text, { rect.X + 5.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, text, 14.0f, false };
		if (state.Editing && state.SelStart >= 0 && state.SelEnd > state.SelStart)
		{
			command.TextSelStart = static_cast<int>(Utf8Offset(state.Buffer, state.SelStart));
			command.TextSelEnd = static_cast<int>(Utf8Offset(state.Buffer, state.SelEnd));
		}
		else if (state.Editing)
			command.TextCursorByte = static_cast<int>(Utf8Offset(state.Buffer, state.Cursor));
		ctx.Commands().push_back(std::move(command));
		return changed;
	}

	bool TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, bool* cancelledOut)
	{
		RegisterAccessNode(id, "text-field", rect, std::string(), buffer, true, true, ctx.Focus() == id);
		WuiEditState& state = ctx.Persist<WuiEditState>(id, {});
		// 仅在按下的那一帧初始化拖选锚点;按住期间持续更新选区。
		if (ctx.Input().MouseClicked[0] && ctx.IsHovered(rect))
		{
			ctx.SetFocus(id);
			const int clicked = CursorAtX(buffer, ctx.Input().MousePos.x - (rect.X + 6.0f), 15.0f);
			state.Cursor = clicked;
			state.DragAnchor = clicked;
			state.MouseSelecting = true;
			state.SelStart = -1;
			state.SelEnd = -1;
		}
		if (state.MouseSelecting && ctx.Input().MouseDown[0])
		{
			const int current = CursorAtX(buffer, ctx.Input().MousePos.x - (rect.X + 6.0f), 15.0f);
			state.Cursor = current;
			if (current != state.DragAnchor)
			{
				state.SelStart = std::min(state.DragAnchor, current);
				state.SelEnd = std::max(state.DragAnchor, current);
			}
			else
			{
				state.SelStart = -1;
				state.SelEnd = -1;
			}
		}
		if (state.MouseSelecting && ctx.Input().MouseReleased[0])
			state.MouseSelecting = false;
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
			// 焦点字段只有在鼠标悬停其上时才显示 I 型光标。
			if (ctx.IsHovered(rect))
				ctx.SetCursor(WuiCursor::IBeam);
		}
		else if (cancelledOut)
			*cancelledOut = false;
		else if (ctx.IsHovered(rect))
			ctx.SetCursor(WuiCursor::IBeam);
		const bool hasSelection = focused && state.SelStart >= 0 && state.SelEnd > state.SelStart;
		const std::string text = buffer;
		WuiDrawCommand command { WuiDrawKind::Text, { rect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, text, 15.0f, false };
		if (hasSelection)
		{
			command.TextSelStart = static_cast<int>(Utf8Offset(buffer, state.SelStart));
			command.TextSelEnd = static_cast<int>(Utf8Offset(buffer, state.SelEnd));
		}
		else if (focused)
			command.TextCursorByte = static_cast<int>(Utf8Offset(buffer, state.Cursor));
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
		RegisterAccessNode(id, "combo", rect, label,
			(selected >= 0 && selected < static_cast<int>(options.size())) ? options[selected] : std::string());
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
				{
					ctx.Commands().push_back({ WuiDrawKind::Rect, item, theme.ButtonHover, 2.0f });
					// ③ 弹层内光标归弹层:候选行是弹层自己的可点控件,显式声明 Hand,
					// 不再让"更早绘制的控件"留下的光标形状代表弹层。
					ctx.SetCursor(WuiCursor::Hand);
				}
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
			// ③ 弹层打开期间:把弹层矩形登记为悬停遮挡区 —— 本帧**之后**绘制的下层控件
			// (同一面板下方的输入框/滑杆/下拉,或后画的兄弟面板)在 HitTest 里判为未命中,
			// 鼠标形状与点击都不再"穿透"弹层落到下层控件上。顺序要求:必须在弹层自己的
			// 条目命中测试与 ClosePopupsOnOutsideClick 之后登记,否则会把弹层自身的点击挡掉、
			// 或把弹层内点击误判成"外部点击"而关掉弹层。BeginFrame 每帧清空遮挡区,弹层开着
			// 时这里每帧重新登记;弹层外不登记,既有"点击弹层外关闭"语义不变。
			if (ctx.IsPopupOpen(id))
				ctx.PushHoverBlocker(panel);
			ctx.PopOverlay();
		}
		return changed;
	}

	// D3:可搜索下拉。选项多(材质/贴图路径)时,用输入框过滤 + 滚轮滚动选择,
	// 交互与常见引擎的资源选择器一致。
	bool SearchableCombo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		const std::vector<std::string>& options, int& selected, const WuiTheme& theme)
	{
		// 触发器本身也可被 ui.invoke 点击(等价于点开下拉)。
		RegisterAccessNode(id, "search-combo", rect, label,
			(selected >= 0 && selected < static_cast<int>(options.size())) ? options[selected] : std::string());
		// 注意:过滤器状态与 TextField 的编辑状态必须用**不同**的持久化 ID。
		// 曾经两者共用 id ^ 0x5A17:Persist 的类型检查失败后仍按错误类型解释内存,
		// 输入时 cursor 变成垃圾值 → 访问越界直接崩溃(World.Wui 单测可复现)。
		const WuiId filterId = id ^ 0x5A17u;
		const WuiId editId = id ^ 0x5A19u;
		std::string& filter = ctx.Persist<std::string>(filterId, std::string());
		const bool open = ctx.IsPopupOpen(id);
		const bool hovered = ctx.IsHovered(rect);
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, hovered || open ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });

		// 未展开时显示当前选中项;展开时输入框承担过滤。
		const WuiRect fieldRect { rect.X + 1.0f, rect.Y + 1.0f, rect.W - 24.0f, rect.H - 2.0f };
		const std::string current = (selected >= 0 && selected < static_cast<int>(options.size()))
			? options[selected] : std::string();
		if (!open)
			ctx.Commands().push_back({ WuiDrawKind::Text, { fieldRect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
				theme.Text, 0, 1.0f, current, 15.0f, false });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + rect.W - 18.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
			theme.TextMuted, 0, 1.0f, open ? "^" : "v", 14.0f, false });
		if (ctx.IsClicked(rect) && !open)
		{
			filter.clear();
			ctx.OpenPopup(id);
			ctx.SetFocus(editId);
		}

		bool changed = false;
		if (!open)
			return false;

		ctx.PushOverlay();
		const float rowH = 22.0f;
		constexpr size_t kMaxVisible = 8;
		// 过滤(大小写不敏感的子串匹配):空串 = 全部。
		std::string needle = filter;
		std::transform(needle.begin(), needle.end(), needle.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::vector<int> matches;
		matches.reserve(options.size());
		for (int i = 0; i < static_cast<int>(options.size()); ++i)
		{
			if (needle.empty())
			{
				matches.push_back(i);
				continue;
			}
			std::string haystack = options[i];
			std::transform(haystack.begin(), haystack.end(), haystack.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (haystack.find(needle) != std::string::npos)
				matches.push_back(i);
		}

		const size_t visible = std::min(matches.size(), kMaxVisible);
		const float panelH = 30.0f + rowH * static_cast<float>(visible) + 6.0f;
		// 弹层向上展开的条件:下方空间不够(用 UI 视口高度判断,避免被屏幕裁掉)。
		const bool flipUp = rect.Y + rect.H + panelH > ctx.Input().ViewportSize.y;
		const WuiRect panel { rect.X, flipUp ? rect.Y - panelH - 2.0f : rect.Y + rect.H + 2.0f, rect.W, panelH };
		DrawPanelSurface(ctx, panel, theme);

		const WuiRect searchRect { panel.X + 4.0f, panel.Y + 4.0f, panel.W - 8.0f, 22.0f };
		// 注意:TextField 在回车/Esc 时会把焦点清 0(它的返回值是"输入结束"语义),
		// 所以必须在调用**之前**记录搜索框是否有焦点,再用 Enter 判定"确认"。
		const bool searchFocused = ctx.Focus() == static_cast<WuiId>(editId);
		TextField(ctx, editId, searchRect, filter, theme);
		const bool submitted = searchFocused && ctx.IsKeyPressed(KeyCodes::Enter);

		float& scroll = ctx.Persist<float>(id ^ 0x5A1Bu, 0.0f);
		const WuiRect listRect { panel.X + 4.0f, panel.Y + 30.0f, panel.W - 8.0f, rowH * static_cast<float>(visible) };
		if (ctx.IsHovered(listRect) && ctx.Input().Wheel != 0.0f)
			scroll = std::clamp(scroll - ctx.Input().Wheel * 24.0f, 0.0f,
				std::max(0.0f, rowH * static_cast<float>(matches.size()) - listRect.H));

		const size_t first = static_cast<size_t>(scroll / rowH);
		for (size_t row = 0; row < visible; ++row)
		{
			const size_t matchIndex = first + row;
			if (matchIndex >= matches.size())
				break;
			const int optionIndex = matches[matchIndex];
			const WuiRect item { listRect.X, listRect.Y + rowH * static_cast<float>(row), listRect.W, rowH };
			// 展开的候选项登记成可点节点:脚本先点开 search-combo,再按 label 点这一项。
			const std::string itemKey = "combo-item:" + std::to_string(id) + ":" + std::to_string(optionIndex);
			RegisterAccessNode(HashId(itemKey.c_str()),
				"combo-item", item, options[optionIndex], std::string());
			ctx.Commands().push_back({ WuiDrawKind::ClipPush, listRect });
			if (ctx.IsHovered(item))
			{
				ctx.Commands().push_back({ WuiDrawKind::Rect, item, theme.ButtonHover, 2.0f });
				// ③ 弹层内光标归弹层:候选行给 Hand。搜索框与 listRect 不重叠,它是 TextField
				// 自己的 IBeam(更早绘制),不会被这里覆盖。
				ctx.SetCursor(WuiCursor::Hand);
			}
			ctx.Commands().push_back({ WuiDrawKind::Text, { item.X + 6.0f, item.Y + 3.0f, 0, 0 },
				theme.Text, 0, 1.0f, options[optionIndex], 15.0f, false });
			ctx.Commands().push_back({ WuiDrawKind::ClipPop });
			if (ctx.IsClicked(item))
			{
				selected = optionIndex;
				changed = true;
				ctx.ClosePopup(id);
				break;
			}
		}
		if (submitted && !matches.empty())
		{
			selected = matches.front();
			changed = true;
			ctx.ClosePopup(id);
		}
		else if (submitted)
		{
			// 没有匹配项时回车只关闭弹层,不改选中值。
			ctx.ClosePopup(id);
		}
		if (visible < matches.size() && !changed)
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ panel.X + 6.0f, panel.Y + panel.H - 16.0f, 0, 0 }, theme.TextMuted, 0, 1.0f,
				"显示前 " + std::to_string(visible) + " / " + std::to_string(matches.size()) + " 项(继续输入以缩小范围)",
				12.0f, false });
		if (matches.empty())
			ctx.Commands().push_back({ WuiDrawKind::Text, { panel.X + 6.0f, panel.Y + 34.0f, 0, 0 },
				theme.TextMuted, 0, 1.0f, "(无匹配项)", 13.0f, false });

		ctx.ClosePopupsOnOutsideClick({ id }, panel);
		if (ctx.IsKeyPressed(KeyCodes::Escape))
			ctx.ClosePopup(id);
		// ③ 弹层打开期间:弹层矩形登记为悬停遮挡区(含向上展开 flipUp 的情况)—— 本帧之后
		// 绘制的下层控件/面板在 HitTest 里判为未命中,光标与点击不再穿透弹层。顺序与 Combo
		// 一致:必须在弹层自身命中测试与"点外关闭"之后登记。BeginFrame 每帧清空遮挡区。
		if (ctx.IsPopupOpen(id))
			ctx.PushHoverBlocker(panel);
		ctx.PopOverlay();
		return changed;
	}

	bool TreeNode(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool leaf, const WuiTheme& theme)
	{
		bool& open = ctx.Persist<bool>(id, false);
		if (ctx.IsClicked(rect))
			open = !leaf && !open;
		RegisterAccessNode(id, "tree-node", rect, label, open ? "open" : "closed", true, !leaf);
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
		RegisterAccessNode(id, "menu-item", rect, label, std::string(), enabled);
		if (ctx.IsHovered(rect) && enabled)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonHover, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, enabled ? theme.Text : theme.TextMuted, 0, 1.0f, label, 15.0f, false });
		(void)id;
		return enabled && ctx.IsClicked(rect);
	}

	bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool checked, bool enabled, const WuiTheme& theme)
	{
		RegisterAccessNode(id, "menu-item", rect, label, checked ? "checked" : "unchecked", enabled);
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

	WindowControl WindowControls(WuiContext& ctx, const WuiRect& bar, const WuiTheme& theme, bool maximized)
	{
		constexpr float buttonW = 34.0f;
		const float x0 = bar.X + bar.W - buttonW * 3.0f;
		const auto buttonRect = [&](int index)
		{
			return WuiRect { x0 + buttonW * static_cast<float>(index), bar.Y, buttonW, bar.H };
		};

		const WuiRect minimize = buttonRect(0);
		const WuiRect maximize = buttonRect(1);
		const WuiRect close = buttonRect(2);
		for (const WuiRect* rect : { &minimize, &maximize, &close })
		{
			if (ctx.IsHovered(*rect))
			{
				const WuiColor bg = rect == &close ? WuiColor { 0.76f, 0.22f, 0.22f, 1 } : theme.ButtonHover;
				ctx.Commands().push_back({ WuiDrawKind::Rect, *rect, bg, 0.0f });
				ctx.SetCursor(WuiCursor::Hand);
			}
		}

		// 最小化:横线;最大化/还原:方框(还原时叠加小方框);关闭:x。
		ctx.Commands().push_back({ WuiDrawKind::Rect,
			{ minimize.X + 11.0f, minimize.Y + minimize.H * 0.5f, 12.0f, 1.0f }, theme.Text, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline,
			{ maximize.X + 11.0f, maximize.Y + maximize.H * 0.5f - 6.0f, 12.0f, 12.0f }, theme.Text, 0.0f, 1.0f });
		if (maximized)
			ctx.Commands().push_back({ WuiDrawKind::RectOutline,
				{ maximize.X + 9.0f, maximize.Y + maximize.H * 0.5f - 3.0f, 12.0f, 12.0f }, theme.Text, 0.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ close.X + 10.0f, close.Y + close.H * 0.5f - 8.0f, 0, 0 }, theme.Text, 0, 1.0f, "x", 14.0f, false });

		if (ctx.IsClicked(minimize))
			return WindowControl::Minimize;
		if (ctx.IsClicked(maximize))
			return WindowControl::Maximize;
		if (ctx.IsClicked(close))
			return WindowControl::Close;
		return WindowControl::None;
	}
}
