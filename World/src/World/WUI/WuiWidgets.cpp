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
	}

	void Tooltip(WuiContext& ctx, const WuiRect& hoverRect, const std::string& text)
	{
		if (text.empty() || !ctx.IsHovered(hoverRect))
			return;
		ctx.SetTooltip(text);
	}

	void DrawTooltip(WuiContext& ctx, const WuiTheme& theme)
	{
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

	void DrawFocusRing(WuiContext& ctx, const WuiRect& rect, WuiId id, const WuiTheme& theme)
	{
		if (id == 0 || ctx.Focus() != id)
			return;
		// 滚出滚动区视口的控件不画环(overlay 不受 ClipPush 影响,必须自己问裁剪栈)。
		if (!ctx.ClipAllows(rect))
			return;
		// 走 overlay 命令层:焦点环在全部普通控件之后绘制,后画的兄弟控件不会盖住它
		// (与 tooltip 同一套机制,不新增层级)。1.5px 描边、圆角取主题令牌。
		ctx.PushOverlay();
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.FocusRing, theme.Radius, 1.5f });
		ctx.PopOverlay();
	}

	bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme)
	{
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "button", rect, label, std::string(), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const bool hovered = ctx.IsHovered(rect);
		const bool pressed = ctx.Input().MouseDown[0] && hovered;
		// U2A 键盘激活:焦点在按钮上时 Enter/Space = 点击一次(KeyPressed 只含本帧新按下,长按不连发)。
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		const WuiColor fill = hovered ? theme.ButtonHover : theme.ButtonBg;
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, fill, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, pressed ? theme.Accent : theme.Text, 0, 1.0f, label, 15.0f, false });
		DrawFocusRing(ctx, rect, id, theme);
		return (hovered && ctx.Input().MouseClicked[0]) || keyActivated;
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
		// 整条底边线:让标签条与下方内容有分界(取面板边框色,不新增样式常量)。
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, rect.Y + rect.H - 1.0f, rect.W, 1.0f }, theme.Border, 0.0f });
		for (size_t i = 0; i < tabs.size(); ++i)
		{
			const WuiRect tab { rect.X + tabW * static_cast<float>(i), rect.Y, tabW, rect.H };
			const WuiId tabId = DerivedChildId(id, ".tab.", i);
			const bool isActive = static_cast<int>(i) == active;
			const bool hovered = ctx.IsHovered(tab);
			RegisterAccessNode(tabId, "tab", tab, tabs[i], isActive ? "true" : "false");
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
		// 组节点:组本身不是可点控件(interactive=false),脚本按 kind="segmented-option" 点具体分段。
		RegisterAccessNode(id, "segmented", rect, std::string(), options[static_cast<size_t>(selected)], true, false);
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
			RegisterAccessNode(optionId, "segmented-option", option, options[i], isSelected ? "true" : "false");
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
			// P4-UX14:Ctrl+A 全选(用户实测"重命名时 Ctrl+A 失效")。放在插入之前,
			// 与后续的字符插入/光标移动互不干扰。
			if (ctx.Input().Ctrl && ctx.WasKeyPressed(KeyCodes::A))
			{
				selStart = 0;
				selEnd = Utf8Count(buffer);
				cursor = selEnd;
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
	}

	bool DragFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float speed, float min, float max, const WuiTheme& theme)
	{
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "drag-float", rect, std::string(), FloatToText(value), true, true, focused);
		ctx.RegisterFocusable(id, rect);
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

		// U2A 键盘微调:焦点在字段上、且不在文本编辑态时,左右箭头 = 现有 drag 步长(±speed)。
		// 编辑态下左右箭头归文本光标(EditUpdate 已消费),这里不抢。
		if (focused && !state.Editing)
		{
			const float step = std::fabs(speed) > 0.0f ? std::fabs(speed) : 1.0f;
			if (ctx.WasKeyPressed(KeyCodes::Left))
			{
				value = std::max(lo, value - step);
				changed = true;
			}
			if (ctx.WasKeyPressed(KeyCodes::Right))
			{
				value = std::min(hi, value + step);
				changed = true;
			}
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
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
	}

	bool DragInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme)
	{
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "drag-int", rect, std::string(), std::to_string(value), true, true, focused);
		ctx.RegisterFocusable(id, rect);
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

		// U2A 键盘微调:焦点在字段上、且不在文本编辑态时,左右箭头 ±1(与拖拽的 1 单位/像素同量级)。
		if (focused && !state.Editing)
		{
			if (ctx.WasKeyPressed(KeyCodes::Left))
			{
				value = std::max(lo, value - 1);
				changed = true;
			}
			if (ctx.WasKeyPressed(KeyCodes::Right))
			{
				value = std::min(hi, value + 1);
				changed = true;
			}
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
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
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
			ctx.PushOverlay();
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
			{
				ctx.PushHoverBlocker(panel);
				// P4-U7:同时登记为覆盖层矩形 → 下一帧它只挡**非覆盖层**控件,
				// 先画的面板(或本面板更早绘制的行)也不会吃掉落在弹层上的点击。
				ctx.RegisterOverlayRect(panel);
			}
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
			// 兼容既有端到端脚本:tools/codex/skills/worldengine-dev/scripts/verify-ai-control.py 按
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
			if (ctx.IsClicked(item))
			{
				if (std::getenv("WLD_TRACE_UI"))
					WLD_CORE_INFO("[ui] search-combo row clicked: id={0} option={1} label='{2}'",
						id, optionIndex, options[optionIndex]);
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
		{
			ctx.PushHoverBlocker(panel);
			// P4-U7:同时登记为覆盖层矩形 → 下一帧只挡非覆盖层控件。
			ctx.RegisterOverlayRect(panel);
		}
		ctx.PopOverlay();
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
		return enabled && (ctx.IsClicked(rect)
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
		return enabled && (ctx.IsClicked(rect)
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

	bool BeginScrollArea(WuiContext& ctx, const WuiRect& viewport, float contentHeight, float& scrollY, const WuiTheme& theme)
	{
		if (ctx.IsHovered(viewport))
			scrollY -= ctx.Input().Wheel * 40.0f;
		scrollY = std::max(0.0f, std::min(scrollY, std::max(0.0f, contentHeight - viewport.H)));
		ctx.Commands().push_back({ WuiDrawKind::ClipPush, viewport, theme.PanelBg });
		ctx.PushClipRect(viewport);
		return true;
	}

	void EndScrollArea(WuiContext& ctx)
	{
		ctx.Commands().push_back({ WuiDrawKind::ClipPop });
		ctx.PopClipRect();
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
		constexpr float kColorPopupWidth = 220.0f;     // 弹层宽
		constexpr float kColorPopupHeight = 132.0f;    // 弹层高:8 + hex 22 + 14 + 4×22 = 132
		constexpr float kColorHexRowHeight = 22.0f;    // 弹层顶部 hex 输入行高
		constexpr float kColorChannelRowHeight = 22.0f;// R/G/B/A 每行高
		constexpr float kColorChannelLabelWidth = 14.0f;
		constexpr float kColorChannelValueWidth = 40.0f;

		// 分隔条:命中带宽与线宽(派工确认:6px 命中带、视觉 1px、悬停加粗)。
		constexpr float kSplitterHitWidth = 6.0f;
		constexpr float kSplitterLineWidth = 1.0f;
		constexpr float kSplitterActiveWidth = 3.0f;

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
			return false;

		// ---- 弹层(与 Combo 同一套 ctx.OpenPopup/ClosePopup/IsPopupOpen,不自造)----
		ctx.PushOverlay();
		WuiRect panel { rect.X, rect.Y + rect.H + 2.0f, kColorPopupWidth, kColorPopupHeight };
		// 下方空间不够时向上展开(与 SearchableCombo 同一判据:用 UI 视口高度判断)。
		if (panel.Y + panel.H > ctx.Input().ViewportSize.y)
			panel.Y = std::max(4.0f, rect.Y - panel.H - 2.0f);
		DrawPanelSurface(ctx, panel, theme);

		const WuiRect hexRect { panel.X + theme.Pad, panel.Y + theme.Pad,
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

		// 视觉:默认 1px 线(theme.Border);悬停 3px(theme.BorderStrong);拖动中 3px(theme.Accent)。
		const float thickness = (hovered || state.Dragging) ? kSplitterActiveWidth : kSplitterLineWidth;
		const WuiColor color = state.Dragging ? theme.Accent : (hovered ? theme.BorderStrong : theme.Border);
		const WuiRect line = vertical
			? WuiRect { centerX - thickness * 0.5f, rect.Y, thickness, rect.H }
			: WuiRect { rect.X, centerY - thickness * 0.5f, rect.W, thickness };
		ctx.Commands().push_back({ WuiDrawKind::Rect, line, color, 0.0f });
		if (hovered || state.Dragging)
			ctx.SetCursor(vertical ? WuiCursor::ResizeEW : WuiCursor::ResizeNS);
		DrawFocusRing(ctx, band, id, theme);
		return changed;
	}

	// ---- P4-UX12 / U2C:向量字段 / 空状态 ----
	namespace
	{
		// 向量字段的持久化状态:同一时刻只可能有一个分量处于"按下/拖动/编辑",所以三个分量共用一份
		// 状态,用 Axis 记住是哪一个(与 DragFloat 的 WuiNumericState 同族,但**不能**共用 id ——
		// Persist 用同一 id 换类型会按错误类型解释内存)。
		struct WuiVec3FieldState
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
		constexpr float kVec3AxisLabelWidth = 12.0f;
		// 空状态左右安全边距与 action 按钮的内边距(派工确认 24px)。
		constexpr float kEmptyStateSideMargin = 24.0f;
		constexpr float kEmptyStateButtonPad = 24.0f;

		// 分量数值文本:两位小数(派工确认)。显示与编辑缓冲共用同一套文本,
		// 避免"看到的数"与"点进去的数"不一致(DragFloat 用同一策略,只是三位小数)。
		std::string Vec3AxisText(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.2f", value);
			return buffer;
		}

		// 整体无障碍节点的 value:"x,y,z"(分量顺序固定,脚本可直接按 ',' 切分)。
		std::string Vec3Text(const glm::vec3& value)
		{
			return Vec3AxisText(value.x) + "," + Vec3AxisText(value.y) + "," + Vec3AxisText(value.z);
		}
	}

	bool Vec3Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec3& value, float speed,
		float minValue, float maxValue, const WuiTheme& theme, int layout)
	{
		const bool focused = ctx.Focus() == id;
		WuiVec3FieldState& state = ctx.Persist<WuiVec3FieldState>(id, {});
		if (state.Axis < 0 || state.Axis > 2)
			state.Axis = 0;
		// min >= max = 无界(与 DragFloat 的哨兵逐条一致:既有调用点的 (-1, 1) 写法不必改)。
		const float lo = minValue < maxValue ? minValue : -1e30f;
		const float hi = minValue < maxValue ? maxValue : 1e30f;
		// 键盘步长取 speed 的绝对值(与 DragFloat 一致;speed 传 0 时退化为 1)。
		const float keyboardStep = std::fabs(speed) > 0.0f ? std::fabs(speed) : 1.0f;
		const bool vertical = layout == 1;
		RegisterAccessNode(id, "vec3-field", rect, std::string(), Vec3Text(value), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		bool changed = false;

		// 每个分量的槽 = 轴标签(12px)+ 输入框;横排三等分(分量之间留 PadSmall),竖排三行等分高度。
		const float axisGap = vertical ? 0.0f : theme.PadSmall;
		const auto slotRect = [&](int axis) -> WuiRect
		{
			if (vertical)
			{
				const float rowH = rect.H / 3.0f;
				return { rect.X, rect.Y + rowH * static_cast<float>(axis), rect.W, rowH };
			}
			const float slotW = std::max(0.0f, (rect.W - axisGap * 2.0f) / 3.0f);
			return { rect.X + (slotW + axisGap) * static_cast<float>(axis), rect.Y, slotW, rect.H };
		};
		const auto fieldRect = [&](const WuiRect& slot) -> WuiRect
		{
			const float labelW = std::min(kVec3AxisLabelWidth, slot.W);
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
				for (int axis = 0; axis < 3; ++axis)
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
						state.Buffer = Vec3AxisText(value[state.Axis]);
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

		static const char* const kAxisLabels[3] = { "X", "Y", "Z" };
		const float textSize = theme.FontSizeBody;
		for (int axis = 0; axis < 3; ++axis)
		{
			const WuiRect slot = slotRect(axis);
			const WuiRect field = fieldRect(slot);
			const bool axisCurrent = state.Axis == axis;
			const bool axisDragging = state.Dragging && axisCurrent;
			const bool axisEditing = state.Editing && axisCurrent;
			const bool hovered = ctx.IsHovered(slot);
			// 子节点:交互可点(脚本注入坐标 = 鼠标点该分量),但不是独立焦点项(焦点始终在整体上)。
			RegisterAccessNode(DerivedChildId(id, ".axis.", static_cast<size_t>(axis)), "vec3-axis", slot,
				kAxisLabels[axis], Vec3AxisText(value[axis]));
			if (hovered)
				ctx.SetCursor(axisEditing ? WuiCursor::IBeam : WuiCursor::ResizeEW);
			// 轴标签:拖动中的分量用强调色高亮,其余 = 次要色 + Caption 字号。
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ slot.X, slot.Y + (slot.H - theme.FontSizeCaption) * 0.5f, 0, 0 },
				axisDragging ? theme.Accent : theme.TextMuted, 0, 1.0f, kAxisLabels[axis],
				theme.FontSizeCaption, false });
			ctx.Commands().push_back({ WuiDrawKind::Rect, field,
				axisEditing ? theme.ButtonHover : theme.ButtonBg, theme.Radius });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, field,
				(axisEditing || hovered || axisDragging) ? theme.Accent : theme.Border, theme.Radius, 1.0f });
			const std::string text = axisEditing ? state.Buffer : Vec3AxisText(value[axis]);
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
