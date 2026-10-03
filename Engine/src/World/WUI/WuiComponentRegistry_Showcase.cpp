#include "WuiComponentRegistry_Internal.h"

namespace World::Wui
{

using namespace WuiComponentRegistryDetail;

namespace WuiComponentRegistryDetail
{

		// 通道 → "<通道>.<状态>" 文本覆盖 → 颜色槽;没覆盖/写坏 = 空槽(回退主题令牌)。
std::optional<WuiColor> ColorOverride(const WuiComponentDraw& draw, const char* channel, const char* state){
			const std::string name = std::string(channel) + "." + state;
			const std::string* text = FindProperty(draw, name.c_str());
			if (text == nullptr || text->empty())
				return std::nullopt;
			WuiColor parsed {};
			return ParseComponentColor(*text, parsed) ? std::optional<WuiColor>(parsed) : std::nullopt;
		}


		// 登记属性 → WuiButtonStyle(只填用户/工作台真的覆盖过的槽)。
WuiButtonStyle ButtonStyle(const WuiComponentDraw& draw){
			WuiButtonStyle style;
			// 越界/非有限值一律当"没给"(showcase 属性协议:非法值忽略、退回当前口径),
			// 而不是夹到边界 —— 夹取会把 "-4px 字号"变成"最小字号",用户看不出是写错了。
			if (const std::optional<float> padding = FloatOverride(draw, "padding"))
				if (*padding >= 0.0f && *padding <= 64.0f)
					style.PaddingX = *padding;
			if (const std::optional<float> fontSize = FloatOverride(draw, "fontSize"))
				if (*fontSize >= 6.0f && *fontSize <= 48.0f)
					style.FontSize = *fontSize;
			style.Bold = BoolProperty(draw, "bold", false);
			style.Disabled = DisabledFor(draw);
			for (size_t index = 0; index < WuiButtonStyle::StateCount; ++index)
			{
				WuiButtonStateColors& colors = style.Colors[index];
				colors.Bg = ColorOverride(draw, "bg", kButtonStateSuffix[index]);
				colors.Border = ColorOverride(draw, "border", kButtonStateSuffix[index]);
				colors.Text = ColorOverride(draw, "text", kButtonStateSuffix[index]);
			}
			return style;
		}


		// Layout 组:首选尺寸("WxH")→ 画布槽位;没覆盖/写坏 = 该件自己的 preferred 保持不变。
		// 高度 <= 0 = 用主题行高(与 Canvas 的默认口径一致)。
void PreferredSizeOverride(const WuiComponentDraw& draw, float& width, float& height){
			const std::string* text = FindProperty(draw, "preferred");
			if (text == nullptr || text->empty())
				return;
			float parsedWidth = 0.0f;
			float parsedHeight = 0.0f;
			if (!ParseComponentSize(*text, parsedWidth, parsedHeight) || parsedWidth <= 0.0f)
				return;
			width = parsedWidth;
			height = parsedHeight > 0.0f ? parsedHeight : 0.0f;
		}


void ShowButton(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			float preferredWidth = 128.0f;
			float preferredHeight = 0.0f;
			PreferredSizeOverride(draw, preferredWidth, preferredHeight);
			const Slot slot = Canvas(draw, *draw.Theme, preferredWidth, preferredHeight);
			const WuiId id = BeginShowcase(draw, "button", "Button", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect, true);
			const WuiButtonStyle style = ButtonStyle(draw);
			Button(ctx, id, slot.Rect, MaybeLongText(draw, LocalizedText(draw, "label", "Apply", "应用")), themed, &style);
		}


void ShowIconButton(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 32.0f);
			const WuiId id = BeginShowcase(draw, "button.icon", "Icon Button", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			// textureId=0 → 控件自己退化成语义文字按钮(真实回退路径,不是复刻)。
			ToolbarIconButton(ctx, id, slot.Rect, 0, WuiRect { 0.0f, 0.0f, 1.0f, 1.0f },
				MaybeLongText(draw, LocalizedText(draw, "label", "Save", "保存")), themed, !DisabledFor(draw));
		}


void ShowResetDefaultButton(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 24.0f);
			const WuiId id = BeginShowcase(draw, "button.reset-default", "Reset Default", slot.Rect);
			// VEC-H7:pressed 需要"悬停 + 按下"两件套(allowPress=true),与 button/button.icon 的 showcase 同口径。
			PseudoState pseudo(draw, id, slot.Rect, true);
			const bool modified = StateIs(draw, { "modified" }) || BoolProperty(draw, "modified", true);
			// disabled 状态 = "偏离默认但当前不可复位"(面板的 Play/只读口径):弱化绘制 + tooltip 带理由。
			ResetDefaultButton(ctx, id, slot.Rect, !DisabledFor(draw) && modified, themed,
				LocalizedText(draw, "label", "Reset", "重置"),
				LocalizedText(draw, "tooltip", "Restore the default value", "恢复默认值"));
		}


void ShowOpenInEditorButton(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 24.0f);
			const WuiId id = BeginShowcase(draw, "button.open-in-editor", "Open In Editor", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			// 纯图标:按钮上原来的文字搬进 tooltip(用户口径),a11y label 仍是同一句语义文案。
			OpenInEditorButton(ctx, id, slot.Rect, themed, !DisabledFor(draw),
				LocalizedText(draw, "label", "Open in Editor", "在编辑器里打开"),
				MaybeLongText(draw, LocalizedText(draw, "tooltip",
					"Open this script asset in the built-in script editor",
					"在内置脚本编辑器里打开这个脚本资产")));
		}


void ShowToggle(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 148.0f);
			const WuiId id = BeginShowcase(draw, "toggle", "Toggle", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			// 开关的持久状态就是控件自己的 Persist<bool>(id):showcase 先按状态/属性写入,
			// 之后用户点击仍然改同一份状态(同 id 同类型,不违反"一个 id 一种类型")。
			bool& value = ctx.Persist<bool>(id, true);
			if (StateIs(draw, { "on", "checked" }))
				value = true;
			else if (StateIs(draw, { "off", "unchecked" }))
				value = false;
			else if (const std::optional<bool> forced = BoolOverride(draw, "value"))
				value = *forced;
			Toggle(ctx, id, slot.Rect, MaybeLongText(draw, LocalizedText(draw, "label", "Enabled", "启用")), themed);
		}


void ShowSegmented(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 210.0f);
			const WuiId id = BeginShowcase(draw, "segmented", "Segmented", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const std::vector<std::string> options = OptionList(draw, "options", { "Light", "Medium", "Heavy" });
			int64_t& selectedState = DrivenInt(ctx, "showcase.segmented.selected", draw, "selected", 1, 0,
				static_cast<int64_t>(options.size()) - 1);
			int selected = static_cast<int>(selectedState);
			Segmented(ctx, id, slot.Rect, options, selected, themed);
			selectedState = selected;
		}


void ShowCheckbox(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 168.0f);
			const WuiId id = BeginShowcase(draw, "checkbox", "Checkbox", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			bool& value = BoolState(ctx, "showcase.checkbox.value", true);
			if (StateIs(draw, { "checked", "on" }))
				value = true;
			else if (StateIs(draw, { "unchecked", "off" }))
				value = false;
			else if (const std::optional<bool> forced = BoolOverride(draw, "checked"))
				value = *forced;
			Checkbox(ctx, id, slot.Rect, MaybeLongText(draw, LocalizedText(draw, "label", "Enabled", "启用")), value, themed);
		}


void ShowCheckboxMixed(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 168.0f);
			const WuiId id = BeginShowcase(draw, "checkbox.mixed", "Checkbox Mixed", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			bool& value = BoolState(ctx, "showcase.checkbox-mixed.value", true);
			const bool mixed = !StateIs(draw, { "checked", "unchecked" }) && BoolProperty(draw, "mixed", true);
			CheckboxMixed(ctx, id, slot.Rect, MaybeLongText(draw, LocalizedText(draw, "label", "Select all", "全选")),
				value, mixed, themed);
		}


void ShowSliderFloat(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 190.0f);
			const WuiId id = BeginShowcase(draw, "slider.float", "Slider", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const float min = FloatOverride(draw, "min").value_or(0.0f);
			const float max = FloatOverride(draw, "max").value_or(1.0f);
			float& value = DrivenFloat(ctx, "showcase.slider.value", draw, "value", 0.35f, min, max);
			SliderFloat(ctx, id, slot.Rect, value, min, max, themed);
		}


void ShowDragFloat(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 150.0f);
			const WuiId id = BeginShowcase(draw, "dragfloat", "Drag Float", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const float min = FloatOverride(draw, "min").value_or(0.0f);
			const float max = FloatOverride(draw, "max").value_or(10.0f);
			const float speed = FloatOverride(draw, "speed").value_or(0.1f);
			float& value = DrivenFloat(ctx, "showcase.dragfloat.value", draw, "value", 2.5f, min, max);
			DragFloat(ctx, id, slot.Rect, value, speed, min, max, themed);
		}


void ShowDragBarFloat(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 200.0f);
			const WuiId id = BeginShowcase(draw, "dragbar.float", "Drag Bar", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const float min = FloatOverride(draw, "min").value_or(0.0f);
			const float max = FloatOverride(draw, "max").value_or(1.0f);
			float& value = DrivenFloat(ctx, "showcase.dragbar.value", draw, "value", 0.45f, min, max);
			WuiNumberStyle style;
			const std::string unit = TextProperty(draw, "unit", "%");
			style.Unit = unit.c_str();
			style.Decimals = 0;
			DragBarFloat(ctx, id, slot.Rect, value, min, max, themed, style);
		}


void ShowNumberFieldInt(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 128.0f);
			const WuiId id = BeginShowcase(draw, "numberfield.int", "Number Field", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			int64_t& value = DrivenInt(ctx, "showcase.numberfield.value", draw, "value", 2048, 1, 16384);
			WuiNumberStyle style;
			const std::string unit = TextProperty(draw, "unit", "px");
			style.Unit = unit.c_str();
			style.Decimals = 0;
			style.Steppers = BoolProperty(draw, "steppers", true);
			NumberFieldInt(ctx, id, slot.Rect, value, 1, 16384, themed, style);
		}


void ShowStepperInt(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 132.0f);
			const WuiId id = BeginShowcase(draw, "stepper.int", "Stepper", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			int64_t& valueState = DrivenInt(ctx, "showcase.stepper.value", draw, "value", 4, 1, 16);
			int value = static_cast<int>(valueState);
			WuiNumberStyle style;
			style.Decimals = 0;
			StepperInt(ctx, id, slot.Rect, value, 1, 16, themed, style);
			valueState = value;
		}


void ShowTextField(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 210.0f);
			const WuiId id = BeginShowcase(draw, "textfield", "Text Field", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			TextFieldA11y a11y;
			a11y.Label = LocalizedText(draw, "label", "Name", "名称");
			a11y.Placeholder = LocalizedText(draw, "placeholder", "Enter a name", "输入名称");
			std::string& buffer = DrivenText(ctx, "showcase.textfield.value", draw, "value",
				LocalizedText(draw, "text", "Player", "玩家"));
			if (draw.State == "long-text")
			{
				std::string longText = MaybeLongText(draw, buffer);
				TextField(ctx, id, slot.Rect, longText, themed, nullptr, &a11y);
			}
			else
			{
				TextField(ctx, id, slot.Rect, buffer, themed, nullptr, &a11y);
			}
		}


void ShowTextFieldError(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 210.0f);
			const WuiId id = BeginShowcase(draw, "textfield.error", "Text Field (Error)", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			TextFieldA11y a11y;
			a11y.Label = LocalizedText(draw, "label", "Mass", "质量");
			a11y.Placeholder = LocalizedText(draw, "placeholder", "Positive number", "正数");
			std::string& buffer = DrivenText(ctx, "showcase.textfield-error.value", draw, "value", "0.0");
			// 默认就带行内错误(这个组件存在的理由);state=valid 展示无错误的正常态。
			const std::string error = draw.State == "valid"
				? std::string()
				: LocalizedText(draw, "error", "Must be greater than zero", "必须大于零");
			TextFieldEx(ctx, id, slot.Rect, buffer, themed, error, &a11y);
		}


void ShowComboImpl(const WuiComponentDraw& draw, const char* componentId, const char* displayName, bool searchable){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 190.0f);
			const WuiId id = BeginShowcase(draw, componentId, displayName, slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const std::vector<std::string> options = OptionList(draw, "options", { "Low", "Medium", "High", "Ultra" });
			const std::string slotKey = std::string("showcase.") + componentId + ".selected";
			int64_t& selectedState = DrivenInt(ctx, slotKey.c_str(), draw, "selected", 1, 0,
				static_cast<int64_t>(options.size()) - 1);
			int selected = static_cast<int>(selectedState);
			// MAT-UI4a:弹层的"强制状态"同步只在 Edit(伪状态)模式做。Play 模式是真实输入直通,
			// draw.State 恒为 "default"(见 WidgetGalleryPanel::DrawCanvasShowcase),每帧同步会在
			// 用户点开弹层后的下一帧把它关掉 —— 现象就是"点一下弹出来立刻消失"。
			if (!draw.RouteRealInput)
			{
				if (draw.State == "open" && !ctx.IsPopupOpen(id))
					ctx.OpenPopup(id);
				else if (draw.State != "open" && ctx.IsPopupOpen(id))
					ctx.ClosePopup(id);
			}
			if (searchable)
				SearchableCombo(ctx, id, slot.Rect, LocalizedText(draw, "label", "Material", "材质"), options, selected, themed);
			else
				Combo(ctx, id, slot.Rect, LocalizedText(draw, "label", "Quality", "画质"), options, selected, themed);
			selectedState = selected;
		}


void ShowCombo(const WuiComponentDraw& draw){
			ShowComboImpl(draw, "combo", "Combo", false);
		}


void ShowSearchableCombo(const WuiComponentDraw& draw){
			ShowComboImpl(draw, "combo.searchable", "Searchable Combo", true);
		}


		// M4-TEX-P10:纹理引用的状态词 → 组件状态枚举(属性面板的 State 下拉用同一套 token)。
WuiTexturePickerState TexturePickerState(const std::string& text){
			const std::string lower = TrimmedLower(text);
			if (lower == "empty") return WuiTexturePickerState::Empty;
			if (lower == "set") return WuiTexturePickerState::Set;
			if (lower == "missing") return WuiTexturePickerState::Missing;
			if (lower == "missing-source") return WuiTexturePickerState::MissingSource;
			if (lower == "stale") return WuiTexturePickerState::Stale;
			if (lower == "unbaked") return WuiTexturePickerState::Unbaked;
			if (lower == "legacy") return WuiTexturePickerState::Legacy;
			if (lower == "container") return WuiTexturePickerState::Container;
			return WuiTexturePickerState::Auto;
		}


		// 一件纹理引用:当前值 + 徽标 + 清空/定位 + 可搜索列表(整条都是库件的真实路径)。
		// 状态按钮 = 预设的"值 + 徽标 token";属性覆盖(Value/Badges/State/Allow*/ReadOnly)优先。
void ShowTexturePicker(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			float preferredWidth = 300.0f;
			float preferredHeight = 0.0f;
			PreferredSizeOverride(draw, preferredWidth, preferredHeight);
			const Slot slot = Canvas(draw, *draw.Theme, preferredWidth, preferredHeight);
			const WuiId id = BeginShowcase(draw, "wui.texture-picker", "Texture Picker", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);

			std::string value = "textures/Icon.wtex";
			std::string badges = "asset,container,baked";
			if (StateIs(draw, { "empty" }))
			{
				value.clear();
				badges.clear();
			}
			else if (StateIs(draw, { "missing" }))
				badges = "asset,missing";
			else if (StateIs(draw, { "missing-source" }))
				badges = "asset,missing-source";
			else if (StateIs(draw, { "stale" }))
				badges = "asset,stale";
			else if (StateIs(draw, { "unbaked" }))
				badges = "asset,unbaked";
			else if (StateIs(draw, { "legacy" }))
				badges = "legacy";
			else if (StateIs(draw, { "container" }))
				badges = "asset,container,baked";
			else if (StateIs(draw, { "long-text" }))
				value = "textures/environment/props/studio_scan/Icon_from_artist_v12_final.wtex";
			else if (StateIs(draw, { "external" }))
				value = "C:/external/textures/Icon.png";   // 内容根外的引用(兜底条目的真实路径)

			WuiTexturePickerOptions options;
			options.Label = LocalizedText(draw, "label", "Albedo", "反照率");
			options.IdPrefix = "showcase.wui.texture-picker";
			options.ImportSourceName = TextProperty(draw, "ImportSourceName", "Icon.png");
			options.Entries = {
				{ "textures/Icon.wtex", "textures/Icon.wtex (Icon.png)", "asset,container,baked" },
				{ "textures/quadrants.wtex", "textures/quadrants.wtex (quadrants.png)", "asset,baked" },
				{ "textures/Icon.png", "textures/Icon.png", "source" },
			};
			value = TextProperty(draw, "Value", value);
			options.Badges = TextProperty(draw, "Badges", badges);
			options.AllowClear = BoolProperty(draw, "AllowClear", true);
			options.AllowReveal = BoolProperty(draw, "AllowReveal", true);
			options.AllowPick = BoolProperty(draw, "AllowPick", true);
			options.ReadOnly = StateIs(draw, { "disabled" }) || BoolProperty(draw, "ReadOnly", false);
			options.State = TexturePickerState(TextProperty(draw, "State", "auto"));
			if (const std::optional<float> fontSize = FloatOverride(draw, "badgeFontSize"))
				options.BadgeFontSize = *fontSize;
			if (const std::optional<float> fillAlpha = FloatOverride(draw, "badgeFillAlpha"))
				options.BadgeFillAlpha = *fillAlpha;
			// Edit 模式:state=open 时强制展开(与 Combo/SearchableCombo 同一条同步口径);
			// Play 模式是真实输入直通,每帧同步会把用户刚点开的弹层立刻关掉(实测过)。
			if (!draw.RouteRealInput)
			{
				if (draw.State == "open" && !ctx.IsPopupOpen(id))
					ctx.OpenPopup(id);
				else if (draw.State != "open" && ctx.IsPopupOpen(id))
					ctx.ClosePopup(id);
			}
			bool revealRequested = false;
			options.RevealRequested = &revealRequested;
			const bool changed = WuiTexturePicker(ctx, id, slot.Rect, value, options, themed);
			(void)changed;   // 工作台只展示:值变化由组件的持久槽 + 属性覆盖决定
		}


void ShowColorField(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 190.0f);
			const WuiId id = BeginShowcase(draw, "colorfield", "Color Field", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			// MAT-UI4a:同 ShowComboImpl —— Play(真实输入)下不做强制状态 → 弹层同步,否则
			// 用户点开的取色器弹层会在下一帧被这条同步关掉(Edit 模式的基线口径不变)。
			if (!draw.RouteRealInput)
			{
				if (draw.State == "open" && !ctx.IsPopupOpen(id))
					ctx.OpenPopup(id);
				else if (draw.State != "open" && ctx.IsPopupOpen(id))
					ctx.ClosePopup(id);
			}
			glm::vec4& color = DrivenColor(ctx, "showcase.colorfield.value", draw, "color", { 0.30f, 0.55f, 1.00f, 1.00f });
			ColorField(ctx, id, slot.Rect, color, themed);
		}


void ShowVec3Field(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 220.0f);
			const WuiId id = BeginShowcase(draw, "vec3field", "Vec3 Field", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			glm::vec3& value = DrivenVec3(ctx, "showcase.vec3.value", draw, "value", { 0.0f, 1.0f, 0.0f });
			const float speed = FloatOverride(draw, "speed").value_or(0.01f);
			Vec3Field(ctx, id, slot.Rect, value, speed, -100.0f, 100.0f, themed,
				draw.State == "vertical" ? 1 : 0);
		}


void ShowVec2Field(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			// 一行两段:与 Vec3Field 同一行高(theme.ControlHeight),不做加高。
			const Slot slot = Canvas(draw, *draw.Theme, 220.0f);
			const WuiId id = BeginShowcase(draw, "vec2field", "Vec2 Field", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			glm::vec4& driven = DrivenVecN(ctx, "showcase.vec2.value", draw, "value", 2,
				{ 0.0f, 1.0f, 0.0f, 0.0f });
			glm::vec2 value { driven.x, driven.y };
			const float speed = FloatOverride(draw, "speed").value_or(0.01f);
			Vec2Field(ctx, id, slot.Rect, value, speed, -100.0f, 100.0f, themed,
				draw.State == "vertical" ? 1 : 0);
			driven.x = value.x;
			driven.y = value.y;
		}


void ShowVec4Field(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			// 2×2 需要两行真实行高,否则每行只剩 12px(值区高度与 Vec3Field 一致:24×2 = 48 + 4)。
			const Slot slot = Canvas(draw, *draw.Theme, 220.0f, 52.0f);
			const WuiId id = BeginShowcase(draw, "vec4field", "Vec4 Field", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			glm::vec4& value = DrivenVecN(ctx, "showcase.vec4.value", draw, "value", 4,
				{ 0.0f, 1.0f, 0.0f, 1.0f });
			const float speed = FloatOverride(draw, "speed").value_or(0.01f);
			Vec4Field(ctx, id, slot.Rect, value, speed, -100.0f, 100.0f, themed,
				draw.State == "vertical" ? 1 : 0);
		}


void ShowSearchField(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 200.0f);
			const WuiId id = BeginShowcase(draw, "searchfield", "Search Field", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			std::string& buffer = DrivenText(ctx, "showcase.searchfield.value", draw, "value", std::string());
			SearchField(ctx, id, slot.Rect, buffer,
				LocalizedText(draw, "placeholder", "Search assets", "搜索资源"), themed);
		}


void ShowSplitter(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 200.0f, 72.0f);
			const WuiId id = BeginShowcase(draw, "splitter", "Splitter", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			float& split = DrivenFloat(ctx, "showcase.splitter.value", draw, "value",
				slot.Rect.W * 0.45f, 24.0f, std::max(24.0f, slot.Rect.W - 24.0f));
			PanelBackground(ctx, { slot.Rect.X, slot.Rect.Y, split, slot.Rect.H }, theme.PanelBg, 3.0f);
			PanelBackground(ctx, { slot.Rect.X + split, slot.Rect.Y, std::max(0.0f, slot.Rect.W - split), slot.Rect.H },
				theme.ContentBg, 3.0f);
			Label(ctx, { slot.Rect.X + 8.0f, slot.Rect.Y + 6.0f }, LocalizedText(draw, "left", "Left", "左栏"),
				theme.Text, theme.FontSizeSmall);
			Label(ctx, { slot.Rect.X + split + 8.0f, slot.Rect.Y + 6.0f }, LocalizedText(draw, "right", "Right", "右栏"),
				theme.TextMuted, theme.FontSizeSmall);
			// 命中带宽固定 6px:传入矩形就是那条带(竖条)。
			Splitter(ctx, id, { slot.Rect.X + split - 3.0f, slot.Rect.Y, 6.0f, slot.Rect.H }, true, split,
				24.0f, std::max(24.0f, slot.Rect.W - 24.0f), theme);
		}


void ShowTabs(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 240.0f);
			const WuiId id = BeginShowcase(draw, "tabs", "Tabs", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const std::vector<std::string> tabs = OptionList(draw, "tabs", { "General", "Rendering", "Physics" });
			int64_t& activeState = DrivenInt(ctx, "showcase.tabs.active", draw, "active", 0, 0,
				static_cast<int64_t>(tabs.size()) - 1);
			int active = static_cast<int>(activeState);
			TabBar(ctx, id, slot.Rect, tabs, active, themed, nullptr);
			activeState = active;
		}


void ShowTreeNode(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 190.0f);
			const WuiId id = BeginShowcase(draw, "treenode", "Tree Node", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const bool leaf = BoolProperty(draw, "leaf", false);
			bool& open = ctx.Persist<bool>(id, true);
			if (StateIs(draw, { "open", "expanded" }))
				open = true;
			else if (StateIs(draw, { "closed", "collapsed" }))
				open = false;
			TreeNode(ctx, id, slot.Rect, LocalizedText(draw, "label", "Materials", "材质"), leaf, themed);
		}


void ShowTreeView(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 230.0f, 96.0f);
			const WuiId id = BeginShowcase(draw, "treeview", "Tree View", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			// P1c-E4:键盘光标与展开态由持久槽驱动 —— 焦点在树上时 ↑/↓ 与 Enter 会改它们,
			// 画布/节点 value 立刻跟着变(键盘契约因此有可观察的结果,不是"按键没人接")。
			int64_t& cursorState = DrivenInt(ctx, "showcase.treeview.cursor", draw, "cursor", 1, 0, 3);
			bool& rootOpen = BoolState(ctx, "showcase.treeview.open", true);
			std::vector<TreeViewItem> items;
			items.push_back({ ShellId("treeview.item.0"), LocalizedText(draw, "label", "Assets", "资源"),
				0, true, rootOpen, cursorState == 0, false });
			items.push_back({ ShellId("treeview.item.1"), "Textures", 1, false, false, cursorState == 1, false });
			items.push_back({ ShellId("treeview.item.2"), "Materials", 1, false, false, cursorState == 2, false });
			items.push_back({ ShellId("treeview.item.3"), LocalizedText(draw, "disabled", "Locked", "已锁定"),
				1, false, false, cursorState == 3, true });
			float& scroll = DrivenFloat(ctx, "showcase.treeview.scroll", draw, "scroll", 0.0f, 0.0f, 200.0f);
			const TreeViewResult tree = TreeView(ctx, slot.Rect, items,
				22.0f * slot.Scale * slot.Density, scroll, theme, id);
			if (tree.KeyMoveTo >= 0)
				cursorState = tree.KeyMoveTo;
			// ←/→ 只对可展开行发 KeyToggleExpand;Enter 激活当前项(演示里只有"Assets"可折叠)。
			if (tree.KeyToggleExpand >= 0 || tree.KeyActivate == 0)
				rootOpen = !rootOpen;
		}


void ShowListView(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 230.0f, 96.0f);
			const WuiId id = BeginShowcase(draw, "listview", "List View", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			// P1c-E4:选中行由持久槽驱动 —— 焦点在列表上时 ↑/↓/Home/End 改它(KeyMoveTo),
			// 节点 value / 行 focused / 画布同时更新。
			int64_t& activeState = DrivenInt(ctx, "showcase.listview.active", draw, "active", 0, 0, 2);
			std::vector<ListViewItem> items;
			ListViewItem assets;
			assets.Id = ShellId("listview.item.0");
			assets.Label = LocalizedText(draw, "label", "Textures", "贴图");
			assets.SubLabel = "12";
			assets.Selected = activeState == 0;
			items.push_back(assets);
			ListViewItem materials = assets;
			materials.Id = ShellId("listview.item.1");
			materials.Label = "Materials";
			materials.SubLabel = "4";
			materials.Selected = activeState == 1;
			items.push_back(materials);
			ListViewItem locked = assets;
			locked.Id = ShellId("listview.item.2");
			locked.Label = LocalizedText(draw, "disabled", "Locked", "已锁定");
			locked.SubLabel.clear();
			locked.Selected = activeState == 2;
			locked.Disabled = true;
			items.push_back(locked);
			float& scroll = DrivenFloat(ctx, "showcase.listview.scroll", draw, "scroll", 0.0f, 0.0f, 200.0f);
			const ListViewResult list = ListView(ctx, slot.Rect, items,
				24.0f * slot.Scale * slot.Density, scroll, theme, id);
			if (list.KeyMoveTo >= 0)
				activeState = list.KeyMoveTo;
			// KeyActivate(Enter/Space)在演示里没有可展示的副作用:"打开/重命名"由真实调用方决定。
		}


void ShowTableHeader(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 260.0f);
			const WuiId id = BeginShowcase(draw, "table.header", "Table Header", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const std::vector<std::string> columns = OptionList(draw, "columns", { "Name", "Type", "Size" });
			std::vector<float> widths;
			widths.reserve(columns.size());
			for (size_t i = 0; i < columns.size(); ++i)
				widths.push_back(slot.Rect.W / static_cast<float>(columns.size()));
			int64_t& sortState = DrivenInt(ctx, "showcase.table.sort", draw, "sort", 0, 0,
				static_cast<int64_t>(columns.size()) - 1);
			int sortColumn = static_cast<int>(sortState);
			bool ascending = BoolProperty(draw, "ascending", true);
			TableHeader(ctx, id, slot.Rect, columns, widths, sortColumn, ascending, theme);
			sortState = sortColumn;
			// 常驻表头下方的第一行数据行:表格的"表头 + 行"在真实界面里成对出现,预览里也给一行。
			const float rowHeight = 22.0f * slot.Scale;
			const WuiRect row { slot.Rect.X, slot.Rect.Y + slot.Rect.H, slot.Rect.W, rowHeight };
			if (row.Y + row.H <= draw.Rect.Y + draw.Rect.H)
			{
				PanelBackground(ctx, row, theme.PanelBg, 0.0f);
				for (size_t i = 0; i < columns.size(); ++i)
				{
					const WuiRect cell = TableCell(row, widths, 0, i, rowHeight);
					Label(ctx, { cell.X + 6.0f, cell.Y + 4.0f },
						i == 0 ? LocalizedText(draw, "row", "Icon.png", "Icon.png") : std::string("-"),
						theme.TextMuted, theme.FontSizeSmall);
				}
			}
		}


void ShowScrollArea(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 220.0f, 96.0f);
			const WuiId id = BeginShowcase(draw, "scrollarea", "Scroll Area", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const float rowHeight = 24.0f * slot.Scale;
			const float contentHeight = rowHeight * 8.0f + 8.0f;
			float& scroll = DrivenFloat(ctx, "showcase.scrollarea.scroll", draw, "scroll", 0.0f, 0.0f, 400.0f);
			if (draw.State == "scrolled")
				scroll = rowHeight * 3.0f;
			// P1c-E4:id 下沉进滚动区 —— Tab 可达 + ↑/↓/PageUp/PageDown/Space/Home/End 可滚
			// (以前只有"鼠标悬停 + 滚轮"一条路径)。
			if (BeginScrollArea(ctx, slot.Rect, contentHeight, scroll, theme, id))
			{
				for (int i = 0; i < 8; ++i)
				{
					const WuiRect row { slot.Rect.X + 4.0f, slot.Rect.Y + 4.0f + rowHeight * static_cast<float>(i) - scroll,
						std::max(0.0f, slot.Rect.W - 8.0f), rowHeight };
					if (row.Y + row.H < slot.Rect.Y || row.Y > slot.Rect.Y + slot.Rect.H)
						continue;
					HoverRow(ctx, row, ctx.IsHovered(row), i == 2, theme, 3.0f);
					Label(ctx, { row.X + 8.0f, row.Y + 4.0f },
						LocalizedText(draw, "rowPrefix", "Row ", "第 ") + std::to_string(i + 1),
						theme.Text, theme.FontSizeSmall);
				}
				EndScrollArea(ctx);
			}
		}


void ShowModal(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 320.0f, 140.0f);
			const WuiId id = BeginShowcase(draw, "modal", "Modal Dialog", slot.Rect);
			// 模态只在 ctx.Modal()==id 的帧绘制:showcase 临时顶上、画完还原(工作台里点其它组件不受影响)。
			const WuiId previousModal = ctx.Modal();
			ctx.SetModal(id);
			WuiRect frame {};
			const glm::vec2 size { slot.Rect.W, slot.Rect.H };
			if (BeginModal(ctx, id, LocalizedText(draw, "title", "Delete entity?", "删除实体?"), size, &frame, theme))
			{
				Label(ctx, { frame.X + 14.0f, frame.Y + 46.0f },
					LocalizedText(draw, "message", "This action cannot be undone.", "此操作不可撤销。"),
					theme.TextMuted, theme.FontSizeSmall);
				const float buttonWidth = std::min(96.0f, frame.W * 0.4f);
				const float buttonY = frame.Y + frame.H - theme.ControlHeight - 12.0f;
				Button(ctx, ShellId("modal.confirm"),
					{ frame.X + frame.W - theme.Pad - buttonWidth, buttonY, buttonWidth, theme.ControlHeight },
					LocalizedText(draw, "confirm", "Confirm", "确认"), theme);
				Button(ctx, ShellId("modal.cancel"),
					{ frame.X + frame.W - theme.Pad * 2.0f - buttonWidth * 2.0f, buttonY, buttonWidth, theme.ControlHeight },
					LocalizedText(draw, "cancel", "Cancel", "取消"), theme);
				EndModal(ctx, id);
			}
			ctx.SetModal(previousModal);
		}


void ShowSectionHeader(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 220.0f);
			const WuiId id = BeginShowcase(draw, "sectionheader", "Section Header", slot.Rect);
			(void)id;
			SectionHeader(ctx, slot.Rect, MaybeLongText(draw, LocalizedText(draw, "title", "Transform", "变换")),
				themed.Text, themed, 15.0f * slot.Scale);
		}


void ShowTooltip(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 150.0f);
			const WuiId id = BeginShowcase(draw, "tooltip", "Tooltip", slot.Rect);
			(void)id;
			// 气泡本来就是"鼠标悬停才出现":showcase 把鼠标临时挪到展示区中心,画完恢复。
			const WuiInputState saved = ctx.Input();
			ctx.Input().MousePos = { slot.Rect.X + slot.Rect.W * 0.5f, slot.Rect.Y + slot.Rect.H * 0.5f };
			Label(ctx, { slot.Rect.X + 6.0f, slot.Rect.Y + 4.0f },
				LocalizedText(draw, "label", "Hover me", "悬停我"), theme.Text, theme.FontSizeSmall);
			Tooltip(ctx, slot.Rect, LocalizedText(draw, "tooltip",
				"Adds a new entity to the current scene.", "向当前场景添加一个新实体。"));
			DrawTooltip(ctx, theme);
			ctx.Input() = saved;
		}


void ShowBreadcrumb(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 230.0f);
			const WuiId id = BeginShowcase(draw, "breadcrumb", "Breadcrumb", slot.Rect);
			const std::string path = TextProperty(draw, "path", "assets/textures/icon.png");
			// P1c-E4:id 已下沉进 Breadcrumb —— 节点/焦点入口由控件自己登记,删掉 P1c-a 的
			// "外壳代登记"(那次是公开头文件还没改的权宜;现在不造第二份节点)。
			// 返回段下标 = 鼠标点击或键盘(←/→ 移光标、Enter/Space 激活)的结果。
			Breadcrumb(ctx, slot.Rect, path, themed, id);
		}


void ShowContextMenu(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 172.0f, 96.0f);
			const WuiId id = BeginShowcase(draw, "contextmenu", "Context Menu", slot.Rect);
			// 右键菜单的用法:位置在打开时钉住(不跟鼠标),由调用方决定开关 —— 预览里保持打开。
			if (!ctx.IsPopupOpen(id))
				ctx.OpenPopup(id);
			WuiRect panel {};
			if (BeginContextMenu(ctx, id, { slot.Rect.X, slot.Rect.Y }, slot.Rect.W, 4, &panel, theme))
			{
				const float itemHeight = 22.0f;
				float y = panel.Y + 4.0f;
				ContextMenuItem(ctx, ShellId("contextmenu.copy"), { panel.X + 4.0f, y, panel.W - 8.0f, itemHeight },
					LocalizedText(draw, "copy", "Copy", "复制"), theme);
				y += itemHeight;
				ContextMenuToggleItem(ctx, ShellId("contextmenu.visible"), { panel.X + 4.0f, y, panel.W - 8.0f, itemHeight },
					LocalizedText(draw, "visible", "Visible", "可见"), true, theme);
				y += itemHeight;
				ContextMenuSeparator(ctx, { panel.X + 4.0f, y, panel.W - 8.0f, itemHeight * 0.5f }, theme);
				y += itemHeight * 0.5f;
				ContextMenuItem(ctx, ShellId("contextmenu.delete"), { panel.X + 4.0f, y, panel.W - 8.0f, itemHeight },
					LocalizedText(draw, "delete", "Delete", "删除"), theme, !DisabledFor(draw));
				EndContextMenu(ctx, id, panel, theme);
			}
		}


void ShowEmptyState(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const WuiRect rect { draw.Rect.X, draw.Rect.Y, std::min(draw.Rect.W, 280.0f), std::min(draw.Rect.H, 120.0f) };
			const WuiId id = BeginShowcase(draw, "empty.state", "Empty State", rect);
			(void)id;
			const bool bare = draw.State == "empty";
			EmptyState(ctx, rect,
				bare ? std::string() : std::string("*"),
				LocalizedText(draw, "title", "No assets yet", "还没有资源"),
				LocalizedText(draw, "hint", "Import a texture or a model to get started.", "导入贴图或模型开始使用。"),
				bare ? std::string() : LocalizedText(draw, "action", "Import", "导入"),
				ShellId("empty.state.action"), theme);
		}


void ShowProgress(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 200.0f, 12.0f);
			const WuiId id = BeginShowcase(draw, "progress", "Progress Bar", slot.Rect);
			(void)id;
			// WuiProgress 是保留模式控件(与读数面板同一条路径):布局 + Paint 到当前 ctx。
			auto progress = std::make_shared<WuiProgress>();
			progress->Fraction = DrivenFloat(ctx, "showcase.progress.value", draw, "value", 0.45f, 0.0f, 1.0f);
			progress->TrackColor = theme.ContentBg;
			progress->FillColor = DisabledFor(draw) ? theme.TextDisabled : theme.Accent;
			LayoutWidgetTree(progress, slot.Rect);
			WuiPaintContext paint(ctx);
			progress->Paint(paint);
		}


void ShowCodeEditor(const WuiComponentDraw& draw){
			WuiContext& ctx = *draw.Context;
			const Slot slot = Canvas(draw, *draw.Theme, 280.0f, 104.0f);
			const WuiId id = BeginShowcase(draw, "codeeditor", "Code Editor", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			WuiTextBuffer& buffer = ctx.Persist<WuiTextBuffer>(HashId("showcase.codeeditor.buffer"), WuiTextBuffer {});
			if (buffer.Text().empty())
				buffer.SetText("local player = World.Entity('Player')\nplayer:SetPosition(0, 1, 0)\n");
			WuiCodeEditorOptions options;
			options.FontSize = 13.0f * slot.Scale;
			options.LineHeight = 19.0f * slot.Scale;
			options.ReadOnly = DisabledFor(draw, "readonly");
			options.ErrorLine = draw.State == "error" ? 1 : -1;
			options.CompletionIdPrefix = "showcase.codeeditor.suggest";
			CodeEditor(ctx, id, slot.Rect, buffer, options);
		}

}
}
