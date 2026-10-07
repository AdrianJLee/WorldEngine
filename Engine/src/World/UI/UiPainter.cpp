#include "wldpch.h"
#include "World/UI/UiPainter.h"

#include "World/Core/Log.h"
#include "World/WUI/WuiLocalization.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace World::UI
{
	namespace
	{
		using Wui::WuiColor;
		using Wui::WuiRect;

		// ---- 属性读取(值编码协议:`UiDocument.h` / `WuiComponentRegistry.h`)----

		const std::string* FindProp(const UiNodeInstance& node, std::string_view name)
		{
			if (node.Source == nullptr)
				return nullptr;
			const UiProp* prop = node.Source->FindProp(name);
			return prop != nullptr ? &prop->Value : nullptr;
		}

		float NumberProp(const UiNodeInstance& node, std::string_view name, float fallback)
		{
			const std::string* value = FindProp(node, name);
			if (value == nullptr || value->empty())
				return fallback;
			char* end = nullptr;
			const float parsed = std::strtof(value->c_str(), &end);
			return end != value->c_str() ? parsed : fallback;
		}

		// ---- M14:主题令牌(解析在本层做,引擎内唯一一处)----
		//
		// 解析后取值:`ResolvedProp` 返回的指针要么指向节点原属性(非令牌,零拷贝),要么指向
		// `scratch`(令牌解析结果)。令牌未定义 / 环 / 引用非法 ⇒ 记一条可读错误 + 返回 nullptr,
		// 调用方走自己的 fallback(即按"未设置"处理,不会静默变成空串/0)。
		const std::string* ResolvedProp(const UiNodePaintContext& ctx, std::string_view name,
			std::string& scratch)
		{
			// M26:运行态覆盖优先 —— 绑定求值结果(键 = 节点稳定 Id + 属性名)先于文档属性;
			// 覆盖值仍走**本层同一份**令牌解析(与文档属性同口径,`$token` 不泄漏给画面)。
			const std::string* raw = ctx.Options.Overrides != nullptr
				? ctx.Options.Overrides->Find(ctx.Node.Id, name) : nullptr;
			if (raw == nullptr)
				raw = FindProp(ctx.Node, name);
			if (raw == nullptr || raw->empty() || !IsUiTokenRef(*raw))
				return raw;
			if (ctx.ResolveTokenValue(name, *raw, scratch))
				return &scratch;
			return nullptr;
		}

		std::string TokenText(const UiNodePaintContext& ctx, std::string_view name,
			std::string fallback = std::string())
		{
			std::string scratch;
			const std::string* value = ResolvedProp(ctx, name, scratch);
			return value != nullptr ? *value : std::move(fallback);
		}

		bool TokenBool(const UiNodePaintContext& ctx, std::string_view name, bool fallback)
		{
			std::string scratch;
			const std::string* value = ResolvedProp(ctx, name, scratch);
			if (value == nullptr || value->empty())
				return fallback;
			return *value == "1" || *value == "true" || *value == "True" || *value == "yes" || *value == "on";
		}

		float TokenNumber(const UiNodePaintContext& ctx, std::string_view name, float fallback)
		{
			std::string scratch;
			const std::string* value = ResolvedProp(ctx, name, scratch);
			if (value == nullptr || value->empty())
				return fallback;
			char* end = nullptr;
			const float parsed = std::strtof(value->c_str(), &end);
			return end != value->c_str() ? parsed : fallback;
		}

		WuiColor TokenColor(const UiNodePaintContext& ctx, std::string_view name, const WuiColor& fallback)
		{
			std::string scratch;
			const std::string* value = ResolvedProp(ctx, name, scratch);
			WuiColor parsed = fallback;
			if (value != nullptr && Wui::ParseComponentColor(*value, parsed))
				return parsed;
			return fallback;
		}

		// 显示文本的两种写法:
		//   `@key`          —— 本地化 key;缺 key 时回退**去掉 `@` 的字面量**(可见 + 已记缺键日志)。
		//   `@key|兜底文案` —— key + **内联兜底**(M33)。缺 key 时显示兜底文案,而不是裸键名。
		//
		// 为什么要内联兜底:引擎自带文案的英文是源码内联的(`Tr(key, "English")`),但 `.wui` 里
		// 没有"源码",以前只能写 key ⇒ 任何语言缺这个 key 就显示裸键名(实测用户可见)。
		// 现在作者可以写 `@ui.close|Close`:有译文用译文,没译文/缺 key 也有可读英文。
		std::string Localized(std::string_view text)
		{
			if (!text.empty() && text.front() == '@')
			{
				const std::size_t separator = text.find('|');
				if (separator == std::string_view::npos)
				{
					const std::string key(text.substr(1));
					return Wui::Tr(key, key);
				}
				const std::string key(text.substr(1, separator - 1));
				const std::string fallback(text.substr(separator + 1));
				// 空 key 或空兜底 = 写法非法:按"没有本地化"处理(原样显示,便于一眼看出写错)。
				if (key.empty() || fallback.empty())
					return std::string(text);
				return Wui::Tr(key, fallback);
			}
			return std::string(text);
		}

		// 设计单位属性 → 物理长度(乘视口缩放);未给 = 设计默认值。
		float ScaledProp(const UiNodePaintContext& ctx, std::string_view name, float designFallback)
		{
			return TokenNumber(ctx, name, designFallback) * ctx.Scale;
		}

		// ---- 命令发射(WuiContext 是唯一出口)----

		void AddRect(const UiNodePaintContext& ctx, const WuiRect& rect, const WuiColor& color, float radius)
		{
			Wui::WuiDrawCommand command;
			command.Kind = Wui::WuiDrawKind::Rect;
			command.Rect = rect;
			command.Color = color;
			command.Rounding = radius;
			ctx.Context.Commands().push_back(std::move(command));
		}

		void AddRectOutline(const UiNodePaintContext& ctx, const WuiRect& rect, const WuiColor& color,
			float radius, float thickness)
		{
			Wui::WuiDrawCommand command;
			command.Kind = Wui::WuiDrawKind::RectOutline;
			command.Rect = rect;
			command.Color = color;
			command.Rounding = radius;
			command.Thickness = thickness;
			ctx.Context.Commands().push_back(std::move(command));
		}

		void AddText(const UiNodePaintContext& ctx, float x, float y, const std::string& text,
			const WuiColor& color, float fontSize, bool bold)
		{
			if (text.empty())
				return;
			Wui::WuiDrawCommand command;
			command.Kind = Wui::WuiDrawKind::Text;
			command.Rect = WuiRect { x, y, 0, 0 };
			command.Color = color;
			command.Text = text;
			command.FontSize = fontSize;
			command.Bold = bold;
			command.Family = Wui::WuiFontFamily::Ui;
			ctx.Context.Commands().push_back(std::move(command));
		}

		// 稳定节点路径(如 "root.hp");与 `UiValidationIssue::Path` 同一 "父.子" 口径。
		std::string NodePath(const UiScreen& screen, const UiNodeInstance& node)
		{
			std::vector<std::string_view> ids;
			const UiNodeInstance* current = &node;
			while (current != nullptr)
			{
				ids.push_back(current->Id);
				current = current->Parent >= 0
					? &screen.Nodes()[static_cast<std::size_t>(current->Parent)] : nullptr;
			}
			std::string path;
			for (auto it = ids.rbegin(); it != ids.rend(); ++it)
			{
				if (!path.empty())
					path += '.';
				path += *it;
			}
			return path;
		}

		// ---- 内置类型绘制(每件都只发 WuiContext 命令)----

		void PaintPanel(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const float radius = ScaledProp(ctx, "radius", theme.Radius);
			AddRect(ctx, ctx.Rect, TokenColor(ctx, "bg", theme.PanelBg), radius);
			AddRectOutline(ctx, ctx.Rect, TokenColor(ctx, "border", theme.Border), radius, 1.0f);

			const std::string title = Localized(TokenText(ctx, "title"));
			if (!title.empty())
			{
				AddText(ctx, ctx.Rect.X + theme.Pad * ctx.Scale, ctx.Rect.Y + 3.0f * ctx.Scale,
					title, theme.Text, theme.FontSizeTitle * ctx.Scale, false);
			}
			ctx.Label = title;
		}

		void PaintLabel(UiNodePaintContext& ctx)
		{
			const std::string text = Localized(TokenText(ctx, "text", TokenText(ctx, "label")));
			const float font = ScaledProp(ctx, "fontSize", 15.0f);
			const float y = ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - font) * 0.5f);
			AddText(ctx, ctx.Rect.X, y, text, TokenColor(ctx, "color", ctx.Theme.Text), font,
				TokenBool(ctx, "bold", false));
			ctx.Label = text;
			ctx.Value = text;
		}

		void PaintButton(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = TokenBool(ctx, "disabled", false);
			// 未给覆盖 = 主题令牌;`bg`/`bg.default` 是覆盖值(text 编码 #RRGGBB[AA])。
			const WuiColor bg = disabled ? theme.ContentBg
				: TokenColor(ctx, "bg", TokenColor(ctx, "bg.default", theme.ButtonBg));
			const WuiColor border = TokenColor(ctx, "border",
				TokenColor(ctx, "border.default", theme.Border));
			const WuiColor textColor = disabled ? theme.TextDisabled
				: TokenColor(ctx, "text", TokenColor(ctx, "text.default", theme.Text));
			const float radius = ScaledProp(ctx, "radius", theme.Radius);
			AddRect(ctx, ctx.Rect, bg, radius);
			AddRectOutline(ctx, ctx.Rect, border, radius, 1.0f);

			const std::string label = Localized(TokenText(ctx, "label"));
			const float font = ScaledProp(ctx, "fontSize", 15.0f);
			const float padding = ScaledProp(ctx, "padding", 8.0f);
			const float width = ctx.Context.MeasureTextWidth(label, font, Wui::WuiFontFamily::Ui);
			const float x = ctx.Rect.X + std::max(padding, (ctx.Rect.W - width) * 0.5f);
			const float y = ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - font) * 0.5f);
			AddText(ctx, x, y, label, textColor, font, TokenBool(ctx, "bold", false));

			ctx.Label = label;
			ctx.Value = disabled ? std::string("disabled") : std::string();
			ctx.Disabled = disabled;
			if (disabled)
				ctx.States = "disabled";
		}

		void PaintImage(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const float textureId = TokenNumber(ctx, "textureId", 0.0f);
			const float radius = ScaledProp(ctx, "radius", theme.Radius);
			if (textureId > 0.0f)
			{
				Wui::WuiDrawCommand command;
				command.Kind = Wui::WuiDrawKind::Image;
				command.Rect = ctx.Rect;
				command.Color = TokenColor(ctx, "tint", WuiColor { 1, 1, 1, 1 });
				command.Image = static_cast<uint64_t>(textureId);
				command.Uv = WuiRect { 0, 0, 1, 1 };
				ctx.Context.Commands().push_back(std::move(command));
			}
			else
			{
				// textureId=0 = 空图槽位:画可见占位框,节点仍在(不是"静默画空气")。
				AddRect(ctx, ctx.Rect, theme.ContentBg, radius);
				AddRectOutline(ctx, ctx.Rect, theme.Border, radius, 1.0f);
			}
			ctx.Label = Localized(TokenText(ctx, "label"));
			ctx.Value = textureId > 0.0f ? std::to_string(static_cast<long long>(textureId)) : std::string();
		}

		void PaintProgressBar(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = TokenBool(ctx, "disabled", false);
			const float value = std::clamp(TokenNumber(ctx, "value", 0.0f), 0.0f, 1.0f);
			const float radius = ScaledProp(ctx, "radius", 3.0f);
			AddRect(ctx, ctx.Rect, TokenColor(ctx, "trackColor", theme.ContentBg), radius);
			if (value > 0.0f)
			{
				const WuiColor fill = disabled ? theme.TextDisabled
					: TokenColor(ctx, "fillColor", theme.Accent);
				AddRect(ctx, WuiRect { ctx.Rect.X, ctx.Rect.Y, ctx.Rect.W * value, ctx.Rect.H }, fill, radius);
			}

			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3f", value);
			ctx.Label = Localized(TokenText(ctx, "label"));
			ctx.Value = buffer;
			ctx.Disabled = disabled;
			if (disabled)
				ctx.States = "disabled";
		}

		void PaintToggle(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = TokenBool(ctx, "disabled", false);
			const bool on = TokenBool(ctx, "value", false);
			const float box = ScaledProp(ctx, "boxSize", 16.0f);
			const float radius = 3.0f * ctx.Scale;
			const WuiRect boxRect { ctx.Rect.X,
				ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - box) * 0.5f), box, box };
			const WuiColor boxFill = disabled ? theme.ContentBg : (on ? theme.Accent : theme.ButtonBg);
			AddRect(ctx, boxRect, boxFill, radius);
			AddRectOutline(ctx, boxRect, theme.Border, radius, 1.0f);

			const std::string label = Localized(TokenText(ctx, "label"));
			const float font = ScaledProp(ctx, "fontSize", 15.0f);
			const WuiColor color = disabled ? theme.TextDisabled : TokenColor(ctx, "color", theme.Text);
			AddText(ctx, boxRect.X + box + 8.0f * ctx.Scale,
				ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - font) * 0.5f), label, color, font, false);

			ctx.Label = label;
			ctx.Value = on ? std::string("on") : std::string("off");
			ctx.Disabled = disabled;
			ctx.States = std::string(on ? "checked" : "unchecked") + (disabled ? ",disabled" : "");
		}

		// ---- M10:商业控件面(滚动 / 滑条 / 复选 / 输入框 / 列表 / 网格)----

		void PaintSlider(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = TokenBool(ctx, "disabled", false);
			const float minValue = TokenNumber(ctx, "min", 0.0f);
			const float maxValue = TokenNumber(ctx, "max", 1.0f);
			const float raw = TokenNumber(ctx, "value", 0.0f);
			const float span = maxValue - minValue;
			const float fraction = std::fabs(span) > 1.0e-6f
				? std::clamp((raw - minValue) / span, 0.0f, 1.0f) : 0.0f;
			const float radius = ScaledProp(ctx, "radius", 4.0f);
			const float trackH = std::min(ctx.Rect.H, ScaledProp(ctx, "trackHeight", 4.0f));
			const float knob = std::min(ctx.Rect.H, ScaledProp(ctx, "knobSize", 14.0f));

			const WuiRect track { ctx.Rect.X, ctx.Rect.Y + (ctx.Rect.H - trackH) * 0.5f, ctx.Rect.W, trackH };
			AddRect(ctx, track, TokenColor(ctx, "trackColor", theme.ContentBg), radius);
			if (fraction > 0.0f)
			{
				AddRect(ctx, WuiRect { track.X, track.Y, track.W * fraction, track.H },
					disabled ? theme.TextDisabled : TokenColor(ctx, "fillColor", theme.Accent), radius);
			}
			const float knobX = ctx.Rect.X + std::max(0.0f, ctx.Rect.W * fraction - knob * 0.5f);
			AddRect(ctx, WuiRect { knobX, ctx.Rect.Y + (ctx.Rect.H - knob) * 0.5f, knob, knob },
				disabled ? theme.TextDisabled : TokenColor(ctx, "knobColor", theme.Text), knob * 0.5f);

			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3f", raw);
			const std::string label = Localized(TokenText(ctx, "label"));
			// 矩形够高时补文字(标签左、当前值右);紧凑高度只走无障碍 Value,不糊在轨道上。
			const float font = ScaledProp(ctx, "fontSize", 13.0f);
			if (ctx.Rect.H >= font * 1.8f)
			{
				AddText(ctx, ctx.Rect.X, ctx.Rect.Y, label, theme.Text, font, false);
				const float valueWidth =
					ctx.Context.MeasureTextWidth(std::string(buffer), font, Wui::WuiFontFamily::Ui);
				AddText(ctx, ctx.Rect.X + std::max(ctx.Rect.W - valueWidth, 0.0f), ctx.Rect.Y,
					buffer, theme.Text, font, false);
			}
			ctx.Label = label;
			ctx.Value = buffer;
			ctx.Disabled = disabled;
			ctx.States = disabled ? "disabled" : std::string();
		}

		void PaintCheckbox(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = TokenBool(ctx, "disabled", false);
			// 组件登记属性名是 `checked`;`value` 作为兼容别名(与 Toggle 同一编码)。
			const bool on = TokenBool(ctx, "checked", TokenBool(ctx, "value", false));
			const float box = ScaledProp(ctx, "boxSize", 16.0f);
			const float radius = 3.0f * ctx.Scale;
			const WuiRect boxRect { ctx.Rect.X,
				ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - box) * 0.5f), box, box };
			AddRect(ctx, boxRect, disabled ? theme.ContentBg : (on ? theme.Accent : theme.ButtonBg), radius);
			AddRectOutline(ctx, boxRect, theme.Border, radius, 1.0f);
			if (on)
			{
				// 勾选:内缩方块(纯命令路径,不引入图标资源)。
				const float inset = box * 0.28f;
				const float inner = std::max(box - inset * 2.0f, 0.0f);
				AddRect(ctx, WuiRect { boxRect.X + inset, boxRect.Y + inset, inner, inner },
					theme.PanelBg, radius * 0.5f);
			}

			const std::string label = Localized(TokenText(ctx, "label"));
			const float font = ScaledProp(ctx, "fontSize", 15.0f);
			const WuiColor color = disabled ? theme.TextDisabled : TokenColor(ctx, "color", theme.Text);
			AddText(ctx, boxRect.X + box + 8.0f * ctx.Scale,
				ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - font) * 0.5f), label, color, font, false);

			ctx.Label = label;
			ctx.Value = on ? std::string("true") : std::string("false");
			ctx.Disabled = disabled;
			ctx.States = std::string(on ? "checked" : "unchecked") + (disabled ? ",disabled" : "");
		}

		void PaintTextField(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = TokenBool(ctx, "disabled", false);
			const float radius = ScaledProp(ctx, "radius", theme.Radius);
			AddRect(ctx, ctx.Rect, disabled ? theme.ContentBg : TokenColor(ctx, "bg", theme.ContentBg), radius);
			AddRectOutline(ctx, ctx.Rect, TokenColor(ctx, "border", theme.Border), radius, 1.0f);

			const std::string label = Localized(TokenText(ctx, "label"));
			const std::string value = TokenText(ctx, "value");
			const std::string placeholder = Localized(TokenText(ctx, "placeholder"));
			const bool empty = value.empty();
			const std::string shown = empty ? placeholder : value;
			const float font = ScaledProp(ctx, "fontSize", 15.0f);
			const float padding = ScaledProp(ctx, "padding", 8.0f);
			const WuiColor color = disabled ? theme.TextDisabled
				: (empty ? theme.TextDisabled : TokenColor(ctx, "color", theme.Text));
			const float y = ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - font) * 0.5f);

			float x = ctx.Rect.X + padding;
			if (!label.empty())
			{
				AddText(ctx, x, y, label, theme.TextDisabled, font, false);
				x += ctx.Context.MeasureTextWidth(label, font, Wui::WuiFontFamily::Ui) + 6.0f * ctx.Scale;
			}
			AddText(ctx, x, y, shown, color, font, false);
			if (!empty)
			{
				const float caretX = x + ctx.Context.MeasureTextWidth(shown, font, Wui::WuiFontFamily::Ui) + 1.0f * ctx.Scale;
				AddRect(ctx, WuiRect { caretX, ctx.Rect.Y + 4.0f * ctx.Scale, 1.0f * ctx.Scale,
					std::max(ctx.Rect.H - 8.0f * ctx.Scale, 0.0f) }, theme.Text, 0.0f);
			}

			ctx.Label = label;
			ctx.Value = value;
			ctx.Disabled = disabled;
			ctx.States = disabled ? "disabled" : std::string();
		}

		// 程序化列表:行不是文档子节点 ⇒ **虚拟化**:只画可见行(±1 行缓冲)。
		// 1000 行的命令数与"可见行数"同阶,与总行数无关。
		void PaintList(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = TokenBool(ctx, "disabled", false);
			const float radius = ScaledProp(ctx, "radius", theme.Radius);
			AddRect(ctx, ctx.Rect, TokenColor(ctx, "bg", theme.ContentBg), radius);
			AddRectOutline(ctx, ctx.Rect, theme.Border, radius, 1.0f);

			const int rows = std::max(0, static_cast<int>(TokenNumber(ctx, "rowCount", 0.0f)));
			const float rowHeight = TokenNumber(ctx, "rowHeight", 24.0f) * ctx.Scale;
			int drawn = 0;
			if (rows > 0 && rowHeight > 0.0f)
			{
				const float offsetY = ctx.Screen.ScrollOffset(ctx.Node.Id).y * ctx.Scale;
				const int first = std::max(static_cast<int>(std::floor(offsetY / rowHeight)) - 1, 0);
				const int last = std::min(
					static_cast<int>(std::ceil((offsetY + ctx.Rect.H) / rowHeight)) + 1, rows - 1);
				const float font = ScaledProp(ctx, "fontSize", 14.0f);
				const float padding = ScaledProp(ctx, "padding", 6.0f);
				const std::string prefix = Localized(TokenText(ctx, "rowPrefix"));
				const WuiColor rowColorA = TokenColor(ctx, "rowColor", theme.PanelBg);
				const WuiColor rowColorB = TokenColor(ctx, "rowColorAlt", theme.ContentBg);
				for (int row = first; row <= last; ++row)
				{
					const float y = ctx.Rect.Y + static_cast<float>(row) * rowHeight - offsetY;
					const WuiRect rowRect { ctx.Rect.X + 1.0f * ctx.Scale, y,
						std::max(ctx.Rect.W - 2.0f * ctx.Scale, 0.0f), rowHeight };
					AddRect(ctx, rowRect, (row % 2 == 0) ? rowColorA : rowColorB, 0.0f);
					AddText(ctx, rowRect.X + padding, y + std::max(0.0f, (rowHeight - font) * 0.5f),
						prefix + std::to_string(row), theme.Text, font, false);
					++drawn;
				}
			}

			ctx.Label = Localized(TokenText(ctx, "label"));
			ctx.Value = "items=" + std::to_string(rows) + " visible=" + std::to_string(drawn);
			ctx.Disabled = disabled;
			ctx.States = disabled ? "disabled" : std::string();
		}

		// 程序化网格:同 List,按行带虚拟化(只画可见行 × 全部列)。
		void PaintGrid(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = TokenBool(ctx, "disabled", false);
			const float radius = ScaledProp(ctx, "radius", theme.Radius);
			AddRect(ctx, ctx.Rect, TokenColor(ctx, "bg", theme.ContentBg), radius);
			AddRectOutline(ctx, ctx.Rect, theme.Border, radius, 1.0f);

			const int columns = std::max(1, static_cast<int>(TokenNumber(ctx, "columns", 4.0f)));
			const int rows = std::max(0, static_cast<int>(TokenNumber(ctx, "rowCount", 0.0f)));
			const float cellW = TokenNumber(ctx, "cellW",
				(ctx.Rect.W / ctx.Scale) / static_cast<float>(columns)) * ctx.Scale;
			const float cellH = TokenNumber(ctx, "cellH", 24.0f) * ctx.Scale;
			int drawn = 0;
			if (rows > 0 && cellW > 0.0f && cellH > 0.0f)
			{
				const float offsetY = ctx.Screen.ScrollOffset(ctx.Node.Id).y * ctx.Scale;
				const int first = std::max(static_cast<int>(std::floor(offsetY / cellH)) - 1, 0);
				const int last = std::min(
					static_cast<int>(std::ceil((offsetY + ctx.Rect.H) / cellH)) + 1, rows - 1);
				const float font = ScaledProp(ctx, "fontSize", 13.0f);
				const float gap = ScaledProp(ctx, "gap", 2.0f);
				const WuiColor cellColor = TokenColor(ctx, "cellColor", theme.PanelBg);
				for (int row = first; row <= last; ++row)
				{
					const float y = ctx.Rect.Y + static_cast<float>(row) * cellH - offsetY;
					for (int column = 0; column < columns; ++column)
					{
						const WuiRect cell { ctx.Rect.X + static_cast<float>(column) * cellW, y,
							std::max(cellW - gap, 0.0f), std::max(cellH - gap, 0.0f) };
						AddRect(ctx, cell, cellColor, 0.0f);
						AddText(ctx, cell.X + 4.0f * ctx.Scale, cell.Y + std::max(0.0f, (cell.H - font) * 0.5f),
							std::to_string(row * columns + column), theme.Text, font, false);
						++drawn;
					}
				}
			}

			ctx.Label = Localized(TokenText(ctx, "label"));
			ctx.Value = "cells=" + std::to_string(rows * columns) + " visible=" + std::to_string(drawn);
			ctx.Disabled = disabled;
			ctx.States = disabled ? "disabled" : std::string();
		}

		// ---- M10:滚动内容尺寸自报(设计空间)----
		// 行/格不是文档子节点,UiScreen 量不到内容;由类型按属性自报,偏移钳位才正确。

		bool ListContentSize(const UiNodeInstance& node, glm::vec2& outSize)
		{
			const int rows = static_cast<int>(NumberProp(node, "rowCount", 0.0f));
			const float rowHeight = NumberProp(node, "rowHeight", 24.0f);
			if (rows <= 0 || !(rowHeight > 0.0f))
				return false;
			outSize = glm::vec2 { node.Rect.W, static_cast<float>(rows) * rowHeight };
			return true;
		}

		bool GridContentSize(const UiNodeInstance& node, glm::vec2& outSize)
		{
			const int columns = std::max(1, static_cast<int>(NumberProp(node, "columns", 4.0f)));
			const int rows = static_cast<int>(NumberProp(node, "rowCount", 0.0f));
			if (rows <= 0)
				return false;
			const float cellW = NumberProp(node, "cellW", node.Rect.W / static_cast<float>(columns));
			const float cellH = NumberProp(node, "cellH", 24.0f);
			if (!(cellW > 0.0f) || !(cellH > 0.0f))
				return false;
			outSize = glm::vec2 { cellW * static_cast<float>(columns), cellH * static_cast<float>(rows) };
			return true;
		}
	}

	// 内置类型:`.wui` Type → 既有组件登记 id + 无障碍 role + 绘制入口。
	// Panel→box / ProgressBar→progress 是"复用既有组件命名"的映射(不新增控件名):
	// `WuiNodeRegistry` 不重复定义属性表,属性元数据走 `UiNodeRegistry::Component()`。
	void RegisterBuiltinUiNodeTypes()
	{
		auto add = [](UiNodeTypeDesc desc, const char* doc) {
			// M10:滚动容器属性对**所有**类型有效(任何容器节点都能滚动)。统一声明,
			// 否则 `scrollable`/`overflow` 会被"未知属性"提示刷屏。
			for (const char* scrollProp : { "scrollable", "scroll", "overflow" })
				desc.ExtraProps.emplace_back(scrollProp);
			// M41:功能介绍(英文源文,本地化键 wui.component.<ComponentId>.doc)与新建默认尺寸
			// (取自组件登记表 SizeNotes 的首选;唯一解析实现在 Wui::PreferredComponentSize,失败 = 0)。
			desc.Doc = doc != nullptr ? doc : "";
			glm::vec2 preferred { 0.0f, 0.0f };
			if (Wui::PreferredComponentSize(desc.ComponentId, preferred))
				desc.DefaultSize = preferred;
			std::string error;
			if (!UiNodeRegistry::RegisterType(std::move(desc), &error))
				WLD_CORE_ERROR("[ui] builtin node type registration failed: {0}", error);
		};

		add({ "Panel", "box", "group", { "WuiBox" }, false, &PaintPanel,
			{ "bg", "border", "radius", "title" } },
			"Container surface with a themed background, border and optional title that lays out child "
			"nodes inside; use it for cards, dialogs and toolbars.");
		add({ "Label", "label", "text", { "WuiLabel" }, false, &PaintLabel, { "color" } },
			"Read-only single line of text. Use it for captions, field labels and status readouts.");
		add({ "Button", "button", "button", { "WuiButton" }, true, &PaintButton,
			{ "bg", "border", "text", "radius" } },
			"Clickable push button with a label and hover/pressed/focus/disabled states; use it as the "
			"main action in a dialog, toolbar or form.");
		add({ "Image", "image", "img", { "WuiImage" }, false, &PaintImage, { "radius" } },
			"Draws a texture (icon, thumbnail or preview) into the node rectangle, with optional "
			"rounding and tint.");
		add({ "ProgressBar", "progress", "progressbar", { "progress", "WuiProgress" }, false,
			&PaintProgressBar, { "fillColor", "trackColor", "radius", "label" } },
			"Horizontal bar that fills a track from a 0..1 value; use it for loading, health and "
			"task-progress readouts.");
		add({ "Toggle", "toggle", "checkbox", { "WuiToggle" }, true, &PaintToggle,
			{ "color", "boxSize" } },
			"On/off switch with a square indicator and a label; use it for options that apply "
			"immediately.");

		// ---- M10:商业控件面 ----
		// ComponentId 必须命中 `WuiComponentRegistry::Find`;登记表里没有恰好叫 slider/grid 的 id,
		// 取最接近的既有登记项(Slider→`slider.float`, List/Grid→`listview`),不另起命名。
		add({ "Slider", "slider.float", "slider", { "SliderFloat", "WuiSlider" }, true, &PaintSlider,
			{ "label", "radius", "trackColor", "fillColor", "knobColor", "trackHeight", "knobSize" } },
			"Drag or arrow-key control that picks a number within a min..max range; use it for "
			"volume, intensity and similar values.");
		add({ "Checkbox", "checkbox", "checkbox", { "WuiCheckbox", "CheckboxEx" }, true, &PaintCheckbox,
			{ "boxSize", "color", "fontSize" } },
			"Checkbox with a label for one yes/no choice; pairs with an Apply/OK button in forms.");
		add({ "TextField", "textfield", "text-field", { "WuiTextField", "Input" }, true, &PaintTextField,
			{ "radius", "bg", "border", "fontSize", "padding", "color" } },
			"Single-line editable text input with placeholder support; use it for names, paths and "
			"other short values.");
		add({ "List", "listview", "list", { "ListView", "WuiList" }, true, &PaintList,
			{ "rowCount", "rowHeight", "radius", "bg", "fontSize", "rowPrefix", "padding",
				"rowColor", "rowColorAlt" }, &ListContentSize },
			"Scrollable list of rows that reports the clicked row; use it for item pickers and "
			"asset browsers.");
		add({ "Grid", "listview", "grid", { "GridView", "WuiGrid" }, true, &PaintGrid,
			{ "columns", "cellW", "cellH", "rowCount", "radius", "bg", "fontSize", "cellColor", "gap" },
			&GridContentSize },
			"Scrollable grid of equal cells with a fixed column count; use it for tile pickers and "
			"icon galleries.");
	}

	// ---- M26:运行态属性覆盖表 ----
	//
	// 覆盖表的**唯一**消费点是绘制期属性读取(见上面的 `ResolvedProp`);这里只实现存储。
	// 规模 = 每个 `Bind:` 条目一条(文档里的绑定数量级),线性查找足够;不做哈希索引。

	void UiPropertyOverrideTable::Set(std::string nodeId, std::string property, std::string value)
	{
		if (nodeId.empty() || property.empty())
			return;   // 不可查的键:拒绝写入,而不是静默塞进表里
		for (UiPropertyOverride& entry : m_Entries)
		{
			if (entry.NodeId == nodeId && entry.Property == property)
			{
				entry.Value = std::move(value);
				return;
			}
		}
		m_Entries.push_back(UiPropertyOverride { std::move(nodeId), std::move(property), std::move(value) });
	}

	const std::string* UiPropertyOverrideTable::Find(std::string_view nodeId, std::string_view property) const
	{
		for (const UiPropertyOverride& entry : m_Entries)
		{
			if (entry.NodeId == nodeId && entry.Property == property)
				return &entry.Value;
		}
		return nullptr;
	}

	UiPaintResult UiPainter::Paint(Wui::WuiContext& ctx, const UiScreen& screen, const UiPaintOptions& options)
	{
		UiPaintResult result;

		const Wui::WuiTheme& theme = Wui::CurrentTheme();
		const UiViewport& viewport = screen.Viewport();
		const UiDocument& document = screen.Document();

		const std::string page = options.Page.empty() ? document.Screen : options.Page;
		std::string panel = options.PanelId;
		if (panel.empty())
			panel = Wui::WuiAccessibility::Get().CurrentPanel();
		if (panel.empty())
			panel = page;

		const bool registerAccessibility =
			options.RegisterAccessibility && Wui::WuiAccessibility::Get().Enabled();

		const std::vector<UiNodeInstance>& nodes = screen.Nodes();

		// 每个节点的子树末尾下标(前序 = 连续区间)⇒ 平铺循环里也能成对压/弹滚动裁剪。
		std::vector<int> subtreeEnd(nodes.size(), 0);
		for (std::size_t i = nodes.size(); i-- > 0;)
		{
			int end = static_cast<int>(i);
			for (const int child : nodes[i].Children)
				end = std::max(end, subtreeEnd[static_cast<std::size_t>(child)]);
			subtreeEnd[i] = end;
		}

		struct ClipFrame
		{
			int End = 0;
			Wui::WuiRect Rect { 0, 0, 0, 0 };
		};
		std::vector<ClipFrame> clips;
		const auto overlaps = [](const Wui::WuiRect& a, const Wui::WuiRect& b) {
			return a.X + a.W > b.X && a.X < b.X + b.W && a.Y + a.H > b.Y && a.Y < b.Y + b.H;
		};
		const auto clipIntersection = [&clips]() {
			Wui::WuiRect rect = clips.front().Rect;
			for (std::size_t c = 1; c < clips.size(); ++c)
			{
				const Wui::WuiRect& other = clips[c].Rect;
				const float x0 = std::max(rect.X, other.X);
				const float y0 = std::max(rect.Y, other.Y);
				const float x1 = std::min(rect.X + rect.W, other.X + other.W);
				const float y1 = std::min(rect.Y + rect.H, other.Y + other.H);
				rect = Wui::WuiRect { x0, y0, std::max(x1 - x0, 0.0f), std::max(y1 - y0, 0.0f) };
			}
			return rect;
		};

		for (std::size_t i = 0; i < nodes.size(); ++i)
		{
			// 已离开的滚动容器:弹裁剪(渲染命令 + 裁剪栈同进同出)。
			while (!clips.empty() && static_cast<int>(i) > clips.back().End)
			{
				ctx.Commands().push_back(Wui::WuiDrawCommand { Wui::WuiDrawKind::ClipPop });
				ctx.PopClipRect();
				clips.pop_back();
				result.Commands += 1;
			}

			const UiNodeInstance& node = nodes[i];
			const std::string path = NodePath(screen, node);
			const UiNodeTypeDesc* type = UiNodeRegistry::Find(node.Type);
			const Wui::WuiRect physical = viewport.DesignRectToPhysical(node.Rect);
			const std::size_t before = ctx.Commands().size();

			// 滚动容器:先压裁剪矩形(含容器自身,子节点/程序化行都受它约束),子树画完再弹。
			if (screen.IsScrollContainer(node))
			{
				ctx.Commands().push_back(Wui::WuiDrawCommand { Wui::WuiDrawKind::ClipPush, physical });
				ctx.PushClipRect(physical);
				clips.push_back(ClipFrame { subtreeEnd[i], physical });
			}

			if (type == nullptr)
			{
				// 未知类型 = 可读报错 + 跳过(不得静默画空气);同批其它节点继续画。
				result.SkippedNodes++;
				result.Errors.push_back(UiPaintError { path, node.Type,
					"unknown UI node type '" + node.Type + "' at '" + path +
					"': not registered in UiNodeRegistry (node skipped, nothing drawn)" });
				result.Commands += ctx.Commands().size() - before;
				continue;
			}

			// 完全落在滚动裁剪之外 ⇒ 不画、不登记(与 `UiScreen::HitTest` 同一裁剪口径)。
			if (!clips.empty() && !overlaps(clipIntersection(), physical))
			{
				result.ClippedNodes++;
				result.Commands += ctx.Commands().size() - before;
				continue;
			}

			UiNodePaintContext paint {
				ctx, screen, node, *type, physical, viewport.Scale,
				theme, options, path,
				std::string(), std::string(), std::string(), false, &result.Warnings,
				&document, &result.Errors };

			type->Paint(paint);   // RegisterType 保证非空
			const std::size_t emitted = ctx.Commands().size() - before;
			if (emitted > 0)
				result.DrawnNodes++;
			result.Commands += emitted;

			if (node.Source != nullptr)
			{
				// 未知属性 = 非致命提示(contract.ui-document-format §6 的可读信号;
				// 致命校验归 `ValidateUiDocument`,本层不重复拒绝)。
				for (const UiProp& prop : node.Source->Props)
				{
					if (!UiNodeRegistry::DeclaresProperty(*type, prop.Name))
					{
						result.Warnings.push_back(UiPaintWarning { path,
							"unknown property '" + prop.Name + "' on type '" + type->Type +
							"' (declared properties come from WuiComponentRegistry id '" +
							type->ComponentId + "')" });
					}
				}
			}

			if (registerAccessibility)
			{
				// 窗口坐标 = 物理坐标(与命中同一映射)。Id 用稳定节点 Id 的 FNV-1a,
				// 与 `Wui::HashId` 同算法 —— 脚本可直接 hash_id("<node id>") 得到它。
				Wui::WuiAccessNode access;
				access.Id = Wui::HashId(node.Id.c_str());
				access.Window = options.WindowKey;
				access.Panel = panel;
				access.Kind = type->ComponentId;
				access.Label = paint.Label;
				access.Value = paint.Value;
				access.Rect = paint.Rect;
				access.Enabled = !paint.Disabled;
				access.Interactive = type->Interactive && !paint.Disabled;
				access.Focused = false;
				access.Role = type->Role;
				access.States = paint.States;
				access.Actions = type->Interactive ? std::string("click") : std::string();
				access.Path = path;
				access.Page = page;
				access.Layer = options.Layer;
				Wui::WuiAccessibility::Get().Register(access);
				result.AccessNodes++;
			}
		}

		// 收尾:弹掉所有未闭合的滚动裁剪(命令流必须 Push/Pop 成对)。
		while (!clips.empty())
		{
			ctx.Commands().push_back(Wui::WuiDrawCommand { Wui::WuiDrawKind::ClipPop });
			ctx.PopClipRect();
			clips.pop_back();
			result.Commands += 1;
		}

		return result;
	}
}
