#include "WuiWidgets_Internal.h"

namespace World::Wui
{

using namespace WuiWidgetsDetail;

namespace WuiWidgetsDetail
{

std::string FormatFloatDisplay(float value, int decimals){
			if (!std::isfinite(value))
				return "--";
			char buffer[64] = {};
			const int places = std::max(0, std::min(9, decimals < 0 ? 3 : decimals));
			std::snprintf(buffer, sizeof(buffer), "%.*f", places, static_cast<double>(value));
			return TrimNumberText(buffer);
		}


		// 解析:允许首尾空白;要求整串被消费且结果是有限值。失败时调用方保留原值。
bool ParseFloatText(const std::string& text, float* out){
			if (out == nullptr)
				return false;
			char* end = nullptr;
			const double parsed = std::strtod(text.c_str(), &end);
			if (end == text.c_str())
				return false;
			while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
				++end;
			if (*end != 0 || !std::isfinite(parsed))
				return false;
			*out = static_cast<float>(parsed);
			return true;
		}


bool ParseIntText(const std::string& text, int64_t* out){
			if (out == nullptr)
				return false;
			char* end = nullptr;
			const long long parsed = std::strtoll(text.c_str(), &end, 10);
			if (end == text.c_str())
				return false;
			while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
				++end;
			if (*end != 0)
				return false;
			*out = static_cast<int64_t>(parsed);
			return true;
		}


void BeginNumericEdit(WuiContext& ctx, WuiNumericState& state, WuiId id, const std::string& text){
			state.Editing = true;
			state.Buffer = text;
			state.Cursor = -1;
			state.SelStart = 0;
			state.SelEnd = Utf8Count(text);
			state.ErrorFrames = 0;
			ctx.SetFocus(id);
			ctx.SetTextInputActive(true);
		}


void EndNumericEdit(WuiNumericState& state){
			state.Editing = false;
			state.Pressed = false;
			state.Dragging = false;
			state.Cursor = -1;
			state.SelStart = -1;
			state.SelEnd = -1;
		}


		// 数值文本命令:rightAlign = 值区(文本右对齐,宽度固定);否则输入框(左对齐)。
void PushNumericTextCommand(WuiContext& ctx, const WuiRect& textRect, const std::string& text, const WuiTheme& theme, const WuiNumericState& state, bool editing, bool rightAlign, const WuiColor& color){
			const float fontSize = 14.0f;
			const float x = rightAlign
				? textRect.X + std::max(0.0f, textRect.W - ctx.MeasureTextWidth(text, fontSize))
				: textRect.X + 5.0f;
			WuiDrawCommand command { WuiDrawKind::Text,
				{ x, textRect.Y + (textRect.H - 15.0f) * 0.5f, 0, 0 }, color, 0, 1.0f, text, fontSize, false };
			if (editing)
			{
				if (state.SelStart >= 0 && state.SelEnd > state.SelStart)
				{
					command.TextSelStart = static_cast<int>(Utf8Offset(text, state.SelStart));
					command.TextSelEnd = static_cast<int>(Utf8Offset(text, state.SelEnd));
				}
				else
					command.TextCursorByte = static_cast<int>(Utf8Offset(text, state.Cursor));
			}
			ctx.Commands().push_back(std::move(command));
		}

}

bool DragFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float speed, float min, float max, const WuiTheme& theme){
		// VEC-H4(§规则 6):Shift = 0.1× 精细微调、Ctrl = 10× 粗调 —— 拖动与 ↑/↓ 共用同一倍率。
		const float modifier = ctx.Input().Shift ? 0.1f : (ctx.Input().Ctrl ? 10.0f : 1.0f);
		const bool focused = ctx.Focus() == id;
		WuiNumericState& state = ctx.Persist<WuiNumericState>(id, {});
		// U24:编辑态把 kind 切成 "text-field" —— 值区此刻就是文本输入,
		// AI 通道的 ui.type 只向 editor/text-field 节点注入文本;非编辑态保持原 kind。
		RegisterAccessNode(id, state.Editing ? "text-field" : "drag-float", rect, std::string(),
			state.Editing ? state.Buffer : FloatToText(value), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		bool changed = false;
		const bool hovered = ctx.IsHovered(rect);
		const float lo = min < max ? min : -1e30f;
		const float hi = min < max ? max : 1e30f;
		if (state.ErrorFrames > 0)
			--state.ErrorFrames;

		if (state.Editing)
		{
			bool submitted = false, cancelled = false;
			if (EditUpdate(ctx, state.Buffer, state.Cursor, state.SelStart, state.SelEnd, submitted, cancelled))
			{
				if (submitted)
				{
					float parsed = 0.0f;
					if (ParseFloatText(state.Buffer, &parsed))
					{
						const float next = std::max(lo, std::min(hi, parsed));
						changed = changed || next != value;
						value = next;
						EndNumericEdit(state);
					}
					else
					{
						// U24:非法输入保留原值与编辑缓冲区,红框 + 危险色数值提示(1.5s),
						// 用户可以直接改;Esc 仍按"取消"退出。
						state.ErrorFrames = 90;
					}
				}
				else
					EndNumericEdit(state);
			}
			else if (ctx.Input().MouseClicked[0] && !hovered)
			{
				float parsed = 0.0f;
				if (ParseFloatText(state.Buffer, &parsed))
				{
					const float next = std::max(lo, std::min(hi, parsed));
					changed = changed || next != value;
					value = next;
				}
				else
					state.ErrorFrames = 90;
				EndNumericEdit(state);
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
					value = std::max(lo, std::min(hi, static_cast<float>(state.PressValue + dx * speed * modifier)));
					changed = true;
					ctx.SetCursor(WuiCursor::ResizeEW);
				}
				if (ctx.Input().MouseReleased[0])
				{
					if (!state.Dragging)
						BeginNumericEdit(ctx, state, id, FormatFloatDisplay(value, 3));
					state.Pressed = false;
					state.Dragging = false;
				}
			}
			else if (hovered)
				ctx.SetCursor(WuiCursor::ResizeEW);
		}

		// U2A/U24 键盘微调:焦点在字段上、且不在文本编辑态时,左右/上下箭头 = drag 步长(±speed)。
		// 编辑态下方向键归文本光标(EditUpdate 已消费),这里不抢。
		if (focused && !state.Editing)
		{
			const float step = (std::fabs(speed) > 0.0f ? std::fabs(speed) : 1.0f) * modifier;
			if (ctx.WasKeyPressed(KeyCodes::Left) || ctx.WasKeyPressed(KeyCodes::Down))
			{
				value = std::max(lo, value - step);
				changed = true;
			}
			if (ctx.WasKeyPressed(KeyCodes::Right) || ctx.WasKeyPressed(KeyCodes::Up))
			{
				value = std::min(hi, value + step);
				changed = true;
			}
		}

		const bool error = state.ErrorFrames > 0;
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, state.Editing ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
			error ? theme.Danger : ((state.Editing || hovered || state.Dragging) ? theme.Accent : theme.Border),
			3.0f, 1.0f });
		std::string text;
		if (state.Editing) text = state.Buffer;
		else text = FormatFloatDisplay(value, 3);
		PushNumericTextCommand(ctx, rect, text, theme, state, state.Editing, false,
			error ? theme.Danger : theme.Text);
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
	}


bool DragInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme){
		// VEC-H4:与 DragFloat 同一口径 —— Shift = 0.1×、Ctrl = 10×(整数按 1 步的倍率取整,最小 1)。
		const float modifier = ctx.Input().Shift ? 0.1f : (ctx.Input().Ctrl ? 10.0f : 1.0f);
		const int64_t keyStep = std::max<int64_t>(1, static_cast<int64_t>(std::llround(modifier)));
		const bool focused = ctx.Focus() == id;
		WuiNumericState& state = ctx.Persist<WuiNumericState>(id, {});
		RegisterAccessNode(id, state.Editing ? "text-field" : "drag-int", rect, std::string(),
			state.Editing ? state.Buffer : std::to_string(value), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		bool changed = false;
		const bool hovered = ctx.IsHovered(rect);
		const int64_t lo = min < max ? min : INT64_MIN;
		const int64_t hi = min < max ? max : INT64_MAX;
		if (state.ErrorFrames > 0)
			--state.ErrorFrames;

		if (state.Editing)
		{
			bool submitted = false, cancelled = false;
			if (EditUpdate(ctx, state.Buffer, state.Cursor, state.SelStart, state.SelEnd, submitted, cancelled))
			{
				if (submitted)
				{
					int64_t parsed = 0;
					if (ParseIntText(state.Buffer, &parsed))
					{
						const int64_t next = std::max(lo, std::min(hi, parsed));
						changed = changed || next != value;
						value = next;
						EndNumericEdit(state);
					}
					else
						state.ErrorFrames = 90;   // 保留原值与编辑缓冲区,红框提示
				}
				else
					EndNumericEdit(state);
			}
			else if (ctx.Input().MouseClicked[0] && !hovered)
			{
				int64_t parsed = 0;
				if (ParseIntText(state.Buffer, &parsed))
				{
					const int64_t next = std::max(lo, std::min(hi, parsed));
					changed = changed || next != value;
					value = next;
				}
				else
					state.ErrorFrames = 90;
				EndNumericEdit(state);
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
					value = std::max(lo, std::min(hi,
						static_cast<int64_t>(state.PressValue + static_cast<double>(dx) * static_cast<double>(modifier))));
					changed = true;
					ctx.SetCursor(WuiCursor::ResizeEW);
				}
				if (ctx.Input().MouseReleased[0])
				{
					if (!state.Dragging)
						BeginNumericEdit(ctx, state, id, std::to_string(value));
					state.Pressed = false;
					state.Dragging = false;
				}
			}
			else if (hovered)
				ctx.SetCursor(WuiCursor::ResizeEW);
		}

		// U2A/U24 键盘微调:焦点在字段上、且不在文本编辑态时,左右/上下箭头 ±1(与拖拽同量级)。
		if (focused && !state.Editing)
		{
			if (ctx.WasKeyPressed(KeyCodes::Left) || ctx.WasKeyPressed(KeyCodes::Down))
			{
				value = std::max(lo, value - keyStep);
				changed = true;
			}
			if (ctx.WasKeyPressed(KeyCodes::Right) || ctx.WasKeyPressed(KeyCodes::Up))
			{
				value = std::min(hi, value + keyStep);
				changed = true;
			}
		}

		const bool error = state.ErrorFrames > 0;
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, state.Editing ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
			error ? theme.Danger : ((state.Editing || hovered || state.Dragging) ? theme.Accent : theme.Border),
			3.0f, 1.0f });
		std::string text;
		if (state.Editing) text = state.Buffer;
		else text = std::to_string(value);
		PushNumericTextCommand(ctx, rect, text, theme, state, state.Editing, false,
			error ? theme.Danger : theme.Text);
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
	}


	// ---- U24:新数值控件族(分类规则的落点) ----
	// 规则:感知型归一化区间 → DragBarFloat;计数/索引/ID/大范围整数 → NumberFieldInt;
	//       小整数(1..16)→ StepperInt;通用自由拖动 → 既有 DragFloat/DragInt。
	// 三者共同的口径:值始终可见;单击值区进文本编辑(Enter 提交 / Esc 取消 /
	// 非法输入保留原值并给红框反馈);值区宽度固定,不随内容/恢复默认的存在而变。

bool DragBarFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme, const WuiNumberStyle& style){
		if (rect.W <= 2.0f || rect.H <= 2.0f)
			return false;
		const bool focused = ctx.Focus() == id;
		const float lo = min < max ? min : -1e30f;
		const float hi = min < max ? max : 1e30f;
		const float range = std::max(1e-6f, hi - lo);
		// 值区宽度固定(与内容、与调用方是否画恢复默认无关):文本右对齐,单位靠右。
		const float gap = 6.0f;
		const float valueW = std::max(40.0f, std::min(style.ValueWidth, rect.W * 0.45f));
		const WuiRect bar { rect.X, rect.Y, std::max(1.0f, rect.W - valueW - gap), rect.H };
		const WuiRect valueRect { bar.X + bar.W + gap, rect.Y, valueW, rect.H };
		const std::string unit = style.Unit != nullptr ? style.Unit : std::string();
		WuiNumericState& state = ctx.Persist<WuiNumericState>(id, {});
		if (state.ErrorFrames > 0)
			--state.ErrorFrames;
		bool changed = false;
		// 无障碍 kind 非编辑态沿用 "slider"(与既有 SliderFloat 同一语义/id 口径),value = 显示值 + 单位;
		// 编辑态切成 "text-field"(值区此刻是文本输入,AI 通道 ui.type 要求该 kind)。
		RegisterAccessNode(id, state.Editing ? "text-field" : "slider", rect, std::string(),
			(state.Editing ? state.Buffer : FormatFloatDisplay(value, style.Decimals)) + unit,
			true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const float fontSize = 14.0f;
		const float unitW = unit.empty() ? 0.0f : ctx.MeasureTextWidth(unit, fontSize) + 4.0f;
		const WuiRect textRect { valueRect.X + 4.0f, valueRect.Y,
			std::max(1.0f, valueRect.W - 8.0f - unitW), valueRect.H };

		const bool hovered = ctx.IsHovered(rect);
		// MAT-UI3a:条体/值区的**原始**命中(不看编辑态)—— 用于"编辑态下按条体就立刻拖动"这一步。
		const bool hoverBarRaw = ctx.IsHovered(bar);
		const bool hoverValueRaw = ctx.IsHovered(valueRect);
		// 编辑态里按条体 = 先提交编辑(与 Enter 同一条路径:非法文本保留原值并给红框),再把这帧
		// 交给下面的"条体按下"分支 —— 用户不必先点别处退出编辑(用户原话:「点击右侧值后左侧滑条
		// 没法滑动了」)。按值区仍继续编辑;按控件外仍是提交并结束。
		if (state.Editing && ctx.Input().MouseClicked[0] && hoverBarRaw)
		{
			float parsed = 0.0f;
			if (ParseFloatText(state.Buffer, &parsed))
			{
				const float next = std::max(lo, std::min(hi, parsed));
				changed = changed || next != value;
				value = next;
			}
			else
				state.ErrorFrames = 90;   // 与 Enter 提交失败同一条反馈:保留原值 + 缓冲
			EndNumericEdit(state);
		}
		const bool hoverBar = !state.Editing && hoverBarRaw;
		const bool hoverValue = !state.Editing && hoverValueRaw;
		if (state.Editing)
		{
			bool submitted = false, cancelled = false;
			if (EditUpdate(ctx, state.Buffer, state.Cursor, state.SelStart, state.SelEnd, submitted, cancelled))
			{
				if (submitted)
				{
					float parsed = 0.0f;
					if (ParseFloatText(state.Buffer, &parsed))
					{
						const float next = std::max(lo, std::min(hi, parsed));
						changed = changed || next != value;
						value = next;
						EndNumericEdit(state);
					}
					else
						state.ErrorFrames = 90;   // 保留原值与缓冲区,红框提示
				}
				else
					EndNumericEdit(state);
			}
			else if (ctx.Input().MouseClicked[0] && !hovered)
			{
				float parsed = 0.0f;
				if (ParseFloatText(state.Buffer, &parsed))
				{
					const float next = std::max(lo, std::min(hi, parsed));
					changed = changed || next != value;
					value = next;
				}
				else
					state.ErrorFrames = 90;
				EndNumericEdit(state);
			}
			if (ctx.IsHovered(valueRect))
				ctx.SetCursor(WuiCursor::IBeam);
		}
		else
		{
			if (ctx.Input().MouseClicked[0] && (hoverBar || hoverValue))
			{
				state.Pressed = true;
				state.PressOnValue = hoverValue;
				state.PressX = ctx.Input().MousePos.x;
				state.PressValue = value;
			}
			if (state.Pressed)
			{
				if (state.PressOnValue)
				{
					// 值区:松手 = 文本编辑(单击/双击同一条路径);拖动不改值。
					if (ctx.Input().MouseReleased[0])
					{
						BeginNumericEdit(ctx, state, id, FormatFloatDisplay(value, style.Decimals));
						state.Pressed = false;
					}
				}
				else
				{
					// 条体:按像素比例(绝对位置)改值,按下即生效,按住持续跟随。
					ctx.SetFocus(id);
					const float fraction = std::max(0.0f, std::min(1.0f,
						(ctx.Input().MousePos.x - bar.X) / std::max(1.0f, bar.W)));
					const float next = lo + fraction * range;
					changed = changed || next != value;
					value = next;
					ctx.SetCursor(WuiCursor::ResizeEW);
					if (ctx.Input().MouseReleased[0])
						state.Pressed = false;
				}
			}
			else if (hoverBar)
				ctx.SetCursor(WuiCursor::ResizeEW);
			else if (hoverValue)
				ctx.SetCursor(WuiCursor::IBeam);
		}

		// 键盘步进 = 1% 值域(与 SliderFloat 的既有键盘口径一致);←/→ 保留兼容。
		if (focused && !state.Editing)
		{
			const float step = range * 0.01f;
			if (ctx.WasKeyPressed(KeyCodes::Left) || ctx.WasKeyPressed(KeyCodes::Down))
			{
				value = std::max(lo, value - step);
				changed = true;
			}
			if (ctx.WasKeyPressed(KeyCodes::Right) || ctx.WasKeyPressed(KeyCodes::Up))
			{
				value = std::min(hi, value + step);
				changed = true;
			}
		}

		const bool error = state.ErrorFrames > 0;
		const float trackH = 6.0f;
		const float trackY = bar.Y + bar.H * 0.5f - trackH * 0.5f;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { bar.X, trackY, bar.W, trackH }, theme.ButtonBg, 3.0f });
		const float fraction = std::max(0.0f, std::min(1.0f, (value - lo) / range));
		if (fraction > 0.0f)
			ctx.Commands().push_back({ WuiDrawKind::Rect, { bar.X, trackY, bar.W * fraction, trackH }, theme.Accent, 3.0f });
		const float thumbW = 6.0f;
		ctx.Commands().push_back({ WuiDrawKind::Rect,
			{ bar.X + bar.W * fraction - thumbW * 0.5f, bar.Y + 2.0f, thumbW, std::max(2.0f, bar.H - 4.0f) },
			(hoverBar || state.Pressed || focused) ? theme.Text : theme.TextMuted, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, valueRect, theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, valueRect,
			error ? theme.Danger : ((state.Editing || focused) ? theme.Accent : theme.Border),
			3.0f, state.Editing ? 1.5f : 1.0f });
		PushNumericTextCommand(ctx, textRect, state.Editing ? state.Buffer : FormatFloatDisplay(value, style.Decimals),
			theme, state, state.Editing, true, error ? theme.Danger : theme.Text);
		if (!unit.empty())
		{
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ valueRect.X + valueRect.W - 4.0f - ctx.MeasureTextWidth(unit, fontSize),
					valueRect.Y + (valueRect.H - fontSize) * 0.5f, 0, 0 },
				theme.TextMuted, 0, 1.0f, unit, fontSize, false });
		}
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
	}

}
