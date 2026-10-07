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
namespace WuiWidgetsDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace WuiWidgetsDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace WuiWidgetsDetail
	{
void RegisterAccessNode(WuiId id, const char* kind, const WuiRect& rect, const std::string& label, const std::string& value, bool enabled = true, bool interactive = true, bool focused = false, const std::string& tooltip = std::string());

std::string FloatToText(float value);


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
void AppendUtf8(std::string& buffer, uint32_t codepoint);

void PopUtf8(std::string& buffer);

WuiId DerivedChildId(WuiId parent, const char* category, size_t index);

WuiId ComboOptionId(WuiId comboId, size_t index);

void PushLineQuad(WuiContext& ctx, glm::vec2 from, glm::vec2 to, float thickness, const WuiColor& color);

std::string EllipsizeToWidth(const WuiContext& ctx, std::string_view text, float maxWidth, float fontSize);


	}
void DrawPanelSurface(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme);

void Panel(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiTheme& theme);

void Label(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const WuiColor& color, float fontSize);

void LabelWithTerm(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const std::string& term, const WuiColor& color, float fontSize, const WuiTheme& theme);

void LabelWithTerm(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const std::string& term, const WuiColor& color, float fontSize, const WuiTheme& theme, float width);


	namespace WuiWidgetsDetail
	{
std::vector<std::string> WrapTooltipText(WuiContext& ctx, const std::string& text, float fontSize, float maxWidth);


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
WuiDeferredPopupLayer& DeferredPopupLayer(WuiContext& ctx);


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
void FlushDeferredPopupDraws(WuiContext& ctx);

	}
void Tooltip(WuiContext& ctx, const WuiRect& hoverRect, const std::string& text);

void DrawTooltip(WuiContext& ctx, const WuiTheme& theme);

void DrawFocusRing(WuiContext& ctx, const WuiRect& rect, WuiId id, const WuiTheme& theme, const WuiColor* ringColor);

WuiButtonResolvedStyle ResolveButtonStyle(const WuiButtonStyle* style, bool disabled, bool pressed, bool hovered, bool focused, const WuiColor& fallbackBg, const WuiColor& fallbackBorder, const WuiColor& fallbackText, const WuiColor& fallbackFocusRing);

bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme, const WuiButtonStyle* style);

bool ButtonEx(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme, bool enabled, bool primary, const std::string& tooltip);

bool Toggle(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);

bool TabBar(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::vector<std::string>& tabs, int& active, const WuiTheme& theme, int* closeRequested);

bool Segmented(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::vector<std::string>& options, int& selected, const WuiTheme& theme);

bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool& value, const WuiTheme& theme);

bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const std::string& term, bool& value, const WuiTheme& theme);

bool CheckboxMixed(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool& value, bool mixed, const WuiTheme& theme);

void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme);


	namespace WuiWidgetsDetail
	{
		struct NumericDragState
		{
			bool Editing = false;
			bool Dragging = false;
			std::string Buffer;
			float DragStartX = 0;
		};
	}

	namespace WuiWidgetsDetail
	{
		// PROJ-17:`WasFocused` = 上一帧这个框是否持有焦点(单行文本框用来判断"焦点是从别处进来的",
		// 见 TextFieldCore 里的遗留选区口径)。
		struct WuiEditState
		{
			int Cursor = -1;
			int SelStart = -1;
			int SelEnd = -1;
			int DragAnchor = -1;
			bool MouseSelecting = false;
			bool WasFocused = false;
		};
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
int Utf8Count(const std::string& text);

size_t Utf8Offset(const std::string& text, int cursor);

int CursorAtX(const std::string& text, float x, float fontSize);

void InsertUtf8At(std::string& text, size_t offset, uint32_t codepoint);

void EraseBefore(std::string& text, int& cursor);

void EraseAt(std::string& text, int cursor);

bool EditUpdate(WuiContext& ctx, std::string& buffer, int& cursor, int& selStart, int& selEnd, bool& submitted, bool& cancelled);

void PushTextFieldCommand(WuiContext& ctx, const WuiRect& rect, const std::string& text, const WuiTheme& theme, bool focused, bool hovered, int selStart, int selEnd);

std::string TrimNumberText(std::string text);

std::string FormatFloatDisplay(float value, int decimals);

bool ParseFloatText(const std::string& text, float* out);

bool ParseIntText(const std::string& text, int64_t* out);

void BeginNumericEdit(WuiContext& ctx, WuiNumericState& state, WuiId id, const std::string& text);

void EndNumericEdit(WuiNumericState& state);

void PushNumericTextCommand(WuiContext& ctx, const WuiRect& textRect, const std::string& text, const WuiTheme& theme, const WuiNumericState& state, bool editing, bool rightAlign, const WuiColor& color);

	}
bool DragFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float speed, float min, float max, const WuiTheme& theme);

bool DragInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme);

bool DragBarFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme, const WuiNumberStyle& style);


	namespace WuiWidgetsDetail
	{
bool IntNumberFieldCore(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme, const WuiNumberStyle& style, bool steppers, const char* kind);

	}
bool NumberFieldInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme, const WuiNumberStyle& style);

bool StepperInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int& value, int min, int max, const WuiTheme& theme, const WuiNumberStyle& style);

bool ResetDefaultButton(WuiContext& ctx, WuiId id, const WuiRect& rect, bool modified, const WuiTheme& theme, const std::string& label, const std::string& tooltip);


	// ---- VEC-H2:属性行 / 折叠分组头 / 集合行(属性面板的行语义收进库)----
	namespace WuiWidgetsDetail
	{
		// 几何与字号 = 属性库件的单一事实源(4px 栅格:行高 24、动作列 24、字段高 20)。
		constexpr float kPropertyRowHeight = 24.0f;
		constexpr float kPropertyFieldHeight = 20.0f;
		constexpr float kPropertyActionWidth = 24.0f;
		constexpr float kPropertyActionGap = 2.0f;
		constexpr float kPropertyLabelMaxWidth = 140.0f;
		constexpr float kPropertyLabelSize = 13.0f;
		constexpr float kPropertyActionGlyphSize = 13.0f;
		constexpr float kPropertyDotSize = 4.0f;
		constexpr float kPropertyLabelTextX = 8.0f;
		constexpr float kPropertyFieldGutter = 4.0f;
		// VEC-H4:嵌套结构的每层缩进(4px 栅格 × 3)。只挪标签文字与树导线,不动行/值列几何 ——
		// 同一面板里所有行的值列仍然竖向对齐(Unity Inspector / Unreal Details 的层级语法)。
		constexpr float kPropertyIndentStep = 12.0f;
		// "值不可用/多值不同"的占位文本在值列里的水平内边距(与字段控件的文字起点一致)。
		constexpr float kPropertyPlaceholderPadX = 8.0f;
void RegisterRowNode(WuiId id, const char* kind, const WuiRect& rect, const std::string& label, const std::string& value, bool enabled, bool interactive, bool focused, const std::string& tooltip);

float RowLabelWidth(const WuiRect& row, float requested);

WuiRect InsideActionRect(const WuiRect& row, int index);

WuiRect OutsideActionRect(const WuiRect& row, int index);

const char* RowExpandMarker(bool open);


		// VEC-H5:展开标记的固定推进量(箭头字形 + 尾随空格)。标记字形在字体度量钩子里量不准
		// (实测:MeasureTextWidth("▶ ") ≈ 4,而渲染实际推进 ≈ 13),所以标记**单独画 + 固定占位**:
		// ① 主名被缩略时标记不会跟着消失(用户口径「DirectionalLightComponent 没有折叠标识」);
		// ② 不会因为量成 0 而把主名画到箭头上。13 = 箭头推进 9 + 空格 4(与 13px 标签字号配套)。
		constexpr float kPropertyExpandMarkerAdvance = 13.0f;
void DrawRowIndentGuide(WuiContext& ctx, const WuiRect& row, float labelIndent, const WuiTheme& theme);

void DrawRowLabel(WuiContext& ctx, const WuiRect& row, const std::string& label, const std::string& term, bool open, bool expandable, const WuiColor& color, float labelWidth, const WuiTheme& theme, float labelIndent = 0.0f);

void DrawModifiedDot(WuiContext& ctx, const WuiRect& row, const WuiTheme& theme);

void RowHoverBackdrop(WuiContext& ctx, const WuiRect& row, bool hovered, bool enabled, const WuiTheme& theme);

bool RowActionButton(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& glyph, const std::string& a11yLabel, const std::string& tooltip, bool enabled, const WuiTheme& theme, bool danger);

	}
float PropertyRowLabelWidth(const WuiRect& row);

float PropertyRowHeight();

PropertyRowLayout MeasurePropertyRow(const WuiRect& row, float labelWidth, bool showReset,
	float trailingReserve);

float CollectionActionColumnWidth();

PropertyRowResult PropertyRow(WuiContext& ctx, WuiId id, const WuiRect& row, const PropertyRowDesc& desc, const WuiTheme& theme);

PropertyGroupHeaderResult PropertyGroupHeader(WuiContext& ctx, WuiId id, const WuiRect& row, const PropertyGroupHeaderDesc& desc, const WuiTheme& theme);

CollectionRowResult CollectionRow(WuiContext& ctx, WuiId id, const WuiRect& row, const CollectionRowDesc& desc, const WuiTheme& theme);

bool CollectionActionButton(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& glyph, const std::string& tooltip, bool enabled, const WuiTheme& theme, bool danger);

bool ActionButton(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme, bool enabled, const std::string& tooltip, bool danger);

bool OpenInEditorButton(WuiContext& ctx, WuiId id, const WuiRect& rect, const WuiTheme& theme, bool enabled, const std::string& label, const std::string& tooltip);


	namespace WuiWidgetsDetail
	{
bool TextFieldCore(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, bool* cancelledOut, const std::string& error, const TextFieldA11y* a11y = nullptr);

	}
bool TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, bool* cancelledOut, const TextFieldA11y* a11y);

bool TextFieldEx(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, const std::string& error, const TextFieldA11y* a11y);

void Image(WuiContext& ctx, const WuiRect& rect, uint64_t textureId, const WuiRect& uv, const WuiTheme& theme);

void GradientFill(WuiContext& ctx, const WuiRect& rect, const WuiColor& topLeft, const WuiColor& topRight, const WuiColor& bottomRight, const WuiColor& bottomLeft);

void GradientFill(WuiContext& ctx, const WuiRect& rect, const WuiColor& from, const WuiColor& to, bool vertical);

void LineSegment(WuiContext& ctx, const glm::vec2& from, const glm::vec2& to, const WuiColor& color, float thickness);

std::string EllipsizeMiddleToWidth(const WuiContext& ctx, std::string_view text, float width, float fontSize);

bool Combo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const std::vector<std::string>& options, int& selected, const WuiTheme& theme);

bool SearchableCombo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const std::vector<std::string>& options, int& selected, const WuiTheme& theme);

bool TreeNode(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool leaf, const WuiTheme& theme);

bool BeginMenuBar(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme);

void EndMenuBar(WuiContext& ctx);

bool BeginMenu(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);

void EndMenu(WuiContext& ctx, WuiId id, const WuiRect& panel, const WuiTheme& theme);

bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool enabled, const WuiTheme& theme);

bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool checked, bool enabled, const WuiTheme& theme);

bool BeginModal(WuiContext& ctx, WuiId id, const std::string& title, const glm::vec2& size, WuiRect* panel, const WuiTheme& theme);

void EndModal(WuiContext& ctx, WuiId id);

bool BeginScrollArea(WuiContext& ctx, const WuiRect& viewport, float contentHeight, float& scrollY, const WuiTheme& theme, WuiId id);

void EndScrollArea(WuiContext& ctx);

bool ScrollBar(WuiContext& ctx, WuiId id, const WuiRect& rect, float contentHeight, float viewportHeight, float& scrollY, const WuiTheme& theme, bool pageButtons);

WuiRect TableCell(const WuiRect& table, const std::vector<float>& columns, size_t row, size_t column, float rowHeight);


	// ---- P4-UX7 / U2B:表头排序 / 颜色字段 / 分隔条 ----
	namespace WuiWidgetsDetail
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
glm::vec3 ColorToHsv(const glm::vec3& rgb);

glm::vec3 HsvToColor(const glm::vec3& hsv);

WuiColor ToWuiColor(const glm::vec3& rgb, float alpha = 1.0f);


		// 分隔条:命中带宽与线宽(6px 命中带、视觉恒 1px;P4-U29 去掉了 hover/drag 加粗)。
		constexpr float kSplitterHitWidth = 6.0f;
		constexpr float kSplitterLineWidth = 1.0f;

		// 表头:列名右侧给排序箭头保留的宽度(派工确认 14px)。
		constexpr float kTableSortArrowReserve = 14.0f;
std::string FormatColorHex(const glm::vec4& rgba);

bool ParseColorHex(std::string_view text, glm::vec4& out);

bool SameColor(const glm::vec4& a, const glm::vec4& b);

	}
bool TableHeader(WuiContext& ctx, WuiId id, const WuiRect& table, const std::vector<std::string>& columns, const std::vector<float>& columnWidths, int& sortColumn, bool& ascending, const WuiTheme& theme);

bool ColorField(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec4& rgba, const WuiTheme& theme);

bool Splitter(WuiContext& ctx, WuiId id, const WuiRect& rect, bool vertical, float& value, float minValue, float maxValue, const WuiTheme& theme);


	// ---- P4-UX12 / U2C / MAT-UI3a:向量字段(2/3/4 分量共用同一份实现) / 空状态 ----
	namespace WuiWidgetsDetail
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
std::string VecAxisText(float value);

std::string VecFieldText(const float* value, int count);


		// 标签一律大写单字母(与 Vec3Field 既有外观一致:11px Caption 下大写更清楚)。
		const char* const kVec2AxisLabels[2] = { "X", "Y" };
		const char* const kVec3AxisLabels[3] = { "X", "Y", "Z" };
		const char* const kVec4AxisLabels[4] = { "X", "Y", "Z", "W" };
	}
bool VecFieldCore(WuiContext& ctx, WuiId id, const WuiRect& rect, float* value, int count, int columns, const char* roleKind, const char* axisKind, const char* const* axisLabels, float speed, float minValue, float maxValue, const WuiTheme& theme, int layout);

bool Vec2Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec2& value, float speed, float minValue, float maxValue, const WuiTheme& theme, int layout);

bool Vec3Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec3& value, float speed, float minValue, float maxValue, const WuiTheme& theme, int layout);

bool Vec4Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec4& value, float speed, float minValue, float maxValue, const WuiTheme& theme, int layout);

bool EmptyState(WuiContext& ctx, const WuiRect& rect, const std::string& glyph, const std::string& title, const std::string& hint, const std::string& actionLabel, WuiId actionId, const WuiTheme& theme);

WindowControl WindowControls(WuiContext& ctx, const WuiRect& bar, const WuiTheme& theme, bool maximized);

}
