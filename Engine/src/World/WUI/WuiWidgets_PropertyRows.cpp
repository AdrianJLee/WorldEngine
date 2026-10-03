#include "WuiWidgets_Internal.h"

namespace World::Wui
{

using namespace WuiWidgetsDetail;

namespace WuiWidgetsDetail
{

		// 行节点:与 WuiWidgets 既有的 RegisterAccessNode 同一格式 + Tooltip 字段(悬停说明的第二通道)。
void RegisterRowNode(WuiId id, const char* kind, const WuiRect& rect, const std::string& label, const std::string& value, bool enabled, bool interactive, bool focused, const std::string& tooltip){
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


float RowLabelWidth(const WuiRect& row, float requested){
			const float available = std::max(0.0f, row.W - kPropertyFieldGutter - kPropertyActionWidth);
			if (requested > 0.0f)
				return std::min(requested, available);
			return std::min(kPropertyLabelMaxWidth, std::max(0.0f, std::min(row.W * 0.45f, available)));
		}


		// 行内动作列(index 从 0 起,自右向左排):固定占位,不随状态改几何。
WuiRect InsideActionRect(const WuiRect& row, int index){
			const float step = kPropertyActionWidth + kPropertyActionGap;
			const float x = row.X + row.W - kPropertyActionGap - (static_cast<float>(index) + 1.0f) * step;
			return WuiRect { x, row.Y + (row.H - kPropertyActionWidth) * 0.5f, kPropertyActionWidth,
				kPropertyActionWidth };
		}


		// 行外动作列(index 从 0 起,自左向右排):集合元素行/追加行的动作落在行矩形右侧预留槽里。
WuiRect OutsideActionRect(const WuiRect& row, int index){
			const float step = kPropertyActionWidth + kPropertyActionGap;
			return WuiRect { row.X + row.W + kPropertyActionGap + static_cast<float>(index) * step,
				row.Y + (row.H - kPropertyActionWidth) * 0.5f, kPropertyActionWidth, kPropertyActionWidth };
		}


		// 顶层行是否带展开标记:展开 = ▼、折叠 = ▶(与 CollapsibleHeader 同一语义,比 "-/+ " 前缀更好认)。
const char* RowExpandMarker(bool open){
			return open ? "\u25BC " : "\u25B6 ";
		}


		// VEC-H4:嵌套层级 = 标签文字缩进 + 1px 树导线(导线画在缩进原点,不动行矩形)。
void DrawRowIndentGuide(WuiContext& ctx, const WuiRect& row, float labelIndent, const WuiTheme& theme){
			if (labelIndent <= 0.0f)
				return;
			const float x = row.X + kPropertyLabelTextX + labelIndent - kPropertyIndentStep;
			ctx.Commands().push_back({ WuiDrawKind::Rect,
				{ x, row.Y + 2.0f, 1.0f, std::max(0.0f, row.H - 4.0f) }, theme.Border, 0.0f });
		}


void DrawRowLabel(WuiContext& ctx, const WuiRect& row, const std::string& label, const std::string& term, bool open, bool expandable, const WuiColor& color, float labelWidth, const WuiTheme& theme, float labelIndent ){
			if (label.empty())
				return;
			const float size = kPropertyLabelSize;
			const float indent = std::max(0.0f, labelIndent);
			const float y = row.Y + (row.H - size) * 0.5f - 2.0f;
			float x = row.X + kPropertyLabelTextX + indent;
			float budget = std::max(16.0f, labelWidth - kPropertyLabelTextX - indent - kPropertyFieldGutter);
			if (expandable)
			{
				// VEC-H5:展开标记单独一笔画,且**永不参与主名的缩略** —— 折叠标识始终可见。
				Wui::Label(ctx, { x, y }, RowExpandMarker(open), color, size);
				x += kPropertyExpandMarkerAdvance;
				budget = std::max(16.0f, budget - kPropertyExpandMarkerAdvance);
			}
			Wui::LabelWithTerm(ctx,
				{ x, y }, label, term, color, size, theme, budget);
		}


		// 已修改标记:被改过的字段在标签左侧给一个小圆点(不靠颜色吓人,也不移动文字)。
void DrawModifiedDot(WuiContext& ctx, const WuiRect& row, const WuiTheme& theme){
			const float size = kPropertyDotSize;
			ctx.Commands().push_back({ WuiDrawKind::Rect,
				{ row.X + kPropertyActionGap, row.Y + (row.H - size) * 0.5f, size, size }, theme.Accent,
				size * 0.5f });
		}


void RowHoverBackdrop(WuiContext& ctx, const WuiRect& row, bool hovered, bool enabled, const WuiTheme& theme){
			if (!hovered || !enabled)
				return;
			ctx.Commands().push_back({ WuiDrawKind::Rect, row, theme.HoverBg, 3.0f });
		}


		// 行尾动作按钮的共用状态机:`-`/`+` 与分组头的删除/复位都走它。
bool RowActionButton(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& glyph, const std::string& a11yLabel, const std::string& tooltip, bool enabled, const WuiTheme& theme, bool danger){
			const bool hoveredAny = ctx.IsHovered(rect);
			const bool hovered = enabled && hoveredAny;
			const bool focused = enabled && ctx.Focus() == id;
			// VEC-H4 四态:default(ButtonBg)/ hover(ButtonHover)/ active(按下 = ActiveBg,内容不位移)/
			// disabled(PanelBg + TextDisabled,理由进 tooltip)。
			const bool pressed = hovered && ctx.Input().MouseDown[0];
			WuiColor fill = enabled ? (pressed ? theme.ActiveBg : (hovered ? theme.ButtonHover : theme.ButtonBg))
				: theme.PanelBg;
			if (pressed && danger)
			{
				fill = theme.Danger;
				fill.A = 0.30f;
			}
			else if (hovered && danger)
			{
				fill = theme.Danger;
				fill.A = 0.22f;
			}
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, fill, 3.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
				enabled ? (hovered ? (danger ? theme.Danger : theme.Accent) : theme.Border) : theme.Border,
				3.0f, 1.0f });
			const float size = kPropertyActionGlyphSize;
			const float textWidth = ctx.MeasureTextWidth(glyph, size);
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ rect.X + (rect.W - textWidth) * 0.5f, rect.Y + (rect.H - size) * 0.5f - 1.0f, 0, 0 },
				enabled ? (danger && hovered ? theme.Danger : theme.Text) : theme.TextDisabled, 0, 1.0f, glyph, size,
				false });
			RegisterRowNode(id, "button", rect, a11yLabel.empty() ? glyph : a11yLabel, tooltip, enabled, enabled,
				focused, tooltip);
			if (enabled)
				ctx.RegisterFocusable(id, rect);
			DrawFocusRing(ctx, rect, id, theme);
			if (hoveredAny)
			{
				if (enabled)
					ctx.SetCursor(WuiCursor::Hand);
				// 禁用态也要给理由:悬停提示与 a11y 的 Tooltip/Value 同一句。
				if (!tooltip.empty())
					Tooltip(ctx, rect, tooltip);
			}
			const bool keyActivated = focused
				&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
			return enabled && (ctx.IsClicked(rect) || keyActivated);
		}

}

float PropertyRowLabelWidth(const WuiRect& row){
		return RowLabelWidth(row, 0.0f);
	}


float PropertyRowHeight(){
		return kPropertyRowHeight;
	}


PropertyRowLayout MeasurePropertyRow(const WuiRect& row, float labelWidth, bool showReset){
		PropertyRowLayout layout;
		layout.LabelWidth = RowLabelWidth(row, labelWidth);
		const float resetReserve = showReset ? kPropertyActionWidth + kPropertyActionGap : 0.0f;
		layout.Field = { row.X + layout.LabelWidth, row.Y + (row.H - kPropertyFieldHeight) * 0.5f,
			std::max(24.0f, row.W - layout.LabelWidth - kPropertyFieldGutter - resetReserve), kPropertyFieldHeight };
		if (showReset)
			layout.Reset = InsideActionRect(row, 0);
		return layout;
	}


float CollectionActionColumnWidth(){
		return kPropertyActionWidth + kPropertyActionGap;
	}


PropertyRowResult PropertyRow(WuiContext& ctx, WuiId id, const WuiRect& row, const PropertyRowDesc& desc, const WuiTheme& theme){
		PropertyRowResult result;
		const PropertyRowLayout layout = MeasurePropertyRow(row, desc.LabelWidth, desc.ShowReset);
		const float labelWidth = layout.LabelWidth;
		result.FieldRect = layout.Field;
		result.ResetRect = layout.Reset;
		// 多行控件(向量):字段列高度按实际控件高度给足,垂直居中在行内。
		if (desc.FieldHeight > 0.0f)
		{
			result.FieldRect.H = desc.FieldHeight;
			result.FieldRect.Y = row.Y + (row.H - desc.FieldHeight) * 0.5f;
		}
		const bool hovered = ctx.IsHovered(row);
		RowHoverBackdrop(ctx, row, hovered, desc.Enabled, theme);
		if (desc.Modified)
			DrawModifiedDot(ctx, row, theme);
		DrawRowIndentGuide(ctx, row, desc.LabelIndent, theme);
		if (desc.InlineValue)
		{
			// 只读行:值跟在标签后面,不占字段列(与旧口径同文本形态「标签: 值」)。
			const std::string text = desc.Label.empty() ? desc.InlineValueText
				: (desc.InlineValueText.empty() ? desc.Label : desc.Label + ": " + desc.InlineValueText);
			Wui::LabelWithTerm(ctx,
				{ row.X + kPropertyLabelTextX, row.Y + (row.H - kPropertyLabelSize) * 0.5f - 2.0f }, text,
				desc.Term, desc.Enabled ? theme.TextMuted : theme.TextDisabled, kPropertyLabelSize, theme,
				std::max(16.0f, row.W - kPropertyLabelTextX - kPropertyFieldGutter));
		}
		else
		{
			DrawRowLabel(ctx, row, desc.Label, desc.Term, false, false,
				desc.Enabled ? theme.Text : theme.TextDisabled, labelWidth, theme, desc.LabelIndent);
		}
		// VEC-H4:值列占位(多值不同 "—" / 空集合提示):调用方不给字段控件,库件只画这一行文本。
		if (!desc.FieldPlaceholder.empty() && !desc.InlineValue)
		{
			Wui::Label(ctx,
				{ result.FieldRect.X + kPropertyPlaceholderPadX,
					result.FieldRect.Y + (result.FieldRect.H - kPropertyLabelSize) * 0.5f - 1.0f },
				desc.FieldPlaceholder, desc.Enabled ? theme.TextMuted : theme.TextDisabled, kPropertyLabelSize);
		}
		RegisterRowNode(id, desc.A11yKind, row, desc.A11yLabel.empty() ? desc.Label : desc.A11yLabel,
			desc.A11yValue, desc.A11yEnabled, desc.A11yEnabled, false, desc.Tooltip);
		if (hovered && !desc.Tooltip.empty())
			Tooltip(ctx, row, desc.Tooltip);
		// VEC-H6:ShowReset 决定**占位**(几何不变);ResetModified=false = 该行与脚本默认一致 →
		// 不画 ↺、也不登记节点(用户口径「一致时不出现」,不是禁用态)。
		// VEC-H7:**必须在行悬停底色之后画** —— 之前画在底色之前,行一悬停 ↺ 就被 HoverBg 整块盖掉
		// (用户口径「右侧恢复按钮在鼠标悬浮时几乎看不到」的真因);本控件自己登记完整的
		// kind/label/value/enabled/tooltip + 悬停提示,面板/库件都不再补登记。
		if (desc.ShowReset && desc.ResetModified)
		{
			result.ResetClicked = ResetDefaultButton(ctx, desc.ResetId, result.ResetRect, desc.ResetEnabled, theme,
				desc.ResetLabel, desc.ResetTooltip);
		}
		return result;
	}


PropertyGroupHeaderResult PropertyGroupHeader(WuiContext& ctx, WuiId id, const WuiRect& row, const PropertyGroupHeaderDesc& desc, const WuiTheme& theme){
		PropertyGroupHeaderResult result;
		int actionIndex = 0;
		float actionReserve = 0.0f;
		if (desc.ShowRemove)
		{
			result.RemoveRect = InsideActionRect(row, actionIndex++);
			actionReserve += kPropertyActionWidth + kPropertyActionGap;
		}
		if (desc.ShowReset)
		{
			result.ResetRect = InsideActionRect(row, actionIndex++);
			actionReserve += kPropertyActionWidth + kPropertyActionGap;
		}
		// VEC-H5:分区头没有值列 —— 调用方没给 LabelWidth 时,标签列用满"行宽 − 动作列 − 计数文本",
		// 不再套属性行的 140px 上限(长组件名 + 英文术语对照要在 1280/1600 下都读得全)。
		// 调用方给了 LabelWidth(如脚本检视器的集合头)时保持原口径,不动既有布局。
		const float trailingWidth = desc.Trailing.empty() ? 0.0f
			: ctx.MeasureTextWidth(desc.Trailing, theme.FontSizeSmall);
		const float labelWidth = desc.LabelWidth > 0.0f
			? RowLabelWidth(row, desc.LabelWidth)
			: std::max(0.0f, row.W - actionReserve - kPropertyFieldGutter
				- (desc.Trailing.empty() ? 0.0f : trailingWidth + kPropertyActionGap));
		const bool focused = id != 0 && ctx.Focus() == id;
		const bool hovered = ctx.IsHovered(row);
		// 底色语法与 CollapsibleHeader 一致:展开 = ActiveBg、折叠 = PanelHeader、悬停 = HoverBg。
		const WuiColor fill = hovered && desc.Enabled ? theme.HoverBg : (desc.Open ? theme.ActiveBg : theme.PanelHeader);
		ctx.Commands().push_back({ WuiDrawKind::Rect, row, fill, 2.0f });
		if (desc.Modified)
			DrawModifiedDot(ctx, row, theme);
		DrawRowIndentGuide(ctx, row, desc.LabelIndent, theme);
		DrawRowLabel(ctx, row, desc.Label, desc.Term, desc.Open, true,
			desc.Enabled ? theme.Text : theme.TextDisabled, labelWidth, theme, desc.LabelIndent);
		if (!desc.Trailing.empty())
		{
			const float size = theme.FontSizeSmall;
			const float width = ctx.MeasureTextWidth(desc.Trailing, size);
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ row.X + row.W - actionReserve - kPropertyActionGap - width, row.Y + (row.H - size) * 0.5f, 0, 0 },
				theme.TextMuted, 0, 1.0f, desc.Trailing, size, false });
		}
		const float toggleWidth = desc.ToggleWidth >= 0.0f ? desc.ToggleWidth
			: std::max(0.0f, row.W - actionReserve);
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		result.Toggled = desc.Enabled && (ctx.IsClicked({ row.X, row.Y, toggleWidth, row.H }) || keyActivated);
		RegisterRowNode(id, "button", row, desc.A11yLabel.empty() ? desc.Label : desc.A11yLabel,
			desc.A11yValue.empty() ? (desc.Open ? "open" : "closed") : desc.A11yValue, desc.Enabled, true, focused,
			desc.Tooltip);
		ctx.RegisterFocusable(id, row);
		DrawFocusRing(ctx, row, id, theme);
		if (hovered)
		{
			if (desc.Enabled && ctx.Input().MousePos.x <= row.X + toggleWidth)
				ctx.SetCursor(WuiCursor::Hand);
			if (!desc.Tooltip.empty())
				Tooltip(ctx, row, desc.Tooltip);
		}
		// VEC-H6:集合头同口径 —— 集合里有任一元素/键值/形状偏离默认才画 ↺(占位槽不变)。
		// VEC-H7:本控件自己登记 a11y(kind=button + label/value/enabled/tooltip),这里不再补登记。
		if (desc.ShowReset && desc.ResetModified)
			result.ResetClicked = ResetDefaultButton(ctx, desc.ResetId, result.ResetRect, desc.ResetEnabled, theme,
				desc.ResetLabel, desc.ResetTooltip);
		if (desc.ShowRemove)
			result.RemoveClicked = RowActionButton(ctx, desc.RemoveId, result.RemoveRect, "x", desc.RemoveLabel,
				desc.RemoveTooltip, desc.RemoveEnabled, theme, true);
		return result;
	}


CollectionRowResult CollectionRow(WuiContext& ctx, WuiId id, const WuiRect& row, const CollectionRowDesc& desc, const WuiTheme& theme){
		CollectionRowResult result;
		const float indent = std::max(0.0f, desc.Indent);
		const WuiRect content { row.X + indent, row.Y, std::max(0.0f, row.W - indent), row.H };
		int insideIndex = 0;
		int outsideIndex = 0;
		float actionReserve = 0.0f;
		if (desc.ShowReset)
		{
			result.ResetRect = InsideActionRect(content, insideIndex++);
			actionReserve += kPropertyActionWidth + kPropertyActionGap;
		}
		if (desc.ShowRemove)
		{
			result.RemoveRect = desc.ActionsOutside ? OutsideActionRect(content, outsideIndex++)
				: InsideActionRect(content, insideIndex++);
			if (!desc.ActionsOutside)
				actionReserve += kPropertyActionWidth + kPropertyActionGap;
		}
		if (desc.ShowAdd)
		{
			result.AddRect = desc.ActionsOutside ? OutsideActionRect(content, outsideIndex++)
				: InsideActionRect(content, insideIndex++);
			if (!desc.ActionsOutside)
				actionReserve += kPropertyActionWidth + kPropertyActionGap;
		}
		const float labelWidth = RowLabelWidth(content, desc.LabelWidth);
		result.FieldRect = { content.X + labelWidth, content.Y + (content.H - kPropertyFieldHeight) * 0.5f,
			std::max(24.0f, content.W - labelWidth - kPropertyFieldGutter - actionReserve), kPropertyFieldHeight };
		if (desc.FieldHeight > 0.0f)
		{
			result.FieldRect.H = desc.FieldHeight;
			result.FieldRect.Y = content.Y + (content.H - desc.FieldHeight) * 0.5f;
		}
		const bool hovered = ctx.IsHovered(content);
		RowHoverBackdrop(ctx, content, hovered, desc.Enabled, theme);
		if (desc.Modified)
			DrawModifiedDot(ctx, content, theme);
		DrawRowIndentGuide(ctx, content, desc.LabelIndent, theme);
		DrawRowLabel(ctx, content, desc.Label, desc.Term, false, false,
			desc.Enabled ? theme.Text : theme.TextDisabled, labelWidth, theme, desc.LabelIndent);
		if (!desc.FieldPlaceholder.empty())
		{
			Wui::Label(ctx,
				{ result.FieldRect.X + kPropertyPlaceholderPadX,
					result.FieldRect.Y + (result.FieldRect.H - kPropertyLabelSize) * 0.5f - 1.0f },
				desc.FieldPlaceholder, desc.Enabled ? theme.TextMuted : theme.TextDisabled, kPropertyLabelSize);
		}
		RegisterRowNode(id, desc.A11yKind, content, desc.A11yLabel.empty() ? desc.Label : desc.A11yLabel,
			desc.A11yValue, desc.A11yEnabled, desc.A11yEnabled, false, desc.Tooltip);
		if (hovered && !desc.Tooltip.empty())
			Tooltip(ctx, content, desc.Tooltip);
		// VEC-H6:元素/键值行同口径(单项 ↺ 只在偏离默认时出现)。
		// VEC-H7:本控件自己登记 a11y(kind=button + label/value/enabled/tooltip),这里不再补登记。
		if (desc.ShowReset && desc.ResetModified)
			result.ResetClicked = ResetDefaultButton(ctx, desc.ResetId, result.ResetRect, desc.ResetEnabled, theme,
				desc.ResetLabel, desc.ResetTooltip);
		if (desc.ShowRemove)
			result.RemoveClicked = RowActionButton(ctx, desc.RemoveId, result.RemoveRect, "-", std::string(),
				desc.RemoveTooltip, desc.RemoveEnabled, theme, true);
		if (desc.ShowAdd)
			result.AddClicked = RowActionButton(ctx, desc.AddId, result.AddRect, "+", std::string(), desc.AddTooltip,
				desc.AddEnabled, theme, false);
		return result;
	}


bool CollectionActionButton(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& glyph, const std::string& tooltip, bool enabled, const WuiTheme& theme, bool danger){
		return RowActionButton(ctx, id, rect, glyph, std::string(), tooltip, enabled, theme, danger);
	}


bool ActionButton(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme, bool enabled, const std::string& tooltip, bool danger){
		const bool hoveredAny = ctx.IsHovered(rect);
		const bool hovered = enabled && hoveredAny;
		const bool focused = enabled && ctx.Focus() == id;
		const WuiColor fill = enabled ? (hovered ? theme.ButtonHover : theme.ButtonBg) : theme.PanelBg;
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, fill, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
			enabled ? (hovered ? (danger ? theme.Danger : theme.Accent) : theme.Border) : theme.Border, 3.0f, 1.0f });
		const float size = kPropertyLabelSize;
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ rect.X + 8.0f, rect.Y + (rect.H - size) * 0.5f - 1.0f, 0, 0 },
			enabled ? (danger && hovered ? theme.Danger : theme.Text) : theme.TextDisabled, 0, 1.0f, label, size,
			false });
		RegisterRowNode(id, "button", rect, label, tooltip, enabled, enabled, focused, tooltip);
		if (enabled)
			ctx.RegisterFocusable(id, rect);
		DrawFocusRing(ctx, rect, id, theme);
		if (hoveredAny)
		{
			if (enabled)
				ctx.SetCursor(WuiCursor::Hand);
			if (!tooltip.empty())
				Tooltip(ctx, rect, tooltip);
		}
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		return enabled && (ctx.IsClicked(rect) || keyActivated);
	}


bool OpenInEditorButton(WuiContext& ctx, WuiId id, const WuiRect& rect, const WuiTheme& theme, bool enabled, const std::string& label, const std::string& tooltip){
		if (rect.W <= 2.0f || rect.H <= 2.0f)
			return false;
		// VEC-H6:与 RowActionButton 同一套状态语法(禁用态也要给理由:tooltip 就是那句理由)。
		const bool hoveredAny = ctx.IsHovered(rect);
		const bool hovered = enabled && hoveredAny;
		const bool focused = enabled && ctx.Focus() == id;
		const bool pressed = hovered && ctx.Input().MouseDown[0];
		const WuiColor fill = enabled
			? (pressed ? theme.ActiveBg : (hovered ? theme.ButtonHover : theme.ButtonBg)) : theme.PanelBg;
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, fill, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
			enabled ? (hovered ? theme.Accent : theme.Border) : theme.Border, 3.0f, 1.0f });
		// 纯矢量 glyph `</>`:两段折线尖括号 + 一道斜线。几何按控件短边等比缩放,20x20 与 24x24 同样清楚。
		const WuiColor color = enabled ? theme.Text : theme.TextDisabled;
		const float cx = rect.X + rect.W * 0.5f;
		const float cy = rect.Y + rect.H * 0.5f;
		const float side = std::min(rect.W, rect.H);
		// 几何按短边等比:20x20 的图标位里 `</>` 三个笔画都要能分辨。实测两个坑(都靠 ASCII 像素图复核):
		// ① 斜线太陡 → 与尖括号糊成 "<>";② 斜线太长 → 端点贴上尖括号内侧,整体读成 ")( "。
		// 所以:尖括号让到外侧(gap × reach)、斜线只占中段(明显短于尖括号高度)。
		const float half = std::max(3.0f, side * 0.28f);          // 尖括号半高
		const float reach = std::max(2.4f, side * 0.14f);         // 尖括号横跨
		const float gap = std::max(3.4f, side * 0.21f);           // 中心到尖括号内侧端点的留白(给斜线让位)
		const float slashX = std::max(1.4f, side * 0.09f);        // 斜线水平半宽
		const float slashY = std::max(2.6f, side * 0.17f);        // 斜线纵向半高(短于 half ⇒ 不会贴到尖括号端点)
		constexpr float kStroke = 1.6f;
		// 左 `<`
		PushLineQuad(ctx, { cx - gap, cy - half }, { cx - gap - reach, cy }, kStroke, color);
		PushLineQuad(ctx, { cx - gap - reach, cy }, { cx - gap, cy + half }, kStroke, color);
		// 右 `>`
		PushLineQuad(ctx, { cx + gap, cy - half }, { cx + gap + reach, cy }, kStroke, color);
		PushLineQuad(ctx, { cx + gap + reach, cy }, { cx + gap, cy + half }, kStroke, color);
		// 中缝的 `/`(读成代码,而不是一对普通箭头)
		PushLineQuad(ctx, { cx - slashX, cy + slashY }, { cx + slashX, cy - slashY }, kStroke, color);
		// 无障碍:图标按钮没有可见文字 —— label/value/tooltip 三处都要有语义(读屏与脚本共用)。
		RegisterRowNode(id, "button", rect, label.empty() ? std::string("Open in editor") : label, tooltip,
			enabled, enabled, focused, tooltip);
		if (enabled)
			ctx.RegisterFocusable(id, rect);
		DrawFocusRing(ctx, rect, id, theme);
		if (hoveredAny)
		{
			if (enabled)
				ctx.SetCursor(WuiCursor::Hand);
			if (!tooltip.empty())
				Tooltip(ctx, rect, tooltip);
		}
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		return enabled && (ctx.IsClicked(rect) || keyActivated);
	}

namespace WuiWidgetsDetail
{
		// TextField / TextFieldEx 的共同实现(旧签名语义逐条不变,只是多了 error 参数):
		// error 非空时描边用 theme.Danger,并把 "error=<文本>" 追加进无障碍节点 value(TextFieldEx 的契约)。
bool TextFieldCore(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, bool* cancelledOut, const std::string& error, const TextFieldA11y* a11y ){
			// P4-U5a:label/value 都不能空 —— 空输入时 value 用占位文案(与用户看到的一致),
			// label 用控件名;两者都由调用方给出(文本控件不知道自己的业务语义)。
			std::string accessValue = error.empty() ? buffer : (buffer + " error=" + error);
			if (accessValue.empty() && a11y)
				accessValue = a11y->Placeholder;
			RegisterAccessNode(id, "text-field", rect, a11y ? a11y->Label : std::string(),
				accessValue, true, true, ctx.Focus() == id);
			ctx.RegisterFocusable(id, rect);
			WuiEditState& state = ctx.Persist<WuiEditState>(id, {});
			// PROJ-17:这一次按下是否落在本框内 —— 下面"焦点进入"要区分"点进来"和"Tab/程序进来"。
			const bool clickedInField = ctx.Input().MouseClicked[0] && ctx.IsHovered(rect);
			// 仅在按下的那一帧初始化拖选锚点;按住期间持续更新选区。
			if (clickedInField)
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
			// PROJ-17(用户 2026-09-29:"位置那里,删除按一下就把路径全删了"):选区只活在
			// **当前焦点会话**里。光标/选区按控件 id 长期持久化,而选区高亮只在持有焦点时绘制 ⇒
			// 上一次会话留下的"整段选中"在重新打开对话框或用 Tab 进框时是**看不见的**,第一次
			// Delete/Backspace 就会按整段选区执行,把整条路径一次吃光。
			//   ① 焦点进入(不是点进来)⇒ 丢弃遗留选区,光标落到末尾,与"第一次点开这个框"一致;
			//   ② 失焦 ⇒ 立刻丢弃选区与拖选锚点,不给下一次留下看不见的状态。
			if (focused)
			{
				if (!clickedInField && !state.WasFocused)
				{
					state.Cursor = Utf8Count(buffer);
					state.SelStart = -1;
					state.SelEnd = -1;
					state.DragAnchor = -1;
					state.MouseSelecting = false;
				}
			}
			else
			{
				state.SelStart = -1;
				state.SelEnd = -1;
				state.MouseSelecting = false;
			}
			state.WasFocused = focused;
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

bool TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, bool* cancelledOut, const TextFieldA11y* a11y){
		// 旧签名语义不变:与 TextFieldEx 共用同一条实现,error 恒为空。
		return TextFieldCore(ctx, id, rect, buffer, theme, cancelledOut, std::string(), a11y);
	}


bool TextFieldEx(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, const std::string& error, const TextFieldA11y* a11y){
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


void Image(WuiContext& ctx, const WuiRect& rect, uint64_t textureId, const WuiRect& uv, const WuiTheme& theme){
		ctx.Commands().push_back({ WuiDrawKind::Image, rect, theme.Text, 0, 1.0f, "", 15.0f, false, textureId, uv });
	}


	// P1c-LIB2:渐变填充原语 —— 一条 Gradient 命令,Kind/Rect/Corners 三项就是全部画面输入。
	// 与面板手写渐变的等价判据:同一 rect、同一四角颜色、同一顺序 ⇒ 逐字节相同的命令。
void GradientFill(WuiContext& ctx, const WuiRect& rect, const WuiColor& topLeft, const WuiColor& topRight, const WuiColor& bottomRight, const WuiColor& bottomLeft){
		WuiDrawCommand command;
		command.Kind = WuiDrawKind::Gradient;
		command.Rect = rect;
		command.Corners = { topLeft, topRight, bottomRight, bottomLeft };
		ctx.Commands().push_back(std::move(command));
	}


void GradientFill(WuiContext& ctx, const WuiRect& rect, const WuiColor& from, const WuiColor& to, bool vertical){
		if (vertical)
			GradientFill(ctx, rect, from, from, to, to);
		else
			GradientFill(ctx, rect, from, to, to, from);
	}


	// P1c-LIB3:线段原语 —— 与 ViewportPanel::PushProjectedSegment 末段逐字段等价的那一步:
	// 方向 d = to - from、退化阈值 0.5、法线 (-d.y, d.x)/|d|、偏移 = 法线*thickness/2、
	// 顶点 {from+n, to+n, to-n, from-n}。投影/近平面裁剪/夹取留在调用方(库件不猜几何语义)。
void LineSegment(WuiContext& ctx, const glm::vec2& from, const glm::vec2& to, const WuiColor& color, float thickness){
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


	// M4-TEX-P7:Combo / SearchableCombo 共用的**绘制期**中间省略(声明与口径见 WuiWidgets.h)。
	// 背景(用户 2026-09-25「现在选择不如之前」):贴图下拉曾被面板提前截断成 `textures/Ico…g`,
	// 搜索/读屏/回显都拿不到完整路径;数据侧(选项字符串)必须保持完整,越界只在**画之前**裁。
	// 算法逐条对齐编辑器侧 MaterialEditorPanel::EllipsizeMiddleToWidth:先测整串(装得下原样返回),
	// 再按"尾部保留 1 个码点 → 逐步放宽尾部"的顺序取**最长的头**;只对调用方真的要画的那一条文本度量。
std::string EllipsizeMiddleToWidth(const WuiContext& ctx, std::string_view text, float width, float fontSize){
		if (text.empty() || width <= 0.0f)
			return {};
		if (ctx.MeasureTextWidth(text, fontSize) <= width)
			return std::string(text);
		const std::string dots = "…";
		std::vector<size_t> boundaries;
		boundaries.reserve(text.size());
		for (size_t index = 0; index < text.size(); ++index)
			if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80)
				boundaries.push_back(index);
		for (size_t tail = boundaries.size(); tail > 0; --tail)
		{
			for (size_t head = tail; head > 0; --head)
			{
				std::string candidate(text.substr(0, boundaries[head - 1]));
				candidate += dots;
				candidate.append(text.substr(boundaries[tail - 1]));
				if (ctx.MeasureTextWidth(candidate, fontSize) <= width)
					return candidate;
			}
		}
		return dots;
	}


bool Combo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const std::vector<std::string>& options, int& selected, const WuiTheme& theme){
		const bool focused = ctx.Focus() == id;
		// CPPT-5(2026-09-28):selected 越界(-1 = 无选中,或 >= size 的陈旧下标)时**不得**索引 options ——
		// 当前值只在这里做一次受保护计算,a11y 节点与绘制命令共用;selected 仅在新弹层里点选条目时改写。
		// 背景:空 ScriptName(C++) + 非空脚本注册表 ⇒ selected=-1,旧实现里绘制命令直接 options[-1],
		// 触发 debug STL "vector subscript out of range" 断言(与 a11y 已有的边界检查不一致)。
		const std::string current = (selected >= 0 && selected < static_cast<int>(options.size()))
			? options[selected] : std::string();
		RegisterAccessNode(id, "combo", rect, label, current, true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const bool hovered = ctx.IsHovered(rect);
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, hovered ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });
		// 下拉提示:右下角两段细线组成的小折角(不依赖字体里的箭头字形)。
		const float caretX = rect.X + rect.W - 12.0f;
		const float caretY = rect.Y + rect.H * 0.5f - 3.0f;
		// P4-UX1:下拉只画**当前值**(label 仅用于无障碍节点)。
		// 面板的排版约定是"标签在左、控件在右",把 label 也画进框里会出现
		// "阴影贴图: 2048" / "界面语言: 简体中文" 这种重复且臃肿的文本。
		// M4-TEX-P7:当前值超出可用宽(左内边距 6 → 折角左侧再留 4px 呼吸距离)时,只在这条**绘制命令**里
		// 做中间省略;options / a11y value / 返回值保持完整(搜索、读屏、回显都拿完整路径)。
		const float valueBudget = caretX - (rect.X + 6.0f) - 4.0f;
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
			theme.Text, 0, 1.0f, EllipsizeMiddleToWidth(ctx, current, valueBudget, 15.0f), 15.0f, false });
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
				// M4-TEX-P7:候选行同样只在**绘制命令**里省略(条目没有箭头/滚动条 ⇒ 右侧留 6px 呼吸距离,
				// 文本不会越过条目矩形);上面的 a11y 节点与点击写回的 selected 仍是完整选项串。
				ctx.Commands().push_back({ WuiDrawKind::Text, { item.X + 6.0f, item.Y + 3.0f, 0, 0 }, theme.Text, 0, 1.0f,
					EllipsizeMiddleToWidth(ctx, options[i], item.W - 12.0f, 15.0f), 15.0f, false });
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
bool SearchableCombo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const std::vector<std::string>& options, int& selected, const WuiTheme& theme){
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
		{
			// M4-TEX-P7:右侧给 "v"/"^" 箭头(画在 rect.X + rect.W - 18)留 4px 呼吸距离;当前值超宽时
			// 只在这条**绘制命令**里中间省略 —— 上面的 a11y value(current)保持完整路径。
			const float valueBudget = (rect.X + rect.W - 22.0f) - (fieldRect.X + 6.0f);
			ctx.Commands().push_back({ WuiDrawKind::Text, { fieldRect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
				theme.Text, 0, 1.0f, EllipsizeMiddleToWidth(ctx, current, valueBudget, 15.0f), 15.0f, false });
		}
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
			// M4-TEX-P7:可见候选行只在**绘制命令**里中间省略(条目右侧留 6px 呼吸距离);上面两个 a11y
			// 节点(combo-option / combo-item)的 label 仍是完整选项串 —— 脚本按完整路径点选与搜索都不受影响。
			ctx.Commands().push_back({ WuiDrawKind::Text, { item.X + 6.0f, item.Y + 3.0f, 0, 0 },
				theme.Text, 0, 1.0f, EllipsizeMiddleToWidth(ctx, options[optionIndex], item.W - 12.0f, 15.0f),
				15.0f, false });
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


bool TreeNode(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool leaf, const WuiTheme& theme){
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


bool BeginMenuBar(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme){
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.PanelHeader, 0.0f });
		return true;
	}


void EndMenuBar(WuiContext& ctx){
		(void)ctx;
	}


bool BeginMenu(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme){
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


void EndMenu(WuiContext& ctx, WuiId id, const WuiRect& panel, const WuiTheme& theme){
		ctx.ClosePopupsOnOutsideClick({ id }, panel);
		if (ctx.IsKeyPressed(KeyCodes::Escape))
			ctx.ClosePopup(id);
	}


bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool enabled, const WuiTheme& theme){
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


bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool checked, bool enabled, const WuiTheme& theme){
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


bool BeginModal(WuiContext& ctx, WuiId id, const std::string& title, const glm::vec2& size, WuiRect* panel, const WuiTheme& theme){
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


void EndModal(WuiContext& ctx, WuiId id){
		(void)ctx;
		(void)id;
		ctx.PopOverlay();
	}


bool BeginScrollArea(WuiContext& ctx, const WuiRect& viewport, float contentHeight, float& scrollY, const WuiTheme& theme, WuiId id){
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


void EndScrollArea(WuiContext& ctx){
		ctx.Commands().push_back({ WuiDrawKind::ClipPop });
		ctx.PopClipRect();
	}


	// P1c-LIB2:有状态滚动条 —— 位置读得出(value="scroll=<y>/<max> ratio=<0..1>")、也设得进
	// (拖滑块 / 点轨道 / 上下按钮 / 键盘)。几何与 BeginScrollArea 的滚动口径同源,滚轮不在这。
bool ScrollBar(WuiContext& ctx, WuiId id, const WuiRect& rect, float contentHeight, float viewportHeight, float& scrollY, const WuiTheme& theme, bool pageButtons){
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


WuiRect TableCell(const WuiRect& table, const std::vector<float>& columns, size_t row, size_t column, float rowHeight){
		float x = table.X;
		for (size_t i = 0; i < column && i < columns.size(); ++i)
			x += columns[i];
		const float width = column < columns.size() ? columns[column] : table.W;
		return { x, table.Y + rowHeight * static_cast<float>(row), width, rowHeight };
	}

}
