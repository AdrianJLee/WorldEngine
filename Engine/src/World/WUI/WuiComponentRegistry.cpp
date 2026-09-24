#include "wldpch.h"

#include "World/WUI/WuiComponentRegistry.h"

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiTextBuffer.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include "World/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// WUI-P0a:组件登记表(唯一事实源)+ 每件控件一条"真实控件路径"的 showcase。
//
// 约定(工作台 P0-3、探针 P0-4、门禁 P0-5 共用):
//  1) showcase 的控件 id = HashId("showcase." + 登记 id)。ExtraA11yIds 里存**可直接哈希**的
//     文本,HashId(文本) 就是无障碍节点 id —— 探针据此断言"id 稳定";派生子节点的规则写在
//     对应条目的 A11yNotes 里(形如父 id + ".tab." + i,与 WuiWidgets.cpp 的 DerivedChildId 一致)。
//  2) 控件自身不登记 a11y 节点时,showcase 先登记一个 kind="component-root"、interactive=false
//     的锚点(同 id);控件自己登记同 id 节点时以控件为准(后登记覆盖先登记)。
//  3) 属性覆盖:文本属性即时生效;数值/布尔属性经持久槽,文本值变化时重设(工作台改一次即生效)。
//     未知属性名、非法数值一律忽略并退回当前值 —— 不抛异常、不崩。
//  4) 状态:除 default 外的伪状态由 PseudoState 在本帧临时改输入/焦点,作用域结束即恢复;
//     没有对应视觉/交互表达的控件不登记该状态(States 只列"画得出来"的)。
//  5) TypeName = 该件在**面板侧**的控件入口名:面板以保留模式类出现时填类名(如 "WuiButton"),
//     纯立即模式控件填 WuiWidgets.h / WuiChrome.h / WuiCodeEditor.h 里的入口名(如 "Segmented")。
//     门禁 tools/agents/check-ui-components.ps1 按这个字段比对"面板用到的控件类型是否都已登记",
//     单测 §19 要求它非空、且能在 Engine/src/World/WUI 的头文件里找到同名声明。

namespace World::Wui
{
	namespace
	{
		using Property = WuiComponentProperty;
		using State = WuiComponentState;

		// ---- 属性覆盖解析 ----

		float ClampFloat(float value, float low, float high)
		{
			return value < low ? low : (value > high ? high : value);
		}

		const std::string* FindProperty(const WuiComponentDraw& draw, const char* name)
		{
			for (const std::pair<std::string, std::string>& entry : draw.Properties)
				if (entry.first == name)
					return &entry.second;
			return nullptr;
		}

		std::string TrimmedLower(const std::string& text)
		{
			size_t begin = 0;
			size_t end = text.size();
			while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n'))
				++begin;
			while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
				--end;
			std::string out = text.substr(begin, end - begin);
			for (char& ch : out)
				if (ch >= 'A' && ch <= 'Z')
					ch = static_cast<char>(ch - 'A' + 'a');
			return out;
		}

		std::optional<bool> BoolOverride(const WuiComponentDraw& draw, const char* name)
		{
			const std::string* text = FindProperty(draw, name);
			if (!text)
				return std::nullopt;
			const std::string lower = TrimmedLower(*text);
			if (lower == "true" || lower == "1" || lower == "on" || lower == "yes" || lower == "checked")
				return true;
			if (lower == "false" || lower == "0" || lower == "off" || lower == "no" || lower == "unchecked")
				return false;
			return std::nullopt;   // 认不出的写法 = 没给
		}

		bool BoolProperty(const WuiComponentDraw& draw, const char* name, bool fallback)
		{
			return BoolOverride(draw, name).value_or(fallback);
		}

		std::optional<float> FloatOverride(const WuiComponentDraw& draw, const char* name)
		{
			const std::string* text = FindProperty(draw, name);
			if (!text || text->empty())
				return std::nullopt;
			char* end = nullptr;
			const float parsed = std::strtof(text->c_str(), &end);
			if (end == text->c_str() || !std::isfinite(parsed))
				return std::nullopt;
			return parsed;
		}

		std::optional<int64_t> IntOverride(const WuiComponentDraw& draw, const char* name)
		{
			const std::string* text = FindProperty(draw, name);
			if (!text || text->empty())
				return std::nullopt;
			char* end = nullptr;
			const long long parsed = std::strtoll(text->c_str(), &end, 10);
			if (end == text->c_str())
				return std::nullopt;
			return static_cast<int64_t>(parsed);
		}

		std::string TextProperty(const WuiComponentDraw& draw, const char* name, const std::string& fallback)
		{
			const std::string* text = FindProperty(draw, name);
			return (text && !text->empty()) ? *text : fallback;
		}

		bool IsChinese(const WuiComponentDraw& draw)
		{
			return draw.Locale.rfind("zh", 0) == 0;
		}

		// 文案:属性覆盖 > Locale 默认(英文 / 简体中文)。
		std::string LocalizedText(const WuiComponentDraw& draw, const char* name, const char* english, const char* chinese)
		{
			return TextProperty(draw, name, IsChinese(draw) ? std::string(chinese) : std::string(english));
		}

		// 逗号/竖线/分号分隔的选项表(工作台"属性 → 下拉选项")。少于两项时不采用覆盖值。
		std::vector<std::string> OptionList(const WuiComponentDraw& draw, const char* name,
			std::initializer_list<const char*> defaults)
		{
			std::vector<std::string> fallback(defaults.begin(), defaults.end());
			const std::string* text = FindProperty(draw, name);
			if (!text || text->empty())
				return fallback;
			std::vector<std::string> parsed;
			std::string current;
			for (char ch : *text)
			{
				const bool separator = ch == ',' || ch == '|' || ch == ';';
				if (!separator)
				{
					current.push_back(ch);
					continue;
				}
				if (!TrimmedLower(current).empty())
					parsed.push_back(current);
				current.clear();
			}
			if (!TrimmedLower(current).empty())
				parsed.push_back(current);
			return parsed.size() >= 2 ? parsed : fallback;
		}

		// ---- 持久槽(属性覆盖 → 控件可写状态) ----

		struct FloatSlot
		{
			float Value = 0.0f;
			std::string Applied;
			bool Initialized = false;
		};

		struct IntSlot
		{
			int64_t Value = 0;
			std::string Applied;
			bool Initialized = false;
		};

		struct BoolSlot
		{
			bool Value = false;
			std::string Applied;
			bool Initialized = false;
		};

		struct TextSlot
		{
			std::string Value;
			std::string Applied;
			bool Initialized = false;
		};

		struct ColorSlot
		{
			glm::vec4 Value { 0.30f, 0.55f, 1.00f, 1.00f };
			std::string Applied;
			bool Initialized = false;
		};

		int HexDigit(char ch)
		{
			if (ch >= '0' && ch <= '9')
				return ch - '0';
			if (ch >= 'a' && ch <= 'f')
				return ch - 'a' + 10;
			if (ch >= 'A' && ch <= 'F')
				return ch - 'A' + 10;
			return -1;
		}

		// "#RRGGBB" / "#RRGGBBAA"(可省 '#',大小写不限);失败返回 false 并保持原值。
		bool ParseHexColor(const std::string& text, glm::vec4& out)
		{
			std::string digits;
			for (char ch : text)
			{
				if (ch == '#')
					continue;
				if (HexDigit(ch) < 0)
					return false;
				digits.push_back(ch);
			}
			if (digits.size() != 6 && digits.size() != 8)
				return false;
			const auto channel = [&](size_t index) -> float
			{
				return static_cast<float>(HexDigit(digits[index]) * 16 + HexDigit(digits[index + 1])) / 255.0f;
			};
			out = { channel(0), channel(2), channel(4), digits.size() == 8 ? channel(6) : 1.0f };
			return true;
		}

		bool& BoolState(WuiContext& ctx, const char* slot, bool initial)
		{
			BoolSlot& state = ctx.Persist<BoolSlot>(HashId(slot), BoolSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			return state.Value;
		}

		// 数值槽:属性文本变化时重设(工作台改一次属性立刻生效);非法文本忽略。
		float& DrivenFloat(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name,
			float initial, float min, float max)
		{
			FloatSlot& state = ctx.Persist<FloatSlot>(HashId(slot), FloatSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				if (const std::optional<float> parsed = FloatOverride(draw, name))
					state.Value = ClampFloat(*parsed, min, max);
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}

		int64_t& DrivenInt(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name,
			int64_t initial, int64_t min, int64_t max)
		{
			IntSlot& state = ctx.Persist<IntSlot>(HashId(slot), IntSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				if (const std::optional<int64_t> parsed = IntOverride(draw, name))
					state.Value = std::max(min, std::min(max, *parsed));
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}

		std::string& DrivenText(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name,
			const std::string& initial)
		{
			TextSlot& state = ctx.Persist<TextSlot>(HashId(slot), TextSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				state.Value = *text;
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}

		glm::vec4& DrivenColor(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name,
			const glm::vec4& initial)
		{
			ColorSlot& state = ctx.Persist<ColorSlot>(HashId(slot), ColorSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				glm::vec4 parsed {};
				if (ParseHexColor(*text, parsed))
					state.Value = parsed;
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}

		// ---- 状态词 ----

		bool StateIs(const WuiComponentDraw& draw, std::initializer_list<const char*> states)
		{
			for (const char* state : states)
				if (draw.State == state)
					return true;
			return false;
		}

		// ---- 画布与外壳 ----

		struct Slot
		{
			WuiRect Rect {};
			float Scale = 1.0f;
			float Density = 1.0f;
		};

		// 画布槽:宽度/高度按 UiScale 缩放、按 Density 压高度(默认行高 = theme.ControlHeight),
		// 在 draw.Rect 内左对齐、垂直居中;绝不越出 draw.Rect。
		Slot Canvas(const WuiComponentDraw& draw, const WuiTheme& theme, float preferredWidth, float preferredHeight = 0.0f)
		{
			Slot slot;
			slot.Scale = ClampFloat(draw.UiScale, 0.5f, 3.0f);
			slot.Density = ClampFloat(draw.Density, 0.5f, 2.0f);
			const float baseWidth = std::max(8.0f, preferredWidth) * slot.Scale;
			const float baseHeight = std::max(8.0f, preferredHeight > 0.0f ? preferredHeight : theme.ControlHeight);
			const float height = std::min(draw.Rect.H, baseHeight * slot.Scale * slot.Density);
			const float width = std::min(draw.Rect.W, baseWidth);
			slot.Rect = { draw.Rect.X, draw.Rect.Y + (draw.Rect.H - height) * 0.5f, width, height };
			return slot;
		}

		WuiId ShellId(const char* componentId)
		{
			const std::string key = std::string("showcase.") + componentId;
			return HashId(key.c_str());
		}

		// 组件外壳锚点:控件自己不登记 a11y 时,探针/工作台仍有稳定 id 可用。
		// 控件自己登记同 id 节点时后登记覆盖本节点(kind 变成控件自己的 role)。
		WuiId BeginShowcase(const WuiComponentDraw& draw, const char* componentId, const char* displayName,
			const WuiRect& rect)
		{
			const WuiId id = ShellId(componentId);
			if (WuiAccessibility::Get().Enabled())
			{
				WuiAccessNode node;
				node.Id = id;
				node.Window = WuiAccessibility::Get().CurrentWindow();
				node.Panel = WuiAccessibility::Get().CurrentPanel();
				node.Kind = "component-root";
				node.Label = displayName;
				node.Value = draw.State;
				node.Rect = rect;
				node.Interactive = false;
				WuiAccessibility::Get().Register(node);
			}
			return id;
		}

		// 伪状态:state=hover/pressed 时本帧临时挪鼠标(必要时按下),state=focus 时临时设焦点;
		// 离开作用域恢复原输入与焦点 —— 不污染同一帧里其它组件的绘制。
		class PseudoState
		{
		public:
			PseudoState(const WuiComponentDraw& draw, WuiId focusId, const WuiRect& rect, bool allowPress = false)
				: m_Context(*draw.Context), m_SavedInput(draw.Context->Input()), m_SavedFocus(draw.Context->Focus())
			{
				const bool hover = draw.State == "hover" || (allowPress && draw.State == "pressed");
				if (hover)
					m_Context.Input().MousePos = { rect.X + rect.W * 0.5f, rect.Y + rect.H * 0.5f };
				if (allowPress && draw.State == "pressed")
					m_Context.Input().MouseDown[0] = true;
				if (focusId != 0 && draw.State == "focus")
					m_Context.SetFocus(focusId);
			}

			~PseudoState()
			{
				m_Context.Input() = m_SavedInput;
				m_Context.SetFocus(m_SavedFocus);
			}

			PseudoState(const PseudoState&) = delete;
			PseudoState& operator=(const PseudoState&) = delete;

		private:
			WuiContext& m_Context;
			WuiInputState m_SavedInput;
			WuiId m_SavedFocus = 0;
		};

		// 禁用态:控件没有 enabled 参数时,用主题里的禁用令牌表达(TextDisabled/ContentBg),
		// 仍是控件自己的绘制路径(不再叠一层半透明遮罩这种假外观)。
		WuiTheme DisabledTheme(const WuiTheme& theme)
		{
			WuiTheme out = theme;
			out.Text = theme.TextDisabled;
			out.TextMuted = theme.TextDisabled;
			out.ButtonBg = theme.ContentBg;
			out.ButtonHover = theme.ContentBg;
			out.ActiveBg = theme.ContentBg;
			out.HoverBg = theme.ContentBg;
			out.Accent = theme.TextDisabled;
			out.FocusRing = theme.TextDisabled;
			out.Border = { theme.Border.R, theme.Border.G, theme.Border.B, 0.6f };
			return out;
		}

		bool DisabledFor(const WuiComponentDraw& draw, const char* property = "disabled")
		{
			return draw.State == "disabled" || BoolProperty(draw, property, false);
		}

		WuiTheme ThemedFor(const WuiComponentDraw& draw, const WuiTheme& theme, const char* property = "disabled")
		{
			return DisabledFor(draw, property) ? DisabledTheme(theme) : theme;
		}

		// 长文本压力:LongTextState 时把默认文案换成明显超宽的一串,用于观察裁剪/溢出行为。
		std::string MaybeLongText(const WuiComponentDraw& draw, const std::string& text)
		{
			if (draw.State != "long-text")
				return text;
			return text + " — The quick brown fox jumps over the lazy dog 0123456789";
		}

		struct Vec3Slot
		{
			glm::vec3 Value { 0.0f, 0.0f, 0.0f };
			std::string Applied;
			bool Initialized = false;
		};

		// "x,y,z" 形式的向量覆盖值;非法文本忽略。
		glm::vec3& DrivenVec3(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name,
			const glm::vec3& initial)
		{
			Vec3Slot& state = ctx.Persist<Vec3Slot>(HashId(slot), Vec3Slot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				glm::vec3 parsed {};
				int count = 0;
				size_t start = 0;
				while (start <= text->size() && count < 3)
				{
					const size_t separator = text->find_first_of(",;| ", start);
					const std::string token = text->substr(start,
						separator == std::string::npos ? std::string::npos : separator - start);
					if (!token.empty())
					{
						char* end = nullptr;
						const float value = std::strtof(token.c_str(), &end);
						if (end != token.c_str() && std::isfinite(value))
							parsed[count++] = value;
					}
					if (separator == std::string::npos)
						break;
					start = separator + 1;
				}
				if (count == 3)
					state.Value = parsed;
				state.Applied = applied;
			}
			else if (!text)
			{
				state.Applied.clear();
			}
			return state.Value;
		}

		// ---- 组件 showcase(每条正好一件真实控件) ----

		void ShowButton(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 128.0f);
			const WuiId id = BeginShowcase(draw, "button", "Button", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect, true);
			Button(ctx, id, slot.Rect, MaybeLongText(draw, LocalizedText(draw, "label", "Apply", "应用")), themed);
		}

		void ShowIconButton(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 32.0f);
			const WuiId id = BeginShowcase(draw, "button.icon", "Icon Button", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			// textureId=0 → 控件自己退化成语义文字按钮(真实回退路径,不是复刻)。
			ToolbarIconButton(ctx, id, slot.Rect, 0, WuiRect { 0.0f, 0.0f, 1.0f, 1.0f },
				MaybeLongText(draw, LocalizedText(draw, "label", "Save", "保存")), themed, !DisabledFor(draw));
		}

		void ShowResetDefaultButton(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 24.0f);
			const WuiId id = BeginShowcase(draw, "button.reset-default", "Reset Default", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			const bool modified = StateIs(draw, { "modified" }) || BoolProperty(draw, "modified", true);
			ResetDefaultButton(ctx, id, slot.Rect, modified, themed,
				LocalizedText(draw, "label", "Reset", "重置"),
				LocalizedText(draw, "tooltip", "Restore the default value", "恢复默认值"));
		}

		void ShowToggle(const WuiComponentDraw& draw)
		{
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

		void ShowSegmented(const WuiComponentDraw& draw)
		{
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

		void ShowCheckbox(const WuiComponentDraw& draw)
		{
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

		void ShowCheckboxMixed(const WuiComponentDraw& draw)
		{
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

		void ShowSliderFloat(const WuiComponentDraw& draw)
		{
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

		void ShowDragFloat(const WuiComponentDraw& draw)
		{
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

		void ShowDragBarFloat(const WuiComponentDraw& draw)
		{
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

		void ShowNumberFieldInt(const WuiComponentDraw& draw)
		{
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

		void ShowStepperInt(const WuiComponentDraw& draw)
		{
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

		void ShowTextField(const WuiComponentDraw& draw)
		{
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

		void ShowTextFieldError(const WuiComponentDraw& draw)
		{
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

		void ShowComboImpl(const WuiComponentDraw& draw, const char* componentId, const char* displayName, bool searchable)
		{
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
			if (draw.State == "open" && !ctx.IsPopupOpen(id))
				ctx.OpenPopup(id);
			else if (draw.State != "open" && ctx.IsPopupOpen(id))
				ctx.ClosePopup(id);
			if (searchable)
				SearchableCombo(ctx, id, slot.Rect, LocalizedText(draw, "label", "Material", "材质"), options, selected, themed);
			else
				Combo(ctx, id, slot.Rect, LocalizedText(draw, "label", "Quality", "画质"), options, selected, themed);
			selectedState = selected;
		}

		void ShowCombo(const WuiComponentDraw& draw)
		{
			ShowComboImpl(draw, "combo", "Combo", false);
		}

		void ShowSearchableCombo(const WuiComponentDraw& draw)
		{
			ShowComboImpl(draw, "combo.searchable", "Searchable Combo", true);
		}

		void ShowColorField(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 190.0f);
			const WuiId id = BeginShowcase(draw, "colorfield", "Color Field", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			if (draw.State == "open" && !ctx.IsPopupOpen(id))
				ctx.OpenPopup(id);
			else if (draw.State != "open" && ctx.IsPopupOpen(id))
				ctx.ClosePopup(id);
			glm::vec4& color = DrivenColor(ctx, "showcase.colorfield.value", draw, "color", { 0.30f, 0.55f, 1.00f, 1.00f });
			ColorField(ctx, id, slot.Rect, color, themed);
		}

		void ShowVec3Field(const WuiComponentDraw& draw)
		{
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

		void ShowSearchField(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 200.0f);
			const WuiId id = BeginShowcase(draw, "searchfield", "Search Field", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			std::string& buffer = DrivenText(ctx, "showcase.searchfield.value", draw, "value", std::string());
			SearchField(ctx, id, slot.Rect, buffer,
				LocalizedText(draw, "placeholder", "Search assets", "搜索资源"), themed);
		}

		void ShowSplitter(const WuiComponentDraw& draw)
		{
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

		void ShowTabs(const WuiComponentDraw& draw)
		{
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

		void ShowTreeNode(const WuiComponentDraw& draw)
		{
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

		void ShowTreeView(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 230.0f, 96.0f);
			const WuiId id = BeginShowcase(draw, "treeview", "Tree View", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			std::vector<TreeViewItem> items;
			items.push_back({ ShellId("treeview.item.0"), LocalizedText(draw, "label", "Assets", "资源"),
				0, true, true, false, false });
			items.push_back({ ShellId("treeview.item.1"), "Textures", 1, false, false, true, false });
			items.push_back({ ShellId("treeview.item.2"), "Materials", 1, false, false, false, false });
			items.push_back({ ShellId("treeview.item.3"), LocalizedText(draw, "disabled", "Locked", "已锁定"),
				1, false, false, false, true });
			float& scroll = DrivenFloat(ctx, "showcase.treeview.scroll", draw, "scroll", 0.0f, 0.0f, 200.0f);
			TreeView(ctx, slot.Rect, items, 22.0f * slot.Scale * slot.Density, scroll, theme, id);
		}

		void ShowListView(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, *draw.Theme, 230.0f, 96.0f);
			const WuiId id = BeginShowcase(draw, "listview", "List View", slot.Rect);
			PseudoState pseudo(draw, id, slot.Rect);
			std::vector<ListViewItem> items;
			ListViewItem assets;
			assets.Id = ShellId("listview.item.0");
			assets.Label = LocalizedText(draw, "label", "Textures", "贴图");
			assets.SubLabel = "12";
			assets.Selected = true;
			items.push_back(assets);
			ListViewItem materials = assets;
			materials.Id = ShellId("listview.item.1");
			materials.Label = "Materials";
			materials.SubLabel = "4";
			materials.Selected = false;
			items.push_back(materials);
			ListViewItem locked = assets;
			locked.Id = ShellId("listview.item.2");
			locked.Label = LocalizedText(draw, "disabled", "Locked", "已锁定");
			locked.SubLabel.clear();
			locked.Selected = false;
			locked.Disabled = true;
			items.push_back(locked);
			float& scroll = DrivenFloat(ctx, "showcase.listview.scroll", draw, "scroll", 0.0f, 0.0f, 200.0f);
			ListView(ctx, slot.Rect, items, 24.0f * slot.Scale * slot.Density, scroll, theme);
		}

		void ShowTableHeader(const WuiComponentDraw& draw)
		{
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

		void ShowScrollArea(const WuiComponentDraw& draw)
		{
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
			if (BeginScrollArea(ctx, slot.Rect, contentHeight, scroll, theme))
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

		void ShowModal(const WuiComponentDraw& draw)
		{
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

		void ShowSectionHeader(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 220.0f);
			const WuiId id = BeginShowcase(draw, "sectionheader", "Section Header", slot.Rect);
			(void)id;
			SectionHeader(ctx, slot.Rect, MaybeLongText(draw, LocalizedText(draw, "title", "Transform", "变换")),
				themed.Text, themed, 15.0f * slot.Scale);
		}

		void ShowTooltip(const WuiComponentDraw& draw)
		{
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

		void ShowBreadcrumb(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme themed = ThemedFor(draw, *draw.Theme);
			const Slot slot = Canvas(draw, *draw.Theme, 230.0f);
			const WuiId id = BeginShowcase(draw, "breadcrumb", "Breadcrumb", slot.Rect);
			(void)id;
			Breadcrumb(ctx, slot.Rect, TextProperty(draw, "path", "assets/textures/icon.png"), themed);
		}

		void ShowContextMenu(const WuiComponentDraw& draw)
		{
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

		void ShowEmptyState(const WuiComponentDraw& draw)
		{
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

		void ShowProgress(const WuiComponentDraw& draw)
		{
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

		void ShowCodeEditor(const WuiComponentDraw& draw)
		{
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

		// ---- P1a:面板里的保留模式件(WuiLabel / WuiImage / WuiBox / WuiSpacer / WuiListRow)----
		// 这五件只在面板里以**对象树**出现(Editor/src/WUI/Panels/**),showcase 走与 WuiProgress
		// 完全相同的路径:建树 → LayoutWidgetTree → WuiPaintContext::Paint —— 不是复刻 demo。

		void PaintRetained(const WuiComponentDraw& draw, const WuiWidgetPtr& root, const WuiRect& rect)
		{
			LayoutWidgetTree(root, rect);
			WuiPaintContext paint(*draw.Context);
			root->Paint(paint);
		}

		WuiWidgetPtr RetainedLabel(const std::string& text, float fontSize, const WuiColor& color, bool bold = false)
		{
			auto label = std::make_shared<WuiLabel>();
			label->Text = text;
			label->FontSize = fontSize;
			label->Color = color;
			label->Bold = bold;
			return label;
		}

		void ShowLabel(const WuiComponentDraw& draw)
		{
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

		void ShowImage(const WuiComponentDraw& draw)
		{
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

		void ShowBox(const WuiComponentDraw& draw)
		{
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

		void ShowSpacer(const WuiComponentDraw& draw)
		{
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

		void ShowListRow(const WuiComponentDraw& draw)
		{
			WuiContext& ctx = *draw.Context;
			const WuiTheme& theme = *draw.Theme;
			const Slot slot = Canvas(draw, theme, 224.0f, 72.0f);
			const WuiId id = BeginShowcase(draw, "listrow", "List Row", slot.Rect);
			auto column = std::make_shared<WuiBox>();
			column->Direction = WuiDirection::Column;
			column->Gap = 2.0f * slot.Scale;
			column->Add(RetainedLabel(LocalizedText(draw, "root", "Assets", "资源"), 13.0f * slot.Scale, theme.TextMuted));

			auto row = std::make_shared<WuiListRow>();
			row->SetId(id);   // 行自己登记 kind=list-row(与层级面板同一条 a11y 契约),覆盖外壳锚点
			row->Text = MaybeLongText(draw, LocalizedText(draw, "label", "Textures/Icon.png", "textures/Icon.png"));
			row->FontSize = 14.0f * slot.Scale;
			row->Indent = DrivenFloat(ctx, "showcase.listrow.indent", draw, "indent", 14.0f, 0.0f, 32.0f) * slot.Scale;
			row->Selected = draw.State == "selected" || BoolProperty(draw, "selected", false);
			row->AccessValue = row->Selected ? "depth=1 selected=true" : "depth=1 selected=false";
			column->Add(row);

			column->Add(RetainedLabel(LocalizedText(draw, "sibling", "Scenes", "场景"), 13.0f * slot.Scale, theme.TextMuted));
			// hover 伪状态:鼠标落到画布中心 ≈ 中间那行的位置(布局变化时仍落在行内)。
			PseudoState pseudo(draw, id, slot.Rect);
			PaintRetained(draw, column, slot.Rect);
		}

		// ---- 登记存储 ----

		struct RegistryStore
		{
			std::vector<WuiComponentDesc> Items;
			std::unordered_map<std::string, size_t> Index;
			bool Sorted = true;
		};

		RegistryStore& Store()
		{
			static RegistryStore store;
			return store;
		}

		void EnsureSorted(RegistryStore& store)
		{
			if (store.Sorted)
				return;
			std::sort(store.Items.begin(), store.Items.end(),
				[](const WuiComponentDesc& left, const WuiComponentDesc& right)
				{
					if (left.Category != right.Category)
						return left.Category < right.Category;
					if (left.DisplayName != right.DisplayName)
						return left.DisplayName < right.DisplayName;
					return left.Id < right.Id;   // 同分类同名时的确定性 tie-break
				});
			store.Index.clear();
			for (size_t i = 0; i < store.Items.size(); ++i)
				store.Index[store.Items[i].Id] = i;
			store.Sorted = true;
		}

		void RegisterBuiltins();
		std::once_flag& BuiltinsOnce()
		{
			static std::once_flag once;
			return once;
		}
	}

	const std::vector<WuiComponentDesc>& WuiComponentRegistry::All()
	{
		std::call_once(BuiltinsOnce(), RegisterBuiltins);
		RegistryStore& store = Store();
		EnsureSorted(store);
		return store.Items;
	}

	const WuiComponentDesc* WuiComponentRegistry::Find(const std::string& id)
	{
		std::call_once(BuiltinsOnce(), RegisterBuiltins);
		RegistryStore& store = Store();
		EnsureSorted(store);
		const auto found = store.Index.find(id);
		if (found == store.Index.end())
			return nullptr;
		return &store.Items[found->second];
	}

	void WuiComponentRegistry::Register(WuiComponentDesc desc)
	{
		RegistryStore& store = Store();
		if (desc.Id.empty())
		{
			WLD_CORE_WARN("[wui] component registry: 拒绝空 id 的登记项(DisplayName='{0}')", desc.DisplayName);
			return;
		}
		const auto found = store.Index.find(desc.Id);
		if (found != store.Index.end())
		{
			// 幂等:同 id 覆盖 + 一条可读告警(后登记者胜,注册顺序不参与语义)。
			WLD_CORE_WARN("[wui] component registry: 组件 id '{0}' 重复登记,覆盖旧条目('{1}')",
				desc.Id, store.Items[found->second].DisplayName);
			store.Items[found->second] = std::move(desc);
			store.Sorted = false;
			return;
		}
		store.Index[desc.Id] = store.Items.size();
		store.Items.push_back(std::move(desc));
		store.Sorted = false;
	}

	size_t WuiComponentRegistry::Count()
	{
		std::call_once(BuiltinsOnce(), RegisterBuiltins);
		return Store().Items.size();
	}

	namespace
	{
		// ---- 登记表构造小工具 ----

		Property PropBool(const char* name)
		{
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Bool;
			return property;
		}

		Property PropFloat(const char* name, float min, float max, float step)
		{
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Float;
			property.Min = min;
			property.Max = max;
			property.Step = step;
			return property;
		}

		Property PropInt(const char* name, float min, float max, float step)
		{
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Int;
			property.Min = min;
			property.Max = max;
			property.Step = step;
			return property;
		}

		Property PropText(const char* name, const char* sample)
		{
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Text;
			property.DefaultText = sample;
			return property;
		}

		const char* StateLabel(const std::string& id)
		{
			static const std::pair<const char*, const char*> kLabels[] = {
				{ "default", "Default" },
				{ "hover", "Hover" },
				{ "pressed", "Pressed" },
				{ "focus", "Focus" },
				{ "disabled", "Disabled" },
				{ "error", "Error" },
				{ "open", "Open" },
				{ "on", "On" },
				{ "off", "Off" },
				{ "checked", "Checked" },
				{ "unchecked", "Unchecked" },
				{ "mixed", "Mixed" },
				{ "selected", "Selected" },
				{ "scrolled", "Scrolled" },
				{ "empty", "Empty" },
				{ "modified", "Modified" },
				{ "long-text", "Long text" },
				{ "vertical", "Vertical" },
			};
			for (const std::pair<const char*, const char*>& entry : kLabels)
				if (id == entry.first)
					return entry.second;
			return nullptr;
		}

		std::vector<State> StateList(std::initializer_list<const char*> ids)
		{
			std::vector<State> states;
			states.reserve(ids.size());
			for (const char* id : ids)
			{
				State state;
				state.Id = id;
				const char* label = StateLabel(state.Id);
				state.Label = label != nullptr ? label : state.Id;
				states.push_back(std::move(state));
			}
			return states;
		}

		std::vector<std::string> A11yIds(std::initializer_list<const char*> ids)
		{
			return std::vector<std::string>(ids.begin(), ids.end());
		}

		// 外壳锚点 id(HashId("showcase." + 登记 id));控件自身登记同 id 节点时以控件为准。
		std::vector<std::string> ShellIds(const char* componentId)
		{
			std::vector<std::string> ids;
			ids.push_back(std::string("showcase.") + componentId);
			return ids;
		}

		// typeName 见文件头约定 5):面板侧控件入口名(保留模式类名,或立即模式控件入口名)。
		WuiComponentDesc Desc(const char* id, const char* typeName, const char* displayName, const char* category,
			WuiComponentStatus status, const char* sourceFile, const char* a11yNotes, const char* sizeNotes,
			std::vector<std::string> extraA11yIds, std::vector<State> states, std::vector<Property> properties,
			void (*showcase)(const WuiComponentDraw&))
		{
			WuiComponentDesc desc;
			desc.Id = id;
			desc.TypeName = typeName;
			desc.DisplayName = displayName;
			desc.Category = category;
			desc.Status = status;
			desc.SourceFile = sourceFile;
			desc.A11yNotes = a11yNotes;
			desc.SizeNotes = sizeNotes;
			desc.ExtraA11yIds = std::move(extraA11yIds);
			desc.States = std::move(states);
			desc.Properties = std::move(properties);
			desc.Showcase = showcase;
			return desc;
		}

		// ---- 登记现有控件(P0-2) ----
		// Status:P1a 起**全部为 Draft**。Approved 只能由 P1 的"像素基线 + 探针 + 批准提交号"证据产生
		// (工作台的 Approve 记录在 build/**/approved.json,不直接改登记表);Deprecated 留给将来退役的件。

		void RegisterBuiltins()
		{
			// ---- Buttons ----
			WuiComponentRegistry::Register(Desc(
				"button", "WuiButton", "Button", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=button;id=HashId('showcase.button')(控件自身登记);label=label 属性;disabled 用主题禁用令牌;焦点环走 DrawFocusRing",
				"showcase 首选 128x24;控件无最小宽(窄于文字会溢出);H=theme.ControlHeight×Density×UiScale",
				ShellIds("button"),
				StateList({ "default", "hover", "pressed", "focus", "disabled", "long-text" }),
				{ PropText("label", "Apply"), PropBool("disabled") },
				&ShowButton));

			WuiComponentRegistry::Register(Desc(
				"button.icon", "WuiImageButton", "Icon Button", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无(控件不登记节点);showcase 外壳节点 kind=component-root、interactive=false;label=label 属性;textureId=0 时退化成语义文字",
				"showcase 首选 32x24(方形图标位);enabled=false 时节点仍由外壳提供",
				ShellIds("button.icon"),
				StateList({ "default", "hover", "disabled", "long-text" }),
				{ PropText("label", "Save"), PropBool("disabled") },
				&ShowIconButton));

			WuiComponentRegistry::Register(Desc(
				"button.reset-default", "ResetDefaultButton", "Reset Default", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=reset-default;id=HashId('showcase.button.reset-default')(控件自身登记);value=modified/default;enabled/interactive 跟随 modified;tooltip 进节点 Tooltip",
				"固定占位:首选 24x24(设计口径:调用方按行高恒定预留);不因 modified 变尺寸",
				ShellIds("button.reset-default"),
				StateList({ "default", "modified", "hover", "disabled" }),
				{ PropText("label", "Reset"), PropText("tooltip", "Restore the default value"), PropBool("modified") },
				&ShowResetDefaultButton));

			WuiComponentRegistry::Register(Desc(
				"toggle", "Toggle", "Toggle", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=toggle;id=HashId('showcase.toggle')(控件自身登记);value=on/off;状态写回控件自己的 Persist<bool>(id)",
				"showcase 首选 148x24;标签从 24px 起画,16x16 方块垂直居中;H 同上",
				ShellIds("toggle"),
				StateList({ "default", "on", "off", "hover", "focus", "disabled", "long-text" }),
				{ PropText("label", "Enabled"), PropBool("value"), PropBool("disabled") },
				&ShowToggle));

			WuiComponentRegistry::Register(Desc(
				"segmented", "Segmented", "Segmented", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=segmented(组,interactive=false)+ 子节点 kind=segmented-option(派生 id=HashId(str(父id)+'.segment.'+i));组节点覆盖外壳锚点;value=当前下标",
				"showcase 首选 210x24;项等分宽度;建议 2–4 项(空表直接返回)",
				ShellIds("segmented"),
				StateList({ "default", "hover", "focus" }),
				{ PropText("options", "Light,Medium,Heavy"), PropInt("selected", 0, 2, 1) },
				&ShowSegmented));

			// ---- Inputs ----
			WuiComponentRegistry::Register(Desc(
				"checkbox", "Checkbox", "Checkbox", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=checkbox;id=HashId('showcase.checkbox')(控件自身登记);value=true/false;返回值=本帧是否改值",
				"showcase 首选 168x24;16x16 方块 + 24px 起画文字;H 同上",
				ShellIds("checkbox"),
				StateList({ "default", "checked", "unchecked", "hover", "focus", "disabled", "long-text" }),
				{ PropText("label", "Enabled"), PropBool("checked"), PropBool("disabled") },
				&ShowCheckbox));

			WuiComponentRegistry::Register(Desc(
				"checkbox.mixed", "CheckboxMixed", "Checkbox (Mixed)", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=checkbox;id=HashId('showcase.checkbox.mixed');value=mixed/true/false;mixed 由调用方按值传入(本控件不持久化三态)",
				"同 checkbox:首选 168x24",
				ShellIds("checkbox.mixed"),
				StateList({ "default", "mixed", "checked", "unchecked", "disabled", "long-text" }),
				{ PropText("label", "Select all"), PropBool("mixed"), PropBool("disabled") },
				&ShowCheckboxMixed));

			WuiComponentRegistry::Register(Desc(
				"slider.float", "SliderFloat", "Slider", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=slider;id=HashId('showcase.slider.float')(控件自身登记);value=三位小数文本;焦点上 ←/→ = 1% 值域步进",
				"showcase 首选 190x24;轨道 4px 垂直居中;min/max 覆盖非法时退回 0..1",
				ShellIds("slider.float"),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropFloat("value", 0.0f, 1.0f, 0.01f), PropFloat("min", -1000.0f, 1000.0f, 0.1f),
					PropFloat("max", -1000.0f, 1000.0f, 0.1f), PropBool("disabled") },
				&ShowSliderFloat));

			WuiComponentRegistry::Register(Desc(
				"dragfloat", "DragFloat", "Drag Float", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=drag-float(编辑态切 text-field);id=HashId('showcase.dragfloat');value=数值文本;↑/↓ = 步进,speed 为拖动灵敏度",
				"showcase 首选 150x24;范围默认 0..10;min>=max 视为无界(与控件哨兵约定一致)",
				ShellIds("dragfloat"),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropFloat("value", -1000.0f, 1000.0f, 0.1f), PropFloat("min", -1000.0f, 1000.0f, 0.1f),
					PropFloat("max", -1000.0f, 1000.0f, 0.1f), PropFloat("speed", 0.001f, 100.0f, 0.01f),
					PropBool("disabled") },
				&ShowDragFloat));

			WuiComponentRegistry::Register(Desc(
				"dragbar.float", "DragBarFloat", "Drag Bar", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=slider(编辑态切 text-field);id=HashId('showcase.dragbar.float');value=显示值+单位;右侧值区宽度固定(style.ValueWidth,最小 40)",
				"showcase 首选 200x24;值区固定宽度是「点恢复默认不改行矩形」的前提",
				ShellIds("dragbar.float"),
				StateList({ "default", "hover", "focus" }),
				{ PropFloat("value", 0.0f, 1.0f, 0.01f), PropFloat("min", -1000.0f, 1000.0f, 0.1f),
					PropFloat("max", -1000.0f, 1000.0f, 0.1f), PropText("unit", "%") },
				&ShowDragBarFloat));

			WuiComponentRegistry::Register(Desc(
				"numberfield.int", "NumberFieldInt", "Number Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=number-field(编辑态切 text-field);id=HashId('showcase.numberfield.int');value=整数+单位;steppers=true 时额外登记 DerivedChildId(id,'.dec'/'.inc',0) 两个 stepper-button",
				"showcase 首选 128x24;步进钮宽 = clamp(18..24, 18% 宽);计数/索引类字段不做拖动改值",
				ShellIds("numberfield.int"),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropInt("value", 1, 16384, 1), PropText("unit", "px"), PropBool("steppers"), PropBool("disabled") },
				&ShowNumberFieldInt));

			WuiComponentRegistry::Register(Desc(
				"stepper.int", "StepperInt", "Stepper", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=stepper(编辑态切 text-field);id=HashId('showcase.stepper.int');[−]/[+] 为 DerivedChildId(id,'.dec'/'.inc',0)",
				"showcase 首选 132x24;小范围整数(本条目展示 1..16);值区可键入",
				ShellIds("stepper.int"),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropInt("value", 1, 16, 1), PropBool("disabled") },
				&ShowStepperInt));

			WuiComponentRegistry::Register(Desc(
				"textfield", "WuiTextField", "Text Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=text-field;id=HashId('showcase.textfield')(控件自身登记);label=属性 label、占位=属性 placeholder(label/value 都进节点);光标走 TextCursorByte",
				"showcase 首选 210x24;行内无纵向余量;长文本由控件自己滚动/裁剪",
				ShellIds("textfield"),
				StateList({ "default", "hover", "focus", "disabled", "long-text" }),
				{ PropText("value", "Player"), PropText("label", "Name"), PropText("placeholder", "Enter a name"),
					PropBool("disabled") },
				&ShowTextField));

			WuiComponentRegistry::Register(Desc(
				"textfield.error", "TextFieldEx", "Text Field (Error)", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=text-field;id=HashId('showcase.textfield.error');value 追加 ' error=<文本>';错误行画在控件下方一行(Caption),调用方负责留高度",
				"showcase 首选 210x24 + 下方 14px 错误行;错误态描边用 theme.Danger",
				ShellIds("textfield.error"),
				StateList({ "default", "valid", "focus", "disabled" }),
				{ PropText("value", "0.0"), PropText("label", "Mass"), PropText("error", "Must be greater than zero") },
				&ShowTextFieldError));

			WuiComponentRegistry::Register(Desc(
				"combo", "Combo", "Combo", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=combo;id=HashId('showcase.combo');弹层条目 kind=combo-option(id=HashId(str(父id)+'.option.'+i));state=open 时 showcase 打开弹层",
				"showcase 首选 190x24;弹层高 = 条目 22px×n + 8,在画布下方需要留空间",
				ShellIds("combo"),
				StateList({ "default", "hover", "focus", "open", "disabled" }),
				{ PropText("options", "Low,Medium,High,Ultra"), PropInt("selected", 0, 3, 1), PropBool("disabled") },
				&ShowCombo));

			WuiComponentRegistry::Register(Desc(
				"combo.searchable", "SearchableCombo", "Searchable Combo", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=search-combo;id=HashId('showcase.combo.searchable');弹层 combo-option + combo-item(id=HashId(itemKey));过滤串用 id^0x5A17 的持久槽(与编辑缓冲不同 id)",
				"showcase 首选 190x24;选项多时用滚轮;同一 id 的不同类型持久槽是崩溃源,勿混用",
				ShellIds("combo.searchable"),
				StateList({ "default", "hover", "focus", "open" }),
				{ PropText("options", "Low,Medium,High,Ultra"), PropInt("selected", 0, 3, 1) },
				&ShowSearchableCombo));

			WuiComponentRegistry::Register(Desc(
				"colorfield", "ColorField", "Color Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=color-field;id=HashId('showcase.colorfield');value=#RRGGBB(带 alpha 8 位);弹层子节点 .sv./.hue./.alpha./.slider./.preset./.hex.(派生 id 见 WuiWidgets.cpp DerivedChildId)",
				"showcase 首选 190x22;弹层 220x132 需要画布右侧/下方留空间",
				ShellIds("colorfield"),
				StateList({ "default", "hover", "focus", "open", "disabled" }),
				{ PropText("color", "#4C8DFF"), PropBool("disabled") },
				&ShowColorField));

			WuiComponentRegistry::Register(Desc(
				"vec3field", "Vec3Field", "Vec3 Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=vec3-field;id=HashId('showcase.vec3field');value='x,y,z';三个分量各登记 vec3-axis(id=HashId(str(父id)+'.axis.'+i))",
				"showcase 首选 220x24;窄画布(<约 220)时 layout=1 竖排(本条目用 state=vertical 展示)",
				ShellIds("vec3field"),
				StateList({ "default", "hover", "focus", "vertical", "disabled" }),
				{ PropText("value", "0,1,0"), PropFloat("speed", 0.001f, 1.0f, 0.001f), PropBool("disabled") },
				&ShowVec3Field));

			WuiComponentRegistry::Register(Desc(
				"searchfield", "SearchField", "Search Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无(空且未聚焦时不登记节点);focus 后内部转发 TextField(id=HashId('showcase.searchfield')) → kind=text-field;showcase 外壳锚点 kind=component-root",
				"showcase 首选 200x24;放大镜占 18px、清除按钮占 18px",
				ShellIds("searchfield"),
				StateList({ "default", "focus", "disabled" }),
				{ PropText("value", ""), PropText("placeholder", "Search assets"), PropBool("disabled") },
				&ShowSearchField));

			WuiComponentRegistry::Register(Desc(
				"codeeditor", "CodeEditor", "Code Editor", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiCodeEditor.cpp",
				"role=无根节点(编辑器只在补全浮层可见时登记 '<CompletionIdPrefix>.<i>' 与 '.status',悬停登记 '.hover');showcase 外壳锚点 kind=component-root;本条目 = Draft,探针先按像素/命令断言",
				"showcase 首选 280x104;行高 19×UiScale;ErrorLine 用 state=error 展示",
				ShellIds("codeeditor"),
				StateList({ "default", "focus", "error" }),
				{ PropBool("readonly") },
				&ShowCodeEditor));

			// ---- Containers ----
			WuiComponentRegistry::Register(Desc(
				"tabs", "TabBar", "Tabs", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=tab(每个标签一个子节点,id=HashId(str(父id)+'.tab.'+i),value=true/false);父节点不登记 —— showcase 外壳锚点 kind=component-root",
				"showcase 首选 240x24;标签等分整条矩形;中键点击上报 closeRequested(本 showcase 不接)",
				ShellIds("tabs"),
				StateList({ "default", "hover", "focus" }),
				{ PropText("tabs", "General,Rendering,Physics"), PropInt("active", 0, 2, 1) },
				&ShowTabs));

			WuiComponentRegistry::Register(Desc(
				"treenode", "TreeNode", "Tree Node", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=tree-node;id=HashId('showcase.treenode')(控件自身登记);value=open/closed;leaf=true 时 interactive=false 且不进 Tab 顺序",
				"showcase 首选 190x20;展开/收起标记用 '+'/'-' 文本,不依赖字体箭头字形",
				ShellIds("treenode"),
				StateList({ "default", "open", "closed", "hover", "focus", "disabled" }),
				{ PropText("label", "Materials"), PropBool("leaf") },
				&ShowTreeNode));

			WuiComponentRegistry::Register(Desc(
				"treeview", "TreeView", "Tree View", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiChrome.cpp",
				"role=tree-item(每行节点 id=条目自己的 Id —— showcase 用 HashId('showcase.treeview.item.N'),ExtraA11yIds 可复算);value='depth=.. expanded=.. children=..';父容器不登记节点",
				"showcase 首选 230x96(自带滚动裁剪);行高 22×Density;半滚出行不登记节点(中心必须落在可视区)",
				A11yIds({ "showcase.treeview", "showcase.treeview.item.0", "showcase.treeview.item.1",
					"showcase.treeview.item.2", "showcase.treeview.item.3" }),
				StateList({ "default", "hover", "selected", "focus", "disabled" }),
				{ PropText("label", "Assets"), PropText("disabled", "Locked"), PropFloat("scroll", 0.0f, 200.0f, 1.0f) },
				&ShowTreeView));

			WuiComponentRegistry::Register(Desc(
				"listview", "ListView", "List View", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiChrome.cpp",
				"role=无(列表行不登记 a11y 节点);showcase 外壳锚点 kind=component-root、interactive=false;行 Id 仍是稳定 id 但只用于调用方交互",
				"showcase 首选 230x96(自带滚动裁剪);行高 24×Density;选中行底色 theme.PanelBg、悬停 ButtonHover",
				ShellIds("listview"),
				StateList({ "default", "hover", "selected", "disabled", "scrolled" }),
				{ PropText("label", "Textures"), PropText("disabled", "Locked"), PropFloat("scroll", 0.0f, 200.0f, 1.0f) },
				&ShowListView));

			WuiComponentRegistry::Register(Desc(
				"table.header", "TableHeader", "Table Header", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=table-header(每列一个子节点 id=HashId(str(父id)+'.col.'+i),value=asc/desc/空);父节点不登记 —— 外壳锚点 kind=component-root",
				"showcase 首选 260x24 表头 + 22px 数据行;列宽等分(真实表格由调用方给列宽表)",
				ShellIds("table.header"),
				StateList({ "default", "hover", "focus" }),
				{ PropText("columns", "Name,Type,Size"), PropInt("sort", 0, 2, 1), PropBool("ascending") },
				&ShowTableHeader));

			WuiComponentRegistry::Register(Desc(
				"scrollarea", "WuiScrollArea", "Scroll Area", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=无(裁剪容器不登记节点,调用方给内容行各自登记);外壳锚点 kind=component-root;滚轮只改 scrollY",
				"showcase 首选 220x96,内容 8 行×24;state=scrolled 展示滚动后的裁剪边界",
				ShellIds("scrollarea"),
				StateList({ "default", "scrolled" }),
				{ PropFloat("scroll", 0.0f, 400.0f, 1.0f), PropText("rowPrefix", "Row ") },
				&ShowScrollArea));

			WuiComponentRegistry::Register(Desc(
				"modal", "BeginModal", "Modal Dialog", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=无(外框不登记节点;内部按钮各自登记 button = HashId('showcase.modal.confirm'/'showcase.modal.cancel'));外壳锚点 kind=component-root",
				"居中外框按 ctx.ViewportSize() 计算:size 传入即被采用;遮罩/外框会登记覆盖层(下一帧挡下层),工作台宜给它独立画布",
				A11yIds({ "showcase.modal", "showcase.modal.confirm", "showcase.modal.cancel" }),
				StateList({ "default", "long-text" }),
				{ PropText("title", "Delete entity?"), PropText("message", "This action cannot be undone."),
					PropText("confirm", "Confirm"), PropText("cancel", "Cancel") },
				&ShowModal));

			WuiComponentRegistry::Register(Desc(
				"empty.state", "EmptyState", "Empty State", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=empty-state;节点 id = actionLabel 非空时 DerivedChildId(actionId,'.empty-state.',0),否则 HashId('empty-state:'+title);value=hint;interactive=false;有 action 时按钮由 Button 自己登记",
				"showcase 首选 280x120;左右各留 24px 安全边距;state=empty 展示无 glyph/无按钮的纯文案形态",
				ShellIds("empty.state"),
				StateList({ "default", "empty" }),
				{ PropText("title", "No assets yet"), PropText("hint", "Import a texture or a model to get started."),
					PropText("action", "Import") },
				&ShowEmptyState));

			WuiComponentRegistry::Register(Desc(
				"splitter", "Splitter", "Splitter", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=splitter;id=HashId('showcase.splitter')(控件自身登记);value=当前值;命中带宽固定 6px(传入矩形即那条带)",
				"showcase 首选 200x72(两栏各一块底板);拖动按按下时的值 + 轴向位移累加,夹在 [min,max]",
				ShellIds("splitter"),
				StateList({ "default", "hover", "focus" }),
				{ PropFloat("value", 24.0f, 200.0f, 1.0f) },
				&ShowSplitter));

			WuiComponentRegistry::Register(Desc(
				"box", "WuiBox", "Box (Layout)", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(布局容器不登记节点;子件各自登记自己的 a11y);showcase 外壳锚点 kind=component-root、interactive=false",
				"填满画布(showcase 首选 240x84);Direction/Gap 由属性控制;子件按 intrinsic 尺寸经 SolveFlex 排布 —— 与面板同一条布局路径",
				ShellIds("box"),
				StateList({ "default", "row" }),
				{ PropFloat("gap", 0.0f, 24.0f, 1.0f), PropText("direction", "column") },
				&ShowBox));

			WuiComponentRegistry::Register(Desc(
				"spacer", "WuiSpacer", "Spacer", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(不画像素、不进焦点表,所以没有自己的节点);showcase 外壳锚点 kind=component-root、interactive=false,并用左右两个 WuiLabel 标出它撑开的间距",
				"自身尺寸 = Width×Height(默认 0x0,由调用方给);showcase 首选 220x20;示例间距 48px(UiScale 参与缩放)",
				ShellIds("spacer"),
				StateList({ "default" }),
				{ PropFloat("width", 0.0f, 240.0f, 1.0f), PropText("left", "Left"), PropText("right", "Right") },
				&ShowSpacer));

			WuiComponentRegistry::Register(Desc(
				"listrow", "WuiListRow", "List Row", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=list-row(SetId 后才进树:id=HashId('showcase.listrow');label=行文本;value=AccessValue);外层 WuiBox/同级标题不登记节点",
				"showcase 首选 224x72(父行 + 本行 + 兄弟行);行高 = FontSize+6;Indent 只挪文字,不动选中/悬停底色",
				ShellIds("listrow"),
				StateList({ "default", "hover", "selected" }),
				{ PropText("label", "Textures/Icon.png"), PropBool("selected"), PropFloat("indent", 0.0f, 32.0f, 1.0f) },
				&ShowListRow));

			// ---- Chrome ----
			WuiComponentRegistry::Register(Desc(
				"sectionheader", "SectionHeader", "Section Header", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无(纯展示);外壳锚点 kind=component-root、interactive=false;字号默认 15(UiScale 参与缩放)",
				"showcase 首选 220x24;标题 + 下方 1px 分隔线;长标题按传入宽度绘制(控件不自行省略)",
				ShellIds("sectionheader"),
				StateList({ "default", "long-text" }),
				{ PropText("title", "Transform") },
				&ShowSectionHeader));

			WuiComponentRegistry::Register(Desc(
				"breadcrumb", "Breadcrumb", "Breadcrumb", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无(路径段不登记节点);外壳锚点 kind=component-root;命中按 '/' 分段,返回被点段下标",
				"showcase 首选 230x20;段宽按粗略字宽(7px/字符)算,仅影响命中不影响绘制",
				ShellIds("breadcrumb"),
				StateList({ "default", "hover" }),
				{ PropText("path", "assets/textures/icon.png") },
				&ShowBreadcrumb));

			WuiComponentRegistry::Register(Desc(
				"tooltip", "Tooltip", "Tooltip", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无独立节点(tooltip 是文本,不是可聚焦控件);宿主画完所有面板后调 DrawTooltip 一次,画到 overlay;文本进 ctx.Tooltip() 并由调用控件带进 a11y 节点 Tooltip 字段",
				"气泡跟随光标右下 16/20,超出视口翻到另一侧;最大文本宽 380px,超宽换行",
				ShellIds("tooltip"),
				StateList({ "default" }),
				{ PropText("label", "Hover me"), PropText("tooltip", "Adds a new entity to the current scene.") },
				&ShowTooltip));

			WuiComponentRegistry::Register(Desc(
				"label", "WuiLabel", "Label", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(WuiLabel 不登记节点,纯展示);showcase 外壳锚点 kind=component-root、interactive=false;文本/字号/粗体都由调用方给",
				"showcase 首选 220x20;宽高 = 字宽×文本 / FontSize+6;不裁剪(超宽会溢出调用方的矩形)",
				ShellIds("label"),
				StateList({ "default", "long-text" }),
				{ PropText("text", "Player Name"), PropFloat("fontSize", 8.0f, 32.0f, 1.0f), PropBool("bold") },
				&ShowLabel));

			WuiComponentRegistry::Register(Desc(
				"image", "WuiImage", "Image", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(纯展示纹理,不登记节点);showcase 外壳锚点 kind=component-root、interactive=false;纹理 id 必须来自宿主的 WuiTextureRegistry(0 = 空图,节点仍在)",
				"showcase 首选 96x96;纹理 id / UV / tint 由调用方给(视口、材质预览、模型预览都是这条路径)",
				ShellIds("image"),
				StateList({ "default" }),
				{ PropInt("textureId", 0.0f, 100000.0f, 1.0f), PropText("tint", "#FFFFFF") },
				&ShowImage));

			// ---- Menus ----
			WuiComponentRegistry::Register(Desc(
				"contextmenu", "BeginContextMenu", "Context Menu", "Menus", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=menu-item(每项 id=HashId('showcase.contextmenu.<action>'));带勾选项 value=checked/unchecked;菜单面板本身不登记节点;位置在打开时钉住",
				"面板高 = 条目 22px×itemCount + 8;showcase 画 4 项(含 1 分隔与 1 禁用项)",
				A11yIds({ "showcase.contextmenu", "showcase.contextmenu.copy", "showcase.contextmenu.visible",
					"showcase.contextmenu.delete" }),
				StateList({ "default", "hover", "focus", "disabled" }),
				{ PropText("copy", "Copy"), PropText("visible", "Visible"), PropText("delete", "Delete"),
					PropBool("disabled") },
				&ShowContextMenu));

			// ---- Feedback ----
			WuiComponentRegistry::Register(Desc(
				"progress", "WuiProgress", "Progress Bar", "Feedback", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidget.cpp",
				"role=无(WuiProgress 不登记节点 —— 它是保留模式控件,读数面板用同样的 LayoutWidgetTree+Paint);外壳锚点 kind=component-root;Fraction 夹在 0..1",
				"showcase 首选 200x12;轨道/填充都是 3px 圆角矩形;调用方决定行高",
				ShellIds("progress"),
				StateList({ "default", "disabled" }),
				{ PropFloat("value", 0.0f, 1.0f, 0.01f), PropBool("disabled") },
				&ShowProgress));
		}
	}
}
