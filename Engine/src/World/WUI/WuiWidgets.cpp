#include "WuiWidgets_Internal.h"

namespace World::Wui
{

using namespace WuiWidgetsDetail;

namespace WuiWidgetsDetail
{
		// 控件绘制时登记无障碍节点(AI 控制通道的 ui.tree / ui.invoke 数据源)。
		// 关闭控制通道时这一步只是往一个 vector 里追加,无额外分配以外的副作用。
		// VEC-H7:tooltip 也进节点 —— "看得到的信息"与悬停提示必须同源(禁用件的理由走它)。
void RegisterAccessNode(WuiId id, const char* kind, const WuiRect& rect, const std::string& label, const std::string& value, bool enabled , bool interactive , bool focused , const std::string& tooltip ){
			if (id == 0)
				return;
			WuiAccessNode node;
			node.Id = id;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = interactive;
			node.Focused = focused;
			WuiAccessibility::Get().Register(node);
		}


std::string FloatToText(float value){
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3f", value);
			return buffer;
		}


void AppendUtf8(std::string& buffer, uint32_t codepoint){
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


void PopUtf8(std::string& buffer){
			if (buffer.empty())
				return;
			buffer.pop_back();
			while (!buffer.empty() && (static_cast<unsigned char>(buffer.back()) & 0xC0) == 0x80)
				buffer.pop_back();
		}


		// 子控件(弹层条目 / 标签 / 分段项)的无障碍 id:由父控件 id + 类别 + 下标派生
		// (与 Wui::HashId 同一套 FNV-1a)。不用文本参与哈希:同名条目仍有各自独立的 id。
WuiId DerivedChildId(WuiId parent, const char* category, size_t index){
			const std::string key = std::to_string(parent) + category + std::to_string(index);
			return HashId(key.c_str());
		}


		// 弹层条目的无障碍 id:父控件 id + 选项下标(既有约定,逐字节保持 —— 脚本按它算 id)。
WuiId ComboOptionId(WuiId comboId, size_t index){
			return DerivedChildId(comboId, ".option.", index);
		}


		// 两点 + 线宽 → 一个任意四边形命令(勾、斜线这类轴对齐矩形覆盖不到的形状)。
		// 顶点顺序遵循 WuiDrawKind::Quad 约定(左上/右上/右下/左下)。
void PushLineQuad(WuiContext& ctx, glm::vec2 from, glm::vec2 to, float thickness, const WuiColor& color){
			const glm::vec2 delta = to - from;
			const float length = glm::length(delta);
			if (length <= 0.0001f)
				return;
			const glm::vec2 normal { -delta.y / length * thickness * 0.5f, delta.x / length * thickness * 0.5f };
			WuiDrawCommand command;
			command.Kind = WuiDrawKind::Quad;
			command.Color = color;
			command.Vertices = { from + normal, to + normal, to - normal, from - normal };
			ctx.Commands().push_back(std::move(command));
		}


		// 文本按像素宽度截断(超宽补 '…')。语义与 WuiCodeEditor 的 EllipsizeToWidth 一致
		// (那边是文件内匿名实现,不可跨 TU 复用,故在此按同一语义重写一份):
		// 逐码点累加,候选宽度 + 12px 余量超过 maxWidth 就停;maxWidth <= 0 时不裁剪。
std::string EllipsizeToWidth(const WuiContext& ctx, std::string_view text, float maxWidth, float fontSize){
			if (maxWidth <= 0.0f || text.empty())
				return std::string(text);
			if (ctx.MeasureTextWidth(text, fontSize) <= maxWidth)
				return std::string(text);
			std::string out;
			size_t i = 0;
			bool any = false;
			while (i < text.size())
			{
				const size_t begin = i;
				size_t length = 1;
				const unsigned char lead = static_cast<unsigned char>(text[i]);
				if ((lead & 0xE0) == 0xC0) length = 2;
				else if ((lead & 0xF0) == 0xE0) length = 3;
				else if ((lead & 0xF8) == 0xF0) length = 4;
				length = std::min(length, text.size() - i);
				i += length;
				std::string candidate = out;
				candidate.append(text.substr(begin, length));
				if (ctx.MeasureTextWidth(candidate, fontSize) + 12.0f > maxWidth)
					break;
				any = true;
				out = std::move(candidate);
			}
			// 连一个字符都放不下(只够 '…')时返回空串:调用方据此干脆不画,而不是画一个孤立省略号。
			if (!any)
				return std::string();
			out += "…";
			return out;
		}

}

void DrawPanelSurface(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme){
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.PanelBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });
	}


void Panel(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiTheme& theme){
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.PanelBg, 4.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 4.0f, 1.0f });
		const WuiRect header { rect.X, rect.Y, rect.W, 26.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, header, theme.PanelHeader, 4.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { header.X + 10.0f, header.Y + 4.0f, 0, 0 }, theme.Text, 0, 1.0f, title, 15.0f, false });
	}


void Label(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const WuiColor& color, float fontSize){
		ctx.Commands().push_back({ WuiDrawKind::Text, { pos.x, pos.y, 0, 0 }, color, 0, 1.0f, text, fontSize, false });
	}


void LabelWithTerm(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const std::string& term, const WuiColor& color, float fontSize, const WuiTheme& theme){
		// 兼容重载:旧调用点不带宽度,按标签列的默认预算裁剪(否则长术语会压住右侧控件)。
		LabelWithTerm(ctx, pos, text, term, color, fontSize, theme, LabelDefaultWidth);
	}


void LabelWithTerm(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const std::string& term, const WuiColor& color, float fontSize, const WuiTheme& theme, float width){
		// P4-UX5 标签列裁剪 + VEC-H5 优先级修正(用户口径:「组件名称有时候会只剩 "..."」):
		// width = 本标签可用的设计单位宽度(含术语)。降级顺序**永远是主名优先** ——
		// ① 主名 + 术语都放得下 → 原样;② 主名放得下 → 主名完整,术语只吃剩余宽度(超宽先缩略,
		//  连一个字符 + 省略号都放不下就不画);③ 主名自己放不下 → 整列归主名缩略,术语让位。
		// 旧口径先把术语的完整宽度从预算里扣掉,长术语(DirectionalLightComponent 这类)会把主名
		// 挤成 "…"、甚至把整串(含展开标记)挤得连一个字符都放不下 → 标题只剩省略号/只剩术语。
		const float gap = 6.0f;
		const float termSize = theme.FontSizeCaption;
		const bool hasTerm = !term.empty();
		const bool limited = width > 0.0f;
		const float textWidth = ctx.MeasureTextWidth(text, fontSize);
		const float termWidth = hasTerm ? ctx.MeasureTextWidth(term, termSize) : 0.0f;
		const bool overflows = limited && textWidth + (hasTerm ? gap + termWidth : 0.0f) > width;

		if (!overflows)
		{
			// 放得下:原样绘制(与旧行为逐字节一致)。
			Label(ctx, pos, text, color, fontSize);
			if (hasTerm)
				ctx.Commands().push_back({ WuiDrawKind::Text, { pos.x + textWidth + gap, pos.y + (fontSize - termSize) * 0.5f, 0, 0 },
					theme.TextMuted, 0, 1.0f, term, termSize, false });
			return;
		}

		// ② 主名先拿"整列优先权":放得下就完整画主名(不因术语被压)。
		if (textWidth <= width)
		{
			Label(ctx, pos, text, color, fontSize);
			if (!hasTerm)
				return;
			const float termBudget = width - textWidth - gap;
			if (termBudget <= 0.0f)
				return;   // 主名已占满整列:不画术语
			const std::string shownTerm = termWidth <= termBudget
				? term
				: EllipsizeToWidth(ctx, term, termBudget, termSize);
			if (shownTerm.empty())
				return;   // 连一个字符加省略号都放不下:不画术语,而不是画一个孤立 '…'
			ctx.Commands().push_back({ WuiDrawKind::Text, { pos.x + textWidth + gap, pos.y + (fontSize - termSize) * 0.5f, 0, 0 },
				theme.TextMuted, 0, 1.0f, shownTerm, termSize, false });
			return;
		}

		// ③ 主名自己也放不下:整列归主名缩略;术语不再参与(它不得反压主名)。
		const std::string shownText = EllipsizeToWidth(ctx, text, width, fontSize);
		if (!shownText.empty())
			Label(ctx, pos, shownText, color, fontSize);
	}

namespace WuiWidgetsDetail
{
		// 按绘制宽度折行:支持 '\n' 硬换行;对 CJK(无空格)按字符断行。
std::vector<std::string> WrapTooltipText(WuiContext& ctx, const std::string& text, float fontSize, float maxWidth){
			std::vector<std::string> lines;
			const auto emit = [&](const std::string& paragraph)
			{
				std::string current;
				size_t index = 0;
				while (index < paragraph.size())
				{
					// 取一个 UTF-8 字符
					const unsigned char lead = static_cast<unsigned char>(paragraph[index]);
					size_t length = 1;
					if ((lead & 0xE0) == 0xC0) length = 2;
					else if ((lead & 0xF0) == 0xE0) length = 3;
					else if ((lead & 0xF8) == 0xF0) length = 4;
					length = std::min(length, paragraph.size() - index);
					std::string candidate = current + paragraph.substr(index, length);
					if (!current.empty() && ctx.MeasureTextWidth(candidate, fontSize) > maxWidth)
					{
						lines.push_back(current);
						current.clear();
						continue;   // 重新尝试放这个字符
					}
					current = std::move(candidate);
					// 西文按空格优先断行:遇到空格且下一段超宽时在此断开
					index += length;
				}
				if (!current.empty())
					lines.push_back(current);
			};
			std::string paragraph;
			for (size_t i = 0; i < text.size(); ++i)
			{
				if (text[i] == '\n')
				{
					emit(paragraph);
					paragraph.clear();
					continue;
				}
				paragraph.push_back(text[i]);
			}
			emit(paragraph);
			if (lines.empty())
				lines.push_back(std::string());
			return lines;
		}

		// NumberFieldInt / StepperInt 的共同实现:steppers=true 时左右各一个 [−]/[+] 步进钮。
		// 计数/索引类字段**不做拖动改值**(避免拖出奇怪的大整数);值区右对齐、单位靠右。
bool IntNumberFieldCore(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme, const WuiNumberStyle& style, bool steppers, const char* kind){
			if (rect.W <= 2.0f || rect.H <= 2.0f)
				return false;
			const bool focused = ctx.Focus() == id;
			const int64_t lo = min < max ? min : INT64_MIN;
			const int64_t hi = min < max ? max : INT64_MAX;
			const float stepW = steppers ? std::max(18.0f, std::min(24.0f, rect.W * 0.18f)) : 0.0f;
			const float gap = steppers ? 3.0f : 0.0f;
			const WuiRect decRect { rect.X, rect.Y, stepW, rect.H };
			const WuiRect incRect { rect.X + rect.W - stepW, rect.Y, stepW, rect.H };
			const WuiRect fieldRect { rect.X + (steppers ? stepW + gap : 0.0f), rect.Y,
				std::max(1.0f, rect.W - (steppers ? (stepW + gap) * 2.0f : 0.0f)), rect.H };
			const std::string unit = style.Unit != nullptr ? style.Unit : std::string();
			WuiNumericState& state = ctx.Persist<WuiNumericState>(id, {});
			if (state.ErrorFrames > 0)
				--state.ErrorFrames;
			bool changed = false;
			RegisterAccessNode(id, state.Editing ? "text-field" : kind, rect, std::string(),
				(state.Editing ? state.Buffer : std::to_string(value)) + unit, true, true, focused);
			ctx.RegisterFocusable(id, rect);
			if (steppers)
			{
				// 子按钮 id 走 DerivedChildId(id, ".dec"/".inc", 0):脚本可按同一算法复算。
				RegisterAccessNode(DerivedChildId(id, ".dec", 0), "stepper-button", decRect, "-", "", true, true, false);
				RegisterAccessNode(DerivedChildId(id, ".inc", 0), "stepper-button", incRect, "+", "", true, true, false);
			}
			const float fontSize = 14.0f;
			const float unitW = unit.empty() ? 0.0f : ctx.MeasureTextWidth(unit, fontSize) + 4.0f;
			const WuiRect textRect { fieldRect.X + 4.0f, fieldRect.Y,
				std::max(1.0f, fieldRect.W - 8.0f - unitW), fieldRect.H };
			const bool hovered = ctx.IsHovered(fieldRect);

			if (steppers)
			{
				// MAT-UI3a:步进钮与"正在编辑的缓冲"是同一类冲突(与 DragBar 的"值编辑挡住条体拖动"
				// 同源)—— 点 +/− 前先把缓冲提交,否则这次步进会被随后的 Enter/点别处用旧缓冲覆盖,
				// 用户看到"按了 + 却没有变化"。非法缓冲沿用旧口径:保留原值 + 红框,并结束编辑。
				const bool stepperPress = ctx.Input().MouseClicked[0]
					&& (ctx.IsHovered(decRect) || ctx.IsHovered(incRect));
				if (stepperPress && state.Editing)
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
				if (ctx.IsClicked(decRect))
				{
					const int64_t next = std::max(lo, value - 1);
					changed = changed || next != value;
					value = next;
				}
				if (ctx.IsClicked(incRect))
				{
					const int64_t next = std::min(hi, value + 1);
					changed = changed || next != value;
					value = next;
				}
				if (ctx.IsHovered(decRect) || ctx.IsHovered(incRect))
					ctx.SetCursor(WuiCursor::Hand);
			}

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
							state.ErrorFrames = 90;
					}
					else
						EndNumericEdit(state);
				}
				else if (ctx.Input().MouseClicked[0] && !ctx.IsHovered(rect))
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
				if (ctx.IsHovered(fieldRect))
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
					if (ctx.Input().MouseReleased[0])
					{
						BeginNumericEdit(ctx, state, id, std::to_string(value));
						state.Pressed = false;
					}
				}
				else if (hovered)
					ctx.SetCursor(WuiCursor::IBeam);
			}
			if (focused && !state.Editing)
			{
				if (ctx.WasKeyPressed(KeyCodes::Left) || ctx.WasKeyPressed(KeyCodes::Down))
				{
					value = std::max(lo, value - 1);
					changed = true;
				}
				if (ctx.WasKeyPressed(KeyCodes::Right) || ctx.WasKeyPressed(KeyCodes::Up))
				{
					value = std::min(hi, value + 1);
					changed = true;
				}
			}

			const bool error = state.ErrorFrames > 0;
			ctx.Commands().push_back({ WuiDrawKind::Rect, fieldRect, theme.ButtonBg, 3.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, fieldRect,
				error ? theme.Danger : ((state.Editing || focused) ? theme.Accent : theme.Border),
				3.0f, state.Editing ? 1.5f : 1.0f });
			PushNumericTextCommand(ctx, textRect, state.Editing ? state.Buffer : std::to_string(value),
				theme, state, state.Editing, true, error ? theme.Danger : theme.Text);
			if (!unit.empty())
			{
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ fieldRect.X + fieldRect.W - 4.0f - ctx.MeasureTextWidth(unit, fontSize),
						fieldRect.Y + (fieldRect.H - fontSize) * 0.5f, 0, 0 },
					theme.TextMuted, 0, 1.0f, unit, fontSize, false });
			}
			if (steppers)
			{
				const bool decHover = ctx.IsHovered(decRect);
				const bool incHover = ctx.IsHovered(incRect);
				ctx.Commands().push_back({ WuiDrawKind::Rect, decRect, decHover ? theme.ButtonHover : theme.ButtonBg, 3.0f });
				ctx.Commands().push_back({ WuiDrawKind::RectOutline, decRect, theme.Border, 3.0f, 1.0f });
				ctx.Commands().push_back({ WuiDrawKind::Rect, incRect, incHover ? theme.ButtonHover : theme.ButtonBg, 3.0f });
				ctx.Commands().push_back({ WuiDrawKind::RectOutline, incRect, theme.Border, 3.0f, 1.0f });
				const float glyphSize = 15.0f;
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ decRect.X + (decRect.W - ctx.MeasureTextWidth("-", glyphSize)) * 0.5f,
						decRect.Y + (decRect.H - glyphSize) * 0.5f, 0, 0 },
					theme.Text, 0, 1.0f, "-", glyphSize, false });
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ incRect.X + (incRect.W - ctx.MeasureTextWidth("+", glyphSize)) * 0.5f,
						incRect.Y + (incRect.H - glyphSize) * 0.5f, 0, 0 },
					theme.Text, 0, 1.0f, "+", glyphSize, false });
			}
			DrawFocusRing(ctx, rect, id, theme);
			return changed;
		}

}

bool NumberFieldInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme, const WuiNumberStyle& style){
		return IntNumberFieldCore(ctx, id, rect, value, min, max, theme, style,
			style.Steppers, "number-field");
	}


bool StepperInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int& value, int min, int max, const WuiTheme& theme, const WuiNumberStyle& style){
		int64_t wide = static_cast<int64_t>(value);
		const bool changed = IntNumberFieldCore(ctx, id, rect, wide, static_cast<int64_t>(min),
			static_cast<int64_t>(max), theme, style, true, "stepper");
		value = static_cast<int>(wide);
		return changed;
	}


	// VEC-H7(用户 2026-09-27:「对于一个普通值的属性展示,右侧恢复按钮在鼠标悬浮时几乎看不到」):
	// 复位按钮 = **一眼可见的可点控件** —— 外观语言与行内 `-`/`+` 的 RowActionButton 对齐
	// (底色 + 描边 + 亮字形),不再"常态只剩一条灰线、悬停还可能被底色盖住"。
	//
	//  · 命中区 = 调用方给的 rect(属性行 = 24×24,4px 栅格,≥ 20×20);**可见底板** = 20×20 居中在命中区里
	//    (设计口径的"20×20 图标按钮"):缩小绘制面,不缩小可点面;
	//  · 四态 —— default(ButtonBg 底 + Border 描边 + Text 字形,**不靠 hover 才出现**)/
	//    hover(ButtonHover 底 + Accent 描边 + Accent 字形)/ pressed(Selection 底 + Accent 描边,内容不位移)/
	//    disabled(PanelBg 底 + Border 描边 + TextDisabled 字形,理由进 tooltip);
	//  · focus 走 DrawFocusRing(overlay 层,后画的兄弟控件盖不住);
	//  · **绘制顺序契约**:调用方必须先画行/容器底色再调本控件 —— VEC-H7 前的 PropertyRow 把本控件画在
	//    行悬停底色之前,行一悬停 ↺ 就被底色整块盖掉(用户看到的就是"悬浮时几乎看不到");
	//  · 字形 = 自己画的回旋箭头(不依赖字体里有没有 ↺ 字形),弧线 + 箭头随底板等比缩放,线宽 ≥ 1px;
	//  · 无障碍:kind="button"(与行内 `-`/`+`、分组头、ButtonEx 同一契约)、value="modified"/"default"、
	//    enabled/interactive 跟随 modified、tooltip 进节点 Tooltip(禁用态 = 那条理由,与悬停提示同源)。
bool ResetDefaultButton(WuiContext& ctx, WuiId id, const WuiRect& rect, bool modified, const WuiTheme& theme, const std::string& label, const std::string& tooltip){
		if (rect.W <= 2.0f || rect.H <= 2.0f)
			return false;
		// 固定占位:两种状态都登记同一个 id/rect;modified=false 时 enabled/interactive=false,
		// 且不参与焦点表 —— 尺寸与位置逐像素不变,只是弱化。
		const float box = std::min(std::min(rect.W, rect.H), 20.0f);
		const WuiRect face { rect.X + (rect.W - box) * 0.5f, rect.Y + (rect.H - box) * 0.5f, box, box };
		const bool hoveredAny = ctx.IsHovered(rect);
		const bool hovered = modified && hoveredAny;
		const bool focused = modified && ctx.Focus() == id;
		const bool pressed = hovered && ctx.Input().MouseDown[0];
		const WuiColor fill = !modified ? theme.PanelBg
			: (pressed ? theme.Selection : (hovered ? theme.ButtonHover : theme.ButtonBg));
		const WuiColor outline = !modified ? theme.Border
			: ((hovered || pressed) ? theme.Accent : theme.Border);
		const WuiColor color = !modified ? theme.TextDisabled
			: (pressed ? theme.Text : (hovered ? theme.Accent : theme.Text));
		ctx.Commands().push_back({ WuiDrawKind::Rect, face, fill, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, face, outline, 3.0f, 1.0f });
		const float cx = face.X + face.W * 0.5f;
		const float cy = face.Y + face.H * 0.5f;
		const float radius = std::max(3.0f, std::min(6.5f, box * 0.30f));
		const float stroke = std::max(1.0f, box * 0.08f);
		constexpr float kPi = 3.14159265358979323846f;
		const float startAngle = -0.55f * kPi;
		const float sweep = 1.62f * kPi;
		constexpr int kSegments = 12;
		glm::vec2 previous { cx + radius * std::cos(startAngle), cy + radius * std::sin(startAngle) };
		for (int i = 1; i <= kSegments; ++i)
		{
			const float angle = startAngle + sweep * (static_cast<float>(i) / static_cast<float>(kSegments));
			const glm::vec2 point { cx + radius * std::cos(angle), cy + radius * std::sin(angle) };
			PushLineQuad(ctx, previous, point, stroke, color);
			previous = point;
		}
		// 回旋箭头(不依赖字体里有没有 ↺ 字形):在弧线末端按切线方向画两段短线,长度随弧半径等比。
		const float tangentX = -std::sin(startAngle + sweep);
		const float tangentY = std::cos(startAngle + sweep);
		const float arrowLength = radius * 0.52f;
		const float arrowHalf = radius * 0.25f;
		const glm::vec2 arrowA { previous.x - tangentX * arrowLength + tangentY * arrowHalf,
			previous.y - tangentY * arrowLength - tangentX * arrowHalf };
		const glm::vec2 arrowB { previous.x - tangentX * arrowLength - tangentY * arrowHalf,
			previous.y - tangentY * arrowLength + tangentX * arrowHalf };
		PushLineQuad(ctx, previous, arrowA, stroke, color);
		PushLineQuad(ctx, previous, arrowB, stroke, color);
		RegisterAccessNode(id, "button", rect, label.empty() ? std::string("Reset") : label,
			modified ? "modified" : "default", modified, modified, focused, tooltip);
		if (modified)
			ctx.RegisterFocusable(id, rect);
		if (hovered)
			ctx.SetCursor(WuiCursor::Hand);
		DrawFocusRing(ctx, rect, id, theme);
		// 禁用态也要给理由(与 RowActionButton / ButtonEx 同一口径):悬停提示与 a11y 的 Tooltip 同源。
		if (hoveredAny && !tooltip.empty())
			Tooltip(ctx, rect, tooltip);
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		return modified && (ctx.IsClicked(rect) || keyActivated);
	}

namespace WuiWidgetsDetail
{

		// RGB(0..1) ↔ HSV(色相 0..360,饱和/明度 0..1)。
glm::vec3 ColorToHsv(const glm::vec3& rgb){
			const float maxChannel = std::max(rgb.x, std::max(rgb.y, rgb.z));
			const float minChannel = std::min(rgb.x, std::min(rgb.y, rgb.z));
			const float delta = maxChannel - minChannel;
			float hue = 0.0f;
			if (delta > 1e-6f)
			{
				if (maxChannel == rgb.x) hue = 60.0f * std::fmod((rgb.y - rgb.z) / delta, 6.0f);
				else if (maxChannel == rgb.y) hue = 60.0f * ((rgb.z - rgb.x) / delta + 2.0f);
				else hue = 60.0f * ((rgb.x - rgb.y) / delta + 4.0f);
			}
			if (hue < 0.0f) hue += 360.0f;
			const float saturation = maxChannel <= 1e-6f ? 0.0f : delta / maxChannel;
			return { hue, saturation, maxChannel };
		}


glm::vec3 HsvToColor(const glm::vec3& hsv){
			const float hue = std::fmod(std::fmod(hsv.x, 360.0f) + 360.0f, 360.0f);
			const float saturation = std::clamp(hsv.y, 0.0f, 1.0f);
			const float value = std::clamp(hsv.z, 0.0f, 1.0f);
			const float c = value * saturation;
			const float x = c * (1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f));
			const float m = value - c;
			glm::vec3 rgb { 0.0f };
			if (hue < 60.0f) rgb = { c, x, 0.0f };
			else if (hue < 120.0f) rgb = { x, c, 0.0f };
			else if (hue < 180.0f) rgb = { 0.0f, c, x };
			else if (hue < 240.0f) rgb = { 0.0f, x, c };
			else if (hue < 300.0f) rgb = { x, 0.0f, c };
			else rgb = { c, 0.0f, x };
			return rgb + glm::vec3 { m };
		}


WuiColor ToWuiColor(const glm::vec3& rgb, float alpha ){
			return { std::clamp(rgb.x, 0.0f, 1.0f), std::clamp(rgb.y, 0.0f, 1.0f),
				std::clamp(rgb.z, 0.0f, 1.0f), std::clamp(alpha, 0.0f, 1.0f) };
		}


		// 当前色的规范 hex 文本:6 位 = 不透明,带 alpha(未满)时 8 位,与解析规则对称。
std::string FormatColorHex(const glm::vec4& rgba){
			const auto channel = [](float value)
			{
				return static_cast<unsigned>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
			};
			char buffer[16] = {};
			const bool opaque = rgba.a >= 0.999f;
			std::snprintf(buffer, sizeof(buffer), opaque ? "#%02X%02X%02X" : "#%02X%02X%02X%02X",
				channel(rgba.r), channel(rgba.g), channel(rgba.b), channel(rgba.a));
			return buffer;
		}


		// 解析 "#RRGGBB" / "#RRGGBBAA":允许省略 '#'、允许首尾空白、大小写均可。
		// 6 位 = RGB + alpha 归 1;8 位 = RGBA。非法输入返回 false 且**不改动** out
		// (调用方据此"保持原值"),与 ColorField 的"非法输入保持原值"契约一致。
bool ParseColorHex(std::string_view text, glm::vec4& out){
			size_t begin = 0;
			size_t end = text.size();
			while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
				++begin;
			while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
				--end;
			if (begin < end && text[begin] == '#')
				++begin;
			const size_t digits = end - begin;
			if (digits != 6 && digits != 8)
				return false;
			for (size_t i = begin; i < end; ++i)
			{
				const char c = text[i];
				const bool hexDigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
				if (!hexDigit)
					return false;
			}
			const auto byteAt = [&text, begin](size_t offset)
			{
				const char pair[3] = { text[begin + offset], text[begin + offset + 1], 0 };
				return static_cast<float>(std::strtoul(pair, nullptr, 16)) / 255.0f;
			};
			out = glm::vec4 { byteAt(0), byteAt(2), byteAt(4), digits == 8 ? byteAt(6) : 1.0f };
			return true;
		}


bool SameColor(const glm::vec4& a, const glm::vec4& b){
			return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
		}

}

bool TableHeader(WuiContext& ctx, WuiId id, const WuiRect& table, const std::vector<std::string>& columns, const std::vector<float>& columnWidths, int& sortColumn, bool& ascending, const WuiTheme& theme){
		// 常驻表头的底板 + 底部 1px 分隔线(表格行的网格线由调用方画,这里只负责"常驻表头")。
		ctx.Commands().push_back({ WuiDrawKind::Rect, table, theme.PanelHeader, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect,
			{ table.X, table.Y + std::max(0.0f, table.H - 1.0f), table.W, 1.0f }, theme.BorderStrong, 0.0f });
		// 表头整行的节点(P1c-a):列等分整行 → 节点矩形里任何一点都落在某一列上,按它注入的
		// 点击(= 组中心那一列)一定触发一次真实排序;要精确点某一列仍用子节点 kind="table-header"。
		{
			std::string sortInfo;
			if (sortColumn >= 0 && sortColumn < static_cast<int>(columns.size()))
				sortInfo = "sort=" + columns[static_cast<size_t>(sortColumn)] + (ascending ? " asc" : " desc");
			RegisterAccessNode(id, "table-header-row", table, std::string(), sortInfo, true, true);
		}

		const float fontSize = theme.FontSizeBody;
		const float arrowSize = theme.FontSizeCaption;
		const float textPad = theme.PadSmall * 1.5f;   // 列名左侧内边距(6 设计单位,与表体文本一致)
		bool changed = false;
		// 列宽表比列名短时以两者较小值为准(Table.Cell 对缺失列会回退整表宽,故先夹住数量)。
		const size_t count = std::min(columns.size(), columnWidths.size());
		for (size_t i = 0; i < count; ++i)
		{
			const WuiRect cell = TableCell(table, columnWidths, 0, i, table.H);
			const WuiId columnId = DerivedChildId(id, ".col.", i);
			const bool sorted = sortColumn == static_cast<int>(i);
			const bool focused = ctx.Focus() == columnId;
			RegisterAccessNode(columnId, "table-header", cell, columns[i],
				sorted ? (ascending ? "asc" : "desc") : std::string(), true, true, focused);
			ctx.RegisterFocusable(columnId, cell);
			const bool hovered = ctx.IsHovered(cell);
			if (hovered)
			{
				// 悬停底留出底部 1px:不要盖住常驻表头的分隔线。
				ctx.Commands().push_back({ WuiDrawKind::Rect,
					{ cell.X, cell.Y, cell.W, std::max(0.0f, cell.H - 1.0f) }, theme.HoverBg, 0.0f });
				ctx.SetCursor(WuiCursor::Hand);
			}
			// 列名左对齐,右侧给排序箭头留 14px;超宽按省略号裁剪,不压到箭头/相邻列。
			const float labelBudget = std::max(0.0f, cell.W - textPad - kTableSortArrowReserve);
			const std::string label = EllipsizeToWidth(ctx, columns[i], labelBudget, fontSize);
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ cell.X + textPad, cell.Y + (cell.H - fontSize) * 0.5f, 0, 0 },
				sorted ? theme.Text : theme.TextMuted, 0, 1.0f, label, fontSize, false });
			if (sorted)
			{
				// ▲/▼ 在 Noto 回退列里;排在保留区内居中,不用字体里的箭头字形拼线。
				const std::string arrow = ascending ? "▲" : "▼";
				const float arrowWidth = ctx.MeasureTextWidth(arrow, arrowSize);
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ cell.X + cell.W - kTableSortArrowReserve + std::max(0.0f, (kTableSortArrowReserve - arrowWidth) * 0.5f),
					  cell.Y + (cell.H - arrowSize) * 0.5f, 0, 0 },
					theme.Text, 0, 1.0f, arrow, arrowSize, false });
			}
			DrawFocusRing(ctx, cell, columnId, theme);
			// 点击与键盘(Enter/Space)访问同一个状态改动;脚本 ui.invoke 注入的也是这里。
			const bool activated = ctx.IsClicked(cell)
				|| (focused && (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space)));
			if (activated)
			{
				if (sorted)
					ascending = !ascending;
				else
					sortColumn = static_cast<int>(i);
				changed = true;
			}
		}
		return changed;
	}


bool ColorField(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec4& rgba, const WuiTheme& theme){
		WuiColorFieldState& state = ctx.Persist<WuiColorFieldState>(id, {});
		if (!state.Initialized)
		{
			state.Hex = FormatColorHex(rgba);
			state.Initialized = true;
		}
		const WuiId hexId = DerivedChildId(id, ".hex.", 0);
		const bool focused = ctx.Focus() == id;
		const bool hovered = ctx.IsHovered(rect);
		const bool open = ctx.IsPopupOpen(id);
		const std::string canonical = FormatColorHex(rgba);
		RegisterAccessNode(id, "color-field", rect, std::string(), canonical, true, true, focused);
		ctx.RegisterFocusable(id, rect);
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect,
			(hovered || open) ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
			focused ? theme.Accent : theme.Border, 3.0f, focused ? 1.5f : 1.0f });

		// 左侧 6px 色块:棋盘格底(ButtonBg/ButtonHover 两色交替)+ 当前色覆盖(保留 alpha)。
		const WuiRect swatch { rect.X + kColorSwatchInset, rect.Y + kColorSwatchInset, kColorSwatchWidth,
			std::max(0.0f, rect.H - kColorSwatchInset * 2.0f) };
		ctx.Commands().push_back({ WuiDrawKind::Rect, swatch, theme.ButtonBg, theme.Radius });
		for (int cx = 0; kColorCheckerSize * static_cast<float>(cx) < swatch.W; ++cx)
		{
			for (int cy = 0; kColorCheckerSize * static_cast<float>(cy) < swatch.H; ++cy)
			{
				if (((cx + cy) & 1) == 0)
					continue;
				const float offsetX = kColorCheckerSize * static_cast<float>(cx);
				const float offsetY = kColorCheckerSize * static_cast<float>(cy);
				const WuiRect square { swatch.X + offsetX, swatch.Y + offsetY,
					std::min(kColorCheckerSize, swatch.W - offsetX), std::min(kColorCheckerSize, swatch.H - offsetY) };
				ctx.Commands().push_back({ WuiDrawKind::Rect, square, theme.ButtonHover, 0.0f });
			}
		}
		ctx.Commands().push_back({ WuiDrawKind::Rect, swatch,
			WuiColor { std::clamp(rgba.r, 0.0f, 1.0f), std::clamp(rgba.g, 0.0f, 1.0f),
				std::clamp(rgba.b, 0.0f, 1.0f), std::clamp(rgba.a, 0.0f, 1.0f) }, theme.Radius });

		// 右侧色值文本:折叠态永远显示"当前值"的规范写法(编辑中的非法文本不会显示在这里)。
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ swatch.X + swatch.W + theme.PadSmall, rect.Y + (rect.H - theme.FontSizeBody) * 0.5f, 0, 0 },
			theme.Text, 0, 1.0f, canonical, theme.FontSizeBody, false });
		DrawFocusRing(ctx, rect, id, theme);

		bool changed = false;
		const bool keyToggle = focused && (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		if (ctx.IsClicked(rect) || keyToggle)
		{
			if (open)
				ctx.ClosePopup(id);
			else
			{
				state.Hex = canonical;
				ctx.OpenPopup(id);
			}
		}
		if (!ctx.IsPopupOpen(id))
		{
			// 弹层关闭 = 拖动收口(不留幽灵捕获:否则下次打开时第一帧会用旧归属改写颜色)。
			state.DragTarget = kColorDragNone;
			return false;
		}

		// ---- 弹层(与 Combo 同一套 ctx.OpenPopup/ClosePopup/IsPopupOpen,不自造)----
		ctx.PushOverlay();
		WuiRect panel { rect.X, rect.Y + rect.H + 2.0f, kColorPopupWidth, kColorPopupHeight };
		// P4-U10 弹层比原来的 hex+滑杆版高得多:下方放不下就向上翻,再整体夹进视口
		// (夹不住时顶部对齐 —— 取色器被窗口边缘裁掉比"位置略偏"更糟)。
		const float viewportHeight = ctx.Input().ViewportSize.y;
		if (panel.Y + panel.H > viewportHeight)
			panel.Y = rect.Y - panel.H - 2.0f;
		panel.Y = std::max(4.0f, std::min(panel.Y, std::max(4.0f, viewportHeight - panel.H - 4.0f)));
		panel.X = std::max(4.0f, std::min(panel.X, std::max(4.0f, ctx.Input().ViewportSize.x - panel.W - 4.0f)));
		DrawPanelSurface(ctx, panel, theme);

		// ---- ① 饱和度 × 明度色板(四角渐变:左上白 → 右上古纯色 → 下黑)----
		// 外部改动(hex/滑杆/预设)先同步 HSV;拖动中则以 HSV 为准。
		if (!state.HsvValid)
		{
			const glm::vec3 hsv = ColorToHsv(glm::vec3 { rgba });
			state.H = hsv.x;
			state.S = hsv.y;
			state.V = hsv.z;
			state.HsvValid = true;
		}
		else
		{
			const glm::vec3 derived = HsvToColor({ state.H, state.S, state.V });
			if (std::fabs(derived.x - rgba.r) > 1.0f / 255.0f || std::fabs(derived.y - rgba.g) > 1.0f / 255.0f
				|| std::fabs(derived.z - rgba.b) > 1.0f / 255.0f)
			{
				const glm::vec3 hsv = ColorToHsv(glm::vec3 { rgba });
				state.H = hsv.x;
				state.S = hsv.y;
				state.V = hsv.z;
			}
		}
		const glm::vec3 hueColor = HsvToColor({ state.H, 1.0f, 1.0f });
		const WuiRect svRect { panel.X + theme.Pad, panel.Y + theme.Pad,
			panel.W - theme.Pad * 2.0f, kColorSvHeight };
		{
			WuiDrawCommand gradient;
			gradient.Kind = WuiDrawKind::Gradient;
			gradient.Rect = svRect;
			gradient.Corners = { ToWuiColor({ 1.0f, 1.0f, 1.0f }), ToWuiColor(hueColor),
				ToWuiColor({ 0.0f, 0.0f, 0.0f }), ToWuiColor({ 0.0f, 0.0f, 0.0f }) };
			ctx.Commands().push_back(gradient);
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, svRect, theme.Border, 3.0f, 1.0f });
			// 标记点(实心点 + 白描边):黑白背景上都看得见。
			const glm::vec2 marker { svRect.X + state.S * svRect.W, svRect.Y + (1.0f - state.V) * svRect.H };
			const WuiRect markerRect { marker.x - 5.0f, marker.y - 5.0f, 10.0f, 10.0f };
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, markerRect, { 0, 0, 0, 1 }, 5.0f, 1.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline,
				{ markerRect.X + 1.0f, markerRect.Y + 1.0f, 8.0f, 8.0f }, { 1, 1, 1, 1 }, 4.0f, 1.0f });
			RegisterAccessNode(DerivedChildId(id, ".sv.", 0), "color-area", svRect,
				Wui::Tr("wui.color.saturation_value", "Saturation / Value"),
				FloatToText(state.S) + "," + FloatToText(state.V), true, true, false);
			// 交互(MAT-UI3a):按下起手 → **松手才结束**的拖动跟随。起手必须落在 SV 区内(或按住
			// 扫入,兼容旧口径);起手之后指针移到哪儿都用**当前坐标**按 SV 区夹取更新,每一帧都写回
			// —— 旧实现每帧要求 `IsHovered(svRect)`,拖出那块 110px 高的板就停更(用户报的
			// 「选取颜色板左键拖拽不松开,颜色不会变」)。
			if (ctx.Input().MouseClicked[0] && ctx.IsHovered(svRect))
				state.DragTarget = kColorDragSv;
			else if (state.DragTarget == kColorDragNone && ctx.Input().MouseDown[0] && ctx.IsHovered(svRect))
				state.DragTarget = kColorDragSv;
			if (state.DragTarget == kColorDragSv)
			{
				state.S = std::clamp((ctx.Input().MousePos.x - svRect.X) / std::max(1.0f, svRect.W), 0.0f, 1.0f);
				state.V = 1.0f - std::clamp((ctx.Input().MousePos.y - svRect.Y) / std::max(1.0f, svRect.H), 0.0f, 1.0f);
				ctx.SetCursor(WuiCursor::Hand);
				if (!ctx.Input().MouseDown[0])
					state.DragTarget = kColorDragNone;   // release 帧:落最终值后收口
			}
			else if (ctx.IsHovered(svRect))
				ctx.SetCursor(WuiCursor::Hand);
		}

		// ---- ② 色相条(6 段四角渐变拼出 360° 连续色相)----
		const WuiRect hueRect { panel.X + theme.Pad, svRect.Y + svRect.H + 8.0f,
			panel.W - theme.Pad * 2.0f, kColorBarHeight };
		for (int segment = 0; segment < 6; ++segment)
		{
			const float left = static_cast<float>(segment) / 6.0f;
			const float right = static_cast<float>(segment + 1) / 6.0f;
			const WuiRect band { hueRect.X + hueRect.W * left, hueRect.Y, hueRect.W * (right - left) + 0.5f, hueRect.H };
			WuiDrawCommand gradient;
			gradient.Kind = WuiDrawKind::Gradient;
			gradient.Rect = band;
			const WuiColor from = ToWuiColor(HsvToColor({ left * 360.0f, 1.0f, 1.0f }));
			const WuiColor to = ToWuiColor(HsvToColor({ right * 360.0f, 1.0f, 1.0f }));
			gradient.Corners = { from, to, to, from };
			ctx.Commands().push_back(gradient);
		}
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, hueRect, theme.Border, 3.0f, 1.0f });
		{
			const float hueX = hueRect.X + (state.H / 360.0f) * hueRect.W;
			const WuiRect thumb { hueX - 2.0f, hueRect.Y - 2.0f, 4.0f, hueRect.H + 4.0f };
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, thumb, { 1, 1, 1, 1 }, 2.0f, 2.0f });
			RegisterAccessNode(DerivedChildId(id, ".hue.", 0), "slider", hueRect,
				Wui::Tr("wui.color.hue", "Hue"), FloatToText(state.H) + "°", true, true, false);
			// 交互与 SV 板同一口径(MAT-UI3a):按下起手 → 松手才结束;色相条只有 12px 高,
			// 旧实现"每帧要求悬停在条内"最容易表现为"拖到一半就不动了"。
			if (ctx.Input().MouseClicked[0] && ctx.IsHovered(hueRect))
				state.DragTarget = kColorDragHue;
			else if (state.DragTarget == kColorDragNone && ctx.Input().MouseDown[0] && ctx.IsHovered(hueRect))
				state.DragTarget = kColorDragHue;
			if (state.DragTarget == kColorDragHue)
			{
				state.H = std::clamp((ctx.Input().MousePos.x - hueRect.X) / std::max(1.0f, hueRect.W), 0.0f, 1.0f) * 360.0f;
				ctx.SetCursor(WuiCursor::Hand);
				if (!ctx.Input().MouseDown[0])
					state.DragTarget = kColorDragNone;
			}
			else if (ctx.IsHovered(hueRect))
				ctx.SetCursor(WuiCursor::Hand);
		}

		// ---- ③ alpha 条(棋盘格 + 透明→不透明渐变)----
		const WuiRect alphaRect { panel.X + theme.Pad, hueRect.Y + hueRect.H + 6.0f,
			panel.W - theme.Pad * 2.0f, kColorBarHeight };
		ctx.Commands().push_back({ WuiDrawKind::Rect, alphaRect, theme.ButtonBg, 2.0f });
		for (int cx = 0; kColorCheckerSize * static_cast<float>(cx) < alphaRect.W; ++cx)
		{
			for (int cy = 0; kColorCheckerSize * static_cast<float>(cy) < alphaRect.H; ++cy)
			{
				if (((cx + cy) & 1) == 0)
					continue;
				ctx.Commands().push_back({ WuiDrawKind::Rect,
					{ alphaRect.X + kColorCheckerSize * static_cast<float>(cx),
					  alphaRect.Y + kColorCheckerSize * static_cast<float>(cy),
					  kColorCheckerSize, kColorCheckerSize }, theme.ButtonHover, 0.0f });
			}
		}
		{
			const glm::vec3 rgb { rgba };
			WuiDrawCommand gradient;
			gradient.Kind = WuiDrawKind::Gradient;
			gradient.Rect = alphaRect;
			gradient.Corners = { ToWuiColor(rgb, 1.0f), ToWuiColor(rgb, 0.0f),
				ToWuiColor(rgb, 0.0f), ToWuiColor(rgb, 1.0f) };
			ctx.Commands().push_back(gradient);
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, alphaRect, theme.Border, 3.0f, 1.0f });
			const WuiRect thumb { alphaRect.X + rgba.a * alphaRect.W - 2.0f, alphaRect.Y - 2.0f, 4.0f, alphaRect.H + 4.0f };
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, thumb, { 1, 1, 1, 1 }, 2.0f, 2.0f });
			RegisterAccessNode(DerivedChildId(id, ".alpha.", 0), "slider", alphaRect,
				Wui::Tr("wui.color.alpha", "Alpha"), FloatToText(rgba.a), true, true, false);
			// 交互与色相条同一口径(MAT-UI3a):按下起手 → 松手才结束。
			if (ctx.Input().MouseClicked[0] && ctx.IsHovered(alphaRect))
				state.DragTarget = kColorDragAlpha;
			else if (state.DragTarget == kColorDragNone && ctx.Input().MouseDown[0] && ctx.IsHovered(alphaRect))
				state.DragTarget = kColorDragAlpha;
			if (state.DragTarget == kColorDragAlpha)
			{
				rgba.a = std::clamp((ctx.Input().MousePos.x - alphaRect.X) / std::max(1.0f, alphaRect.W), 0.0f, 1.0f);
				changed = true;
				ctx.SetCursor(WuiCursor::Hand);
				if (!ctx.Input().MouseDown[0])
					state.DragTarget = kColorDragNone;
			}
			else if (ctx.IsHovered(alphaRect))
				ctx.SetCursor(WuiCursor::Hand);
		}

		// ---- ④ 色板/色相条产生的 RGB 写回(alpha 保持)----
		{
			const glm::vec3 rgb = HsvToColor({ state.H, state.S, state.V });
			if (std::fabs(rgb.x - rgba.r) > 1.0f / 255.0f || std::fabs(rgb.y - rgba.g) > 1.0f / 255.0f
				|| std::fabs(rgb.z - rgba.b) > 1.0f / 255.0f)
			{
				rgba.r = rgb.x;
				rgba.g = rgb.y;
				rgba.b = rgb.z;
				changed = true;
			}
		}

		// ④ 之上的 hex 行(色板/色相/alpha 之下),再接 R/G/B/A 滑杆与预设调色板。
		const WuiRect hexRect { panel.X + theme.Pad, alphaRect.Y + alphaRect.H + 8.0f,
			panel.W - theme.Pad * 2.0f, kColorHexRowHeight };
		const bool hexFocused = ctx.Focus() == hexId;
		// 失焦即把缓冲同步回规范值:非法输入不残留(值本身从来不被非法文本改写)。
		if (!hexFocused)
			state.Hex = FormatColorHex(rgba);
		glm::vec4 parsed = rgba;
		const bool validBeforeInput = ParseColorHex(state.Hex, parsed);
		TextFieldEx(ctx, hexId, hexRect, state.Hex, theme,
			validBeforeInput ? std::string() : std::string("Invalid hex (use #RRGGBB or #RRGGBBAA)"));
		// 只在编辑中(本帧之前 hex 行有焦点)把合法文本写回 rgba,避免"点到滑杆那帧"被旧缓冲覆盖。
		if (hexFocused && ParseColorHex(state.Hex, parsed) && !SameColor(parsed, rgba))
		{
			rgba = parsed;
			changed = true;
		}

		// R/G/B/A 四条滑杆(0..1,右侧显示两位小数)。几何:hex 行之后先留一行行内错误的位置
		// (TextFieldEx 的错误行:Caption 字号 + 3px 间距),四行滑杆总高 4×22,合计正好 132。
		float rowY = hexRect.Y + hexRect.H + theme.FontSizeCaption + 3.0f;
		for (int i = 0; i < 4; ++i)
		{
			const WuiRect row { panel.X + theme.Pad, rowY,
				panel.W - theme.Pad * 2.0f, kColorChannelRowHeight };
			const std::string channel(1, "RGBA"[i]);
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ row.X, row.Y + (row.H - theme.FontSizeCaption) * 0.5f, 0, 0 },
				theme.TextMuted, 0, 1.0f, channel, theme.FontSizeCaption, false });
			const WuiRect sliderRect { row.X + kColorChannelLabelWidth, row.Y,
				std::max(20.0f, row.W - kColorChannelLabelWidth - kColorChannelValueWidth), row.H };
			const float before = rgba[i];
			SliderFloat(ctx, DerivedChildId(id, ".slider.", static_cast<size_t>(i)), sliderRect,
				rgba[i], 0.0f, 1.0f, theme);
			if (rgba[i] != before)
				changed = true;
			char valueText[16] = {};
			std::snprintf(valueText, sizeof(valueText), "%.2f", rgba[i]);
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ row.X + row.W - kColorChannelValueWidth + theme.PadSmall,
				  row.Y + (row.H - theme.FontSizeCaption) * 0.5f, 0, 0 },
				theme.Text, 0, 1.0f, valueText, theme.FontSizeCaption, false });
			rowY += row.H;
		}

		// ---- ⑤ 预设调色板(2×10 常用色,点击直接套用;每格登记为无障碍节点)----
		rowY += 6.0f;
		const float presetCell = kColorPresetCell;
		const float presetGap = std::max(1.0f, (panel.W - theme.Pad * 2.0f
			- presetCell * static_cast<float>(kColorPresetColumns)) / static_cast<float>(kColorPresetColumns - 1));
		for (int preset = 0; preset < kColorPresetColumns * kColorPresetRows; ++preset)
		{
			const int column = preset % kColorPresetColumns;
			const int rowIndex = preset / kColorPresetColumns;
			const WuiRect cell { panel.X + theme.Pad + (presetCell + presetGap) * static_cast<float>(column),
				rowY + (presetCell + 4.0f) * static_cast<float>(rowIndex), presetCell, presetCell };
			const glm::vec3 presetRgb = kColorPresets[preset];
			const bool hovered = ctx.IsHovered(cell);
			ctx.Commands().push_back({ WuiDrawKind::Rect, cell, ToWuiColor(presetRgb), 3.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, cell,
				hovered ? theme.Accent : theme.Border, 3.0f, hovered ? 2.0f : 1.0f });
			RegisterAccessNode(DerivedChildId(id, ".preset.", static_cast<size_t>(preset)), "color-swatch",
				cell, FormatColorHex(glm::vec4 { presetRgb, 1.0f }), std::string(), true, true, false);
			if (hovered)
				ctx.SetCursor(WuiCursor::Hand);
			// P4-U28:预设色块也是弹层条目 —— 按下后拖出去松手不取色。
			if (ctx.IsClickCompleted(DerivedChildId(id, ".preset.", static_cast<size_t>(preset)), cell))
			{
				rgba = glm::vec4 { presetRgb, rgba.a };
				const glm::vec3 hsv = ColorToHsv(presetRgb);
				state.H = hsv.x;
				state.S = hsv.y;
				state.V = hsv.z;
				changed = true;
				ctx.ConsumePointerClick();
			}
		}

		ctx.ClosePopupsOnOutsideClick({ id }, panel);
		// hex 行有焦点时 Escape 归它(取消本次编辑、弹层保持展开);弹层内其它地方 Escape 关弹层。
		if (ctx.IsKeyPressed(KeyCodes::Escape) && !hexFocused)
			ctx.ClosePopup(id);
		// 弹层打开期间登记悬停遮挡区(顺序与 Combo 一致:必须在弹层自身命中测试与"点外关闭"之后),
		// 否则本帧之后绘制的下层控件会穿过弹层收到点击。
		if (ctx.IsPopupOpen(id))
		{
			ctx.PushHoverBlocker(panel);
			// P4-U7:同时登记为覆盖层矩形 → 下一帧只挡非覆盖层控件。
			ctx.RegisterOverlayRect(panel);
		}
		ctx.PopOverlay();
		return changed;
	}


bool Splitter(WuiContext& ctx, WuiId id, const WuiRect& rect, bool vertical, float& value, float minValue, float maxValue, const WuiTheme& theme){
		const float lo = std::min(minValue, maxValue);
		const float hi = std::max(minValue, maxValue);
		const float centerX = rect.X + rect.W * 0.5f;
		const float centerY = rect.Y + rect.H * 0.5f;
		// 命中带宽固定 6px、居中于传入 rect 的轴线:调用方只需把 rect 摆在"线的位置",
		// 带由控件自己撑开(传 1px 或 6px 宽都得到同一条 6px 命中带)。
		const WuiRect band = vertical
			? WuiRect { centerX - kSplitterHitWidth * 0.5f, rect.Y, kSplitterHitWidth, rect.H }
			: WuiRect { rect.X, centerY - kSplitterHitWidth * 0.5f, rect.W, kSplitterHitWidth };

		WuiSplitterState& state = ctx.Persist<WuiSplitterState>(id, {});
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "splitter", band, std::string(), FloatToText(value), true, true, focused);
		ctx.RegisterFocusable(id, band);

		const bool hovered = ctx.IsHovered(band);
		bool changed = false;
		if (ctx.Input().MouseClicked[0] && hovered)
		{
			state.Dragging = true;
			state.PressValue = value;
			state.PressAxis = vertical ? ctx.Input().MousePos.x : ctx.Input().MousePos.y;
			ctx.SetFocus(id);
		}
		if (state.Dragging)
		{
			const float axis = vertical ? ctx.Input().MousePos.x : ctx.Input().MousePos.y;
			const float next = std::clamp(state.PressValue + (axis - state.PressAxis), lo, hi);
			if (next != value)
			{
				value = next;
				changed = true;
			}
			if (ctx.Input().MouseReleased[0])
				state.Dragging = false;
		}
		// 键盘:焦点在分隔条上时按轴向箭头 ±theme.Pad(与鼠标同一条写值/夹取路径)。
		if (focused && !state.Dragging)
		{
			const float step = theme.Pad;
			const float delta = vertical
				? ((ctx.WasKeyPressed(KeyCodes::Left) ? -step : 0.0f) + (ctx.WasKeyPressed(KeyCodes::Right) ? step : 0.0f))
				: ((ctx.WasKeyPressed(KeyCodes::Up) ? -step : 0.0f) + (ctx.WasKeyPressed(KeyCodes::Down) ? step : 0.0f));
			if (delta != 0.0f)
			{
				const float next = std::clamp(value + delta, lo, hi);
				if (next != value)
				{
					value = next;
					changed = true;
				}
			}
		}

		// 视觉(P4-U29,用户截图:分隔条被画成一整条蓝色圆角选中框 —— 那是 hover/拖动时
		// 3px 强调色加粗 + DrawFocusRing 的 1.5px 强调色描边):全程**中性色**、恒定 1px、
		// 线心在 6px 命中带轴线上,只换线色 Border → hover BorderStrong → drag TextMuted;
		// 可交互(hover/拖动)时在中点画 3 个 grip 圆点,离开即消失。
		const WuiColor color = state.Dragging ? theme.TextMuted
			: (hovered ? theme.BorderStrong : theme.Border);
		const WuiRect line = vertical
			? WuiRect { centerX - kSplitterLineWidth * 0.5f, rect.Y, kSplitterLineWidth, rect.H }
			: WuiRect { rect.X, centerY - kSplitterLineWidth * 0.5f, rect.W, kSplitterLineWidth };
		ctx.Commands().push_back({ WuiDrawKind::Rect, line, color, 0.0f });
		if (hovered || state.Dragging)
		{
			ctx.SetCursor(vertical ? WuiCursor::ResizeEW : WuiCursor::ResizeNS);
			// grip 三点:沿轴向 ±4px,2×2 圆点;颜色同为中性色,拖动时更亮一档。
			const WuiColor grip = state.Dragging ? theme.TextMuted : theme.BorderStrong;
			constexpr float kGripDot = 2.0f;
			constexpr float kGripStep = 4.0f;
			for (int index = -1; index <= 1; ++index)
			{
				const float offset = kGripStep * static_cast<float>(index);
				const WuiRect dot = vertical
					? WuiRect { centerX - kGripDot * 0.5f, centerY + offset - kGripDot * 0.5f,
						kGripDot, kGripDot }
					: WuiRect { centerX + offset - kGripDot * 0.5f, centerY - kGripDot * 0.5f,
						kGripDot, kGripDot };
				ctx.Commands().push_back({ WuiDrawKind::Rect, dot, grip, 1.0f });
			}
		}
		// P4-U29:不再画 DrawFocusRing —— 键盘焦点时那条强调色圆角框就是用户报的"蓝色选中框"。
		// 焦点语义保持不变(RegisterFocusable / 箭头改值 / 双击复位都在调用方与上方实现)。
		return changed;
	}

namespace WuiWidgetsDetail
{

		// 分量数值文本:两位小数(派工确认)。显示与编辑缓冲共用同一套文本,
		// 避免"看到的数"与"点进去的数"不一致(DragFloat 用同一策略,只是三位小数)。
std::string VecAxisText(float value){
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.2f", value);
			return buffer;
		}


		// 整体无障碍节点的 value:"x,y[,z[,w]]"(分量顺序固定,脚本可直接按 ',' 切分)。
std::string VecFieldText(const float* value, int count){
			std::string text;
			for (int axis = 0; axis < count; ++axis)
				text += (axis == 0 ? "" : ",") + VecAxisText(value[axis]);
			return text;
		}

}

	// 向量字段唯一实现:count = 2/3/4 的分量数,columns = 横排(layout 0)时的每行列数。
	// count==3 / columns==3 / labels=X,Y,Z 时输出与 P4-UX12 的 Vec3Field **逐命令相同**
	// (槽宽公式、(slotW+gap)*axis 的排布、子节点 id ".axis."、kind 前缀都由同一份代码给出)。
bool VecFieldCore(WuiContext& ctx, WuiId id, const WuiRect& rect, float* value, int count, int columns, const char* roleKind, const char* axisKind, const char* const* axisLabels, float speed, float minValue, float maxValue, const WuiTheme& theme, int layout){
		const bool focused = ctx.Focus() == id;
		WuiVecFieldState& state = ctx.Persist<WuiVecFieldState>(id, {});
		if (state.Axis < 0 || state.Axis >= count)
			state.Axis = 0;
		// min >= max = 无界(与 DragFloat 的哨兵逐条一致:既有调用点的 (-1, 1) 写法不必改)。
		const float lo = minValue < maxValue ? minValue : -1e30f;
		const float hi = minValue < maxValue ? maxValue : 1e30f;
		// 键盘步长取 speed 的绝对值(与 DragFloat 一致;speed 传 0 时退化为 1)。
		const float keyboardStep = std::fabs(speed) > 0.0f ? std::fabs(speed) : 1.0f;
		const bool vertical = layout == 1;
		RegisterAccessNode(id, roleKind, rect, std::string(), VecFieldText(value, count), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		bool changed = false;

		// 每个分量的槽 = 轴标签(12px)+ 输入框;横排按 columns 列分栏(列间留 PadSmall),
		// 竖排 count 行等分高度。count==3/columns==3 == 既有 Vec3Field 的横排三等分。
		const float axisGap = vertical ? 0.0f : theme.PadSmall;
		const auto slotRect = [&](int axis) -> WuiRect
		{
			if (vertical)
			{
				const float rowH = rect.H / static_cast<float>(count);
				return { rect.X, rect.Y + rowH * static_cast<float>(axis), rect.W, rowH };
			}
			const float rows = static_cast<float>((count + columns - 1) / columns);
			const float rowsClamped = std::max(1.0f, rows);
			const float slotW = std::max(0.0f,
				(rect.W - axisGap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
			const float slotH = rect.H / rowsClamped;
			const int row = axis / columns;
			const int column = axis % columns;
			// row == 0 时显式用 rect.Y(不做 `+ slotH * 0`,保证与既有 Vec3Field 的矩形逐位相同)。
			const float slotY = row == 0 ? rect.Y : rect.Y + slotH * static_cast<float>(row);
			return { rect.X + (slotW + axisGap) * static_cast<float>(column),
				slotY, slotW, slotH };
		};
		const auto fieldRect = [&](const WuiRect& slot) -> WuiRect
		{
			const float labelW = std::min(kVecAxisLabelWidth, slot.W);
			return { slot.X + labelW, slot.Y, std::max(0.0f, slot.W - labelW), slot.H };
		};

		// 拖动锚点/编辑缓冲都按"当前轴"落到 value 的分量上;命中范围是整个槽位(含轴标签,点标签
		// 与点输入框等价 —— 标签只是 12px 的视觉前缀,不是独立控件)。
		// VEC-H4:与 DragFloat 同一口径 —— Shift = 0.1× 精细、Ctrl = 10× 粗调(拖动与 ↑/↓ 同倍率)。
		const float modifier = ctx.Input().Shift ? 0.1f : (ctx.Input().Ctrl ? 10.0f : 1.0f);
		const bool activeHovered = ctx.IsHovered(slotRect(state.Axis));
		if (state.Editing)
		{
			bool submitted = false, cancelled = false;
			if (EditUpdate(ctx, state.Buffer, state.Cursor, state.SelStart, state.SelEnd, submitted, cancelled))
			{
				if (submitted)
				{
					char* end = nullptr;
					const float parsed = std::strtof(state.Buffer.c_str(), &end);
					if (end && *end == 0)
					{
						const float next = std::max(lo, std::min(hi, parsed));
						if (next != value[state.Axis])
						{
							value[state.Axis] = next;
							changed = true;
						}
					}
				}
				state.Editing = false;
				state.Cursor = -1;
				state.SelStart = -1;
				state.SelEnd = -1;
			}
			else if (ctx.Input().MouseClicked[0] && !activeHovered)
			{
				// 点别处 = 提交并结束(与 DragFloat 一致:Enter/Esc 之外的退出路径同样落值)。
				char* end = nullptr;
				const float parsed = std::strtof(state.Buffer.c_str(), &end);
				if (end && *end == 0)
				{
					const float next = std::max(lo, std::min(hi, parsed));
					if (next != value[state.Axis])
					{
						value[state.Axis] = next;
						changed = true;
					}
				}
				state.Editing = false;
				state.Cursor = -1;
				state.SelStart = -1;
				state.SelEnd = -1;
			}
		}
		else
		{
			if (ctx.Input().MouseClicked[0])
			{
				for (int axis = 0; axis < count; ++axis)
				{
					if (!ctx.IsHovered(slotRect(axis)))
						continue;
					state.Pressed = true;
					state.Axis = axis;
					state.PressX = ctx.Input().MousePos.x;
					state.PressValue = value[axis];
					// 按下即取焦点(DragFloat 只在开始拖动/进入编辑时取):随后 Up/Down 立刻
					// 作用于刚点的这个分量,不需要先拖一下。
					ctx.SetFocus(id);
					break;
				}
			}
			if (state.Pressed)
			{
				const float dx = ctx.Input().MousePos.x - state.PressX;
				if (std::fabs(dx) > 1.5f)
					state.Dragging = true;
				if (state.Dragging)
				{
					const float next = std::max(lo, std::min(hi,
						static_cast<float>(state.PressValue + dx * speed * modifier)));
					if (next != value[state.Axis])
					{
						value[state.Axis] = next;
						changed = true;
					}
					ctx.SetCursor(WuiCursor::ResizeEW);
				}
				if (ctx.Input().MouseReleased[0])
				{
					if (!state.Dragging)
					{
						state.Editing = true;
					state.Buffer = VecAxisText(value[state.Axis]);
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
		}

		// 键盘微调:焦点在整体上、且不在文本编辑态时,Up/Down 按 |speed| 调当前轴
		// (编辑态下这两个键留给别处,不抢)。
		if (focused && !state.Editing)
		{
			const float step = keyboardStep * modifier;
			const float delta = (ctx.WasKeyPressed(KeyCodes::Up) ? step : 0.0f)
				+ (ctx.WasKeyPressed(KeyCodes::Down) ? -step : 0.0f);
			if (delta != 0.0f)
			{
				const float next = std::clamp(value[state.Axis] + delta, lo, hi);
				if (next != value[state.Axis])
				{
					value[state.Axis] = next;
					changed = true;
				}
			}
		}

		const float textSize = theme.FontSizeBody;
		for (int axis = 0; axis < count; ++axis)
		{
			const WuiRect slot = slotRect(axis);
			const WuiRect field = fieldRect(slot);
			const bool axisCurrent = state.Axis == axis;
			const bool axisDragging = state.Dragging && axisCurrent;
			const bool axisEditing = state.Editing && axisCurrent;
			const bool hovered = ctx.IsHovered(slot);
			// 子节点:交互可点(脚本注入坐标 = 鼠标点该分量),但不是独立焦点项(焦点始终在整体上)。
			RegisterAccessNode(DerivedChildId(id, ".axis.", static_cast<size_t>(axis)), axisKind, slot,
				axisLabels[axis], VecAxisText(value[axis]));
			if (hovered)
				ctx.SetCursor(axisEditing ? WuiCursor::IBeam : WuiCursor::ResizeEW);
			// 轴标签:拖动中的分量用强调色高亮,其余 = 次要色 + Caption 字号。
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ slot.X, slot.Y + (slot.H - theme.FontSizeCaption) * 0.5f, 0, 0 },
				axisDragging ? theme.Accent : theme.TextMuted, 0, 1.0f, axisLabels[axis],
				theme.FontSizeCaption, false });
			ctx.Commands().push_back({ WuiDrawKind::Rect, field,
				axisEditing ? theme.ButtonHover : theme.ButtonBg, theme.Radius });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, field,
				(axisEditing || hovered || axisDragging) ? theme.Accent : theme.Border, theme.Radius, 1.0f });
			const std::string text = axisEditing ? state.Buffer : VecAxisText(value[axis]);
			WuiDrawCommand command { WuiDrawKind::Text,
				{ field.X + 5.0f, field.Y + (field.H - textSize) * 0.5f, 0, 0 },
				axisDragging ? theme.Accent : theme.Text, 0, 1.0f, text, textSize, false };
			if (axisEditing && state.SelStart >= 0 && state.SelEnd > state.SelStart)
			{
				command.TextSelStart = static_cast<int>(Utf8Offset(state.Buffer, state.SelStart));
				command.TextSelEnd = static_cast<int>(Utf8Offset(state.Buffer, state.SelEnd));
			}
			else if (axisEditing)
				command.TextCursorByte = static_cast<int>(Utf8Offset(state.Buffer, state.Cursor));
			ctx.Commands().push_back(std::move(command));
		}
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
	}


	// 三个公开入口都只是"填参数":同一份 VecFieldCore,所以手感/子节点/无障碍口径天然一致。
bool Vec2Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec2& value, float speed, float minValue, float maxValue, const WuiTheme& theme, int layout){
		return VecFieldCore(ctx, id, rect, &value[0], 2, 2, "vec2-field", "vec2-axis",
			kVec2AxisLabels, speed, minValue, maxValue, theme, layout);
	}


bool Vec3Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec3& value, float speed, float minValue, float maxValue, const WuiTheme& theme, int layout){
		return VecFieldCore(ctx, id, rect, &value[0], 3, 3, "vec3-field", "vec3-axis",
			kVec3AxisLabels, speed, minValue, maxValue, theme, layout);
	}


	// Vec4 的默认排布 = 2×2(columns = 2):四段并排会把每个输入框压到 45px 以下
	//(标签固定 12px + 两位小数),2×2 在材质参数行/属性面板的 ~200-300 宽控件列里
	// 给每个分量约 100-145px,读数与拖动手感与 Vec3Field 一致;窄列用 layout=1 四行。
bool Vec4Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec4& value, float speed, float minValue, float maxValue, const WuiTheme& theme, int layout){
		return VecFieldCore(ctx, id, rect, &value[0], 4, 2, "vec4-field", "vec4-axis",
			kVec4AxisLabels, speed, minValue, maxValue, theme, layout);
	}


bool EmptyState(WuiContext& ctx, const WuiRect& rect, const std::string& glyph, const std::string& title, const std::string& hint, const std::string& actionLabel, WuiId actionId, const WuiTheme& theme){
		// 左右安全边距:rect 比 2×边距还窄时退化成"以中线为界",内容仍不出客户区。
		const float margin = std::min(kEmptyStateSideMargin, std::max(0.0f, rect.W * 0.5f));
		const float contentX = rect.X + margin;
		const float contentW = std::max(0.0f, rect.W - margin * 2.0f);
		const auto centered = [&](float width) { return contentX + std::max(0.0f, (contentW - width) * 0.5f); };

		// 节点 id:EmptyState 没有自己的 id 参数(派工签名),因此有 action 时挂在 action id 的派生 id 上
		// (稳定,且与按钮自身 id 不冲突);没有 action 时按 title 内容派生 —— 同一面板里两个同标题的
		// 空状态会共用 id,请给它们不同的文案。
		const WuiId nodeId = actionId != 0
			? DerivedChildId(actionId, ".empty-state.", 0)
			: HashId(("empty-state:" + title).c_str());
		RegisterAccessNode(nodeId, "empty-state", rect, title, hint, true, false);

		// hint 折行复用 tooltip 的折行器(支持 '\n' 与 CJK 逐字符断行),最多两行;被砍掉后续内容时
		// 在末行补 '…'(与 EllipsizeToWidth 的截断语义一致)。
		std::vector<std::string> hintLines;
		if (!hint.empty() && contentW > 0.0f)
		{
			hintLines = WrapTooltipText(ctx, hint, theme.FontSizeSmall, contentW);
			const bool clipped = hintLines.size() > 2;
			if (clipped)
				hintLines.resize(2);
			for (size_t i = 0; i < hintLines.size(); ++i)
			{
				const bool last = i + 1 == hintLines.size();
				const std::string source = (clipped && last) ? (hintLines[i] + "…") : hintLines[i];
				hintLines[i] = EllipsizeToWidth(ctx, source, contentW, theme.FontSizeSmall);
			}
		}

		const std::string shownTitle = EllipsizeToWidth(ctx, title, contentW, theme.FontSizeTitle);
		const float blockGap = theme.Pad;   // 块与块之间的间隔
		const float glyphH = glyph.empty() ? 0.0f : theme.FontSizeHeading + theme.PadSmall;
		const float titleH = title.empty() ? 0.0f : theme.FontSizeTitle + theme.PadSmall;
		const float hintH = static_cast<float>(hintLines.size()) * (theme.FontSizeSmall + theme.PadSmall);
		float buttonW = 0.0f;
		float buttonH = 0.0f;
		if (!actionLabel.empty())
		{
			buttonW = std::min(contentW, ctx.MeasureTextWidth(actionLabel, theme.FontSizeBody) + kEmptyStateButtonPad);
			buttonH = theme.ControlHeight;
		}
		const int blocks = (glyphH > 0.0f ? 1 : 0) + (titleH > 0.0f ? 1 : 0)
			+ (hintH > 0.0f ? 1 : 0) + (buttonH > 0.0f ? 1 : 0);
		const float totalH = glyphH + titleH + hintH + buttonH
			+ blockGap * static_cast<float>(std::max(0, blocks - 1));
		// 垂直居中;内容比 rect 还高时从 rect 顶部开始(不往客户区外画)。
		float y = rect.Y + std::max(0.0f, (rect.H - totalH) * 0.5f);

		if (glyphH > 0.0f)
		{
			const float width = ctx.MeasureTextWidth(glyph, theme.FontSizeHeading);
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ centered(width), y + (glyphH - theme.FontSizeHeading) * 0.5f, 0, 0 },
				theme.TextMuted, 0, 1.0f, glyph, theme.FontSizeHeading, false });
			y += glyphH + blockGap;
		}
		if (titleH > 0.0f)
		{
			const float width = ctx.MeasureTextWidth(shownTitle, theme.FontSizeTitle);
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ centered(width), y + (titleH - theme.FontSizeTitle) * 0.5f, 0, 0 },
				theme.Text, 0, 1.0f, shownTitle, theme.FontSizeTitle, false });
			y += titleH + blockGap;
		}
		for (const std::string& line : hintLines)
		{
			if (!line.empty())
			{
				const float width = ctx.MeasureTextWidth(line, theme.FontSizeSmall);
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ centered(width), y + theme.PadSmall * 0.5f, 0, 0 },
					theme.TextMuted, 0, 1.0f, line, theme.FontSizeSmall, false });
			}
			y += theme.FontSizeSmall + theme.PadSmall;
		}

		bool activated = false;
		if (buttonH > 0.0f && buttonW > 0.0f)
		{
			if (hintH > 0.0f)
				y += blockGap;   // 按钮与上一块之间的间隔(上面的 hint 循环没补)
			const WuiRect button { centered(buttonW), y, buttonW, buttonH };
			activated = Button(ctx, actionId, button, actionLabel, theme);
		}
		return activated;
	}


WindowControl WindowControls(WuiContext& ctx, const WuiRect& bar, const WuiTheme& theme, bool maximized){
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
