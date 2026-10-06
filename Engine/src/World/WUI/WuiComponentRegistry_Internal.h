#include "wldpch.h"

#include "World/WUI/WuiComponentRegistry.h"

#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiCodeEditor.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiTextBuffer.h"
#include "World/WUI/WuiTexturePicker.h"
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
namespace WuiComponentRegistryDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace WuiComponentRegistryDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace WuiComponentRegistryDetail
	{
		using Property = WuiComponentProperty;
		using State = WuiComponentState;
		using Interaction = WuiComponentInteraction;
float ClampFloat(float value, float low, float high);

const std::string* FindProperty(const WuiComponentDraw& draw, const char* name);

std::string TrimmedLower(const std::string& text);

std::optional<bool> BoolOverride(const WuiComponentDraw& draw, const char* name);

bool BoolProperty(const WuiComponentDraw& draw, const char* name, bool fallback);

std::optional<float> FloatOverride(const WuiComponentDraw& draw, const char* name);

std::optional<int64_t> IntOverride(const WuiComponentDraw& draw, const char* name);

std::string TextProperty(const WuiComponentDraw& draw, const char* name, const std::string& fallback);

bool IsChinese(const WuiComponentDraw& draw);

std::string LocalizedText(const WuiComponentDraw& draw, const char* name, const char* english, const char* chinese);

std::vector<std::string> OptionList(const WuiComponentDraw& draw, const char* name, std::initializer_list<const char*> defaults);


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
int HexDigit(char ch);

bool ParseHexColor(const std::string& text, glm::vec4& out);

bool& BoolState(WuiContext& ctx, const char* slot, bool initial);

float& DrivenFloat(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, float initial, float min, float max);

int64_t& DrivenInt(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, int64_t initial, int64_t min, int64_t max);

std::string& DrivenText(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, const std::string& initial);

glm::vec4& DrivenColor(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, const glm::vec4& initial);

bool StateIs(const WuiComponentDraw& draw, std::initializer_list<const char*> states);


		// ---- 画布与外壳 ----

		struct Slot
		{
			WuiRect Rect {};
			float Scale = 1.0f;
			float Density = 1.0f;
		};
Slot Canvas(const WuiComponentDraw& draw, const WuiTheme& theme, float preferredWidth, float preferredHeight = 0.0f);

WuiId ShellId(const char* componentId);

WuiId BeginShowcase(const WuiComponentDraw& draw, const char* componentId, const char* displayName, const WuiRect& rect);


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
WuiTheme DisabledTheme(const WuiTheme& theme);

bool DisabledFor(const WuiComponentDraw& draw, const char* property = "disabled");

WuiTheme ThemedFor(const WuiComponentDraw& draw, const WuiTheme& theme, const char* property = "disabled");

std::string MaybeLongText(const WuiComponentDraw& draw, const std::string& text);


		struct Vec3Slot
		{
			glm::vec3 Value { 0.0f, 0.0f, 0.0f };
			std::string Applied;
			bool Initialized = false;
		};
glm::vec3& DrivenVec3(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, const glm::vec3& initial);


		// MAT-UI3a:"x,y" / "x,y,z,w" 形式的向量覆盖值(Vec2/Vec4 与 Vec3 同一套解析规则:
		// 逗号/分号/竖线/空格分隔、非法文本忽略、数目必须正好等于 count 才写回)。
		struct VecNSlot
		{
			glm::vec4 Value { 0.0f, 0.0f, 0.0f, 0.0f };
			std::string Applied;
			bool Initialized = false;
		};
glm::vec4& DrivenVecN(WuiContext& ctx, const char* slot, const WuiComponentDraw& draw, const char* name, int count, const glm::vec4& initial);


		// ---- 组件 showcase(每条正好一件真实控件) ----

		// WUI-P1.5:Button 的 5 态后缀 —— 顺序 = WuiButtonStyle::State(Normal/Hover/Pressed/Disabled/Focused),
		// 也是登记属性名里的状态段(如 "bg.hover")。
		const char* const kButtonStateSuffix[] = { "default", "hover", "pressed", "disabled", "focus" };
std::optional<WuiColor> ColorOverride(const WuiComponentDraw& draw, const char* channel, const char* state);

WuiButtonStyle ButtonStyle(const WuiComponentDraw& draw);

void PreferredSizeOverride(const WuiComponentDraw& draw, float& width, float& height);

void ShowButton(const WuiComponentDraw& draw);

void ShowIconButton(const WuiComponentDraw& draw);

void ShowResetDefaultButton(const WuiComponentDraw& draw);

void ShowOpenInEditorButton(const WuiComponentDraw& draw);

void ShowToggle(const WuiComponentDraw& draw);

void ShowSegmented(const WuiComponentDraw& draw);

void ShowCheckbox(const WuiComponentDraw& draw);

void ShowCheckboxMixed(const WuiComponentDraw& draw);

void ShowSliderFloat(const WuiComponentDraw& draw);

void ShowDragFloat(const WuiComponentDraw& draw);

void ShowDragBarFloat(const WuiComponentDraw& draw);

void ShowNumberFieldInt(const WuiComponentDraw& draw);

void ShowStepperInt(const WuiComponentDraw& draw);

void ShowTextField(const WuiComponentDraw& draw);

void ShowTextFieldError(const WuiComponentDraw& draw);

void ShowComboImpl(const WuiComponentDraw& draw, const char* componentId, const char* displayName, bool searchable);

void ShowCombo(const WuiComponentDraw& draw);

void ShowSearchableCombo(const WuiComponentDraw& draw);

WuiTexturePickerState TexturePickerState(const std::string& text);

void ShowTexturePicker(const WuiComponentDraw& draw);

void ShowColorField(const WuiComponentDraw& draw);

void ShowVec3Field(const WuiComponentDraw& draw);

void ShowVec2Field(const WuiComponentDraw& draw);

void ShowVec4Field(const WuiComponentDraw& draw);

void ShowSearchField(const WuiComponentDraw& draw);

void ShowSplitter(const WuiComponentDraw& draw);

void ShowTabs(const WuiComponentDraw& draw);

void ShowTreeNode(const WuiComponentDraw& draw);

void ShowTreeView(const WuiComponentDraw& draw);

void ShowListView(const WuiComponentDraw& draw);

void ShowTableHeader(const WuiComponentDraw& draw);

void ShowScrollArea(const WuiComponentDraw& draw);

void ShowModal(const WuiComponentDraw& draw);

void ShowSectionHeader(const WuiComponentDraw& draw);

void ShowTooltip(const WuiComponentDraw& draw);

void ShowBreadcrumb(const WuiComponentDraw& draw);

void ShowContextMenu(const WuiComponentDraw& draw);

void ShowEmptyState(const WuiComponentDraw& draw);

void ShowProgress(const WuiComponentDraw& draw);

void ShowPlot(const WuiComponentDraw& draw);

void ShowCodeEditor(const WuiComponentDraw& draw);

void PaintRetained(const WuiComponentDraw& draw, const WuiWidgetPtr& root, const WuiRect& rect);

WuiWidgetPtr RetainedLabel(const std::string& text, float fontSize, const WuiColor& color, bool bold = false);

void ShowSeparator(const WuiComponentDraw& draw);

void ShowLabel(const WuiComponentDraw& draw);

void ShowImage(const WuiComponentDraw& draw);

void ShowBadge(const WuiComponentDraw& draw);

void ShowIcon(const WuiComponentDraw& draw);

void ShowBox(const WuiComponentDraw& draw);

void ShowSpacer(const WuiComponentDraw& draw);

void ShowListRow(const WuiComponentDraw& draw);

void ShowGradient(const WuiComponentDraw& draw);

void ShowLineSegment(const WuiComponentDraw& draw);

void ShowCollapsibleHeader(const WuiComponentDraw& draw);

void ShowButtonEx(const WuiComponentDraw& draw);

std::string FloatText3(float value);

void ShowPropertyRow(const WuiComponentDraw& draw);

void ShowPropertyGroupHeader(const WuiComponentDraw& draw);

void ShowCollectionRow(const WuiComponentDraw& draw);

void ShowCollectionActionButton(const WuiComponentDraw& draw);

void ShowScrollBar(const WuiComponentDraw& draw);


		// ---- 登记存储 ----

		struct RegistryStore
		{
			std::vector<WuiComponentDesc> Items;
			std::unordered_map<std::string, size_t> Index;
			bool Sorted = true;
		};
RegistryStore& Store();

void EnsureSorted(RegistryStore& store);


		void RegisterBuiltins();
std::once_flag& BuiltinsOnce();

	}
bool ParseComponentColor(const std::string& text, WuiColor& out);

bool ParseComponentSize(const std::string& text, float& width, float& height);


	namespace WuiComponentRegistryDetail
	{
std::string FormatColorChannel(float value);

std::string FormatDimension(float value);

	}
std::string FormatComponentColor(const WuiColor& color);

std::string FormatComponentSize(float width, float height);


	namespace WuiComponentRegistryDetail
	{
Property PropBool(const char* name);

Property PropFloat(const char* name, float min, float max, float step);

Property PropInt(const char* name, float min, float max, float step);

Property PropText(const char* name, const char* sample);

Property Grouped(Property property, WuiComponentPropertyGroup group, const char* doc);

Property UnitOf(Property property, const char* unit);

Property NumberDefault(Property property, float value);

Property PropColor(const char* name, const char* defaultText, bool stateScoped, const char* doc);

Property PropSize2(const char* name, float width, float height, const char* doc);

Interaction Item(WuiInteractionKind kind, const char* targetId, WuiInteractionExpect expect, const char* steps, const char* note);

const char* StateLabel(const std::string& id);

std::vector<State> StateList(std::initializer_list<const char*> ids);

std::vector<std::string> A11yIds(std::initializer_list<const char*> ids);

std::vector<std::string> ShellIds(const char* componentId);

WuiComponentDesc Desc(const char* id, const char* typeName, const char* displayName, const char* category, WuiComponentStatus status, const char* sourceFile, const char* a11yNotes, const char* sizeNotes, std::vector<std::string> extraA11yIds, std::vector<State> states, std::vector<Property> properties, void (*showcase)(const WuiComponentDraw&), std::vector<Interaction> interactions = {});

void RegisterBuiltins();

	}
}
