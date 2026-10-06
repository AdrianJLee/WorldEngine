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

		std::string TextProp(const UiNodeInstance& node, std::string_view name, std::string fallback = std::string())
		{
			const std::string* value = FindProp(node, name);
			return value != nullptr ? *value : std::move(fallback);
		}

		bool BoolProp(const UiNodeInstance& node, std::string_view name, bool fallback)
		{
			const std::string* value = FindProp(node, name);
			if (value == nullptr || value->empty())
				return fallback;
			return *value == "1" || *value == "true" || *value == "True" || *value == "yes" || *value == "on";
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

		WuiColor ColorProp(const UiNodeInstance& node, std::string_view name, const WuiColor& fallback)
		{
			const std::string* value = FindProp(node, name);
			WuiColor parsed = fallback;
			if (value != nullptr && Wui::ParseComponentColor(*value, parsed))
				return parsed;
			return fallback;
		}

		// 显示文本:`@key` = 本地化 key(缺 key 回退去掉 `@` 的字面量),其余原样。
		std::string Localized(std::string_view text)
		{
			if (!text.empty() && text.front() == '@')
			{
				const std::string key(text.substr(1));
				return Wui::Tr(key, key);
			}
			return std::string(text);
		}

		// 设计单位属性 → 物理长度(乘视口缩放);未给 = 设计默认值。
		float ScaledProp(const UiNodePaintContext& ctx, std::string_view name, float designFallback)
		{
			return NumberProp(ctx.Node, name, designFallback) * ctx.Scale;
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
			AddRect(ctx, ctx.Rect, ColorProp(ctx.Node, "bg", theme.PanelBg), radius);
			AddRectOutline(ctx, ctx.Rect, ColorProp(ctx.Node, "border", theme.Border), radius, 1.0f);

			const std::string title = Localized(TextProp(ctx.Node, "title"));
			if (!title.empty())
			{
				AddText(ctx, ctx.Rect.X + theme.Pad * ctx.Scale, ctx.Rect.Y + 3.0f * ctx.Scale,
					title, theme.Text, theme.FontSizeTitle * ctx.Scale, false);
			}
			ctx.Label = title;
		}

		void PaintLabel(UiNodePaintContext& ctx)
		{
			const std::string text = Localized(TextProp(ctx.Node, "text", TextProp(ctx.Node, "label")));
			const float font = ScaledProp(ctx, "fontSize", 15.0f);
			const float y = ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - font) * 0.5f);
			AddText(ctx, ctx.Rect.X, y, text, ColorProp(ctx.Node, "color", ctx.Theme.Text), font,
				BoolProp(ctx.Node, "bold", false));
			ctx.Label = text;
			ctx.Value = text;
		}

		void PaintButton(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = BoolProp(ctx.Node, "disabled", false);
			// 未给覆盖 = 主题令牌;`bg`/`bg.default` 是覆盖值(text 编码 #RRGGBB[AA])。
			const WuiColor bg = disabled ? theme.ContentBg
				: ColorProp(ctx.Node, "bg", ColorProp(ctx.Node, "bg.default", theme.ButtonBg));
			const WuiColor border = ColorProp(ctx.Node, "border",
				ColorProp(ctx.Node, "border.default", theme.Border));
			const WuiColor textColor = disabled ? theme.TextDisabled
				: ColorProp(ctx.Node, "text", ColorProp(ctx.Node, "text.default", theme.Text));
			const float radius = ScaledProp(ctx, "radius", theme.Radius);
			AddRect(ctx, ctx.Rect, bg, radius);
			AddRectOutline(ctx, ctx.Rect, border, radius, 1.0f);

			const std::string label = Localized(TextProp(ctx.Node, "label"));
			const float font = ScaledProp(ctx, "fontSize", 15.0f);
			const float padding = ScaledProp(ctx, "padding", 8.0f);
			const float width = ctx.Context.MeasureTextWidth(label, font, Wui::WuiFontFamily::Ui);
			const float x = ctx.Rect.X + std::max(padding, (ctx.Rect.W - width) * 0.5f);
			const float y = ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - font) * 0.5f);
			AddText(ctx, x, y, label, textColor, font, BoolProp(ctx.Node, "bold", false));

			ctx.Label = label;
			ctx.Value = disabled ? std::string("disabled") : std::string();
			ctx.Disabled = disabled;
			if (disabled)
				ctx.States = "disabled";
		}

		void PaintImage(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const float textureId = NumberProp(ctx.Node, "textureId", 0.0f);
			const float radius = ScaledProp(ctx, "radius", theme.Radius);
			if (textureId > 0.0f)
			{
				Wui::WuiDrawCommand command;
				command.Kind = Wui::WuiDrawKind::Image;
				command.Rect = ctx.Rect;
				command.Color = ColorProp(ctx.Node, "tint", WuiColor { 1, 1, 1, 1 });
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
			ctx.Label = Localized(TextProp(ctx.Node, "label"));
			ctx.Value = textureId > 0.0f ? std::to_string(static_cast<long long>(textureId)) : std::string();
		}

		void PaintProgressBar(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = BoolProp(ctx.Node, "disabled", false);
			const float value = std::clamp(NumberProp(ctx.Node, "value", 0.0f), 0.0f, 1.0f);
			const float radius = ScaledProp(ctx, "radius", 3.0f);
			AddRect(ctx, ctx.Rect, ColorProp(ctx.Node, "trackColor", theme.ContentBg), radius);
			if (value > 0.0f)
			{
				const WuiColor fill = disabled ? theme.TextDisabled
					: ColorProp(ctx.Node, "fillColor", theme.Accent);
				AddRect(ctx, WuiRect { ctx.Rect.X, ctx.Rect.Y, ctx.Rect.W * value, ctx.Rect.H }, fill, radius);
			}

			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3f", value);
			ctx.Label = Localized(TextProp(ctx.Node, "label"));
			ctx.Value = buffer;
			ctx.Disabled = disabled;
			if (disabled)
				ctx.States = "disabled";
		}

		void PaintToggle(UiNodePaintContext& ctx)
		{
			const Wui::WuiTheme& theme = ctx.Theme;
			const bool disabled = BoolProp(ctx.Node, "disabled", false);
			const bool on = BoolProp(ctx.Node, "value", false);
			const float box = ScaledProp(ctx, "boxSize", 16.0f);
			const float radius = 3.0f * ctx.Scale;
			const WuiRect boxRect { ctx.Rect.X,
				ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - box) * 0.5f), box, box };
			const WuiColor boxFill = disabled ? theme.ContentBg : (on ? theme.Accent : theme.ButtonBg);
			AddRect(ctx, boxRect, boxFill, radius);
			AddRectOutline(ctx, boxRect, theme.Border, radius, 1.0f);

			const std::string label = Localized(TextProp(ctx.Node, "label"));
			const float font = ScaledProp(ctx, "fontSize", 15.0f);
			const WuiColor color = disabled ? theme.TextDisabled : ColorProp(ctx.Node, "color", theme.Text);
			AddText(ctx, boxRect.X + box + 8.0f * ctx.Scale,
				ctx.Rect.Y + std::max(0.0f, (ctx.Rect.H - font) * 0.5f), label, color, font, false);

			ctx.Label = label;
			ctx.Value = on ? std::string("on") : std::string("off");
			ctx.Disabled = disabled;
			ctx.States = std::string(on ? "checked" : "unchecked") + (disabled ? ",disabled" : "");
		}
	}

	// 内置类型:`.wui` Type → 既有组件登记 id + 无障碍 role + 绘制入口。
	// Panel→box / ProgressBar→progress 是"复用既有组件命名"的映射(不新增控件名):
	// `WuiNodeRegistry` 不重复定义属性表,属性元数据走 `UiNodeRegistry::Component()`。
	void RegisterBuiltinUiNodeTypes()
	{
		auto add = [](UiNodeTypeDesc desc) {
			std::string error;
			if (!UiNodeRegistry::RegisterType(std::move(desc), &error))
				WLD_CORE_ERROR("[ui] builtin node type registration failed: {0}", error);
		};

		add({ "Panel", "box", "group", { "WuiBox" }, false, &PaintPanel,
			{ "bg", "border", "radius", "title" } });
		add({ "Label", "label", "text", { "WuiLabel" }, false, &PaintLabel, { "color" } });
		add({ "Button", "button", "button", { "WuiButton" }, true, &PaintButton,
			{ "bg", "border", "text", "radius" } });
		add({ "Image", "image", "img", { "WuiImage" }, false, &PaintImage, { "radius" } });
		add({ "ProgressBar", "progress", "progressbar", { "progress", "WuiProgress" }, false,
			&PaintProgressBar, { "fillColor", "trackColor", "radius", "label" } });
		add({ "Toggle", "toggle", "checkbox", { "WuiToggle" }, true, &PaintToggle,
			{ "color", "boxSize" } });
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

		for (const UiNodeInstance& node : screen.Nodes())
		{
			const std::string path = NodePath(screen, node);
			const UiNodeTypeDesc* type = UiNodeRegistry::Find(node.Type);
			if (type == nullptr)
			{
				// 未知类型 = 可读报错 + 跳过(不得静默画空气);同批其它节点继续画。
				result.SkippedNodes++;
				result.Errors.push_back(UiPaintError { path, node.Type,
					"unknown UI node type '" + node.Type + "' at '" + path +
					"': not registered in UiNodeRegistry (node skipped, nothing drawn)" });
				continue;
			}

			UiNodePaintContext paint {
				ctx, screen, node, *type,
				viewport.DesignRectToPhysical(node.Rect), viewport.Scale,
				theme, options, path,
				std::string(), std::string(), std::string(), false, &result.Warnings };

			const std::size_t before = ctx.Commands().size();
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

		return result;
	}
}
