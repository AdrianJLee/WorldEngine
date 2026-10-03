#include "WuiComponentRegistry_Internal.h"

namespace World::Wui
{

using namespace WuiComponentRegistryDetail;

namespace WuiComponentRegistryDetail
{

		// ---- P1a:面板里的保留模式件(WuiLabel / WuiImage / WuiBox / WuiSpacer / WuiListRow)----
		// 这五件只在面板里以**对象树**出现(Editor/src/WUI/Panels/**),showcase 走与 WuiProgress
		// 完全相同的路径:建树 → LayoutWidgetTree → WuiPaintContext::Paint —— 不是复刻 demo。

void PaintRetained(const WuiComponentDraw& draw, const WuiWidgetPtr& root, const WuiRect& rect){
			LayoutWidgetTree(root, rect);
			WuiPaintContext paint(*draw.Context);
			root->Paint(paint);
		}


WuiWidgetPtr RetainedLabel(const std::string& text, float fontSize, const WuiColor& color, bool bold ){
			auto label = std::make_shared<WuiLabel>();
			label->Text = text;
			label->FontSize = fontSize;
			label->Color = color;
			label->Bold = bold;
			return label;
		}


void ShowSeparator(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const Slot slot = Canvas(draw, *draw.Theme, 220.0f, 12.0f);
			const WuiId id = BeginShowcase(draw, "separator", "Separator", slot.Rect);
			(void)id;
			// WuiSeparator 是面板侧的保留模式件(与 WuiLabel/WuiProgress 同一条路径:
			// 建对象 → LayoutWidgetTree → Paint)。线色取控件自己的 Theme(Border 令牌)、
			// 厚度 = thickness 属性 —— showcase 不另画一条假线。
			auto separator = std::make_shared<WuiSeparator>();
			separator->Thickness = DrivenFloat(ctx, "showcase.separator.thickness", draw, "thickness",
				1.0f, 0.5f, 6.0f) * slot.Scale;
			separator->Theme = draw.Theme;
			PaintRetained(draw, separator, slot.Rect);
		}


void ShowLabel(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 220.0f, 20.0f);
			const WuiId id = BeginShowcase(draw, "label", "Label", slot.Rect);
			(void)id;
			auto label = std::make_shared<WuiLabel>();
			label->Text = MaybeLongText(draw, LocalizedText(draw, "text", "Player Name", "玩家名称"));
			label->FontSize = DrivenFloat(ctx, "showcase.label.fontSize", draw, "fontSize", 15.0f, 8.0f, 32.0f) * slot.Scale;
			label->Bold = BoolProperty(draw, "bold", false);
			label->Color = theme.Text;
			PaintRetained(draw, label, slot.Rect);
		}


void ShowImage(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 96.0f, 96.0f);
			const WuiId id = BeginShowcase(draw, "image", "Image", slot.Rect);
			(void)id;
			// WuiImage 只画纹理:纹理 id 由宿主用 WuiTextureRegistry 注册后交给面板(视口/预览都是
			// 这条路径),默认 0 = 空图 —— 控件不造假外观。工作台可用 textureId 属性填一个已注册的
			// 纹理 id 看真实效果(tint 只影响着色,不会凭空造出内容)。
			auto image = std::make_shared<WuiImage>();
			image->TextureId = static_cast<uint64_t>(DrivenInt(ctx, "showcase.image.textureId", draw, "textureId", 0, 0, 100000));
			image->Uv = { 0.0f, 0.0f, 1.0f, 1.0f };
			const glm::vec4 tint = DrivenColor(ctx, "showcase.image.tint", draw, "tint", glm::vec4 { 1.0f, 1.0f, 1.0f, 1.0f });
			image->Tint = WuiColor { tint.r, tint.g, tint.b, tint.a };
			PaintRetained(draw, image, slot.Rect);
		}


		// P1c-LIB1:徽标(W3.1 的"粗体圆形/胶囊字标"缺件)。同一帧画两枚:
		// ① 默认(中性底,填充/字色/字号/粗体都可被属性覆盖);② 变色态(强调色底 + 反白字),
		// 对应面板里的"类型徽标"(Prefab/着色器)——工作台一次就能看到两种着色。
void ShowBadge(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 168.0f, 18.0f);
			const WuiId id = BeginShowcase(draw, "badge", "Badge", slot.Rect);
			(void)id;
			const float fontSize = DrivenFloat(ctx, "showcase.badge.fontSize", draw, "fontSize",
				theme.FontSizeCaption, 8.0f, 20.0f) * slot.Scale;
			const bool bold = BoolProperty(draw, "bold", true);
			const std::string text = MaybeLongText(draw, LocalizedText(draw, "text", "Material Shader", "着色器"));
			const glm::vec4 fill = DrivenColor(ctx, "showcase.badge.fill", draw, "fill",
				glm::vec4 { theme.ActiveBg.R, theme.ActiveBg.G, theme.ActiveBg.B, theme.ActiveBg.A });
			const glm::vec4 textColor = DrivenColor(ctx, "showcase.badge.textColor", draw, "textColor",
				glm::vec4 { theme.Text.R, theme.Text.G, theme.Text.B, theme.Text.A });
			const float corner = slot.Rect.H * 0.5f;
			const float width = std::min(slot.Rect.W, 96.0f * slot.Scale);
			Badge(ctx, { slot.Rect.X, slot.Rect.Y, width, slot.Rect.H }, text,
				WuiColor { fill.r, fill.g, fill.b, fill.a },
				WuiColor { textColor.r, textColor.g, textColor.b, textColor.a }, theme, fontSize, bold, corner);
			// ② 变色态:强调色底 + 反白字(面板里的 Prefab/着色器徽标就是这一档)。
			const float accentX = slot.Rect.X + width + 8.0f * slot.Scale;
			const float accentWidth = std::min(56.0f * slot.Scale,
				std::max(0.0f, slot.Rect.X + slot.Rect.W - accentX));
			if (accentWidth > 8.0f)
				Badge(ctx, { accentX, slot.Rect.Y, accentWidth, slot.Rect.H },
					LocalizedText(draw, "accentText", "Prefab", "预制体"),
					theme.Accent, WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }, theme, fontSize, bold, corner);
		}


		// P1c-LIB1:染色图标(W3.2 的"按资产类型染色图标"缺件)。纹理 id 口径与 WuiImage 一致;
		// 默认 textureId=0 = 空图 → Icon 自己的兜底占位(底 + 棋盘 + 描边),所以工作台里
		// 这件**不需要** image/spacer 那种像素断言豁免。
void ShowIcon(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 32.0f, 32.0f);
			const WuiId id = BeginShowcase(draw, "icon", "Icon", slot.Rect);
			(void)id;
			const uint64_t textureId = static_cast<uint64_t>(
				DrivenInt(ctx, "showcase.icon.textureId", draw, "textureId", 0, 0, 100000));
			const glm::vec4 tint = DrivenColor(ctx, "showcase.icon.tint", draw, "tint",
				glm::vec4 { 1.0f, 1.0f, 1.0f, 1.0f });
			Icon(ctx, slot.Rect, textureId, { 0.0f, 0.0f, 1.0f, 1.0f },
				WuiColor { tint.r, tint.g, tint.b, tint.a }, theme);
		}


void ShowBox(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 240.0f, 84.0f);
			const WuiId id = BeginShowcase(draw, "box", "Box (Layout)", slot.Rect);
			(void)id;
			auto box = std::make_shared<WuiBox>();
			box->Direction = (draw.State == "row" || TrimmedLower(TextProperty(draw, "direction", "column")) == "row")
				? WuiDirection::Row
				: WuiDirection::Column;
			box->Gap = DrivenFloat(ctx, "showcase.box.gap", draw, "gap", 6.0f, 0.0f, 24.0f) * slot.Scale;
			box->AlignCross = WuiAlign::Stretch;
			box->Add(RetainedLabel(LocalizedText(draw, "first", "Header row", "标题行"), 15.0f * slot.Scale, theme.Text));
			box->Add(RetainedLabel(MaybeLongText(draw, LocalizedText(draw, "second", "Body text", "正文")),
				14.0f * slot.Scale, theme.TextMuted));
			box->Add(RetainedLabel(LocalizedText(draw, "third", "Footer row", "页脚行"), 13.0f * slot.Scale, theme.TextMuted));
			PaintRetained(draw, box, slot.Rect);
		}


void ShowSpacer(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 220.0f, 20.0f);
			const WuiId id = BeginShowcase(draw, "spacer", "Spacer", slot.Rect);
			(void)id;
			// WuiSpacer::Paint 是空的 —— 它不画像素、只撑开**布局间距**。showcase 用两个真实
			// WuiLabel 夹一个 spacer,把"它撑开的距离"画出来(而不是画一个假占位块)。
			auto row = std::make_shared<WuiBox>();
			row->Direction = WuiDirection::Row;
			row->AlignCross = WuiAlign::Center;
			row->Add(RetainedLabel(LocalizedText(draw, "left", "Left", "左"), 15.0f * slot.Scale, theme.Text));
			auto spacer = std::make_shared<WuiSpacer>();
			spacer->Width = DrivenFloat(ctx, "showcase.spacer.width", draw, "width", 48.0f, 0.0f, 240.0f) * slot.Scale;
			spacer->Height = 1.0f;
			row->Add(spacer);
			row->Add(RetainedLabel(LocalizedText(draw, "right", "Right", "右"), 15.0f * slot.Scale, theme.Text));
			PaintRetained(draw, row, slot.Rect);
		}


void ShowListRow(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 224.0f, 72.0f);
			const WuiId id = BeginShowcase(draw, "listrow", "List Row", slot.Rect);
			// P1c-E4:行进了焦点表 —— Enter/Space(焦点在行上)= 与点击同一个 OnClick。
			// 演示里"点击/激活"切换本行选中,所以键盘激活在画布与节点 value 上都看得见。
			bool& selectedState = BoolState(ctx, "showcase.listrow.selected", false);
			auto column = std::make_shared<WuiBox>();
			column->Direction = WuiDirection::Column;
			column->Gap = 2.0f * slot.Scale;
			column->Add(RetainedLabel(LocalizedText(draw, "root", "Assets", "资源"), 13.0f * slot.Scale, theme.TextMuted));

			auto row = std::make_shared<WuiListRow>();
			row->SetId(id);   // 行自己登记 kind=list-row(与层级面板同一条 a11y 契约),覆盖外壳锚点
			row->Text = MaybeLongText(draw, LocalizedText(draw, "label", "Textures/Icon.png", "textures/Icon.png"));
			row->FontSize = 14.0f * slot.Scale;
			row->Indent = DrivenFloat(ctx, "showcase.listrow.indent", draw, "indent", 14.0f, 0.0f, 32.0f) * slot.Scale;
			if (const std::optional<bool> forced = BoolOverride(draw, "selected"))
				selectedState = *forced;
			row->Selected = draw.State == "selected" || selectedState;
			row->AccessValue = row->Selected ? "depth=1 selected=true" : "depth=1 selected=false";
			row->OnClick = [&selectedState]() { selectedState = !selectedState; };
			column->Add(row);

			column->Add(RetainedLabel(LocalizedText(draw, "sibling", "Scenes", "场景"), 13.0f * slot.Scale, theme.TextMuted));
			// hover 伪状态:鼠标落到画布中心 ≈ 中间那行的位置(布局变化时仍落在行内)。
			PseudoState pseudo(draw, id, slot.Rect);
			PaintRetained(draw, column, slot.Rect);
		}


		// ---- P1c-LIB2 渐变填充 / 可折叠分区标题 / 禁用+理由按钮 / 有状态滚动条 + P1c-LIB3 线段原语 ----

void ShowGradient(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 220.0f, 64.0f);
			const WuiId id = BeginShowcase(draw, "gradient", "Gradient Fill", slot.Rect);
			(void)id;   // 纯绘制原语:外框锚点已是唯一节点,不再自己登记
			glm::vec4& top = DrivenColor(ctx, "showcase.gradient.top", draw, "top", { 0.24f, 0.27f, 0.33f, 1.0f });
			glm::vec4& bottom = DrivenColor(ctx, "showcase.gradient.bottom", draw, "bottom", { 0.05f, 0.06f, 0.08f, 1.0f });
			const WuiColor from { top.r, top.g, top.b, top.a };
			const WuiColor to { bottom.r, bottom.g, bottom.b, bottom.a };
			// 方向是本件唯一的可变视觉口径:default = 上→下,horizontal = 左→右。
			GradientFill(ctx, slot.Rect, from, to, draw.State != "horizontal");
		}


		// P1c-LIB3:线段原语的 showcase —— 两个方向态,证明"斜线也是同一条旋转四边形"。
void ShowLineSegment(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 220.0f, 64.0f);
			const WuiId id = BeginShowcase(draw, "line-segment", "Line Segment", slot.Rect);
			(void)id;   // 纯绘制原语:外框锚点已是唯一节点,不再自己登记
			const float thickness = DrivenFloat(ctx, "showcase.line-segment.thickness", draw, "thickness",
				2.0f, 0.5f, 16.0f);
			glm::vec4& tint = DrivenColor(ctx, "showcase.line-segment.color", draw, "color",
				{ 0.894f, 0.753f, 0.541f, 1.0f });   // #E4C08A
			const WuiColor stroke { tint.r, tint.g, tint.b, tint.a };
			const float inset = 8.0f;
			// default = 水平线段(法线 = ±y 方向);diagonal = 左下→右上斜线且厚度 x2(法线/顶点顺序在
			// 斜线上才有可见差别)。两个态都是同一公式,端点由本 showcase 给(与面板调用方式一致)。
			if (draw.State == "diagonal")
				LineSegment(ctx, { slot.Rect.X + inset, slot.Rect.Y + slot.Rect.H - inset },
					{ slot.Rect.X + slot.Rect.W - inset, slot.Rect.Y + inset }, stroke, thickness * 2.0f);
			else
				LineSegment(ctx, { slot.Rect.X + inset, slot.Rect.Y + slot.Rect.H * 0.5f },
					{ slot.Rect.X + slot.Rect.W - inset, slot.Rect.Y + slot.Rect.H * 0.5f }, stroke, thickness);
		}


void ShowCollapsibleHeader(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 260.0f, 24.0f);
			const WuiId id = BeginShowcase(draw, "collapsible", "Section Header (Collapsible)", slot.Rect);
			bool& openState = BoolState(ctx, "showcase.collapsible.open", true);
			// 覆盖优先级:state(collapsed)> 属性(open)> 持久槽(真实点击的结果)。覆盖**只影响本帧**,
			// 只有"没有任何覆盖"的帧才把结果写回槽 —— 否则一次合成状态/一次误触就会把默认态永久改掉
			// (实测:先跑 gradient 再跑 collapsible 时,默认态会被写成 closed)。
			const std::optional<bool> forcedOpen = BoolOverride(draw, "open");
			const bool overridden = draw.State == "collapsed" || forcedOpen.has_value();
			bool open = forcedOpen.value_or(draw.State == "collapsed" ? false : openState);
			PseudoState pseudo(draw, id, slot.Rect);
			CollapsibleHeader(ctx, id, slot.Rect,
				LocalizedText(draw, "title", "Shader Parameters", "着色器参数"), open, theme,
				LocalizedText(draw, "term", "", ""),
				LocalizedText(draw, "trailing", "12 items", "12 项"),
				LocalizedText(draw, "tooltip", "Parameters declared by the shader this material references.",
					"材质的着色器声明的参数。"),
				14.0f * slot.Scale);
			if (!overridden)
				openState = open;
		}


void ShowButtonEx(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 148.0f);
			const WuiId id = BeginShowcase(draw, "button.disabled", "Button (Disabled + Reason)", slot.Rect);
			const bool enabled = draw.State != "disabled" && !BoolProperty(draw, "disabled", false);
			const bool primary = draw.State == "primary" || BoolProperty(draw, "primary", false);
			PseudoState pseudo(draw, id, slot.Rect, true);
			ButtonEx(ctx, id, slot.Rect,
				MaybeLongText(draw, LocalizedText(draw, "label", "Assign", "指定")), theme, enabled, primary,
				LocalizedText(draw, "reason", "Select a material instance first", "先选择一个材质实例"));
		}


		// ---- VEC-H2:属性行 / 折叠分组头 / 集合行 / 集合动作按钮 ----
		// showcase 全部走真实控件路径:行结构由库件画,值列里是真实字段控件(DragFloat),
		// 行尾动作是库件自己的按钮 —— 工作台看到的与属性面板里是同一份代码。
std::string FloatText3(float value){
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3f", value);
			return buffer;
		}


void ShowPropertyRow(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme theme = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 280.0f, PropertyRowHeight());
			const WuiId id = BeginShowcase(draw, "property-row", "Property Row", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			float& value = DrivenFloat(ctx, "showcase.propertyrow.value", draw, "value", 5.0f, 0.0f, 20.0f);
			PropertyRowDesc desc;
			desc.Label = LocalizedText(draw, "label", "Move Speed", "移动速度");
			desc.Term = LocalizedText(draw, "term", "", "Move Speed");
			desc.Tooltip = LocalizedText(draw, "tooltip", "Movement speed in units per second (default 5).",
				"每秒移动单位数(默认 5)。");
			desc.A11yLabel = desc.Label;
			desc.A11yValue = FloatText3(value);
			// VEC-H4:层级缩进(只挪标签文字,不动行/值列)+ 多值/不可用的 "—" 值列占位。
			const bool mixed = StateIs(draw, { "mixed" }) || BoolProperty(draw, "mixed", false);
			desc.LabelIndent = FloatOverride(draw, "indent").value_or(0.0f);
			if (mixed)
			{
				desc.FieldPlaceholder = LocalizedText(draw, "mixedText", "—", "—");
				desc.A11yValue = "mixed";
				desc.Tooltip = LocalizedText(draw, "mixedTooltip",
					"Multiple selected objects have different values here; editing writes one value to all of them",
					"选中的多个对象在这里的值不同;一旦编辑会统一写成同一个值");
			}
			desc.Enabled = !StateIs(draw, { "disabled" }) && !BoolProperty(draw, "disabled", false);
			desc.Modified = StateIs(draw, { "modified" }) || BoolProperty(draw, "modified", false);
			desc.ShowReset = BoolProperty(draw, "reset", true);
			desc.ResetEnabled = desc.ShowReset && desc.Enabled;
			desc.ResetId = ShellId("property-row.reset");
			desc.ResetLabel = LocalizedText(draw, "resetLabel", "Reset", "恢复默认值");
			desc.ResetTooltip = LocalizedText(draw, "resetTooltip", "Restore the default value (5)",
				"恢复到默认值(5)");
			const PropertyRowResult row = PropertyRow(ctx, id, slot.Rect, desc, theme);
			if (!mixed)
				DragFloat(ctx, ShellId("property-row.field"), row.FieldRect, value, 0.01f, -1.0f, -1.0f, theme);
		}


void ShowPropertyGroupHeader(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme theme = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 280.0f, PropertyRowHeight());
			const WuiId id = BeginShowcase(draw, "property-group-header", "Property Group Header", slot.Rect);
			bool& openState = BoolState(ctx, "showcase.propertygroupheader.open", true);
			// 覆盖优先级:state(collapsed)> 属性(open)> 持久槽(真实点击的结果);覆盖只影响本帧。
			const std::optional<bool> forcedOpen = BoolOverride(draw, "open");
			const bool overridden = StateIs(draw, { "collapsed" }) || forcedOpen.has_value();
			bool open = forcedOpen.value_or(StateIs(draw, { "collapsed" }) ? false : openState);
			PseudoState pseudo(draw, id, slot.Rect);
			PropertyGroupHeaderDesc desc;
			desc.Label = LocalizedText(draw, "label", "Scores", "分数表");
			desc.Term = LocalizedText(draw, "term", "", "Scores");
			desc.Tooltip = LocalizedText(draw, "tooltip", "Per-level score entries (default 1.5, 2.5, 3.5).",
				"逐关分数(默认 1.5、2.5、3.5)。");
			desc.Trailing = LocalizedText(draw, "trailing", "3 items", "3 项");
			desc.A11yLabel = desc.Label;
			desc.Open = open;
			desc.Enabled = !StateIs(draw, { "disabled" }) && !BoolProperty(draw, "disabled", false);
			desc.Modified = StateIs(draw, { "modified" }) || BoolProperty(draw, "modified", false);
			// 默认态就画出复位(§19 的 ExtraA11yIds 断言在 default 一帧里找节点;状态只作为额外入口)。
			desc.ShowReset = BoolProperty(draw, "reset", true) || StateIs(draw, { "modified" });
			desc.ResetEnabled = desc.ShowReset && desc.Enabled;
			desc.ResetId = ShellId("property-group-header.reset");
			desc.ResetLabel = LocalizedText(draw, "resetLabel", "Reset", "恢复默认值");
			desc.ResetTooltip = LocalizedText(draw, "resetTooltip",
				"Restore the whole collection (asks for confirmation)", "恢复整个集合(会先弹确认)");
			const PropertyGroupHeaderResult header = PropertyGroupHeader(ctx, id, slot.Rect, desc, theme);
			if (header.Toggled)
				open = !open;
			if (!overridden)
				openState = open;
		}


void ShowCollectionRow(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme theme = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 280.0f, PropertyRowHeight());
			const WuiId id = BeginShowcase(draw, "collection-row", "Collection Row", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			float& value = DrivenFloat(ctx, "showcase.collectionrow.value", draw, "value", 1.5f, 0.0f, 20.0f);
			CollectionRowDesc desc;
			desc.Label = TextProperty(draw, "label", "1");
			desc.Tooltip = LocalizedText(draw, "tooltip", "Element 1 of Scores (default 1.5).",
				"分数表第 1 个元素(默认 1.5)。");
			desc.A11yLabel = desc.Label;
			desc.A11yValue = FloatText3(value);
			desc.Enabled = !StateIs(draw, { "disabled" }) && !BoolProperty(draw, "disabled", false);
			desc.ShowReset = BoolProperty(draw, "reset", true);
			desc.ResetEnabled = desc.ShowReset && desc.Enabled;
			desc.ResetId = ShellId("collection-row.reset");
			desc.ResetLabel = LocalizedText(draw, "resetLabel", "Reset", "恢复默认值");
			desc.ResetTooltip = LocalizedText(draw, "resetTooltip",
				"Restore this element to the script default", "把该元素恢复到脚本默认值");
			desc.ShowRemove = BoolProperty(draw, "remove", true);
			desc.RemoveEnabled = desc.ShowRemove && desc.Enabled;
			desc.RemoveId = ShellId("collection-row.remove");
			desc.RemoveTooltip = LocalizedText(draw, "removeTooltip",
				"Remove this element from the collection (the scene stores the list)",
				"从集合中移除此元素(场景将保存此列表)");
			desc.ShowAdd = StateIs(draw, { "add" }) || BoolProperty(draw, "add", false);
			desc.AddEnabled = desc.ShowAdd && desc.Enabled;
			desc.AddId = ShellId("collection-row.add");
			desc.AddTooltip = LocalizedText(draw, "addTooltip", "Append one element to the list",
				"向列表末尾追加一个元素");
			const CollectionRowResult row = CollectionRow(ctx, id, slot.Rect, desc, theme);
			DragFloat(ctx, ShellId("collection-row.field"), row.FieldRect, value, 0.01f, -1.0f, -1.0f, theme);
		}


void ShowCollectionActionButton(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme theme = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 24.0f, 24.0f);
			const WuiId id = BeginShowcase(draw, "collection-action-button", "Collection Action Button", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const bool enabled = !StateIs(draw, { "disabled" }) && !BoolProperty(draw, "disabled", false);
			CollectionActionButton(ctx, id, slot.Rect, TextProperty(draw, "glyph", "-"),
				LocalizedText(draw, "tooltip", "Remove this element from the collection",
					"从集合中移除此元素"),
				enabled, theme, BoolProperty(draw, "danger", true));
		}


void ShowScrollBar(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 14.0f, 168.0f);
			const WuiId id = BeginShowcase(draw, "scrollbar", "Scroll Bar", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			float& scroll = DrivenFloat(ctx, "showcase.scrollbar.scroll", draw, "scroll", 60.0f, 0.0f, 400.0f);
			// disabled = 内容装得下(没有滚动量):节点 Enabled/Interactive=false,整条退化成纯轨道。
			const float viewport = 200.0f;
			const float content = draw.State == "disabled" ? 160.0f : 400.0f;
			const bool overridden = draw.State == "top" || draw.State == "bottom" || draw.State == "disabled";
			float value = scroll;
			if (draw.State == "top")
				value = 0.0f;
			else if (draw.State == "bottom")
				value = content - viewport;
			// 状态帧先写回槽:滑块位置与 value 同源(否则切换状态后的第一帧滑块还停在旧位置);
			// 但状态覆盖**不**写回持久槽 —— 只有真实拖动/键盘的结果才算数(切回 default 时位置稳定)。
			if (!overridden)
				scroll = value;
			ScrollBar(ctx, id, slot.Rect, content, viewport, value, theme);
			if (!overridden)
				scroll = value;
		}

}
}
