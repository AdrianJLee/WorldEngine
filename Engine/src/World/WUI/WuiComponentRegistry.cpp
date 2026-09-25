#include "wldpch.h"

#include "World/WUI/WuiComponentRegistry.h"

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiTextBuffer.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiControls.h"

#include "World/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
//  6) WUI-P1.5 起:属性覆盖的**值编码协议**由 ParseComponentColor / ParseComponentSize 定义
//     (颜色 #RRGGBB[AA]、尺寸 WxH),属性的 Group/Unit/Doc/StateScoped/类型化默认只是元数据;
//     showcase 侧的语义是"没覆盖 = 沿用主题令牌或旧硬编码口径" —— 因此旧基线不因加值而漂移,
//     per-state 颜色只在用户真的改了那一态时生效。
//  7) WUI-P1.5a2 起,`button` 的两张脸读**同一份** WuiButtonStyle + 同一份解析 ResolveButtonStyle:
//     展示台/面板走立即模式入口 Wui::Button(WuiWidgets.cpp),面板里的保留模式类 Wui::WuiButton
//     (WuiWidget.h/.cpp,成员 `Style`)用同一套状态优先级/覆盖判据/内边距·字号哨兵;
//     未覆盖槽各自回退历史口径(两面差异清单钉在 tests/World/WuiTests.cpp §28)。

namespace World::Wui
{
	namespace
	{
		using Property = WuiComponentProperty;
		using State = WuiComponentState;
		using Interaction = WuiComponentInteraction;

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
		// **只模拟外观,不模拟输入**:伪状态期间清掉按下/抬起沿与"按住的键"(pressed 外观例外,
		// 只有它自己需要 MouseDown 画按下态),否则真实的点击/拖拽会落在假鼠标位置上(P1c-a)。
		class PseudoState
		{
		public:
			PseudoState(const WuiComponentDraw& draw, WuiId focusId, const WuiRect& rect, bool allowPress = false)
				: m_Context(*draw.Context), m_SavedInput(draw.Context->Input()), m_SavedFocus(draw.Context->Focus())
			{
				// WUI-P1.6:Play 模式直通真实输入 —— 不挪鼠标/不设焦点/不清按下沿,
				// hover/pressed/focus 由用户真实交互产生(Edit 模式默认 false,行为不变)。
				if (draw.RouteRealInput)
					return;
				const bool hover = draw.State == "hover" || (allowPress && draw.State == "pressed");
				if (hover)
					m_Context.Input().MousePos = { rect.X + rect.W * 0.5f, rect.Y + rect.H * 0.5f };
				const bool pressedVisual = allowPress && draw.State == "pressed";
				if (pressedVisual)
					m_Context.Input().MouseDown[0] = true;
				else
				{
					// 伪状态不是**拖拽**来源:滑杆这类"按住 + 悬停就改值"的控件,如果看见一只
					// 按住的键,就会把值改到假鼠标位置上(实测:slider.float 两轮 default 漂移)。
					m_Context.Input().MouseDown[0] = false;
					m_Context.Input().MouseDown[1] = false;
					m_Context.Input().MouseDown[2] = false;
				}
				if (focusId != 0 && draw.State == "focus")
					m_Context.SetFocus(focusId);
				// ①清按下/抬起沿:组件自己的点击动作会在假鼠标位置上真的发生(实测:组件处于 hover 态
				// 时,点工作台其它按钮会改动它的持久值/选中项 → 两轮之间像素漂移),而且这次按下的
				// 归属会被它抢走,真目标(下拉弹层条目)release 帧确认不到(状态下拉第 3 条起点不动)。
				for (int button = 0; button < 3; ++button)
				{
					m_Context.Input().MouseClicked[button] = false;
					m_Context.Input().MouseReleased[button] = false;
					m_Context.Input().MouseDoubleClicked[button] = false;
				}
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

		// MAT-UI3a:"x,y" / "x,y,z,w" 形式的向量覆盖值(Vec2/Vec4 与 Vec3 同一套解析规则:
		// 逗号/分号/竖线/空格分隔、非法文本忽略、数目必须正好等于 count 才写回)。
		struct VecNSlot
		{
			glm::vec4 Value { 0.0f, 0.0f, 0.0f, 0.0f };
			std::string Applied;
			bool Initialized = false;
		};

		glm::vec4& DrivenVecN(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name,
			int count, const glm::vec4& initial)
		{
			VecNSlot& state = ctx.Persist<VecNSlot>(HashId(slot), VecNSlot {});
			if (!state.Initialized)
			{
				state.Value = initial;
				state.Initialized = true;
			}
			const std::string* text = FindProperty(draw, name);
			const std::string applied = text ? *text : std::string();
			if (text && applied != state.Applied)
			{
				glm::vec4 parsed { 0.0f, 0.0f, 0.0f, 0.0f };
				int found = 0;
				size_t start = 0;
				while (start <= text->size() && found < count)
				{
					const size_t separator = text->find_first_of(",;| ", start);
					const std::string token = text->substr(start,
						separator == std::string::npos ? std::string::npos : separator - start);
					if (!token.empty())
					{
						char* end = nullptr;
						const float value = std::strtof(token.c_str(), &end);
						if (end != token.c_str() && std::isfinite(value))
							parsed[found++] = value;
					}
					if (separator == std::string::npos)
						break;
					start = separator + 1;
				}
				if (found == count)
					state.Value = parsed;
				state.Applied = applied;
			}
			else if (!text)
				state.Applied.clear();
			return state.Value;
		}

		// ---- 组件 showcase(每条正好一件真实控件) ----

		// WUI-P1.5:Button 的 5 态后缀 —— 顺序 = WuiButtonStyle::State(Normal/Hover/Pressed/Disabled/Focused),
		// 也是登记属性名里的状态段(如 "bg.hover")。
		const char* const kButtonStateSuffix[] = { "default", "hover", "pressed", "disabled", "focus" };

		// 通道 → "<通道>.<状态>" 文本覆盖 → 颜色槽;没覆盖/写坏 = 空槽(回退主题令牌)。
		std::optional<WuiColor> ColorOverride(const WuiComponentDraw& draw, const char* channel, const char* state)
		{
			const std::string name = std::string(channel) + "." + state;
			const std::string* text = FindProperty(draw, name.c_str());
			if (text == nullptr || text->empty())
				return std::nullopt;
			WuiColor parsed {};
			return ParseComponentColor(*text, parsed) ? std::optional<WuiColor>(parsed) : std::nullopt;
		}

		// 登记属性 → WuiButtonStyle(只填用户/工作台真的覆盖过的槽)。
		WuiButtonStyle ButtonStyle(const WuiComponentDraw& draw)
		{
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
		void PreferredSizeOverride(const WuiComponentDraw& draw, float& width, float& height)
		{
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

		void ShowButton(const WuiComponentDraw& draw)
		{
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

		void ShowVec2Field(const WuiComponentDraw& draw)
		{
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

		void ShowVec4Field(const WuiComponentDraw& draw)
		{
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

		void ShowListView(const WuiComponentDraw& draw)
		{
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
			const std::string path = TextProperty(draw, "path", "assets/textures/icon.png");
			// P1c-E4:id 已下沉进 Breadcrumb —— 节点/焦点入口由控件自己登记,删掉 P1c-a 的
			// "外壳代登记"(那次是公开头文件还没改的权宜;现在不造第二份节点)。
			// 返回段下标 = 鼠标点击或键盘(←/→ 移光标、Enter/Space 激活)的结果。
			Breadcrumb(ctx, slot.Rect, path, themed, id);
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

		void ShowSeparator(const WuiComponentDraw& draw)
		{
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

		// P1c-LIB1:徽标(W3.1 的"粗体圆形/胶囊字标"缺件)。同一帧画两枚:
		// ① 默认(中性底,填充/字色/字号/粗体都可被属性覆盖);② 变色态(强调色底 + 反白字),
		// 对应面板里的"类型徽标"(Prefab/着色器)——工作台一次就能看到两种着色。
		void ShowBadge(const WuiComponentDraw& draw)
		{
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
		void ShowIcon(const WuiComponentDraw& draw)
		{
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

		void ShowGradient(const WuiComponentDraw& draw)
		{
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
		void ShowLineSegment(const WuiComponentDraw& draw)
		{
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

		void ShowCollapsibleHeader(const WuiComponentDraw& draw)
		{
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

		void ShowButtonEx(const WuiComponentDraw& draw)
		{
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

		void ShowScrollBar(const WuiComponentDraw& draw)
		{
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

	// ---- 属性覆盖的值编码协议(WUI-P1.5;声明与口径见 WuiComponentRegistry.h)----

	bool ParseComponentColor(const std::string& text, WuiColor& out)
	{
		glm::vec4 parsed {};
		if (!ParseHexColor(text, parsed))
			return false;
		out = { parsed.r, parsed.g, parsed.b, parsed.a };
		return true;
	}

	bool ParseComponentSize(const std::string& text, float& width, float& height)
	{
		// "<宽>x<高>":分隔符接受 'x' / 'X' / '*',两侧允许空格;两个数都必须是有限数,
		// 且除了空白不允许有别的残留字符("128x24x2"、"12px" 一律判非法)。
		size_t begin = 0;
		size_t end = text.size();
		while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
			++begin;
		while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
			--end;
		if (begin >= end)
			return false;
		const std::string token = text.substr(begin, end - begin);
		const size_t split = token.find_first_of("xX*");
		if (split == std::string::npos || split == 0 || split + 1 >= token.size())
			return false;
		const std::string left = token.substr(0, split);
		const std::string right = token.substr(split + 1);
		char* leftEnd = nullptr;
		char* rightEnd = nullptr;
		const float parsedWidth = std::strtof(left.c_str(), &leftEnd);
		const float parsedHeight = std::strtof(right.c_str(), &rightEnd);
		const auto onlySpace = [](const char* tail)
		{
			for (; *tail != '\0'; ++tail)
				if (*tail != ' ' && *tail != '\t')
					return false;
			return true;
		};
		if (leftEnd == left.c_str() || rightEnd == right.c_str())
			return false;
		if (!std::isfinite(parsedWidth) || !std::isfinite(parsedHeight))
			return false;
		if (!onlySpace(leftEnd) || !onlySpace(rightEnd))
			return false;
		width = parsedWidth;
		height = parsedHeight;
		return true;
	}

	namespace
	{
		// 0..1 通道 → 两位大写 hex(自动夹取,面板回显用)。
		std::string FormatColorChannel(float value)
		{
			const float clamped = ClampFloat(value, 0.0f, 1.0f);
			const int quantized = static_cast<int>(clamped * 255.0f + 0.5f);
			static const char kDigits[] = "0123456789ABCDEF";
			std::string out;
			out.push_back(kDigits[(quantized >> 4) & 0xF]);
			out.push_back(kDigits[quantized & 0xF]);
			return out;
		}

		// 设计单位 → 最多两位小数、去尾零、去尾点("128.00" → "128")。
		std::string FormatDimension(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
			std::string out = buffer;
			while (!out.empty() && out.back() == '0')
				out.pop_back();
			if (!out.empty() && out.back() == '.')
				out.pop_back();
			return out.empty() ? std::string("0") : out;
		}
	}

	std::string FormatComponentColor(const WuiColor& color)
	{
		std::string out = "#" + FormatColorChannel(color.R) + FormatColorChannel(color.G)
			+ FormatColorChannel(color.B);
		if (color.A < 0.999f)
			out += FormatColorChannel(color.A);
		return out;
	}

	std::string FormatComponentSize(float width, float height)
	{
		return FormatDimension(width) + "x" + FormatDimension(height);
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

		// ---- WUI-P1.5 加值:分组 / 单位 / 类型化默认 / Color / Size2 / 交互契约 ----

		Property Grouped(Property property, WuiComponentPropertyGroup group, const char* doc)
		{
			property.Group = group;
			property.Doc = doc;
			return property;
		}

		Property UnitOf(Property property, const char* unit)
		{
			property.Unit = unit;
			return property;
		}

		Property NumberDefault(Property property, float value)
		{
			property.DefaultNumber = value;
			return property;
		}

		// 颜色属性:DefaultText 与 DefaultColor 都写(旧文本行编辑器只认前者,新面板用后者)。
		Property PropColor(const char* name, const char* defaultText, bool stateScoped, const char* doc)
		{
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Color;
			property.DefaultText = defaultText;
			WuiColor parsed {};
			if (ParseComponentColor(defaultText, parsed))
				property.DefaultColor = parsed;
			property.StateScoped = stateScoped;
			property.Group = WuiComponentPropertyGroup::Style;
			property.Doc = doc;
			return property;
		}

		// 尺寸属性(宽×高,设计单位)。
		Property PropSize2(const char* name, float width, float height, const char* doc)
		{
			Property property;
			property.Name = name;
			property.Type = Property::Kind::Size2;
			property.DefaultSize = { width, height };
			property.DefaultText = FormatComponentSize(width, height);
			property.Group = WuiComponentPropertyGroup::Layout;
			property.Doc = doc;
			return property;
		}

		Interaction Item(WuiInteractionKind kind, const char* targetId, WuiInteractionExpect expect,
			const char* steps, const char* note)
		{
			Interaction out;
			out.Kind = kind;
			out.TargetId = targetId;
			out.Expect = expect;
			out.Steps = steps;
			out.Note = note;
			return out;
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
		// interactions(P1.5)= 交互契约;不传 = 空表(静态件/尚未声明的件),既有登记条目一个都不用改。
		WuiComponentDesc Desc(const char* id, const char* typeName, const char* displayName, const char* category,
			WuiComponentStatus status, const char* sourceFile, const char* a11yNotes, const char* sizeNotes,
			std::vector<std::string> extraA11yIds, std::vector<State> states, std::vector<Property> properties,
			void (*showcase)(const WuiComponentDraw&), std::vector<Interaction> interactions = {})
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
			desc.Interactions = std::move(interactions);
			desc.Showcase = showcase;
			return desc;
		}

		// ---- 登记现有控件(P0-2) ----
		// Status:P1a 起**全部为 Draft**。Approved 只能由 P1 的"像素基线 + 探针 + 批准提交号"证据产生
		// (工作台的 Approve 记录在 build/**/approved.json,不直接改登记表);Deprecated 留给将来退役的件。

		void RegisterBuiltins()
		{
			// ---- Buttons ----
			// WUI-P1.5:Button 样板 —— 属性按 UE 式三块 + Behavior 分组;5 态颜色是**真实生效**的
			// 覆盖值(名字 "<通道>.<状态>",StateScoped=true),`preferred` 决定画布槽位,
			// 字号/粗细/内边距改的是真实绘制参数;全部未覆盖 = 旧硬编码/主题令牌(像素基线不动)。
			WuiComponentRegistry::Register(Desc(
				"button", "WuiButton", "Button", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=button;id=HashId('showcase.button')(控件自身登记);label=label 属性;disabled 用主题禁用令牌;焦点环走 DrawFocusRing",
				"showcase 首选 128x24(preferred 属性可覆盖,宽×高设计单位);控件无最小宽(窄于文字会溢出);H=preferred 高度(未覆盖=theme.ControlHeight)×Density×UiScale",
				ShellIds("button"),
				StateList({ "default", "hover", "pressed", "focus", "disabled", "long-text" }),
				{
					// Content
					Grouped(PropText("label", "Apply"), WuiComponentPropertyGroup::Content,
						"按钮上的文字(文本归 Content;字号/颜色归 Style)。"),
					// Style:排版 + 内边距
					Grouped(UnitOf(NumberDefault(PropFloat("fontSize", 6.0f, 48.0f, 1.0f), 15.0f), "px"),
						WuiComponentPropertyGroup::Style, "文字字号(设计单位;未覆盖 = 15)。"),
					Grouped(PropBool("bold"), WuiComponentPropertyGroup::Style,
						"文字是否粗体(未覆盖 = 常规)。"),
					Grouped(UnitOf(NumberDefault(PropFloat("padding", 0.0f, 40.0f, 1.0f), 8.0f), "px"),
						WuiComponentPropertyGroup::Style, "文字左内边距(未覆盖 = 8)。"),
					// Style:5 态 × 3 通道颜色(#RRGGBB / #RRGGBBAA;未覆盖 = 主题令牌)
					PropColor("bg.default", "#22272F", true, "Normal 态填充(未覆盖 = 主题 ButtonBg)。"),
					PropColor("bg.hover", "#2A3038", true, "Hover 态填充(未覆盖 = 主题 ButtonHover)。"),
					PropColor("bg.pressed", "#2A3038", true, "Pressed 态填充(未覆盖 = Hover 的兜底值)。"),
					PropColor("bg.focus", "#22272F", true, "Focus 且未悬停时的填充(未覆盖 = Normal 的兜底值)。"),
					PropColor("bg.disabled", "#101318", true, "Disabled 态填充(未覆盖 = 主题 ContentBg)。"),
					PropColor("border.default", "#2B3138", true, "Normal 态描边(未覆盖 = 主题 Border)。"),
					PropColor("border.hover", "#2B3138", true, "Hover 态描边(未覆盖 = 主题 Border)。"),
					PropColor("border.pressed", "#2B3138", true, "Pressed 态描边(未覆盖 = 主题 Border)。"),
					PropColor("border.focus", "#4C8DFF", true,
						"Focus 态描边 + 焦点环颜色(未覆盖 = 主题 FocusRing)。"),
					PropColor("border.disabled", "#2B313899", true,
						"Disabled 态描边(未覆盖 = 主题 Border 的 60% alpha)。"),
					PropColor("text.default", "#D7DCE3", true, "Normal 态文字(未覆盖 = 主题 Text)。"),
					PropColor("text.hover", "#D7DCE3", true, "Hover 态文字(未覆盖 = 主题 Text)。"),
					PropColor("text.pressed", "#4C8DFF", true, "Pressed 态文字(未覆盖 = 主题 Accent)。"),
					PropColor("text.focus", "#D7DCE3", true, "Focus 态文字(未覆盖 = 主题 Text)。"),
					PropColor("text.disabled", "#5A626D", true, "Disabled 态文字(未覆盖 = 主题 TextDisabled)。"),
					// Layout
					PropSize2("preferred", 128.0f, 24.0f, "首选尺寸 宽×高(设计单位;未覆盖 = 128x24)。"),
					// Behavior
					Grouped(PropBool("disabled"), WuiComponentPropertyGroup::Behavior,
						"按禁用态绘制(禁用的**行为**由 ButtonEx/调用方决定)。"),
				},
				&ShowButton,
				{
					Item(WuiInteractionKind::Hover, "showcase.button", WuiInteractionExpect::PixelChange,
						"鼠标移到按钮中心(1 帧)→ 移开(1 帧)",
						"悬停填充 = bg.hover(未覆盖 = 主题 ButtonHover);两帧的像素哈希应不同"),
					Item(WuiInteractionKind::Click, "showcase.button", WuiInteractionExpect::Event,
						"在按钮中心注入按下(1 帧)+ 抬起(1 帧)",
						"一次激活;画布命令数不因点击变化,事件由操作记录/调用方观测"),
					Item(WuiInteractionKind::Key, "showcase.button", WuiInteractionExpect::Event,
						"Tab 到按钮出现焦点环 → Enter;再验一次 Space",
						"Enter/Space 等价于一次点击(只认本帧新按下);焦点环颜色 = border.focus"),
				}));

			WuiComponentRegistry::Register(Desc(
				"button.icon", "WuiImageButton", "Icon Button", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=button;id=HashId('showcase.button.icon')(控件自己登记,与保留模式 WuiImageButton 同一口径);label=label 属性;enabled/interactive/focused 跟随 enabled 与焦点;enabled=true 时进焦点表(Tab 可达、Enter/Space 激活,P1c-E4);disabled 不进焦点表但节点仍在;textureId=0 时退化成语义文字",
				"showcase 首选 32x24(方形图标位);enabled=false 时节点仍在,enabled/interactive 都是 false",
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
				"role=segmented;id=HashId('showcase.segmented')(控件自己登记,覆盖外壳锚点);组节点 interactive=true —— 分段铺满整条矩形,按它注入的点击落在组中心那一格分段;组节点 focused=组内是否有焦点(P1c-E4);逐格精确点击/读焦点位用子节点 kind=segmented-option(派生 id=HashId(str(父id)+'.segment.'+i),focused=焦点是否在该格);value=当前选中项文案",
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
				"vec2field", "Vec2Field", "Vec2 Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=vec2-field;id=HashId('showcase.vec2field');value='x,y';两个分量各登记 vec2-axis(id=HashId(str(父id)+'.axis.'+i),label='X'/'Y');默认一行两段,layout=1 两行",
				"showcase 首选 220x24;一行两段(与 Vec3Field 同一行高);窄列(<约 140)用 layout=1 竖排(本条目用 state=vertical 展示)",
				ShellIds("vec2field"),
				StateList({ "default", "hover", "focus", "vertical", "disabled" }),
				{ PropText("value", "0,1"), PropFloat("speed", 0.001f, 1.0f, 0.001f), PropBool("disabled") },
				&ShowVec2Field));

			WuiComponentRegistry::Register(Desc(
				"vec4field", "Vec4Field", "Vec4 Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=vec4-field;id=HashId('showcase.vec4field');value='x,y,z,w';四个分量各登记 vec4-axis(id=HashId(str(父id)+'.axis.'+i),label='X'/'Y'/'Z'/'W');默认 2×2(第一行 x y / 第二行 z w),layout=1 四行",
				"showcase 首选 220x52(2×2:两行各 26);layout=1 竖排要 4 行(窄列/矮槽下更挤,本条目用 state=vertical 展示)",
				ShellIds("vec4field"),
				StateList({ "default", "hover", "focus", "vertical", "disabled" }),
				{ PropText("value", "0,1,0,1"), PropFloat("speed", 0.001f, 1.0f, 0.001f), PropBool("disabled") },
				&ShowVec4Field));

			WuiComponentRegistry::Register(Desc(
				"searchfield", "SearchField", "Search Field", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=text-field;id=HashId('showcase.searchfield') —— P1c-E4 起**无条件**登记节点并进焦点表(Tab 可达;以前空且未聚焦时不登记节点,焦点入口又藏在聚焦分支里 → 永远到不了);label=占位文案、value=内容(空时回落占位文案)、focused 跟随焦点;聚焦后同一节点由内部 TextField 重登记(同 id/同 kind)",
				"showcase 首选 200x24;放大镜占 18px、清除按钮占 18px",
				ShellIds("searchfield"),
				StateList({ "default", "focus", "disabled" }),
				{ PropText("value", ""), PropText("placeholder", "Search assets"), PropBool("disabled") },
				&ShowSearchField));

			WuiComponentRegistry::Register(Desc(
				"codeeditor", "CodeEditor", "Code Editor", "Inputs", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiCodeEditor.cpp",
				"role=code-editor;id=HashId('showcase.codeeditor')(控件自己登记);interactive=true(点它 = 聚焦 + caret 定位,之后 ui.type/ui.key 落在该焦点上);enabled=!readonly;value='<n> lines[/, read-only]';P1c-E4 起进焦点表(Tab 可达、DrawFocusRing),持焦后 Tab 归编辑器(缩进)、Escape 退出;补全浮层可见时另登记 '<CompletionIdPrefix>.<i>' 与 '.status',悬停登记 '.hover'",
				"showcase 首选 280x104;行高 19×UiScale;ErrorLine 用 state=error 展示",
				ShellIds("codeeditor"),
				StateList({ "default", "focus", "error" }),
				{ PropBool("readonly") },
				&ShowCodeEditor));

			// ---- Containers ----
			WuiComponentRegistry::Register(Desc(
				"tabs", "TabBar", "Tabs", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=tab-bar;id=HashId('showcase.tabs')(控件自己登记,覆盖外壳锚点);父节点 interactive=true —— 标签等分整条矩形,按它注入的点击落在组中心那一个标签上;父节点 focused=组内是否有焦点(P1c-E4);逐标签精确点击/读焦点位用子节点 kind=tab(id=HashId(str(父id)+'.tab.'+i),value=true/false,focused=焦点是否在该标签)",
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
				"role=tree(容器,id 同传入 id)+ tree-item(每行节点 id=条目自己的 Id —— showcase 用 HashId('showcase.treeview.item.N'),ExtraA11yIds 可复算;value='depth=.. expanded=.. children=..');容器 P1c-E4 起登记:rect=可见行带、value='items=N selected=<行>'、interactive=true(点它 = 命中中心那一行)、focused=焦点在树上;焦点在树上时容器与当前行都带 focused=true;键盘:↑/↓/Home/End 移光标、←/→ 折叠展开、Enter 激活(只报告,改模型归调用方)",
				"showcase 首选 230x96(自带滚动裁剪);行高 22×Density;半滚出行不登记节点(中心必须落在可视区)",
				A11yIds({ "showcase.treeview", "showcase.treeview.item.0", "showcase.treeview.item.1",
					"showcase.treeview.item.2", "showcase.treeview.item.3" }),
				StateList({ "default", "hover", "selected", "focus", "disabled" }),
				{ PropText("label", "Assets"), PropText("disabled", "Locked"), PropFloat("scroll", 0.0f, 200.0f, 1.0f) },
				&ShowTreeView));

			WuiComponentRegistry::Register(Desc(
				"listview", "ListView", "List View", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiChrome.cpp",
				"role=list(容器,P1c-E4 起 id 由调用方下沉:rect=可见行带、value='items=N selected=<行>'、interactive=true(点它 = 命中中心那一行)、focused=焦点在列表上)+ list-row(控件用调用方给的 items[i].Id 登记,showcase 用 HashId('showcase.listview.item.N'));label=行文案、value=SubLabel、disabled 行 enabled/interactive=false;焦点在列表上时当前行 focused=true;键盘:↑/↓/Home/End 移光标(KeyMoveTo)、Enter/Space 激活(KeyActivate,只报告);行中心必须落在可视区内才登记(半滚出的行不登记)",
				"showcase 首选 230x96(自带滚动裁剪);行高 24×Density;选中行底色 theme.PanelBg、悬停 ButtonHover",
				A11yIds({ "showcase.listview", "showcase.listview.item.0", "showcase.listview.item.1",
					"showcase.listview.item.2" }),
				StateList({ "default", "hover", "selected", "disabled", "scrolled" }),
				{ PropText("label", "Textures"), PropText("disabled", "Locked"), PropFloat("scroll", 0.0f, 200.0f, 1.0f) },
				&ShowListView));

			WuiComponentRegistry::Register(Desc(
				"table.header", "TableHeader", "Table Header", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=table-header-row;id=HashId('showcase.table.header')(控件自己登记,覆盖外壳锚点);整行 interactive=true —— 列等分整行,按它注入的点击落在组中心那一列并触发排序,value='sort=<列> asc|desc';逐列精确点击用子节点 kind=table-header(id=HashId(str(父id)+'.col.'+i),value=asc/desc/空)",
				"showcase 首选 260x24 表头 + 22px 数据行;列宽等分(真实表格由调用方给列宽表)",
				ShellIds("table.header"),
				StateList({ "default", "hover", "focus" }),
				{ PropText("columns", "Name,Type,Size"), PropInt("sort", 0, 2, 1), PropBool("ascending") },
				&ShowTableHeader));

			WuiComponentRegistry::Register(Desc(
				"scrollarea", "WuiScrollArea", "Scroll Area", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=scroll-area(P1c-E4:id 由调用方下沉;value='scroll=<y>/<max>'、interactive=false —— 容器自己不是点击目标,内容行才是);进焦点表后 Tab 可达并画焦点环,焦点在它上面时 ↑/↓=40px、PageUp/PageDown=0.9 屏、Space=下一页、Home/End=两端;鼠标悬停滚轮照旧(旧调用点不传 id = 行为不变)",
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
				"role=list-row(SetId 后才进树与焦点表:id=HashId('showcase.listrow');label=行文本;value=AccessValue;focused=焦点在该行;P1c-E4 起 Tab 可达,Enter/Space 与点击走同一个 OnClick — 行级焦点,不抢调用方的选中模型);外层 WuiBox/同级标题不登记节点",
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
				"separator", "WuiSeparator", "Separator", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiControls.cpp",
				"role=无(纯展示分隔线,不登记节点);外壳锚点 kind=component-root、interactive=false;不进焦点表 —— 分隔线不是输入目标;线色取主题 Border 令牌、厚度=thickness 属性",
				"showcase 首选 220x12(画布高度用 12 而不是 1:线仍画在矩形垂直中心、厚度=thickness×UiScale,占位高一点便于看与点);宽度由调用方给,不改布局",
				ShellIds("separator"),
				StateList({ "default" }),
				{ PropFloat("thickness", 0.5f, 6.0f, 0.5f) },
				&ShowSeparator));

			WuiComponentRegistry::Register(Desc(
				"breadcrumb", "Breadcrumb", "Breadcrumb", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=breadcrumb;id=HashId('showcase.breadcrumb')(P1c-E4 起由控件自己登记:id 参数已下沉进 Breadcrumb,以前的外壳代登记已删);interactive=true —— 点它 = 命中组中心所在的那一段;label=整条路径、value='segments=N cursor=<光标段>'、focused=焦点在整条上;键盘:←/→ 移段光标(该段画悬停同档高亮)、Enter/Space 激活光标段(返回值 = 段下标,与点击同语义);路径段不单独登记节点",
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

			WuiComponentRegistry::Register(Desc(
				"badge", "Badge", "Badge", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无(纯展示徽标,不登记节点);外壳锚点 kind=component-root、interactive=false;填充/字色/字号/粗体都由调用方给(粗体默认开);文字不裁剪(超宽会溢出调用方的矩形)",
				"showcase 首选 168x18,同一帧画两枚(中性底 + 强调色底);正方形(如 16x16 实例徽标)+ radius<0 = 圆,胶囊给 W≥H 的矩形",
				ShellIds("badge"),
				StateList({ "default", "long-text" }),
				{ PropText("text", "Material Shader"), PropText("fill", "#2A3038"), PropText("textColor", "#D7DCE3"),
					PropFloat("fontSize", 8.0f, 20.0f, 0.5f), PropBool("bold") },
				&ShowBadge));

			WuiComponentRegistry::Register(Desc(
				"icon", "Icon", "Icon (Tinted)", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=无(纯展示图标,不登记节点);外壳锚点 kind=component-root、interactive=false;纹理 id 来自宿主 WuiTextureRegistry(与 WuiImage 同一口径),tint 由调用方给;textureId=0 = 空图 → 兜底占位(底 + 2×2 棋盘 + 描边),不是空白、也不是纯色",
				"showcase 首选 32x32;方形图标位(内容网格 20x20 / 列表 16x16 / 工具条 18x18 都按调用方给的矩形画,控件不改布局)",
				ShellIds("icon"),
				StateList({ "default" }),
				{ PropInt("textureId", 0.0f, 100000.0f, 1.0f), PropText("tint", "#FFFFFF") },
				&ShowIcon));

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

			// ---- P1c-LIB2 渐变填充 / 可折叠分区标题 / 禁用+理由按钮 / 有状态滚动条 + P1c-LIB3 线段原语 ----
			WuiComponentRegistry::Register(Desc(
				"gradient", "GradientFill", "Gradient Fill", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=无(纯绘制原语,不登记节点);外壳锚点 kind=component-root、interactive=false;四角颜色顺序 TL/TR/BR/BL(与 WuiDrawCommand::Corners 一致),矩形逐字段透传、不裁剪不圆角",
				"showcase 首选 220x64;两个方向态(纵向上→下 / 横向左→右)共用同一块矩形;四角颜色由调用方给,控件不改几何",
				ShellIds("gradient"),
				StateList({ "default", "horizontal" }),
				{ PropText("top", "#3D4554"), PropText("bottom", "#0D0F14") },
				&ShowGradient));

			WuiComponentRegistry::Register(Desc(
				"line-segment", "LineSegment", "Line Segment", "Chrome", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=无(纯绘制原语,不登记节点);外壳锚点 kind=component-root、interactive=false;端点原样使用(投影/近平面裁剪/夹取留在调用方),法线=(-dy,dx)/|d|、偏移=法线*thickness/2、顶点顺序 {from+n,to+n,to-n,from-n},|to-from|<0.5 不产出命令",
				"showcase 首选 220x64;一条线段 = 一条 Quad 命令(端点由调用方给);thickness 不钳制,斜线态用 x2 厚度;需要裁剪时由调用方套 ClipScope",
				ShellIds("line-segment"),
				StateList({ "default", "diagonal" }),
				{ PropFloat("thickness", 0.5f, 16.0f, 0.5f), PropText("color", "#E4C08A") },
				&ShowLineSegment));

			WuiComponentRegistry::Register(Desc(
				"collapsible", "CollapsibleHeader", "Section Header (Collapsible)", "Containers",
				WuiComponentStatus::Draft,
				"Engine/src/World/WUI/Widgets/WuiChrome.cpp",
				"role=button;id=HashId('showcase.collapsible')(控件自己登记;面板用 caller-provided 稳定 id,如 properties.section.<DisplayName> 的 HashId);label=title、value=open/closed、interactive=true、focused 跟随焦点;进焦点表(Tab 可达),Enter/Space = 切换",
				"showcase 首选 260x24;整行可点;展开/折叠只改底色与标记,自身矩形不变(布局高度由调用方按 open 算)",
				ShellIds("collapsible"),
				StateList({ "default", "collapsed", "hover", "focus" }),
				{ PropText("title", "Shader Parameters"), PropText("trailing", "12 items"), PropBool("open") },
				&ShowCollapsibleHeader));

			WuiComponentRegistry::Register(Desc(
				"button.disabled", "ButtonEx", "Button (Disabled + Reason)", "Buttons", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=button;id=HashId('showcase.button.disabled')(控件自己登记);label=label 属性;enabled=false 时 Enabled=false 且 Value/Tooltip=理由(灰按钮不能没有理由),Interactive 仍为 true;焦点环走 DrawFocusRing,Enter/Space = 激活",
				"showcase 首选 148x24(高 = theme.ControlHeight);禁用态同尺寸弱化绘制,不改矩形;primary=true 时 Accent 填充",
				ShellIds("button.disabled"),
				StateList({ "default", "hover", "focus", "disabled", "primary", "long-text" }),
				{ PropText("label", "Assign"), PropText("reason", "Select a material instance first"),
					PropBool("disabled"), PropBool("primary") },
				&ShowButtonEx));

			WuiComponentRegistry::Register(Desc(
				"scrollbar", "ScrollBar", "Scroll Bar", "Containers", WuiComponentStatus::Draft,
				"Engine/src/World/WUI/WuiWidgets.cpp",
				"role=scrollbar;id=HashId('showcase.scrollbar')(控件自己登记);value='scroll=<y>/<max> ratio=<0..1>'(前缀与 BeginScrollArea 的 scroll=<y>/<max> 同口径);还有滚动量时 Enabled/Interactive=true 并进焦点表,内容装得下时 Enabled/Interactive=false",
				"showcase 首选 14x168(rect.H >= 72 才画上下翻页按钮,按钮高 24);内容高/视口高由调用方给,控件不改布局",
				ShellIds("scrollbar"),
				StateList({ "default", "hover", "focus", "top", "bottom", "disabled" }),
				{ PropFloat("scroll", 0.0f, 400.0f, 1.0f) },
				&ShowScrollBar));
		}
	}
}
