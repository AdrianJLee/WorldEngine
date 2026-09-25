#include "wldpch.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/Core/KeyCodes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

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

		// MAT-UI3a:焦点环的两笔几何/透明度(见 WuiWidgets.h DrawFocusRing 的说明)。
		// 属于控件几何,不进主题令牌(与 kColorSwatchWidth / kSplitterHitWidth 同一处理)。
		constexpr float kFocusRingCoreAlpha = 0.72f;
		constexpr float kFocusRingCoreThickness = 1.25f;
		constexpr float kFocusRingGlowAlpha = 0.16f;
		constexpr float kFocusRingGlowThickness = 2.5f;
		constexpr float kFocusRingGlowInset = 1.5f;
		// 取色器拖动跟随:一次按下归属哪一个子区域(0 = 没有拖动)。
		constexpr int kColorDragNone = 0;
		constexpr int kColorDragSv = 1;
		constexpr int kColorDragHue = 2;
		constexpr int kColorDragAlpha = 3;

		// P1c-LIB2:有状态滚动条:拖动状态(按下时抓住的滑块内偏移)。拖动期间按
		// "鼠标位置 − 抓点"反算滚动量,所以鼠标离开轨道也不会中断拖动(与 WuiSplitterState 同一套手感)。
		struct WuiScrollBarState
		{
			bool Dragging = false;
			float GrabOffset = 0.0f;
		};

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

		// 子控件(弹层条目 / 标签 / 分段项)的无障碍 id:由父控件 id + 类别 + 下标派生
		// (与 Wui::HashId 同一套 FNV-1a)。不用文本参与哈希:同名条目仍有各自独立的 id。
		WuiId DerivedChildId(WuiId parent, const char* category, size_t index)
		{
			const std::string key = std::to_string(parent) + category + std::to_string(index);
			return HashId(key.c_str());
		}

		// 弹层条目的无障碍 id:父控件 id + 选项下标(既有约定,逐字节保持 —— 脚本按它算 id)。
		WuiId ComboOptionId(WuiId comboId, size_t index)
		{
			return DerivedChildId(comboId, ".option.", index);
		}

		// 两点 + 线宽 → 一个任意四边形命令(勾、斜线这类轴对齐矩形覆盖不到的形状)。
		// 顶点顺序遵循 WuiDrawKind::Quad 约定(左上/右上/右下/左下)。
		void PushLineQuad(WuiContext& ctx, glm::vec2 from, glm::vec2 to, float thickness, const WuiColor& color)
		{
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
		std::string EllipsizeToWidth(const WuiContext& ctx, std::string_view text, float maxWidth,
			float fontSize)
		{
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
		// 兼容重载:旧调用点不带宽度,按标签列的默认预算裁剪(否则长术语会压住右侧控件)。
		LabelWithTerm(ctx, pos, text, term, color, fontSize, theme, LabelDefaultWidth);
	}

	void LabelWithTerm(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const std::string& term,
		const WuiColor& color, float fontSize, const WuiTheme& theme, float width)
	{
		// P4-UX5 标签列裁剪(验证者指出"FixedAspectRatio"这类长术语会压住右侧控件):
		// width = 本标签可用的设计单位宽度(含术语),超出时按优先级降级 ——
		// ① 术语缩略('…');② 仍放不下就不画术语(保住主文案);③ 主文案自己超宽时同样截断。
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

		// 主文案优先:只给它留出术语的剩余空间(没有术语就整列归主文案)。
		const float textBudget = hasTerm ? std::max(0.0f, width - gap - termWidth) : width;
		const std::string shownText = textWidth > textBudget ? EllipsizeToWidth(ctx, text, textBudget, fontSize) : text;
		const float shownTextWidth = ctx.MeasureTextWidth(shownText, fontSize);
		Label(ctx, pos, shownText, color, fontSize);
		if (!hasTerm)
			return;

		const float termX = pos.x + shownTextWidth + gap;
		const float termBudget = width - (shownTextWidth + gap);
		if (termBudget <= 0.0f)
			return;   // ② 主文案已占满:不画术语
		const std::string shownTerm = ctx.MeasureTextWidth(term, termSize) <= termBudget
			? term
			: EllipsizeToWidth(ctx, term, termBudget, termSize);
		if (shownTerm.empty())
			return;   // ① 连一个字符加省略号都放不下:不画术语,而不是画一个孤立 '…'
		ctx.Commands().push_back({ WuiDrawKind::Text, { termX, pos.y + (fontSize - termSize) * 0.5f, 0, 0 },
			theme.TextMuted, 0, 1.0f, shownTerm, termSize, false });
	}

	namespace
	{
		// 按绘制宽度折行:支持 '\n' 硬换行;对 CJK(无空格)按字符断行。
		std::vector<std::string> WrapTooltipText(WuiContext& ctx, const std::string& text,
			float fontSize, float maxWidth)
		{
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

		// ---- P4-U29:弹层延后绘制 ----
		//
		// overlay 命令是**单一列表、后画遮先画**:模态/面板里后绘制的内容会把下拉弹层盖住
		// (用户实测:新建材质向导的"目录"下拉被下方的 Parent 行与按钮压住)。命中测试、
		// a11y 登记与遮挡登记必须留在原地 —— U28 的 press+release 归属与"关闭帧多挡一帧"
		// 依赖那里的调用顺序;只有**绘制命令**可以搬到帧末。
		//
		// 做法:弹层绘制段用 WuiDeferredPopupScope 收集命令,存进上下文持久状态里的延后批次;
		// 帧末唯一的 overlay 收口 Wui::DrawTooltip(两个宿主 EditorShell / FloatWindowHost
		// 都在所有面板与模态之后调用它)把批次追加到 overlay 列表末尾,tooltip 仍在其上。
		struct WuiDeferredPopupLayer
		{
			uint64_t Frame = 0;
			std::vector<WuiDrawCommand> Commands;
		};

		// 本帧的延后批次(按上下文持有;首帧/换帧时清空上一帧的残留)。
		WuiDeferredPopupLayer& DeferredPopupLayer(WuiContext& ctx)
		{
			WuiDeferredPopupLayer& layer = ctx.Persist<WuiDeferredPopupLayer>(
				HashId("world.wui.deferred-popup"), {});
			if (layer.Frame != ctx.Frame())
			{
				layer.Commands.clear();
				layer.Frame = ctx.Frame();
			}
			return layer;
		}

		// 把一段 overlay 绘制命令收进延后批次:进入时 PushOverlay,离开时搬走这段命令
		// (保持相对顺序),因此作用域内的 SetCursor / RegisterOverlayRect / 命中判定
		// 仍然当场生效,只有像素被推后。
		class WuiDeferredPopupScope
		{
		public:
			explicit WuiDeferredPopupScope(WuiContext& ctx) : m_Ctx(ctx)
			{
				m_Ctx.PushOverlay();
				m_Commands = &m_Ctx.Commands();
				m_Mark = m_Commands->size();
			}

			~WuiDeferredPopupScope()
			{
				Collect();
			}

			void Collect()
			{
				if (m_Commands == nullptr)
					return;
				std::vector<WuiDrawCommand>& deferred = DeferredPopupLayer(m_Ctx).Commands;
				deferred.insert(deferred.end(), m_Commands->begin() + m_Mark, m_Commands->end());
				m_Commands->resize(m_Mark);
				m_Commands = nullptr;
				m_Ctx.PopOverlay();
			}

		private:
			WuiContext& m_Ctx;
			std::vector<WuiDrawCommand>* m_Commands = nullptr;
			size_t m_Mark = 0;
		};

		// 帧末收口:把本帧攒下的弹层命令追加到 overlay 末尾。
		void FlushDeferredPopupDraws(WuiContext& ctx)
		{
			WuiDeferredPopupLayer& layer = DeferredPopupLayer(ctx);
			if (layer.Frame != ctx.Frame() || layer.Commands.empty())
				return;
			ctx.PushOverlay();
			std::vector<WuiDrawCommand>& target = ctx.Commands();
			for (WuiDrawCommand& command : layer.Commands)
				target.push_back(command);
			ctx.PopOverlay();
			layer.Commands.clear();
		}
	}

	void Tooltip(WuiContext& ctx, const WuiRect& hoverRect, const std::string& text)
	{
		if (text.empty() || !ctx.IsHovered(hoverRect))
			return;
		ctx.SetTooltip(text);
	}

	void DrawTooltip(WuiContext& ctx, const WuiTheme& theme)
	{
		// P4-U29:先把本帧延后的下拉弹层补画到 overlay 末尾(在所有面板/模态内容之上),
		// 再画 tooltip —— tooltip 仍在最上层;没有 tooltip 时也必须走这一步。
		FlushDeferredPopupDraws(ctx);
		const std::string& text = ctx.Tooltip();
		if (text.empty())
			return;
		const float fontSize = theme.FontSizeSmall;
		const float padding = 8.0f;
		const float lineHeight = fontSize + 6.0f;
		const float maxTextWidth = 380.0f;
		const std::vector<std::string> lines = WrapTooltipText(ctx, text, fontSize, maxTextWidth);
		float textWidth = 0.0f;
		for (const std::string& line : lines)
			textWidth = std::max(textWidth, ctx.MeasureTextWidth(line, fontSize));
		const float height = static_cast<float>(lines.size()) * lineHeight + padding * 2.0f;
		const glm::vec2 viewport = ctx.ViewportSize();
		// 跟随光标右下 16/20,贴边时翻到另一侧,保证整块提示在视口内可见。
		glm::vec2 pos { ctx.Input().MousePos.x + 16.0f, ctx.Input().MousePos.y + 20.0f };
		const float panelWidth = textWidth + padding * 2.0f;
		if (pos.x + panelWidth > viewport.x - 4.0f)
			pos.x = std::max(4.0f, ctx.Input().MousePos.x - panelWidth - 12.0f);
		if (pos.y + height > viewport.y - 4.0f)
			pos.y = std::max(4.0f, ctx.Input().MousePos.y - height - 12.0f);
		const WuiRect panel { pos.x, pos.y, panelWidth, height };
		ctx.PushOverlay();
		DrawPanelSurface(ctx, panel, theme);
		float y = panel.Y + padding;
		for (const std::string& line : lines)
		{
			ctx.Commands().push_back({ WuiDrawKind::Text, { panel.X + padding, y, 0, 0 },
				theme.Text, 0, 1.0f, line, fontSize, false });
			y += lineHeight;
		}
		ctx.PopOverlay();
	}

	void DrawFocusRing(WuiContext& ctx, const WuiRect& rect, WuiId id, const WuiTheme& theme,
		const WuiColor* ringColor)
	{
		if (id == 0 || ctx.Focus() != id)
			return;
		// 滚出滚动区视口的控件不画环(overlay 不受 ClipPush 影响,必须自己问裁剪栈)。
		if (!ctx.ClipAllows(rect))
			return;
		// 走 overlay 命令层:焦点环在全部普通控件之后绘制,后画的兄弟控件不会盖住它
		// (与 tooltip 同一套机制,不新增层级)。
		//
		// MAT-UI3a(用户 2026-09-25「这个聚焦选中能否按照人类美学重新设计下」):从"1.5px 不透明
		// 强调色硬描边"改成"圆角细描边 + 外发光",两条都以基色为色相、按低透明度画:
		//   ① 主环:贴着控件矩形(不动几何),1.25px,圆角 = theme.Radius,alpha × 0.72;
		//   ② 发光:矩形外扩 1.5px,2.5px,圆角 = theme.Radius + 2.5,alpha × 0.16 —— 只添氛围,
		//      不占布局、不进命中、不动 a11y(环是纯绘制)。
		// 可辨识性优先:主环 alpha 0.72 + 1.25px 实线在明暗两套主题上都明确可辨(比旧口径低一档
		// 透明度,但描边更圆更细,视觉噪声小得多);禁用件仍走各自"不画环"的分支,与本函数无关。
		// 顺序:主环在前(投影/断言里它就是"这条焦点环"),发光在后(纯附加)。
		const WuiColor base = ringColor != nullptr ? *ringColor : theme.FocusRing;
		const auto scaled = [](const WuiColor& color, float alphaScale)
		{
			return WuiColor { color.R, color.G, color.B,
				std::clamp(color.A * alphaScale, 0.0f, 1.0f) };
		};
		ctx.PushOverlay();
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
			scaled(base, kFocusRingCoreAlpha), theme.Radius, kFocusRingCoreThickness });
		const float glow = kFocusRingGlowInset;
		ctx.Commands().push_back({ WuiDrawKind::RectOutline,
			{ rect.X - glow, rect.Y - glow, rect.W + glow * 2.0f, rect.H + glow * 2.0f },
			scaled(base, kFocusRingGlowAlpha), theme.Radius + 2.5f, kFocusRingGlowThickness });
		ctx.PopOverlay();
	}

	// WUI-P1.5a2:按"当前视觉状态 + 通道"取样式色 —— 保留模式 WuiButton 与立即模式 Button 的**唯一**实现。
	// 槽未覆盖 = 调用方给的兜底(主题令牌/旧口径);哨兵口径(字号<=0 / 内边距<0 / 空 optional)也在这里。
	WuiButtonResolvedStyle ResolveButtonStyle(const WuiButtonStyle* style, bool disabled, bool pressed,
		bool hovered, bool focused, const WuiColor& fallbackBg, const WuiColor& fallbackBorder,
		const WuiColor& fallbackText, const WuiColor& fallbackFocusRing)
	{
		const WuiButtonStyle::State state = disabled ? WuiButtonStyle::State::Disabled
			: (pressed ? WuiButtonStyle::State::Pressed
				: (hovered ? WuiButtonStyle::State::Hover
					: (focused ? WuiButtonStyle::State::Focused : WuiButtonStyle::State::Normal)));
		const WuiButtonStateColors* colors = style != nullptr
			? &style->Colors[static_cast<size_t>(state)] : nullptr;
		WuiButtonResolvedStyle resolved;
		resolved.State = state;
		resolved.BgCovered = colors != nullptr && colors->Bg.has_value();
		resolved.BorderCovered = colors != nullptr && colors->Border.has_value();
		resolved.TextCovered = colors != nullptr && colors->Text.has_value();
		resolved.Bg = resolved.BgCovered ? *colors->Bg : fallbackBg;
		resolved.Border = resolved.BorderCovered ? *colors->Border : fallbackBorder;
		resolved.Text = resolved.TextCovered ? *colors->Text : fallbackText;
		// border.focus 覆盖同时作用于焦点环(与当前状态无关);未覆盖 = 调用方给的主题 FocusRing。
		const std::optional<WuiColor>* ring = style != nullptr
			? &style->Colors[static_cast<size_t>(WuiButtonStyle::State::Focused)].Border : nullptr;
		resolved.FocusRingCovered = ring != nullptr && ring->has_value();
		resolved.FocusRing = resolved.FocusRingCovered ? **ring : fallbackFocusRing;
		resolved.PaddingX = style != nullptr && style->PaddingX >= 0.0f ? style->PaddingX : 8.0f;
		resolved.FontSize = style != nullptr && style->FontSize > 0.0f ? style->FontSize : 15.0f;
		resolved.Bold = style != nullptr && style->Bold;
		return resolved;
	}

	bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme,
		const WuiButtonStyle* style)
	{
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "button", rect, label, std::string(), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const bool hovered = ctx.IsHovered(rect);
		const bool pressed = ctx.Input().MouseDown[0] && hovered;
		// U2A 键盘激活:焦点在按钮上时 Enter/Space = 点击一次(KeyPressed 只含本帧新按下,长按不连发)。
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		// WUI-P1.5:状态优先级 pressed > hover > focus > normal,disabled 覆盖一切;全部槽都未覆盖时
		// 下面三个兜底就是改动前的取值(ButtonHover/ButtonBg、Border、pressed?Accent:Text)⇒ 命令流逐字节不变。
		const WuiButtonResolvedStyle resolved = ResolveButtonStyle(style, style != nullptr && style->Disabled,
			pressed, hovered, focused, hovered ? theme.ButtonHover : theme.ButtonBg,
			theme.Border, pressed ? theme.Accent : theme.Text, theme.FocusRing);
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, resolved.Bg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, resolved.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ rect.X + resolved.PaddingX, rect.Y + (rect.H - resolved.FontSize) * 0.5f, 0, 0 },
			resolved.Text, 0, 1.0f, label, resolved.FontSize, resolved.Bold });
		DrawFocusRing(ctx, rect, id, theme, resolved.FocusRingCovered ? &resolved.FocusRing : nullptr);
		return (hovered && ctx.Input().MouseClicked[0]) || keyActivated;
	}

	// P1c-LIB2:带禁用态 + 理由的按钮 —— 画法与面板侧 ActionButton/ModalActionButton 同源,
	// 语义收进库:"不可用"必须同时体现在 Enabled 与 Value/Tooltip(禁用的理由)上。
	// P1c-LIB3(用户裁决 2026-09-24):enabled=false 时不登记焦点表 ⇒ 不进 Tab 焦点链;
	// a11y 节点照登记(Enabled=false + Value/Tooltip=理由),即"可读不可点"。
	bool ButtonEx(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		const WuiTheme& theme, bool enabled, bool primary, const std::string& tooltip)
	{
		const bool hovered = ctx.IsHovered(rect);
		const bool focused = ctx.Focus() == id;
		const WuiColor fill = !enabled
			? theme.PanelBg
			: (primary ? theme.Accent : (hovered ? theme.ButtonHover : theme.ButtonBg));
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, fill, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
			enabled ? (hovered ? theme.Accent : theme.Border) : theme.Border, 3.0f, 1.0f });
		// accent 填充上压深色文字(白字对比度不够);禁用态用 TextDisabled。
		const WuiColor textColor = !enabled ? theme.TextDisabled : (primary ? theme.WindowBg : theme.Text);
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, textColor, 0, 1.0f, label, 15.0f, false });
		// 灰按钮不能没有理由:禁用时这一句进 Value 与 Tooltip(启用时是普通用途说明)。
		if (id != 0)
		{
			WuiAccessNode node;
			node.Id = id;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = "button";
			node.Label = label;
			node.Value = tooltip;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = true;
			node.Focused = focused;
			WuiAccessibility::Get().Register(node);
		}
		// P1c-LIB3:禁用件不进 Tab 焦点链(焦点表登记 = Tab/Shift+Tab 的唯一入口);
		// 启用件与普通 Button 完全同一条登记路径 —— 普通按钮/其它控件不受影响。
		if (enabled)
		{
			ctx.RegisterFocusable(id, rect);
			// 禁用件不画焦点环(即使调用方在同一帧强设焦点):"不可聚焦"必须在外观上一致。
			DrawFocusRing(ctx, rect, id, theme);
		}
		if (hovered && !tooltip.empty())
			Tooltip(ctx, rect, tooltip);
		if (enabled)
		{
			if (hovered)
				ctx.SetCursor(WuiCursor::Hand);
			const bool keyActivated = focused
				&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
			return (hovered && ctx.Input().MouseClicked[0]) || keyActivated;
		}
		return false;
	}

	bool Toggle(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme)
	{
		bool& value = ctx.Persist<bool>(id, false);
		const bool focused = ctx.Focus() == id;
		// U2A 键盘激活:焦点在开关上时 Enter/Space = 切换。
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		if (ctx.IsClicked(rect) || keyActivated)
			value = !value;
		RegisterAccessNode(id, "toggle", rect, label, value ? "on" : "off", true, true, focused);
		ctx.RegisterFocusable(id, rect);

		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, label, 15.0f, false });
		DrawFocusRing(ctx, rect, id, theme);
		return value;
	}

	bool TabBar(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::vector<std::string>& tabs,
		int& active, const WuiTheme& theme, int* closeRequested)
	{
		if (closeRequested)
			*closeRequested = -1;
		if (id == 0 || tabs.empty())
			return false;
		if (active < 0 || active >= static_cast<int>(tabs.size()))
			active = 0;
		bool changed = false;
		const float tabW = rect.W / static_cast<float>(tabs.size());
		// 标签条本体的节点(P1c-a):标签等分整条矩形 → 节点矩形里任何一点都落在某个标签上,
		// 按它注入的点击(= 组中心那一格)一定激活一个真实标签;要精确点某一个仍用子节点
		// kind="tab"(派生 id)。
		// P1c-E4:焦点停在**某个标签**上(子 id),组节点按"组里有焦点"报 focused=true —— 这样
		// AI 既能按组 id 判"标签条整体有键盘焦点",也能从子节点读"焦点在第几个标签"。
		bool groupFocused = ctx.Focus() == id;
		for (size_t i = 0; i < tabs.size() && !groupFocused; ++i)
			groupFocused = ctx.Focus() == DerivedChildId(id, ".tab.", i);
		RegisterAccessNode(id, "tab-bar", rect, std::string(), tabs[static_cast<size_t>(active)], true, true,
			groupFocused);
		// 整条底边线:让标签条与下方内容有分界(取面板边框色,不新增样式常量)。
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, rect.Y + rect.H - 1.0f, rect.W, 1.0f }, theme.Border, 0.0f });
		for (size_t i = 0; i < tabs.size(); ++i)
		{
			const WuiRect tab { rect.X + tabW * static_cast<float>(i), rect.Y, tabW, rect.H };
			const WuiId tabId = DerivedChildId(id, ".tab.", i);
			const bool isActive = static_cast<int>(i) == active;
			const bool hovered = ctx.IsHovered(tab);
			// P1c-E4:子节点的 focused 实参以前缺省(false)→ "焦点在哪一页"AI 看不见。
			RegisterAccessNode(tabId, "tab", tab, tabs[i], isActive ? "true" : "false", true, true,
				ctx.Focus() == tabId);
			ctx.RegisterFocusable(tabId, tab);
			if (isActive)
				ctx.Commands().push_back({ WuiDrawKind::Rect, tab, theme.ActiveBg, 0.0f });
			else if (hovered)
				ctx.Commands().push_back({ WuiDrawKind::Rect, tab, theme.HoverBg, 0.0f });
			if (hovered)
				ctx.SetCursor(WuiCursor::Hand);
			const std::string text = EllipsizeToWidth(ctx, tabs[i], std::max(0.0f, tab.W - theme.Pad * 2.0f), theme.FontSizeBody);
			if (!text.empty())
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ tab.X + theme.Pad, tab.Y + (tab.H - theme.FontSizeBody) * 0.5f, 0, 0 },
					isActive ? theme.Text : theme.TextMuted, 0, 1.0f, text, theme.FontSizeBody, false });
			if (isActive)
			{
				// 活跃标签:底部 2px 强调色下划线。
				ctx.Commands().push_back({ WuiDrawKind::Rect,
					{ tab.X, tab.Y + tab.H - 2.0f, tab.W, 2.0f }, theme.Accent, 0.0f });
			}
			DrawFocusRing(ctx, tab, tabId, theme);
			const bool keyActivated = ctx.Focus() == tabId
				&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
			if (ctx.IsClicked(tab) || keyActivated)
			{
				if (!isActive)
				{
					active = static_cast<int>(i);
					changed = true;
				}
			}
			else if (closeRequested && ctx.IsClicked(tab, 2))
				*closeRequested = static_cast<int>(i);   // 中键:只上报请求,不自行关闭
		}
		return changed;
	}

	bool Segmented(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::vector<std::string>& options,
		int& selected, const WuiTheme& theme)
	{
		if (options.empty())
			return false;
		if (selected < 0 || selected >= static_cast<int>(options.size()))
			selected = 0;
		bool changed = false;
		// 组节点 + 逐个分段节点都登记(P1c-a):分段把整条矩形铺满,所以**组节点也是真的可点**——
		// 按组节点注入的点击落在组中心,命中的是那里那一格分段,与用户点击走同一条路径
		// (于是 interactive=true 不撒谎);要精确选某一格仍用 kind="segmented-option" 的子节点。
		// P1c-E4:焦点停在**某一格**上(子 id),组节点按"组里有焦点"报 focused=true —— AI 既能按组
		// id 判"分段控件有键盘焦点",也能从子节点读"焦点在第几格"。
		bool groupFocused = ctx.Focus() == id;
		for (size_t i = 0; i < options.size() && !groupFocused; ++i)
			groupFocused = ctx.Focus() == DerivedChildId(id, ".segment.", i);
		RegisterAccessNode(id, "segmented", rect, std::string(), options[static_cast<size_t>(selected)],
			true, true, groupFocused);
		// 同一圆角外壳:铺底 + 1px 描边,内部等分(选中项 ActiveBg + Accent 文本)。
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonBg, theme.Radius });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, theme.Radius, 1.0f });
		const float optionW = rect.W / static_cast<float>(options.size());
		for (size_t i = 0; i < options.size(); ++i)
		{
			const WuiRect option { rect.X + optionW * static_cast<float>(i), rect.Y, optionW, rect.H };
			const WuiId optionId = DerivedChildId(id, ".segment.", i);
			const bool isSelected = static_cast<int>(i) == selected;
			const bool hovered = ctx.IsHovered(option);
			// P1c-E4:子节点的 focused 实参以前缺省(false)→ "焦点在哪一格"AI 看不见。
			RegisterAccessNode(optionId, "segmented-option", option, options[i], isSelected ? "true" : "false",
				true, true, ctx.Focus() == optionId);
			ctx.RegisterFocusable(optionId, option);
			const WuiRect inner { option.X + 1.0f, option.Y + 1.0f,
				std::max(0.0f, option.W - 2.0f), std::max(0.0f, option.H - 2.0f) };
			if (isSelected)
				ctx.Commands().push_back({ WuiDrawKind::Rect, inner, theme.ActiveBg, std::max(0.0f, theme.Radius - 1.0f) });
			else if (hovered)
				ctx.Commands().push_back({ WuiDrawKind::Rect, inner, theme.HoverBg, std::max(0.0f, theme.Radius - 1.0f) });
			if (hovered)
				ctx.SetCursor(WuiCursor::Hand);
			const std::string text = EllipsizeToWidth(ctx, options[i], std::max(0.0f, option.W - theme.Pad * 2.0f), theme.FontSizeSmall);
			if (!text.empty())
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ option.X + theme.Pad, option.Y + (option.H - theme.FontSizeSmall) * 0.5f, 0, 0 },
					isSelected ? theme.Accent : theme.Text, 0, 1.0f, text, theme.FontSizeSmall, false });
			DrawFocusRing(ctx, option, optionId, theme);
			const bool keyActivated = ctx.Focus() == optionId
				&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
			if ((ctx.IsClicked(option) || keyActivated) && !isSelected)
			{
				selected = static_cast<int>(i);
				changed = true;
			}
		}
		return changed;
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
		const bool focused = ctx.Focus() == id;
		// U2A 键盘激活:焦点在勾选框上时 Enter/Space = 切换。
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		if (ctx.IsClicked(rect) || keyActivated)
		{
			value = !value;
			changed = true;
		}
		// 无障碍节点带上英文术语:脚本/自动化在中文界面下也能按英文检索。
		RegisterAccessNode(id, "checkbox", rect,
			term.empty() ? label : (label + " (" + term + ")"), value ? "true" : "false", true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		const float labelSize = 15.0f;
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - labelSize) * 0.5f, 0, 0 },
			theme.Text, 0, 1.0f, label, labelSize, false });
		if (!term.empty())
		{
			// 英文术语对照:Caption/次要色。放不下时与 LabelWithTerm 同族降级 —— 先按可用宽度
			// 省略号截断(而不是整段不画);连一个字符加省略号都放不下才不画。
			const float termSize = theme.FontSizeCaption;
			const float termX = rect.X + 24.0f + ctx.MeasureTextWidth(label, labelSize) + 6.0f;
			const float termBudget = (rect.X + rect.W) - termX;
			const std::string shownTerm = termBudget <= 0.0f
				? std::string()
				: (ctx.MeasureTextWidth(term, termSize) <= termBudget
					? term
					: EllipsizeToWidth(ctx, term, termBudget, termSize));
			if (!shownTerm.empty())
				ctx.Commands().push_back({ WuiDrawKind::Text, { termX, rect.Y + (rect.H - termSize) * 0.5f, 0, 0 },
					theme.TextMuted, 0, 1.0f, shownTerm, termSize, false });
		}
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
	}

	bool CheckboxMixed(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		bool& value, bool mixed, const WuiTheme& theme)
	{
		bool changed = false;
		const bool focused = ctx.Focus() == id;
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		if (ctx.IsClicked(rect) || keyActivated)
		{
			// 多选行的语义:混合态被点击 = "全部选中"(value 写回,调用方再分发给它管辖的对象)。
			mixed = false;
			value = true;
			changed = true;
		}
		// 无障碍节点:混合态 value="mixed",与 Checkbox 的 true/false 区分开(脚本据此判断三态)。
		RegisterAccessNode(id, "checkbox", rect, label,
			mixed ? "mixed" : (value ? "true" : "false"), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, (value || mixed) ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		if (mixed)
		{
			// 混合:水平短横(—),而不是勾。
			ctx.Commands().push_back({ WuiDrawKind::Rect,
				{ box.X + 4.0f, box.Y + box.H * 0.5f - 1.0f, box.W - 8.0f, 2.0f }, theme.Text, 1.0f });
		}
		else if (value)
		{
			// 勾:两段斜线(Quad),不依赖字体里有没有 '✓' 字形。
			PushLineQuad(ctx, { box.X + 4.0f, box.Y + 8.5f }, { box.X + 7.0f, box.Y + 11.5f }, 2.0f, theme.Text);
			PushLineQuad(ctx, { box.X + 7.0f, box.Y + 11.5f }, { box.X + 12.0f, box.Y + 5.0f }, 2.0f, theme.Text);
		}
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
			theme.Text, 0, 1.0f, label, 15.0f, false });
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
	}

	void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme)
	{
		// U2A 键盘微调步长:滑杆没有 drag 步长参数,按值域的 1%(0–1 滑杆 = 0.01/次)。
		constexpr float kKeyboardStepFraction = 0.01f;
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "slider", rect, std::string(), FloatToText(value), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const float range = std::max(0.0001f, max - min);
		if (ctx.Input().MouseDown[0] && ctx.IsHovered(rect))
		{
			const float fraction = (ctx.Input().MousePos.x - rect.X) / std::max(1.0f, rect.W);
			value = min + std::max(0.0f, std::min(1.0f, fraction)) * range;
		}
		// U2A 键盘微调:焦点在滑杆上时左右箭头 ±1% 值域,并夹在 [min,max] 内。
		if (focused)
		{
			const float step = range * kKeyboardStepFraction;
			if (ctx.WasKeyPressed(KeyCodes::Left))
				value = std::max(min, value - step);
			if (ctx.WasKeyPressed(KeyCodes::Right))
				value = std::min(max, value + step);
		}

		const float trackH = 4.0f;
		const float trackY = rect.Y + rect.H * 0.5f - trackH * 0.5f;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, trackY, rect.W, trackH }, theme.ButtonBg, 2.0f });
		const float fraction = (value - min) / range;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, trackY, rect.W * fraction, trackH }, theme.Accent, 2.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X + rect.W * fraction - 4.0f, rect.Y + (rect.H - 12.0f) * 0.5f, 8.0f, 12.0f }, theme.Text, 2.0f });
		DrawFocusRing(ctx, rect, id, theme);
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
		struct WuiNumericState
		{
			bool Pressed = false;
			bool Dragging = false;
			bool Editing = false;
			float PressX = 0;
			double PressValue = 0;
			std::string Buffer;
			int Cursor = -1;
			int SelStart = -1;
			int SelEnd = -1;
			// U24:值区(而非条体)起手 = 松手进文本编辑,不参与拖拽。
			bool PressOnValue = false;
			// U24:非法输入反馈剩余帧数(红框 + 危险色数值);只影响绘制/无障碍,不改值。
			int ErrorFrames = 0;
		};

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
			// P4-UX14:Ctrl+A 全选(用户实测"重命名时 Ctrl+A 失效")。放在插入之前,
			// 与后续的字符插入/光标移动互不干扰。
			if (ctx.Input().Ctrl && ctx.WasKeyPressed(KeyCodes::A))
			{
				selStart = 0;
				selEnd = Utf8Count(buffer);
				cursor = selEnd;
			}
			// MAT-UI8:剪贴板 Ctrl+C/X/V(2026-09-25 用户实测"Ctrl+F 查找框里只能手打" ——
			// 单行文本框此前只有 Ctrl+A,复制/剪切/粘贴全是 no-op)。索引口径:selStart/selEnd/
			// cursor 都是**码点**索引,与 buffer 的字节偏移之间走 Utf8Offset/Utf8Count。
			// 回调由宿主注入(见 WuiContext::GetClipboard/SetClipboard);未注入则静默 no-op。
			if (ctx.Input().Ctrl)
			{
				const bool hasSelection = selStart >= 0 && selEnd > selStart;
				if (ctx.WasKeyPressed(KeyCodes::C) && ctx.SetClipboard && hasSelection)
				{
					const size_t from = Utf8Offset(buffer, selStart);
					const size_t to = Utf8Offset(buffer, selEnd);
					ctx.SetClipboard(std::string_view(buffer).substr(from, to - from));
				}
				if (ctx.WasKeyPressed(KeyCodes::X) && ctx.SetClipboard && hasSelection)
				{
					const size_t from = Utf8Offset(buffer, selStart);
					const size_t to = Utf8Offset(buffer, selEnd);
					ctx.SetClipboard(std::string_view(buffer).substr(from, to - from));
					buffer.erase(from, to - from);
					cursor = selStart;
					selStart = -1;
					selEnd = -1;
				}
				if (ctx.WasKeyPressed(KeyCodes::V) && ctx.GetClipboard)
				{
					std::string clip;
					if (ctx.GetClipboard(clip) && !clip.empty())
					{
						// 单行控件:换行/回车直接丢掉(不插 '\n'、也不截成第一行),其余原样插入。
						clip.erase(std::remove(clip.begin(), clip.end(), '\r'), clip.end());
						clip.erase(std::remove(clip.begin(), clip.end(), '\n'), clip.end());
						const bool replacing = selStart >= 0 && selEnd > selStart;
						const int insertAt = replacing ? selStart : cursor;
						const size_t byteAt = Utf8Offset(buffer, insertAt);
						if (replacing)
							buffer.erase(byteAt, Utf8Offset(buffer, selEnd) - byteAt);
						buffer.insert(byteAt, clip);
						cursor = insertAt + Utf8Count(clip);
						selStart = -1;
						selEnd = -1;
					}
				}
			}
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

		// ---- U24 数值控件的公共件 ----

		// 显示用浮点文本:定点输出后去尾零(0.300 → "0.3",整数 → "0")。
		// 注意:既有 kind="slider"/"drag-float" 的无障碍 value 仍用 FloatToText("%.3f"),
		// 只有新增控件用这里的"有效位"文本,避免改既有节点的 value 语义。
		std::string TrimNumberText(std::string text)
		{
			if (text.find('.') == std::string::npos)
				return text;
			size_t last = text.find_last_not_of('0');
			if (last == std::string::npos)
				return "0";
			if (text[last] == '.')
				--last;
			text.erase(last + 1);
			if (text == "-0")
				text = "0";
			return text;
		}

		std::string FormatFloatDisplay(float value, int decimals)
		{
			if (!std::isfinite(value))
				return "--";
			char buffer[64] = {};
			const int places = std::max(0, std::min(9, decimals < 0 ? 3 : decimals));
			std::snprintf(buffer, sizeof(buffer), "%.*f", places, static_cast<double>(value));
			return TrimNumberText(buffer);
		}

		// 解析:允许首尾空白;要求整串被消费且结果是有限值。失败时调用方保留原值。
		bool ParseFloatText(const std::string& text, float* out)
		{
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

		bool ParseIntText(const std::string& text, int64_t* out)
		{
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

		void BeginNumericEdit(WuiContext& ctx, WuiNumericState& state, WuiId id, const std::string& text)
		{
			state.Editing = true;
			state.Buffer = text;
			state.Cursor = -1;
			state.SelStart = 0;
			state.SelEnd = Utf8Count(text);
			state.ErrorFrames = 0;
			ctx.SetFocus(id);
			ctx.SetTextInputActive(true);
		}

		void EndNumericEdit(WuiNumericState& state)
		{
			state.Editing = false;
			state.Pressed = false;
			state.Dragging = false;
			state.Cursor = -1;
			state.SelStart = -1;
			state.SelEnd = -1;
		}

		// 数值文本命令:rightAlign = 值区(文本右对齐,宽度固定);否则输入框(左对齐)。
		void PushNumericTextCommand(WuiContext& ctx, const WuiRect& textRect, const std::string& text,
			const WuiTheme& theme, const WuiNumericState& state, bool editing, bool rightAlign,
			const WuiColor& color)
		{
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

	bool DragFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float speed, float min, float max, const WuiTheme& theme)
	{
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
					value = std::max(lo, std::min(hi, static_cast<float>(state.PressValue + dx * speed)));
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
			const float step = std::fabs(speed) > 0.0f ? std::fabs(speed) : 1.0f;
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

	bool DragInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme)
	{
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
					value = std::max(lo, std::min(hi, static_cast<int64_t>(state.PressValue + static_cast<double>(dx))));
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

	bool DragBarFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max,
		const WuiTheme& theme, const WuiNumberStyle& style)
	{
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

	namespace
	{
		// NumberFieldInt / StepperInt 的共同实现:steppers=true 时左右各一个 [−]/[+] 步进钮。
		// 计数/索引类字段**不做拖动改值**(避免拖出奇怪的大整数);值区右对齐、单位靠右。
		bool IntNumberFieldCore(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value,
			int64_t min, int64_t max, const WuiTheme& theme, const WuiNumberStyle& style,
			bool steppers, const char* kind)
		{
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

	bool NumberFieldInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value,
		int64_t min, int64_t max, const WuiTheme& theme, const WuiNumberStyle& style)
	{
		return IntNumberFieldCore(ctx, id, rect, value, min, max, theme, style,
			style.Steppers, "number-field");
	}

	bool StepperInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int& value, int min, int max,
		const WuiTheme& theme, const WuiNumberStyle& style)
	{
		int64_t wide = static_cast<int64_t>(value);
		const bool changed = IntNumberFieldCore(ctx, id, rect, wide, static_cast<int64_t>(min),
			static_cast<int64_t>(max), theme, style, true, "stepper");
		value = static_cast<int>(wide);
		return changed;
	}

	bool ResetDefaultButton(WuiContext& ctx, WuiId id, const WuiRect& rect, bool modified,
		const WuiTheme& theme, const std::string& label, const std::string& tooltip)
	{
		if (rect.W <= 2.0f || rect.H <= 2.0f)
			return false;
		// 固定占位:两种状态都登记同一个 id/rect;modified=false 时 enabled/interactive=false,
		// 且不参与焦点表 —— 尺寸与位置逐像素不变,只是弱化。
		const bool focused = modified && ctx.Focus() == id;
		RegisterAccessNode(id, "reset-default", rect, label.empty() ? std::string("Reset") : label,
			modified ? "modified" : "default", modified, modified, focused);
		if (modified)
			ctx.RegisterFocusable(id, rect);
		const bool hovered = modified && ctx.IsHovered(rect);
		if (hovered)
			ctx.SetCursor(WuiCursor::Hand);
		if (modified && (hovered || focused))
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, hovered ? theme.HoverBg : theme.ActiveBg, 3.0f });
		const WuiColor color = modified ? ((hovered || focused) ? theme.Text : theme.TextMuted) : theme.TextDisabled;
		const float cx = rect.X + rect.W * 0.5f;
		const float cy = rect.Y + rect.H * 0.5f;
		const float radius = std::max(3.0f, std::min(6.5f, std::min(rect.W, rect.H) * 0.30f));
		constexpr float kPi = 3.14159265358979323846f;
		const float startAngle = -0.55f * kPi;
		const float sweep = 1.62f * kPi;
		constexpr int kSegments = 12;
		glm::vec2 previous { cx + radius * std::cos(startAngle), cy + radius * std::sin(startAngle) };
		for (int i = 1; i <= kSegments; ++i)
		{
			const float angle = startAngle + sweep * (static_cast<float>(i) / static_cast<float>(kSegments));
			const glm::vec2 point { cx + radius * std::cos(angle), cy + radius * std::sin(angle) };
			PushLineQuad(ctx, previous, point, 1.6f, color);
			previous = point;
		}
		// 回旋箭头(不依赖字体里有没有 ↺ 字形):在弧线末端按切线方向画两段短线。
		const float tangentX = -std::sin(startAngle + sweep);
		const float tangentY = std::cos(startAngle + sweep);
		const glm::vec2 arrowA { previous.x - tangentX * 3.4f + tangentY * 1.6f,
			previous.y - tangentY * 3.4f - tangentX * 1.6f };
		const glm::vec2 arrowB { previous.x - tangentX * 3.4f - tangentY * 1.6f,
			previous.y - tangentY * 3.4f + tangentX * 1.6f };
		PushLineQuad(ctx, previous, arrowA, 1.6f, color);
		PushLineQuad(ctx, previous, arrowB, 1.6f, color);
		DrawFocusRing(ctx, rect, id, theme);
		if (modified && !tooltip.empty())
			Tooltip(ctx, rect, tooltip);
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		return modified && (ctx.IsClicked(rect) || keyActivated);
	}

	namespace
	{
		// TextField / TextFieldEx 的共同实现(旧签名语义逐条不变,只是多了 error 参数):
		// error 非空时描边用 theme.Danger,并把 "error=<文本>" 追加进无障碍节点 value(TextFieldEx 的契约)。
		bool TextFieldCore(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer,
			const WuiTheme& theme, bool* cancelledOut, const std::string& error,
			const TextFieldA11y* a11y = nullptr)
		{
			// P4-U5a:label/value 都不能空 —— 空输入时 value 用占位文案(与用户看到的一致),
			// label 用控件名;两者都由调用方给出(文本控件不知道自己的业务语义)。
			std::string accessValue = error.empty() ? buffer : (buffer + " error=" + error);
			if (accessValue.empty() && a11y)
				accessValue = a11y->Placeholder;
			RegisterAccessNode(id, "text-field", rect, a11y ? a11y->Label : std::string(),
				accessValue, true, true, ctx.Focus() == id);
			ctx.RegisterFocusable(id, rect);
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
			// P1c-E4-fix:单行文本框(TextField/TextFieldEx 与其所有调用者:搜索框、重命名、
			// 属性文本、Combo 过滤、十六进制输入……)登记时带 ReleaseOnTab 标记 ⇒ 按 Tab/Shift+Tab
			// 交出焦点、走正常焦点链,不再把 Tab 吃掉。CodeEditor 不走这条路径 ⇒ Tab=缩进不变。
			if (focused) ctx.SetTextInputActive(true, /*releaseOnTab=*/true);
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
			// 有错误时描边用 Danger(焦点态也一样):行内校验错误比焦点色更需要被看到。
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
				error.empty() ? (focused ? theme.Accent : theme.Border) : theme.Danger, 3.0f, focused ? 1.5f : 1.0f });
			ctx.Commands().push_back(std::move(command));
			DrawFocusRing(ctx, rect, id, theme);
			return submitted;
		}
	}

	bool TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme,
		bool* cancelledOut, const TextFieldA11y* a11y)
	{
		// 旧签名语义不变:与 TextFieldEx 共用同一条实现,error 恒为空。
		return TextFieldCore(ctx, id, rect, buffer, theme, cancelledOut, std::string(), a11y);
	}

	bool TextFieldEx(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer,
		const WuiTheme& theme, const std::string& error, const TextFieldA11y* a11y)
	{
		// 返回值与 TextField 相同(回车提交)。错误说明画在控件下方一行(Caption 字号、Danger 色),
		// 超宽按省略号裁剪;调用方负责给这一行留出高度。
		const bool submitted = TextFieldCore(ctx, id, rect, buffer, theme, nullptr, error, a11y);
		if (!error.empty())
		{
			const std::string shown = EllipsizeToWidth(ctx, error, rect.W, theme.FontSizeCaption);
			if (!shown.empty())
				ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X, rect.Y + rect.H + 2.0f, 0, 0 },
					theme.Danger, 0, 1.0f, shown, theme.FontSizeCaption, false });
		}
		return submitted;
	}

	void Image(WuiContext& ctx, const WuiRect& rect, uint64_t textureId, const WuiRect& uv, const WuiTheme& theme)
	{
		ctx.Commands().push_back({ WuiDrawKind::Image, rect, theme.Text, 0, 1.0f, "", 15.0f, false, textureId, uv });
	}

	// P1c-LIB2:渐变填充原语 —— 一条 Gradient 命令,Kind/Rect/Corners 三项就是全部画面输入。
	// 与面板手写渐变的等价判据:同一 rect、同一四角颜色、同一顺序 ⇒ 逐字节相同的命令。
	void GradientFill(WuiContext& ctx, const WuiRect& rect, const WuiColor& topLeft, const WuiColor& topRight,
		const WuiColor& bottomRight, const WuiColor& bottomLeft)
	{
		WuiDrawCommand command;
		command.Kind = WuiDrawKind::Gradient;
		command.Rect = rect;
		command.Corners = { topLeft, topRight, bottomRight, bottomLeft };
		ctx.Commands().push_back(std::move(command));
	}

	void GradientFill(WuiContext& ctx, const WuiRect& rect, const WuiColor& from, const WuiColor& to,
		bool vertical)
	{
		if (vertical)
			GradientFill(ctx, rect, from, from, to, to);
		else
			GradientFill(ctx, rect, from, to, to, from);
	}

	// P1c-LIB3:线段原语 —— 与 ViewportPanel::PushProjectedSegment 末段逐字段等价的那一步:
	// 方向 d = to - from、退化阈值 0.5、法线 (-d.y, d.x)/|d|、偏移 = 法线*thickness/2、
	// 顶点 {from+n, to+n, to-n, from-n}。投影/近平面裁剪/夹取留在调用方(库件不猜几何语义)。
	void LineSegment(WuiContext& ctx, const glm::vec2& from, const glm::vec2& to, const WuiColor& color,
		float thickness)
	{
		const glm::vec2 delta = to - from;
		const float length = glm::length(delta);
		if (length < 0.5f)
			return;   // 退化线段:与面板同一阈值(0.5px),不产出命令
		const glm::vec2 normal { -delta.y / length, delta.x / length };
		const glm::vec2 offset = normal * (thickness * 0.5f);
		WuiDrawCommand command;
		command.Kind = WuiDrawKind::Quad;
		command.Color = color;
		command.Vertices = { from + offset, to + offset, to - offset, from - offset };
		ctx.Commands().push_back(std::move(command));
	}

	bool Combo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		const std::vector<std::string>& options, int& selected, const WuiTheme& theme)
	{
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "combo", rect, label,
			(selected >= 0 && selected < static_cast<int>(options.size())) ? options[selected] : std::string(),
			true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const bool hovered = ctx.IsHovered(rect);
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, hovered ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });
		// P4-UX1:下拉只画**当前值**(label 仅用于无障碍节点)。
		// 面板的排版约定是"标签在左、控件在右",把 label 也画进框里会出现
		// "阴影贴图: 2048" / "界面语言: 简体中文" 这种重复且臃肿的文本。
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
			theme.Text, 0, 1.0f, options[selected], 15.0f, false });
		// 下拉提示:右下角两段细线组成的小折角(不依赖字体里的箭头字形)。
		const float caretX = rect.X + rect.W - 12.0f;
		const float caretY = rect.Y + rect.H * 0.5f - 3.0f;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { caretX, caretY, 7.0f, 1.5f }, theme.TextMuted, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { caretX + 1.5f, caretY + 3.0f, 4.0f, 1.5f }, theme.TextMuted, 1.0f });
		DrawFocusRing(ctx, rect, id, theme);

		// U2A 键盘:焦点在下拉触发器上时 Enter/Space = 点击一次(开/关弹层,与鼠标同一条路径)。
		const bool keyToggle = focused && (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		if (ctx.IsClicked(rect) || keyToggle)
		{
			if (ctx.IsPopupOpen(id))
				ctx.ClosePopup(id);
			else
				ctx.OpenPopup(id);
		}

		bool changed = false;
		if (ctx.IsPopupOpen(id))
		{
			// P4-U29:绘制延后到帧末(命中/遮挡登记仍在此处当场生效)。
			WuiDeferredPopupScope deferred(ctx);
			const float itemH = 22.0f;
			const WuiRect panel { rect.X, rect.Y + rect.H + 2.0f, rect.W, itemH * options.size() + 8.0f };
			DrawPanelSurface(ctx, panel, theme);
			for (size_t i = 0; i < options.size(); ++i)
			{
				const WuiRect item { panel.X + 4.0f, panel.Y + 4.0f + itemH * static_cast<float>(i), panel.W - 8.0f, itemH };
				// P4-UX5:每个条目登记一个无障碍节点(验证者指出 MSAA/阴影贴图这类下拉的选项
				// 点不到 —— 只有触发器登记过)。节点 id = ComboOptionId(parent, i),弹层关闭时
				// 本段代码不执行,下一帧 BeginFrame 清掉本窗口旧节点 → 节点随弹层自然消失。
				// 点击等价性:ui.invoke 只是把点击注入条目矩形中心,WuiScriptedInput 按坐标注入
				// hover/press/release;条目自己的 ctx.IsHovered/IsClicked 读的仍是面板输入状态,
				// 与用户鼠标点同一行完全同一条路径(与 Checkbox/MenuItem 的可脚本化方式一致)。
				RegisterAccessNode(ComboOptionId(id, i), "combo-option", item,
					options[i], (selected >= 0 && i == static_cast<size_t>(selected)) ? "true" : "false");
				if (ctx.IsHovered(item))
				{
					ctx.Commands().push_back({ WuiDrawKind::Rect, item, theme.ButtonHover, 2.0f });
					// ③ 弹层内光标归弹层:候选行是弹层自己的可点控件,显式声明 Hand,
					// 不再让"更早绘制的控件"留下的光标形状代表弹层。
					ctx.SetCursor(WuiCursor::Hand);
				}
				// P4-U28:条目在 release 帧确认(press+release 必须落在同一项)—— 在条目上
				// 按下后拖到弹层外松手不会选中;关闭那一帧的 release 也不会被下层控件认领。
				if (ctx.IsClickCompleted(ComboOptionId(id, i), item))
				{
					selected = static_cast<int>(i);
					changed = true;
					ctx.ConsumePointerClick();
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
			{
				ctx.PushHoverBlocker(panel);
				// P4-U7:同时登记为覆盖层矩形 → 下一帧它只挡**非覆盖层**控件,
				// 先画的面板(或本面板更早绘制的行)也不会吃掉落在弹层上的点击。
				ctx.RegisterOverlayRect(panel);
			}
		}
		return changed;
	}

	// D3:可搜索下拉。选项多(材质/贴图路径)时,用输入框过滤 + 滚轮滚动选择,
	// 交互与常见引擎的资源选择器一致。
	bool SearchableCombo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		const std::vector<std::string>& options, int& selected, const WuiTheme& theme)
	{
		// 触发器本身也可被 ui.invoke 点击(等价于点开下拉)。
		const bool focused = ctx.Focus() == id;
		const std::string current = (selected >= 0 && selected < static_cast<int>(options.size()))
			? options[selected] : std::string();
		RegisterAccessNode(id, "search-combo", rect, label, current, true, true, focused);
		ctx.RegisterFocusable(id, rect);
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
		if (!open)
			ctx.Commands().push_back({ WuiDrawKind::Text, { fieldRect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
				theme.Text, 0, 1.0f, current, 15.0f, false });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + rect.W - 18.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
			theme.TextMuted, 0, 1.0f, open ? "^" : "v", 14.0f, false });
		DrawFocusRing(ctx, rect, id, theme);
		if (ctx.IsClicked(rect) && !open)
		{
			filter.clear();
			ctx.OpenPopup(id);
			ctx.SetFocus(editId);
		}
		// U2A 键盘:焦点在触发器上、弹层未展开时 Enter/Space = 点开(与鼠标同一条路径 ——
		// 打开后焦点交给弹层里的搜索框,后续输入/回车归它)。
		else if (focused && !open && (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space)))
		{
			filter.clear();
			ctx.OpenPopup(id);
			ctx.SetFocus(editId);
		}

		bool changed = false;
		if (!open)
			return false;

		// P4-U29:同 Combo —— 弹层绘制延后,命中/遮挡登记留在原地。
		WuiDeferredPopupScope deferred(ctx);
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
		// 诊断(WLD_TRACE_UI=1):下拉的几何/过滤/滚动与鼠标位置 —— "点了候选项却没反应"这类
		// 问题(几何对不上 / 被别的控件吃掉)只能靠这几个数直接判定。
		if (std::getenv("WLD_TRACE_UI") && ctx.IsHovered(panel))
			WLD_CORE_INFO("[ui] search-combo id={0} panel=({1},{2},{3},{4}) flip={5} scroll={6} visible={7} matches={8} mouse=({9},{10})",
				id, static_cast<int>(panel.X), static_cast<int>(panel.Y),
				static_cast<int>(panel.W), static_cast<int>(panel.H), flipUp ? 1 : 0, scroll, visible, matches.size(),
				static_cast<int>(ctx.Input().MousePos.x), static_cast<int>(ctx.Input().MousePos.y));
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
			// U2A:与 Combo 用同一条约定 —— id = ComboOptionId(父 id, 选项下标)、kind="combo-option",
			// value = 该选项是否为当前值。上一条遗留项(可搜索下拉的条目对脚本不可见)由此补齐。
			RegisterAccessNode(ComboOptionId(id, static_cast<size_t>(optionIndex)), "combo-option", item,
				options[optionIndex], (selected == optionIndex) ? "true" : "false");
			// 兼容既有端到端脚本:tools/agents/skills/worldengine-dev/scripts/verify-ai-control.py 按
			// kind="combo-item" 检索候选项,而该脚本不在本任务的文件边界内,所以同一条目保留旧节点。
			// 两个节点的矩形/标签一致,点击注入的坐标相同 → 走的是同一条命中路径。
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
			// P4-U28:与 Combo 同一条 release 确认口径(拖出弹层后松手不选中)。
			if (ctx.IsClickCompleted(ComboOptionId(id, static_cast<size_t>(optionIndex)), item))
			{
				if (std::getenv("WLD_TRACE_UI"))
					WLD_CORE_INFO("[ui] search-combo row clicked: id={0} option={1} label='{2}'",
						id, optionIndex, options[optionIndex]);
				selected = optionIndex;
				changed = true;
				ctx.ConsumePointerClick();
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
		{
			ctx.PushHoverBlocker(panel);
			// P4-U7:同时登记为覆盖层矩形 → 下一帧只挡非覆盖层控件。
			ctx.RegisterOverlayRect(panel);
		}
		return changed;
	}

	bool TreeNode(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool leaf, const WuiTheme& theme)
	{
		bool& open = ctx.Persist<bool>(id, false);
		const bool focused = ctx.Focus() == id;
		// U2A 键盘:焦点在节点上时 Enter/Space = 展开/收起。
		const bool keyToggle = focused && !leaf
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		if (ctx.IsClicked(rect) || keyToggle)
			open = !leaf && !open;
		RegisterAccessNode(id, "tree-node", rect, label, open ? "open" : "closed", true, !leaf, focused);
		// 叶子不进 Tab 顺序:与无障碍节点的 interactive=!leaf 保持同一条规则。
		if (!leaf)
			ctx.RegisterFocusable(id, rect);
		const std::string marker = leaf ? "  " : (open ? "- " : "+ ");
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 4.0f, rect.Y + 2.0f, 0, 0 }, theme.TextMuted, 0, 1.0f, marker, 14.0f, false });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 22.0f, rect.Y + 2.0f, 0, 0 }, theme.Text, 0, 1.0f, label, 14.0f, false });
		DrawFocusRing(ctx, rect, id, theme);
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
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "menu-item", rect, label, std::string(), enabled, true, focused);
		if (enabled)
			ctx.RegisterFocusable(id, rect);
		if (ctx.IsHovered(rect) && enabled)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonHover, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, enabled ? theme.Text : theme.TextMuted, 0, 1.0f, label, 15.0f, false });
		DrawFocusRing(ctx, rect, id, theme);
		// P4-U30:菜单项与下拉选项统一为"release 确认" —— press 落在(菜单按钮 ∪ 菜单面板)里,
		// release 落在本项上才触发;在项上按下后拖走松开、或在面板外按下再拖到项上松开都不触发。
		// 键盘(焦点 + Enter/Space)不变。
		if (enabled)
			ctx.RecordMenuPress(id, rect);
		return enabled && (ctx.IsMenuRelease(id, rect)
			|| (focused && (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space))));
	}

	bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool checked, bool enabled, const WuiTheme& theme)
	{
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "menu-item", rect, label, checked ? "checked" : "unchecked", enabled, true, focused);
		if (enabled)
			ctx.RegisterFocusable(id, rect);
		if (ctx.IsHovered(rect) && enabled)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonHover, 0.0f });
		// 复选风格(如 Window 菜单的可见性开关)显示 [x]/[ ];普通动作项走上面的重载。
		const std::string text = std::string(checked ? "[x] " : "[ ] ") + label;
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, enabled ? theme.Text : theme.TextMuted, 0, 1.0f, text, 15.0f, false });
		DrawFocusRing(ctx, rect, id, theme);
		// 与普通菜单项同一口径:P4-U30 起"release 落在本项上"才触发(拖走/面板外按下都不触发)。
		if (enabled)
			ctx.RecordMenuPress(id, rect);
		return enabled && (ctx.IsMenuRelease(id, rect)
			|| (focused && (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space))));
	}

	bool BeginModal(WuiContext& ctx, WuiId id, const std::string& title, const glm::vec2& size, WuiRect* panel, const WuiTheme& theme)
	{
		if (ctx.Modal() != id)
			return false;
		ctx.PushOverlay();
		const glm::vec2 viewport = ctx.ViewportSize();
		// P4-U7:模态遮罩盖住整个客户区 —— 登记为覆盖层矩形,下一帧下层控件不会
		// 吃掉落在遮罩/模态上的点击(模态自己由外层 BeginModalInputBlock 再封一道)。
		ctx.RegisterOverlayRect({ 0.0f, 0.0f, viewport.x, viewport.y });
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

	bool BeginScrollArea(WuiContext& ctx, const WuiRect& viewport, float contentHeight, float& scrollY,
		const WuiTheme& theme, WuiId id)
	{
		if (ctx.IsHovered(viewport))
			scrollY -= ctx.Input().Wheel * 40.0f;
		// P1c-E4:id != 0 时滚动区本身就是个可聚焦控件 —— 以前键盘完全没有滚动入口
		// (只有"鼠标悬停 + 滚轮"),Tab 到不了、↑/↓ 也没人接。焦点在它上面时:
		// ↑/↓ = 40px、PageUp/PageDown = 0.9 屏、Space = 下一页(浏览器同款)、Home/End = 两端。
		const bool focused = id != 0 && ctx.Focus() == id;
		if (id != 0)
		{
			ctx.RegisterFocusable(id, viewport);
			if (focused)
			{
				const float page = std::max(40.0f, viewport.H * 0.9f);
				if (ctx.WasKeyPressed(KeyCodes::Down))
					scrollY += 40.0f;
				else if (ctx.WasKeyPressed(KeyCodes::Up))
					scrollY -= 40.0f;
				else if (ctx.WasKeyPressed(KeyCodes::PageDown) || ctx.WasKeyPressed(KeyCodes::Space))
					scrollY += page;
				else if (ctx.WasKeyPressed(KeyCodes::PageUp))
					scrollY -= page;
				else if (ctx.WasKeyPressed(KeyCodes::Home))
					scrollY = 0.0f;
				else if (ctx.WasKeyPressed(KeyCodes::End))
					scrollY = contentHeight;
			}
		}
		scrollY = std::max(0.0f, std::min(scrollY, std::max(0.0f, contentHeight - viewport.H)));
		ctx.Commands().push_back({ WuiDrawKind::ClipPush, viewport, theme.PanelBg });
		ctx.PushClipRect(viewport);
		if (id != 0)
		{
			// 节点:kind="scroll-area"、value="scroll=<y>/<max>"(键盘与滚轮都会改它,AI 读得到)。
			// 容器自己不是点击目标(点得到的是里面的行)→ interactive=false,不冒充按钮;
			// 聚焦/可见/焦点位照常暴露。
			char buffer[64] = {};
			std::snprintf(buffer, sizeof(buffer), "scroll=%.0f/%.0f", scrollY,
				std::max(0.0f, contentHeight - viewport.H));
			WuiAccessNode node;
			node.Id = id;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = "scroll-area";
			node.Value = buffer;
			node.Rect = viewport;
			node.Enabled = true;
			node.Interactive = false;
			node.Focused = focused;
			WuiAccessibility::Get().Register(node);
			DrawFocusRing(ctx, viewport, id, theme);
		}
		return true;
	}

	void EndScrollArea(WuiContext& ctx)
	{
		ctx.Commands().push_back({ WuiDrawKind::ClipPop });
		ctx.PopClipRect();
	}

	// P1c-LIB2:有状态滚动条 —— 位置读得出(value="scroll=<y>/<max> ratio=<0..1>")、也设得进
	// (拖滑块 / 点轨道 / 上下按钮 / 键盘)。几何与 BeginScrollArea 的滚动口径同源,滚轮不在这。
	bool ScrollBar(WuiContext& ctx, WuiId id, const WuiRect& rect, float contentHeight, float viewportHeight,
		float& scrollY, const WuiTheme& theme, bool pageButtons)
	{
		if (rect.W <= 0.0f || rect.H <= 0.0f || viewportHeight <= 0.0f)
			return false;
		const float maxScroll = std::max(0.0f, contentHeight - viewportHeight);
		const bool enabled = maxScroll > 0.001f;
		const float before = scrollY;
		scrollY = std::max(0.0f, std::min(scrollY, maxScroll));

		// 上下按钮只在放得下时画(≥ 3 个 24px 行高);否则整条 rect 都是轨道。
		const float buttonHeight = (pageButtons && rect.H >= 72.0f) ? 24.0f : 0.0f;
		const WuiRect track { rect.X, rect.Y + buttonHeight, rect.W,
			std::max(2.0f, rect.H - buttonHeight * 2.0f) };
		const float trackInner = track.H;
		const float thumbLength = enabled
			? std::min(trackInner, std::max(24.0f,
				viewportHeight * viewportHeight / std::max(viewportHeight, contentHeight)))
			: trackInner;
		const float travel = std::max(0.0f, trackInner - thumbLength);
		const float fraction = maxScroll > 0.0f ? std::max(0.0f, std::min(scrollY / maxScroll, 1.0f)) : 0.0f;
		const WuiRect thumb { track.X, track.Y + travel * fraction, track.W, thumbLength };
		const WuiRect upButton { rect.X, rect.Y, rect.W, buttonHeight };
		const WuiRect downButton { rect.X, rect.Y + rect.H - buttonHeight, rect.W, buttonHeight };
		const float page = std::max(40.0f, viewportHeight * 0.9f);

		const bool focused = ctx.Focus() == id;
		WuiScrollBarState& state = ctx.Persist<WuiScrollBarState>(id, {});
		const bool hovered = ctx.IsHovered(rect);
		const bool hoveredThumb = enabled && ctx.IsHovered(thumb);
		const glm::vec2 mouse = ctx.Input().MousePos;

		// 鼠标:按下滑块 = 抓拖;按下轨道空白 = 直接定位并继续拖(同一次按下不重复处理)。
		if (enabled && ctx.Input().MouseClicked[0] && hovered)
		{
			ctx.SetFocus(id);
			state.Dragging = true;
			if (hoveredThumb)
				state.GrabOffset = mouse.y - thumb.Y;
			else
			{
				state.GrabOffset = thumbLength * 0.5f;
				scrollY = travel > 0.0f
					? std::max(0.0f, std::min((mouse.y - track.Y - state.GrabOffset) / travel, 1.0f)) * maxScroll
					: 0.0f;
			}
		}
		if (state.Dragging)
		{
			if (ctx.Input().MouseReleased[0] || !ctx.Input().MouseDown[0])
				state.Dragging = false;
			else if (travel > 0.0f)
				scrollY = std::max(0.0f,
					std::min((mouse.y - track.Y - state.GrabOffset) / travel, 1.0f)) * maxScroll;
		}

		// 上下按钮:各翻一页;到底/到顶时按钮本身也弱化(节点不再可点)。
		if (enabled && buttonHeight > 0.0f)
		{
			if (scrollY > 0.001f && ctx.IsClicked(upButton))
				scrollY = std::max(0.0f, scrollY - page);
			if (scrollY < maxScroll - 0.001f && ctx.IsClicked(downButton))
				scrollY = std::min(maxScroll, scrollY + page);
		}

		// 键盘:与 BeginScrollArea 同键位(焦点在滚动条上时)。
		if (enabled && focused && !state.Dragging)
		{
			if (ctx.WasKeyPressed(KeyCodes::Down))
				scrollY += 40.0f;
			else if (ctx.WasKeyPressed(KeyCodes::Up))
				scrollY -= 40.0f;
			else if (ctx.WasKeyPressed(KeyCodes::PageDown) || ctx.WasKeyPressed(KeyCodes::Space))
				scrollY += page;
			else if (ctx.WasKeyPressed(KeyCodes::PageUp))
				scrollY -= page;
			else if (ctx.WasKeyPressed(KeyCodes::Home))
				scrollY = 0.0f;
			else if (ctx.WasKeyPressed(KeyCodes::End))
				scrollY = maxScroll;
		}
		scrollY = std::max(0.0f, std::min(scrollY, maxScroll));

		// 视觉:轨道 PanelHeader、按钮 ButtonBg/HoverBg、滑块 Accent(与面板手写的配色一致)。
		const auto fill = [&ctx](const WuiRect& area, const WuiColor& color)
		{
			ctx.Commands().push_back({ WuiDrawKind::Rect, area, color, 2.0f });
		};
		fill(track, theme.PanelHeader);
		if (buttonHeight > 0.0f)
		{
			const bool upActive = enabled && scrollY > 0.001f;
			const bool downActive = enabled && scrollY < maxScroll - 0.001f;
			fill(upButton, ctx.IsHovered(upButton) ? theme.ButtonHover : theme.ButtonBg);
			fill(downButton, ctx.IsHovered(downButton) ? theme.ButtonHover : theme.ButtonBg);
			ctx.Commands().push_back({ WuiDrawKind::Text, { upButton.X + 1.0f, upButton.Y + 4.0f, 0, 0 },
				upActive ? theme.Text : theme.TextDisabled, 0, 1.0f, "^", 13.0f, false });
			ctx.Commands().push_back({ WuiDrawKind::Text, { downButton.X + 1.0f, downButton.Y + 4.0f, 0, 0 },
				downActive ? theme.Text : theme.TextDisabled, 0, 1.0f, "v", 13.0f, false });
		}
		fill(thumb, enabled ? (hoveredThumb || state.Dragging ? theme.Accent : theme.BorderStrong) : theme.Border);
		DrawFocusRing(ctx, rect, id, theme);

		// 节点:值格式与 BeginScrollArea 的 "scroll=<y>/<max>" 前缀同口径,附加 ratio 便于脚本直接读比例。
		if (id != 0)
		{
			const float ratio = maxScroll > 0.0f ? scrollY / maxScroll : 0.0f;
			char buffer[64] = {};
			std::snprintf(buffer, sizeof(buffer), "scroll=%.0f/%.0f ratio=%.2f", scrollY, maxScroll, ratio);
			WuiAccessNode node;
			node.Id = id;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = "scrollbar";
			node.Value = buffer;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = enabled;
			node.Focused = focused;
			WuiAccessibility::Get().Register(node);
		}
		if (enabled)
			ctx.RegisterFocusable(id, rect);
		return scrollY != before;
	}

	WuiRect TableCell(const WuiRect& table, const std::vector<float>& columns, size_t row, size_t column, float rowHeight)
	{
		float x = table.X;
		for (size_t i = 0; i < column && i < columns.size(); ++i)
			x += columns[i];
		const float width = column < columns.size() ? columns[column] : table.W;
		return { x, table.Y + rowHeight * static_cast<float>(row), width, rowHeight };
	}

	// ---- P4-UX7 / U2B:表头排序 / 颜色字段 / 分隔条 ----
	namespace
	{
		// 颜色字段:hex 编辑缓冲(只在弹层内使用)+ 首帧初始化标记(首帧用当前色填缓冲)。
		struct WuiColorFieldState
		{
			std::string Hex;
			bool Initialized = false;
			// P4-U10:HSV 是取色器(色板/色相条)的编辑状态。拖动时以 HSV 为准,
			// 外部改动(hex/滑杆/预设)导致 RGB(HSV) 与当前 rgba 不一致时重新同步。
			float H = 0.0f;
			float S = 0.0f;
			float V = 1.0f;
			bool HsvValid = false;
			// MAT-UI3a:拖动跟随的归属(0 = 无;1 = SV 板;2 = 色相条;3 = alpha 条)。
			// 按下那一刻定归属,松手才清 —— 期间指针移到哪儿都继续按当前坐标夹取更新。
			int DragTarget = 0;
		};

		// 分隔条:拖动锚点(按下那一帧的值与轴向坐标)。拖动期间按"锚点 + 轴向位移"累加,
		// 所以鼠标离开 6px 命中带(调用方每帧按新 value 重新摆放带)也不会中断拖动。
		struct WuiSplitterState
		{
			bool Dragging = false;
			float PressValue = 0.0f;
			float PressAxis = 0.0f;
		};

		// 颜色字段几何(派工确认的尺寸,设计单位):
		constexpr float kColorSwatchWidth = 6.0f;      // 折叠态左侧色块宽
		constexpr float kColorCheckerSize = 4.0f;      // 棋盘格方块边长
		constexpr float kColorSwatchInset = 3.0f;      // 色块相对字段上下内缩
		constexpr float kColorPopupWidth = 236.0f;     // 弹层宽
		// 弹层高(P4-U10):8 + 色板 110 + 10 + 色相条 12 + 6 + alpha 条 12 + 10 + hex 22
		// + 4×22 通道滑杆 + 12 + 预设 2×18 + 8 ≈ 330。
		constexpr float kColorPopupHeight = 348.0f;
		constexpr float kColorSvHeight = 110.0f;       // 饱和度×明度 色板高
		constexpr float kColorBarHeight = 12.0f;       // 色相条 / alpha 条高
		constexpr float kColorPresetCell = 18.0f;      // 预设色块边长
		constexpr int kColorPresetColumns = 10;
		constexpr int kColorPresetRows = 2;
		constexpr float kColorHexRowHeight = 22.0f;    // 弹层顶部 hex 输入行高
		constexpr float kColorChannelRowHeight = 22.0f;// R/G/B/A 每行高
		constexpr float kColorChannelLabelWidth = 14.0f;
		constexpr float kColorChannelValueWidth = 40.0f;

		// 预设调色板:第一行 = 灰阶/基础色,第二行 = 常用鲜艳色(与 Unity/Blender 的
		// 快速取色同一档位);值就是 sRGB 的 0..1。
		const glm::vec3 kColorPresets[kColorPresetColumns * kColorPresetRows] = {
			{ 0.00f, 0.00f, 0.00f }, { 1.00f, 1.00f, 1.00f }, { 0.50f, 0.50f, 0.50f }, { 0.25f, 0.25f, 0.25f },
			{ 0.75f, 0.75f, 0.75f }, { 1.00f, 0.27f, 0.23f }, { 1.00f, 0.62f, 0.04f }, { 1.00f, 0.84f, 0.25f },
			{ 0.25f, 0.86f, 0.35f }, { 0.29f, 0.62f, 1.00f },
			{ 0.00f, 0.42f, 0.72f }, { 0.20f, 0.80f, 0.85f }, { 0.55f, 0.35f, 0.95f }, { 0.95f, 0.35f, 0.65f },
			{ 0.55f, 0.27f, 0.07f }, { 0.55f, 0.55f, 0.30f }, { 0.30f, 0.45f, 0.35f }, { 0.95f, 0.95f, 0.90f },
			{ 0.10f, 0.05f, 0.15f }, { 0.65f, 0.15f, 0.15f },
		};

		// RGB(0..1) ↔ HSV(色相 0..360,饱和/明度 0..1)。
		glm::vec3 ColorToHsv(const glm::vec3& rgb)
		{
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

		glm::vec3 HsvToColor(const glm::vec3& hsv)
		{
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

		WuiColor ToWuiColor(const glm::vec3& rgb, float alpha = 1.0f)
		{
			return { std::clamp(rgb.x, 0.0f, 1.0f), std::clamp(rgb.y, 0.0f, 1.0f),
				std::clamp(rgb.z, 0.0f, 1.0f), std::clamp(alpha, 0.0f, 1.0f) };
		}

		// 分隔条:命中带宽与线宽(6px 命中带、视觉恒 1px;P4-U29 去掉了 hover/drag 加粗)。
		constexpr float kSplitterHitWidth = 6.0f;
		constexpr float kSplitterLineWidth = 1.0f;

		// 表头:列名右侧给排序箭头保留的宽度(派工确认 14px)。
		constexpr float kTableSortArrowReserve = 14.0f;

		// 当前色的规范 hex 文本:6 位 = 不透明,带 alpha(未满)时 8 位,与解析规则对称。
		std::string FormatColorHex(const glm::vec4& rgba)
		{
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
		bool ParseColorHex(std::string_view text, glm::vec4& out)
		{
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

		bool SameColor(const glm::vec4& a, const glm::vec4& b)
		{
			return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
		}
	}

	bool TableHeader(WuiContext& ctx, WuiId id, const WuiRect& table, const std::vector<std::string>& columns,
		const std::vector<float>& columnWidths, int& sortColumn, bool& ascending, const WuiTheme& theme)
	{
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

	bool ColorField(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec4& rgba, const WuiTheme& theme)
	{
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

	bool Splitter(WuiContext& ctx, WuiId id, const WuiRect& rect, bool vertical, float& value,
		float minValue, float maxValue, const WuiTheme& theme)
	{
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

	// ---- P4-UX12 / U2C / MAT-UI3a:向量字段(2/3/4 分量共用同一份实现) / 空状态 ----
	namespace
	{
		// 向量字段的持久化状态:同一时刻只可能有一个分量处于"按下/拖动/编辑",所以所有分量共用一份
		// 状态,用 Axis 记住是哪一个(与 DragFloat 的 WuiNumericState 同族,但**不能**共用 id ——
		// Persist 用同一 id 换类型会按错误类型解释内存)。MAT-UI3a 起 2/3/4 分量共用本结构:
		// 分量的上限由调用参数 count 决定,越界的 Axis 在 Core 里被夹回 [0,count)。
		struct WuiVecFieldState
		{
			bool Pressed = false;
			bool Dragging = false;
			bool Editing = false;
			int Axis = 0;                // 当前轴:按下/拖动/编辑/键盘微调作用的分量
			float PressX = 0.0f;         // 拖动锚点:按下瞬间的光标 x
			float PressValue = 0.0f;     // 拖动锚点:按下瞬间该分量的值
			std::string Buffer;          // 文本编辑缓冲(只在 Editing 期间有效)
			int Cursor = -1;
			int SelStart = -1;
			int SelEnd = -1;
		};

		// 分量左侧的轴标签列宽(派工确认 12px):属于控件几何,不进主题令牌(与 U2B 的
		// kColorSwatchWidth / kSplitterHitWidth 同一处理)。
		constexpr float kVecAxisLabelWidth = 12.0f;
		// 空状态左右安全边距与 action 按钮的内边距(派工确认 24px)。
		constexpr float kEmptyStateSideMargin = 24.0f;
		constexpr float kEmptyStateButtonPad = 24.0f;

		// 分量数值文本:两位小数(派工确认)。显示与编辑缓冲共用同一套文本,
		// 避免"看到的数"与"点进去的数"不一致(DragFloat 用同一策略,只是三位小数)。
		std::string VecAxisText(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.2f", value);
			return buffer;
		}

		// 整体无障碍节点的 value:"x,y[,z[,w]]"(分量顺序固定,脚本可直接按 ',' 切分)。
		std::string VecFieldText(const float* value, int count)
		{
			std::string text;
			for (int axis = 0; axis < count; ++axis)
				text += (axis == 0 ? "" : ",") + VecAxisText(value[axis]);
			return text;
		}

		// 标签一律大写单字母(与 Vec3Field 既有外观一致:11px Caption 下大写更清楚)。
		const char* const kVec2AxisLabels[2] = { "X", "Y" };
		const char* const kVec3AxisLabels[3] = { "X", "Y", "Z" };
		const char* const kVec4AxisLabels[4] = { "X", "Y", "Z", "W" };
	}

	// 向量字段唯一实现:count = 2/3/4 的分量数,columns = 横排(layout 0)时的每行列数。
	// count==3 / columns==3 / labels=X,Y,Z 时输出与 P4-UX12 的 Vec3Field **逐命令相同**
	// (槽宽公式、(slotW+gap)*axis 的排布、子节点 id ".axis."、kind 前缀都由同一份代码给出)。
	bool VecFieldCore(WuiContext& ctx, WuiId id, const WuiRect& rect, float* value, int count,
		int columns, const char* roleKind, const char* axisKind, const char* const* axisLabels,
		float speed, float minValue, float maxValue, const WuiTheme& theme, int layout)
	{
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
					const float next = std::max(lo, std::min(hi, static_cast<float>(state.PressValue + dx * speed)));
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
			const float delta = (ctx.WasKeyPressed(KeyCodes::Up) ? keyboardStep : 0.0f)
				+ (ctx.WasKeyPressed(KeyCodes::Down) ? -keyboardStep : 0.0f);
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
	bool Vec2Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec2& value, float speed,
		float minValue, float maxValue, const WuiTheme& theme, int layout)
	{
		return VecFieldCore(ctx, id, rect, &value[0], 2, 2, "vec2-field", "vec2-axis",
			kVec2AxisLabels, speed, minValue, maxValue, theme, layout);
	}

	bool Vec3Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec3& value, float speed,
		float minValue, float maxValue, const WuiTheme& theme, int layout)
	{
		return VecFieldCore(ctx, id, rect, &value[0], 3, 3, "vec3-field", "vec3-axis",
			kVec3AxisLabels, speed, minValue, maxValue, theme, layout);
	}

	// Vec4 的默认排布 = 2×2(columns = 2):四段并排会把每个输入框压到 45px 以下
	//(标签固定 12px + 两位小数),2×2 在材质参数行/属性面板的 ~200-300 宽控件列里
	// 给每个分量约 100-145px,读数与拖动手感与 Vec3Field 一致;窄列用 layout=1 四行。
	bool Vec4Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec4& value, float speed,
		float minValue, float maxValue, const WuiTheme& theme, int layout)
	{
		return VecFieldCore(ctx, id, rect, &value[0], 4, 2, "vec4-field", "vec4-axis",
			kVec4AxisLabels, speed, minValue, maxValue, theme, layout);
	}

	bool EmptyState(WuiContext& ctx, const WuiRect& rect, const std::string& glyph, const std::string& title,
		const std::string& hint, const std::string& actionLabel, WuiId actionId, const WuiTheme& theme)
	{
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
