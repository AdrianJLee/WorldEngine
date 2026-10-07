#include "wldpch.h"
#include "WUI/Panels/UiDesignerPanel.h"

// M47:纹理引用行的编辑器侧数据适配层(逻辑路径 → 候选/徽标/状态)。
// 面板只传值、收结果:不自己扫盘、不自己算徽标状态(与材质/纹理面板同一入口)。
#include "WUI/Common/TextureRefCatalog.h"

#include "World/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/Utils/Paths.h"
#include "World/UI/UiPainter.h"
#include "World/UI/UiNodeRegistry.h"
#include "World/UI/UiTypes.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <system_error>
#include <utility>

namespace World
{
	namespace
	{
		// ---- M16:工具栏排队 ----
		// 一排 26px 控件;宽度够时只有一排(工具栏高 34),排不下就把 New/Add/Delete/Duplicate
		// 换到第二排(高 62)。换行阈值 = 单排所需最小宽度,由下面的部件宽度算出:
		// 左右边距 12 + 撤销/重做 2×30 + 路径框 120 + Open/Reload/Save/New 4×62 + Add 132
		// + Delete 70 + Duplicate 84 + 9 个 6px 间隙 = 780。
		constexpr float kToolbarRowHeight = 26.0f;
		constexpr float kToolbarPadY = 4.0f;
		constexpr float kToolbarRowGap = 2.0f;
		constexpr float kToolbarHeightSingle = 34.0f;
		constexpr float kToolbarHeightDouble = 62.0f;
		// M19:工具栏又多了 Fit(和即将到来的 Refresh),单排阈值同步上调,否则会挤在一起。
		constexpr float kToolbarSingleRowMinWidth = 860.0f;

		float ToolbarNeededHeight(float panelWidth)
		{
			return panelWidth >= kToolbarSingleRowMinWidth ? kToolbarHeightSingle : kToolbarHeightDouble;
		}

		constexpr float kSplitterWidth = 5.0f;
		constexpr float kColumnPad = 6.0f;
		constexpr float kStatusHeight = 20.0f;
		constexpr float kTreeRowHeight = 22.0f;
		constexpr float kMinColumnWidth = 120.0f;
		constexpr float kMinCanvasWidth = 140.0f;
		// M31:属性字段下方 @key 预览行的高度(小字 + 行距;PropertyContentHeight 与渲染共用)。
		constexpr float kLocalizationPreviewHeight = 16.0f;

		// 属性行的"元数据":一行 = 本地化键 + 英文回退 + 术语对照 + 目标字段 + 解释回退。
		// 字段用枚举寻址,面板按表驱动地生成 Anchor / Layout 的数值行(不手写 16 段重复代码)。
		enum class FloatField
		{
			AnchorMinX, AnchorMinY, AnchorMaxX, AnchorMaxY,
			AnchorPivotX, AnchorPivotY, AnchorOffsetX, AnchorOffsetY,
			AnchorSizeW, AnchorSizeH,
			LayoutGap, LayoutPadL, LayoutPadT, LayoutPadR, LayoutPadB,
			WorldOffsetX, WorldOffsetY, WorldOffsetZ,
		};

		struct FloatRowDef
		{
			const char* Key;      // 本地化标签键(与控件 id 同源:HashId(Key))
			const char* Label;    // 键缺失时的英文回退(也是 zh-CN 未覆盖时的可见文本)
			const char* Term;     // 英文术语对照(不本地化;与 .wui 字段名对齐)
			FloatField Field;
			// M44:解释 tooltip 的英文回退(键 = <Key> + ".doc")。追加在末尾,
			// 既有四元组的顺序与含义都不动 —— 只补文案,不改字段语义。
			const char* Doc;
		};

		const FloatRowDef kAnchorFloatRows[] =
		{
			{ "panel.ui_designer.anchor.min_x", "Min X", "Min.X", FloatField::AnchorMinX,
				"Anchor left edge: horizontal fraction of the parent rect (0 = left, 1 = right). "
				"Equal to Max X means a point anchor, and Size W is then the real width." },
			{ "panel.ui_designer.anchor.min_y", "Min Y", "Min.Y", FloatField::AnchorMinY,
				"Anchor top edge: vertical fraction of the parent rect (0 = top, 1 = bottom). "
				"Equal to Max Y means a point anchor, and Size H is then the real height." },
			{ "panel.ui_designer.anchor.max_x", "Max X", "Max.X", FloatField::AnchorMaxX,
				"Anchor right edge: horizontal fraction of the parent rect (0 = left, 1 = right). "
				"Different from Min X means the node stretches horizontally." },
			{ "panel.ui_designer.anchor.max_y", "Max Y", "Max.Y", FloatField::AnchorMaxY,
				"Anchor bottom edge: vertical fraction of the parent rect (0 = top, 1 = bottom). "
				"Different from Min Y means the node stretches vertically." },
			{ "panel.ui_designer.anchor.pivot_x", "Pivot X", "Pivot.X", FloatField::AnchorPivotX,
				"Horizontal pivot of the node rect (0 = left edge, 0.5 = centre, 1 = right edge). "
				"Only a point anchor uses it." },
			{ "panel.ui_designer.anchor.pivot_y", "Pivot Y", "Pivot.Y", FloatField::AnchorPivotY,
				"Vertical pivot of the node rect (0 = top, 0.5 = centre, 1 = bottom). "
				"Only a point anchor uses it." },
			{ "panel.ui_designer.anchor.offset_x", "Offset X", "Offset.X", FloatField::AnchorOffsetX,
				"Horizontal offset in design units. Point anchor: pivot position measured from the "
				"anchor. Stretch anchor: left inset from the anchor rect." },
			{ "panel.ui_designer.anchor.offset_y", "Offset Y", "Offset.Y", FloatField::AnchorOffsetY,
				"Vertical offset in design units (down is positive). Same point/stretch rule as Offset X." },
			{ "panel.ui_designer.anchor.size_w", "Size W", "Size.W", FloatField::AnchorSizeW,
				"Width in design units. Point anchor: the real width. Stretch anchor: a delta added "
				"to the anchored horizontal span." },
			{ "panel.ui_designer.anchor.size_h", "Size H", "Size.H", FloatField::AnchorSizeH,
				"Height in design units. Point anchor: the real height. Stretch anchor: a delta added "
				"to the anchored vertical span." },
		};

		const FloatRowDef kLayoutFloatRows[] =
		{
			{ "panel.ui_designer.layout.gap", "Gap", "Gap", FloatField::LayoutGap,
				"Space in design units left between children laid out by this container." },
			{ "panel.ui_designer.layout.pad_left", "Padding Left", "Left", FloatField::LayoutPadL,
				"Inner space on the left edge; children and content start after it." },
			{ "panel.ui_designer.layout.pad_top", "Padding Top", "Top", FloatField::LayoutPadT,
				"Inner space on the top edge; children and content start after it." },
			{ "panel.ui_designer.layout.pad_right", "Padding Right", "Right", FloatField::LayoutPadR,
				"Inner space on the right edge; children and content end before it." },
			{ "panel.ui_designer.layout.pad_bottom", "Padding Bottom", "Bottom", FloatField::LayoutPadB,
				"Inner space on the bottom edge; children and content end before it." },
		};

		// M24:世界锚点偏移(设计单位;投影点 + Offset)。
		const FloatRowDef kWorldFloatRows[] =
		{
			{ "panel.ui_designer.world.offset_x", "Offset X", "Offset.X", FloatField::WorldOffsetX,
				"World-space X offset in metres added to the projected target position." },
			{ "panel.ui_designer.world.offset_y", "Offset Y", "Offset.Y", FloatField::WorldOffsetY,
				"World-space Y offset in metres added to the projected target position." },
			{ "panel.ui_designer.world.offset_z", "Offset Z", "Offset.Z", FloatField::WorldOffsetZ,
				"World-space Z offset in metres added to the projected target position." },
		};

		const UI::UiLayoutKind kLayoutKinds[] =
		{
			UI::UiLayoutKind::Absolute,
			UI::UiLayoutKind::Column,
			UI::UiLayoutKind::Row,
			UI::UiLayoutKind::Overlay,
			UI::UiLayoutKind::Grid,
			UI::UiLayoutKind::Flex,
		};

		float* FloatFieldPtr(UI::UiNode& node, FloatField field)
		{
			switch (field)
			{
			case FloatField::AnchorMinX: return &node.Anchor.Min.x;
			case FloatField::AnchorMinY: return &node.Anchor.Min.y;
			case FloatField::AnchorMaxX: return &node.Anchor.Max.x;
			case FloatField::AnchorMaxY: return &node.Anchor.Max.y;
			case FloatField::AnchorPivotX: return &node.Anchor.Pivot.x;
			case FloatField::AnchorPivotY: return &node.Anchor.Pivot.y;
			case FloatField::AnchorOffsetX: return &node.Anchor.Offset.x;
			case FloatField::AnchorOffsetY: return &node.Anchor.Offset.y;
			case FloatField::AnchorSizeW: return &node.Anchor.Size.x;
			case FloatField::AnchorSizeH: return &node.Anchor.Size.y;
			case FloatField::LayoutGap: return &node.Layout.Gap;
			case FloatField::LayoutPadL: return &node.Layout.Padding[0];
			case FloatField::LayoutPadT: return &node.Layout.Padding[1];
			case FloatField::LayoutPadR: return &node.Layout.Padding[2];
			case FloatField::LayoutPadB: return &node.Layout.Padding[3];
			case FloatField::WorldOffsetX: return &node.World.Offset.x;
			case FloatField::WorldOffsetY: return &node.World.Offset.y;
			case FloatField::WorldOffsetZ: return &node.World.Offset.z;
			}
			return nullptr;
		}

		std::string TrimCopy(const std::string& text)
		{
			std::size_t begin = 0;
			while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
				++begin;
			std::size_t end = text.size();
			while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
				--end;
			return text.substr(begin, end - begin);
		}

		std::string FormatFloat(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
			return buffer;
		}

		// ---- M25:Bind / On 段 ----
		// 行尾动作列(= 字段列右侧的 `-` 删除按钮)与行内两字段共用的几何:两条行都按
		// 同一个收窄量排,Target/Source(Event/Command)两行的字段列同宽、上下对齐。
		constexpr float kRowActionWidth = 24.0f;
		constexpr float kRowActionSize = 20.0f;

		// 空行判据与 `ValidateUiDocument` 逐字同源(空 Target/Source、空 Event/Command
		// 都会被它拒绝,`UiDocumentIO::Parse` 也会因此读不回来):纯空白不算空 —— 引擎接受,
		// 面板也按接受处理(不在 UI 里造第二套判据)。
		bool BindingRowIncomplete(const UI::UiBindingDecl& binding)
		{
			return binding.Target.empty() || binding.Source.empty();
		}

		bool CommandRowIncomplete(const UI::UiCommandDecl& command)
		{
			return command.Event.empty() || command.Command.empty();
		}

		// "格式明显非法"的唯一提示判据:没有 `:` 就不可能有协议前缀(UiBinding::Parse 会按
		// 未知协议拒绝)。协议是否合法仍以引擎的 Parse 为唯一判据 —— 这里不猜。
		bool BindingSourceMissesScheme(const UI::UiBindingDecl& binding)
		{
			return !binding.Source.empty() && binding.Source.find(':') == std::string::npos;
		}

		// 段内提示行(0..2 行;空段不提示)。PropertyContentHeight 与 RenderProperties 都调
		// 这一个函数 ⇒ 预留高度与实际绘制不会错位。
		std::vector<std::string> BindRowHints(const UI::UiNode& node)
		{
			std::vector<std::string> hints;
			if (node.Bind.empty())
				return hints;
			bool incomplete = false;
			bool missesScheme = false;
			for (const UI::UiBindingDecl& binding : node.Bind)
			{
				incomplete = incomplete || BindingRowIncomplete(binding);
				missesScheme = missesScheme || BindingSourceMissesScheme(binding);
			}
			if (incomplete)
			{
				hints.push_back(Wui::Tr("panel.ui_designer.bind.incomplete",
					"Empty Target/Source rows are not written to disk."));
			}
			if (missesScheme)
			{
				hints.push_back(Wui::Tr("panel.ui_designer.bind.scheme",
					"Source needs a protocol prefix (e.g. ecs: / service:)."));
			}
			return hints;
		}

		std::vector<std::string> CommandRowHints(const UI::UiNode& node)
		{
			std::vector<std::string> hints;
			if (node.On.empty())
				return hints;
			bool incomplete = false;
			for (const UI::UiCommandDecl& command : node.On)
				incomplete = incomplete || CommandRowIncomplete(command);
			if (incomplete)
			{
				hints.push_back(Wui::Tr("panel.ui_designer.on.incomplete",
					"Empty Event/Command rows are not written to disk."));
			}
			return hints;
		}

		void DashedSegment(Wui::WuiContext& ctx, const glm::vec2& from, const glm::vec2& to,
			const Wui::WuiColor& color, float thickness)
		{
			const glm::vec2 delta = to - from;
			const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
			if (!(length > 0.5f))
				return;
			const glm::vec2 direction = delta / length;
			constexpr float dash = 6.0f;
			constexpr float gap = 4.0f;
			for (float offset = 0.0f; offset < length; offset += dash + gap)
			{
				const float end = std::min(offset + dash, length);
				Wui::LineSegment(ctx, from + direction * offset, from + direction * end, color, thickness);
			}
		}

		void DashedRectOutline(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
			const Wui::WuiColor& color, float thickness)
		{
			if (!(rect.W > 0.0f) || !(rect.H > 0.0f))
				return;
			const glm::vec2 topLeft { rect.X, rect.Y };
			const glm::vec2 topRight { rect.X + rect.W, rect.Y };
			const glm::vec2 bottomRight { rect.X + rect.W, rect.Y + rect.H };
			const glm::vec2 bottomLeft { rect.X, rect.Y + rect.H };
			DashedSegment(ctx, topLeft, topRight, color, thickness);
			DashedSegment(ctx, topRight, bottomRight, color, thickness);
			DashedSegment(ctx, bottomRight, bottomLeft, color, thickness);
			DashedSegment(ctx, bottomLeft, topLeft, color, thickness);
		}

		void MarkerCross(Wui::WuiContext& ctx, const glm::vec2& center, const Wui::WuiColor& color,
			float half = 5.0f)
		{
			Wui::LineSegment(ctx, glm::vec2 { center.x - half, center.y },
				glm::vec2 { center.x + half, center.y }, color, 1.0f);
			Wui::LineSegment(ctx, glm::vec2 { center.x, center.y - half },
				glm::vec2 { center.x, center.y + half }, color, 1.0f);
		}

		// ---- M46:锚点 9 宫格预设 ----
		// 9 个点位预设(Min == Max = 点锚定)+ 2 个拉伸预设(只改一个轴,另一轴沿用当前值)。
		// `Id` = 稳定 a11y id 后缀(id = "ui_designer.anchor.preset.<Id>");`Key`/`Label` 是
		// 可见文本与 a11y label(键缺失时显示 Label);`Tip` 是 tooltip 的英文兜底(键 = <Key>.tip)。
		struct AnchorPresetDef
		{
			const char* Id;
			const char* Key;
			const char* Label;
			const char* Tip;
			glm::vec2 Min;
			glm::vec2 Max;
			bool KeepMinX;   // true = 该轴沿用节点当前值(拉伸预设用)
			bool KeepMinY;
			bool KeepMaxX;
			bool KeepMaxY;
		};

		const AnchorPresetDef kAnchorPresets[] =
		{
			{ "top_left", "panel.ui_designer.anchor.preset.top_left", "Top Left",
				"Anchor to the top-left corner (Min = Max = 0, 0).",
				{ 0.0f, 0.0f }, { 0.0f, 0.0f }, false, false, false, false },
			{ "top_center", "panel.ui_designer.anchor.preset.top_center", "Top Center",
				"Anchor to the top-centre edge (Min = Max = 0.5, 0).",
				{ 0.5f, 0.0f }, { 0.5f, 0.0f }, false, false, false, false },
			{ "top_right", "panel.ui_designer.anchor.preset.top_right", "Top Right",
				"Anchor to the top-right corner (Min = Max = 1, 0).",
				{ 1.0f, 0.0f }, { 1.0f, 0.0f }, false, false, false, false },
			{ "middle_left", "panel.ui_designer.anchor.preset.middle_left", "Middle Left",
				"Anchor to the left-centre edge (Min = Max = 0, 0.5).",
				{ 0.0f, 0.5f }, { 0.0f, 0.5f }, false, false, false, false },
			{ "center", "panel.ui_designer.anchor.preset.center", "Center",
				"Anchor to the parent centre (Min = Max = 0.5, 0.5).",
				{ 0.5f, 0.5f }, { 0.5f, 0.5f }, false, false, false, false },
			{ "middle_right", "panel.ui_designer.anchor.preset.middle_right", "Middle Right",
				"Anchor to the right-centre edge (Min = Max = 1, 0.5).",
				{ 1.0f, 0.5f }, { 1.0f, 0.5f }, false, false, false, false },
			{ "bottom_left", "panel.ui_designer.anchor.preset.bottom_left", "Bottom Left",
				"Anchor to the bottom-left corner (Min = Max = 0, 1).",
				{ 0.0f, 1.0f }, { 0.0f, 1.0f }, false, false, false, false },
			{ "bottom_center", "panel.ui_designer.anchor.preset.bottom_center", "Bottom Center",
				"Anchor to the bottom-centre edge (Min = Max = 0.5, 1).",
				{ 0.5f, 1.0f }, { 0.5f, 1.0f }, false, false, false, false },
			{ "bottom_right", "panel.ui_designer.anchor.preset.bottom_right", "Bottom Right",
				"Anchor to the bottom-right corner (Min = Max = 1, 1).",
				{ 1.0f, 1.0f }, { 1.0f, 1.0f }, false, false, false, false },
			{ "stretch_horizontal", "panel.ui_designer.anchor.preset.stretch_horizontal",
				"Stretch Horizontal",
				"Stretch horizontally: Min X = 0, Max X = 1; the vertical anchor is kept.",
				{ 0.0f, 0.0f }, { 1.0f, 0.0f }, false, true, false, true },
			{ "stretch_vertical", "panel.ui_designer.anchor.preset.stretch_vertical",
				"Stretch Vertical",
				"Stretch vertically: Min Y = 0, Max Y = 1; the horizontal anchor is kept.",
				{ 0.0f, 0.0f }, { 0.0f, 1.0f }, true, false, true, false },
		};

		constexpr std::size_t kAnchorPresetCount = sizeof(kAnchorPresets) / sizeof(kAnchorPresets[0]);

		// 预设 = 直接写 Min/Max(**不改 Offset/Size/Pivot**;Keep* 轴不写)。
		void ApplyAnchorPreset(UI::UiAnchor& anchor, const AnchorPresetDef& preset)
		{
			if (!preset.KeepMinX) anchor.Min.x = preset.Min.x;
			if (!preset.KeepMinY) anchor.Min.y = preset.Min.y;
			if (!preset.KeepMaxX) anchor.Max.x = preset.Max.x;
			if (!preset.KeepMaxY) anchor.Max.y = preset.Max.y;
		}

		// 预设按钮几何。优先:3×3 点位(上)+ 1 行 2 个拉伸 = 整块 26×36,放在节点框
		// **画布右上角一行 11 个**(固定位置,不贴节点)。为什么不在节点旁边:贴框时预设块正好压在
		// 左上角那一片,而 `HandleCanvasInput` 把"落在预设上"的按下从画布交互里排除 ⇒ 左上/上/左
		// 三个手柄会拖不动(实测)。固定位置与手柄/节点永不重叠,且位置可预期、AI 好寻址。
		// 两段代码(命中判定与绘制)共用这一份几何 —— 分家就会出现"按钮画在这里、命中在那里"。
		std::array<Wui::WuiRect, kAnchorPresetCount> LayoutAnchorPresets(
			const Wui::WuiRect& nodeBox, const Wui::WuiRect& canvasRect)
		{
			std::array<Wui::WuiRect, kAnchorPresetCount> out {};
			constexpr float cell = 8.0f;
			constexpr float gap = 1.0f;
			constexpr float gridSpan = cell * 3.0f + gap * 2.0f;      // 26
			constexpr float cellSpan = cell;
			constexpr float blockW = gridSpan;                        // 26
			// M46 修(实测):预设块**不贴着节点框画** —— 贴框时它正好压在左上角那一片,
			// 而 `HandleCanvasInput` 把"落在预设上"的按下从画布交互里排除 ⇒ **左上/上/左三个手柄
			// 拖不动了**(探针实测:`nSTRETCH` 拖左边缘位移 +1885 而不是 -167 —— 点到预设上把锚点改了)。
			// 因此固定画在画布右上角一行:与手柄/节点永远不重叠,位置可预期、AI 也好寻址。
			// (块尺寸常量仍留给下面算行宽,不要在别处再引 `preferred`。)
			constexpr float rowGap = 2.0f;
			const float rowW = cell * static_cast<float>(kAnchorPresetCount)
				+ rowGap * static_cast<float>(kAnchorPresetCount - 1);
			const float originX = std::max(canvasRect.X + 2.0f,
				canvasRect.X + canvasRect.W - rowW - 6.0f);
			const float originY = canvasRect.Y + 6.0f;
			for (std::size_t i = 0; i < kAnchorPresetCount; ++i)
				out[i] = Wui::WuiRect { originX + static_cast<float>(i) * (cell + rowGap),
					originY, cell, cellSpan };
			return out;
		}

		// ---- M12:8 个缩放手柄 ----
		// 每个手柄由两条边构成(位掩码);AnchorX/Y = 手柄中心在节点矩形内的归一化位置。
		enum HandleBits
		{
			HandleNone = 0,
			HandleLeft = 1,
			HandleRight = 2,
			HandleTop = 4,
			HandleBottom = 8,
		};

		struct HandleDef
		{
			int Bits;
			float AnchorX;
			float AnchorY;
		};

		constexpr float kHandleSize = 8.0f;

		const HandleDef kHandles[] =
		{
			{ HandleLeft | HandleTop,     0.0f, 0.0f },
			{ HandleRight | HandleTop,    1.0f, 0.0f },
			{ HandleRight | HandleBottom, 1.0f, 1.0f },
			{ HandleLeft | HandleBottom,  0.0f, 1.0f },
			{ HandleLeft,                 0.0f, 0.5f },
			{ HandleRight,                1.0f, 0.5f },
			{ HandleTop,                  0.5f, 0.0f },
			{ HandleBottom,               0.5f, 1.0f },
		};

		// 画布上节点框的物理矩形:零面积节点(点锚 + Size=0)给一个最小可视盒,否则画布上
		// 完全看不见、8 个手柄也没有可抓的位置。
		Wui::WuiRect NodeBoxPhysical(const UI::UiViewport& viewport, const Wui::WuiRect& rect)
		{
			Wui::WuiRect box = viewport.DesignRectToPhysical(rect);
			if (box.W < 3.0f || box.H < 3.0f)
				box = Wui::WuiRect { box.X, box.Y, 6.0f, 6.0f };
			return box;
		}

		Wui::WuiRect HandlePhysicalRect(const Wui::WuiRect& box, const HandleDef& handle)
		{
			const float half = kHandleSize * 0.5f;
			return Wui::WuiRect { box.X + handle.AnchorX * box.W - half,
				box.Y + handle.AnchorY * box.H - half, kHandleSize, kHandleSize };
		}

		// 同父内定位节点(上移/下移用)。返回所属兄弟列表与下标。
		bool LocateSibling(std::vector<UI::UiNode>& nodes, const std::string& id,
			std::vector<UI::UiNode>** outList, std::size_t* outIndex)
		{
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (nodes[index].Id == id)
				{
					*outList = &nodes;
					*outIndex = index;
					return true;
				}
				if (LocateSibling(nodes[index].Children, id, outList, outIndex))
					return true;
			}
			return false;
		}

		bool LocateSiblingIndex(const std::vector<UI::UiNode>& nodes, const std::string& id,
			std::size_t* outIndex, std::size_t* outCount)
		{
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (nodes[index].Id == id)
				{
					*outIndex = index;
					*outCount = nodes.size();
					return true;
				}
				if (LocateSiblingIndex(nodes[index].Children, id, outIndex, outCount))
					return true;
			}
			return false;
		}

		UI::UiNode* FindNodeMutable(std::vector<UI::UiNode>& nodes, const std::string& id)
		{
			for (UI::UiNode& node : nodes)
			{
				if (node.Id == id)
					return &node;
				if (UI::UiNode* found = FindNodeMutable(node.Children, id))
					return found;
			}
			return nullptr;
		}

		// ---- M16:节点定位 / 唯一 Id / 深拷贝重发 Id ----

		// 节点定位:所属 Children 列表(根 = &document.Nodes)、下标与**父路径**。
		// 父路径只在这里沿树累积 —— Id 里可以含 '.'(MakeStableId 的产物),
		// 不能靠字符串反推父路径。
		struct NodeLocation
		{
			std::vector<UI::UiNode>* List = nullptr;
			std::size_t Index = 0;
			std::string ParentPath;
		};

		bool LocateNode(std::vector<UI::UiNode>& nodes, const std::string& id,
			const std::string& parentPath, NodeLocation& out)
		{
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (nodes[index].Id == id)
				{
					out.List = &nodes;
					out.Index = index;
					out.ParentPath = parentPath;
					return true;
				}
				const std::string path = parentPath.empty()
					? nodes[index].Id : (parentPath + "." + nodes[index].Id);
				if (LocateNode(nodes[index].Children, id, path, out))
					return true;
			}
			return false;
		}

		// 该 Id 是否是**根列表**里的节点(只扫第一层;节点 Id 文档内唯一,无需递归)。
		bool LocateRootIndex(const std::vector<UI::UiNode>& nodes, const std::string& id,
			std::size_t* outIndex)
		{
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (nodes[index].Id == id)
				{
					if (outIndex != nullptr)
						*outIndex = index;
					return true;
				}
			}
			return false;
		}

		// 稳定 Id 生成(MakeStableId 口径);撞上文档已有 Id 或本轮已分配的 Id 时序号 +1 重试。
		std::string MakeUniqueNodeId(const UI::UiDocument& document,
			const std::set<std::string>& reserved, const std::string& parentPath,
			const std::string& type, std::size_t index)
		{
			// M20:Id 只取 `Type#序号`,**不嵌父路径**。
			//
			// 为什么:Id 是"文档内唯一"的标识,路径由遍历时的 `父.子` 拼出来(见
			// `UiDocument::ForEachNode`)。若 Id 里已经含父路径,拼出的路径就会重复一段 ——
			// 实测三层嵌套会得到 `root.root.Grid#1.Image#0` 这种名字,越长越糟。
			// 唯一性由下面的探测循环保证(document + 本轮 reserved),不依赖路径前缀。
			(void)parentPath;
			std::size_t probe = index;
			std::string id = UI::MakeStableId(std::string(), type, probe);
			while (document.FindNode(id) != nullptr || reserved.find(id) != reserved.end())
			{
				++probe;
				id = UI::MakeStableId(std::string(), type, probe);
			}
			return id;
		}

		// 复制子树:整棵子树重新发 Id(唯一,文档内不重复),父路径逐层累积。
		void RegenerateSubtreeIds(const UI::UiDocument& document, std::set<std::string>& reserved,
			UI::UiNode& node, const std::string& parentPath, std::size_t index)
		{
			node.Id = MakeUniqueNodeId(document, reserved, parentPath, node.Type, index);
			node.IdWasGenerated = false;
			reserved.insert(node.Id);
			const std::string path = parentPath.empty() ? node.Id : (parentPath + "." + node.Id);
			for (std::size_t child = 0; child < node.Children.size(); ++child)
				RegenerateSubtreeIds(document, reserved, node.Children[child], path, child);
		}

		// 节点在文档里的路径(空 = 没找到)。
		std::string FindNodePath(const UI::UiDocument& document, const std::string& id)
		{
			std::string path;
			document.ForEachNode([&](const UI::UiNode& node, const std::string& nodePath)
				{
					if (path.empty() && node.Id == id)
						path = nodePath;
				});
			return path;
		}

		// M7a:内容浏览器双击 `.wui` → 本面板的"按路径打开"待办(见头文件 RequestOpenPath)。
		// 进程内单槽(UI 单线程);登记后由面板下一次渲染取走一次。
		std::string& PendingOpenPath()
		{
			static std::string pending;
			return pending;
		}

		// M20:节点面板的搜索用(大小写不敏感的子串匹配)。
		std::string ToLowerAscii(std::string text)
		{
			for (char& c : text)
			{
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			}
			return text;
		}

		bool ContainsNoCase(std::string_view haystack, const std::string& lowerNeedle)
		{
			if (lowerNeedle.empty())
				return true;
			return ToLowerAscii(std::string(haystack)).find(lowerNeedle) != std::string::npos;
		}

		// M42:非空段用 " · " 连接(节点面板 tooltip = 本地化分类 · 本地化名 · 本地化介绍;
		// 某段为空时不悬着一个分隔符)。
		std::string JoinTooltipParts(const std::string& first, const std::string& second,
			const std::string& third)
		{
			std::string text = first;
			const auto append = [&text](const std::string& part)
			{
				if (part.empty())
					return;
				if (!text.empty())
					text += " \xC2\xB7 ";
				text += part;
			};
			append(second);
			append(third);
			return text;
		}

		// M29:属性覆盖值的读写(属性面板按类型登记表出行的基础)。
		UI::UiProp* FindPropMutable(UI::UiNode& node, std::string_view name)
		{
			for (UI::UiProp& prop : node.Props)
			{
				if (prop.Name == name)
					return &prop;
			}
			return nullptr;
		}

		void WritePropOverride(UI::UiNode& node, const std::string& name, const std::string& value)
		{
			if (UI::UiProp* existing = FindPropMutable(node, name))
			{
				existing->Value = value;
				return;
			}
			node.Props.push_back(UI::UiProp { name, value });
		}

		bool ClearPropOverride(UI::UiNode& node, const std::string& name)
		{
			const auto before = node.Props.size();
			node.Props.erase(std::remove_if(node.Props.begin(), node.Props.end(),
				[&name](const UI::UiProp& prop) { return prop.Name == name; }), node.Props.end());
			return node.Props.size() != before;
		}

		// 属性面板里一行的"引擎元数据默认值"(没有登记项时用属性文本编码的兜底值)。
		std::string PropertyDefaultText(const Wui::WuiComponentProperty& meta)
		{
			switch (meta.Type)
			{
			case Wui::WuiComponentProperty::Kind::Bool:
				return "false";
			case Wui::WuiComponentProperty::Kind::Float:
			case Wui::WuiComponentProperty::Kind::Int:
				return FormatFloat(meta.DefaultNumber);
			case Wui::WuiComponentProperty::Kind::Color:
				return Wui::FormatComponentColor(meta.DefaultColor);
			case Wui::WuiComponentProperty::Kind::Size2:
				return Wui::FormatComponentSize(meta.DefaultSize.x, meta.DefaultSize.y);
			default:
				return meta.DefaultText;
			}
		}

		// 属性组标题的显示名(渲染与高度计算共用一份,避免两边漂移)。
		std::string PropertyGroupLabel(Wui::WuiComponentPropertyGroup group)
		{
			switch (group)
			{
			case Wui::WuiComponentPropertyGroup::Style:
				return Wui::Tr("panel.ui_designer.props.group.style", "Style");
			case Wui::WuiComponentPropertyGroup::Layout:
				return Wui::Tr("panel.ui_designer.props.group.layout", "Layout");
			case Wui::WuiComponentPropertyGroup::Behavior:
				return Wui::Tr("panel.ui_designer.props.group.behavior", "Behavior");
			default:
				return Wui::Tr("panel.ui_designer.props.group.content", "Content");
			}
		}

		// ---- M31:@key 本地化编辑体验 ----
		// 文本类属性(与 UiPainter::Localized 读取的字段同一面:title/text/label/placeholder/rowPrefix,
		// 外加一切 Kind::Text)的值以 @ 开头 = 本地化键。
		bool IsLocalizableProperty(const Wui::WuiComponentProperty* meta, const std::string& name)
		{
			if (meta != nullptr && meta->Type == Wui::WuiComponentProperty::Kind::Text)
				return true;
			return name == "label" || name == "title" || name == "text"
				|| name == "placeholder" || name == "rowPrefix";
		}

		// `@key` / `@key|兜底文案` 的切分(**与 `UiPainter::Localized` 同口径**):
		// 只有第一种写法会在缺 key 时显示裸键名;第二种有内联兜底。空 key / 空兜底 = 写法非法。
		struct LocalizationRef
		{
			bool Valid = false;         // 值以 '@' 开头且切分合法(空 key / 空兜底 = 非法)
			bool HasInlineFallback = false;
			std::string Key;
			std::string Fallback;
		};

		LocalizationRef ParseLocalizationRef(const std::string& value)
		{
			LocalizationRef ref;
			if (value.size() < 2 || value.front() != '@')
				return ref;
			const std::size_t separator = value.find('|');
			if (separator == std::string::npos)
			{
				ref.Key = value.substr(1);
				ref.Valid = !ref.Key.empty();
				return ref;
			}
			ref.Key = value.substr(1, separator - 1);
			ref.Fallback = value.substr(separator + 1);
			ref.Valid = !ref.Key.empty() && !ref.Fallback.empty();
			ref.HasInlineFallback = ref.Valid;
			return ref;
		}

		// 预览行文案:`@key|兜底` 且缺 key ⇒ 提示"将显示内联兜底"(不是报错);
		// 只有裸 `@key` 缺 key 才标 missing(那才是真的会露出键名)。
		std::string LocalizationPreview(const LocalizationRef& ref, bool& missing)
		{
			missing = false;
			if (!ref.Valid)
				return std::string();
			const bool hasTranslation = Wui::HasLocalizationKey(ref.Key);
			if (hasTranslation)
				return "= " + Wui::Tr(ref.Key, ref.Key);
			if (ref.HasInlineFallback)
				return "= " + ref.Fallback;
			missing = true;
			return "missing key: " + ref.Key;
		}

		// 已加载语言码(逗号拼接);tooltip 的语言清单。
		std::string LocalizationLanguageList()
		{
			const std::vector<std::string> languages = Wui::LocalizationLanguages();
			std::string joined;
			for (const std::string& language : languages)
			{
				if (!joined.empty())
					joined += ", ";
				joined += language;
			}
			return joined;
		}

		// ---- M35:画布属性可见反馈 ----
		//
		// 显示文本与 `UiPainter::Localized` **同口径**(M33 起 `.wui` 支持内联兜底):
		//   `@key`          —— 本地化 key;缺 key 时回退去掉 `@` 的字面量。
		//   `@key|兜底文案` —— 缺 key / 缺译文时显示兜底文案,而不是裸键名。
		//   key 或兜底为空 = 写法非法:原样显示,便于一眼看出写错。
		std::string LocalizedCanvasText(std::string_view text)
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
				if (key.empty() || fallback.empty())
					return std::string(text);
				return Wui::Tr(key, fallback);
			}
			return std::string(text);
		}

		// 节点属性值(一次线性查找,无容器构造);未设置返回 nullptr。
		const std::string* NodePropValue(const UI::UiNodeInstance& node, std::string_view name)
		{
			if (node.Source == nullptr)
				return nullptr;
			const UI::UiProp* prop = node.Source->FindProp(name);
			return prop != nullptr ? &prop->Value : nullptr;
		}

		std::string CanvasNodeText(const UI::UiNodeInstance& node)
		{
			static constexpr std::string_view kTextProps[] = { "text", "label", "title" };
			for (std::string_view name : kTextProps)
			{
				const std::string* value = NodePropValue(node, name);
				if (value != nullptr && !value->empty())
					return LocalizedCanvasText(*value);
			}
			return std::string();
		}

		bool TryParseNodeColor(const UI::UiNodeInstance& node, std::string_view name, Wui::WuiColor& out)
		{
			const std::string* value = NodePropValue(node, name);
			return value != nullptr && !value->empty() && Wui::ParseComponentColor(*value, out);
		}

		// 主属性优先,解析失败/未设置时回退次属性(如 `bg` → `bg.default`, `color` → `text.default`)。
		bool CanvasNodeColor(const UI::UiNodeInstance& node, std::string_view primary,
			std::string_view secondary, Wui::WuiColor& out)
		{
			if (TryParseNodeColor(node, primary, out))
				return true;
			return !secondary.empty() && TryParseNodeColor(node, secondary, out);
		}

		// 字号钳到 8..48(避免画爆框);未设置/非法 = 回退默认字号。
		float CanvasNodeFontSize(const UI::UiNodeInstance& node, float fallback)
		{
			const std::string* value = NodePropValue(node, "fontSize");
			if (value == nullptr || value->empty())
				return fallback;
			char* end = nullptr;
			const float parsed = std::strtof(value->c_str(), &end);
			if (end == value->c_str())
				return fallback;
			return std::clamp(parsed, 8.0f, 48.0f);
		}

		// ---- M37:StateScoped 属性的"按状态分槽" ----
		// `StateScoped=true` 的属性名形如 `bg.hover`(通道.状态);本函数取状态后缀。
		// 没有 '.' 的名字按整名处理(登记表不该这样写,但面板不做硬失败)。
		std::string_view StateScopeSuffix(std::string_view name)
		{
			const std::size_t dot = name.rfind('.');
			return dot == std::string_view::npos ? name : name.substr(dot + 1);
		}

		// 该节点类型的**状态选择器选项**(`WuiComponentDesc::States`)。
		// 返回 nullptr = 不显示选择器、也不按状态过滤(类型未登记 / 状态少于 2 个)——
		// 这样"没有状态声明的组件"与引入选择器之前的行为逐字一致。
		const std::vector<Wui::WuiComponentState>* PropStateOptions(const UI::UiNode& node)
		{
			const Wui::WuiComponentDesc* component = UI::UiNodeRegistry::Component(node.Type);
			if (component == nullptr || component->States.size() < 2)
				return nullptr;
			return &component->States;
		}

		// 状态列表里等于 "default" 的那一项;没有就用第一项。
		std::string DefaultPropState(const std::vector<Wui::WuiComponentState>& states)
		{
			for (const Wui::WuiComponentState& state : states)
			{
				if (state.Id == "default")
					return state.Id;
			}
			return states.empty() ? std::string() : states.front().Id;
		}

		// M44:`.wui` Type → **本地化控件名**。键 `wui.component.<ComponentId>.name`,
		// 兜底 = 原始 Type(英文界面下与改动前逐字相同)。大纲行与属性页 Node 段的
		// Type 值都用它;`Id` 与 a11y 标识符**保持原文**(脚本/AI 按它们寻址)。
		// M44:`.wui` Type → **本地化控件名**。键分两层:
		//   ① `wui.node.<Type>.name` —— 按 `.wui` 类型(唯一);
		//   ② `wui.component.<ComponentId>.name` —— 回退(同 id 的多个类型共用名称时用);
		//   ③ 原始 `Type`。
		// 为什么要有 ①:多个 `.wui` 类型可以共用同一个 ComponentId(`List`/`Grid` 都用 `listview`;
		// `Row`/`Column`/`Flex`/`GridContainer` 都用 `box`),只按组件 id 取键会让它们**同名同描述**
		// —— 用户看到的正是「新增的容器全叫面板 / 原始英文标注」。
		// 大纲行与属性页 Node 段的 Type 值都用它;`Id` 与 a11y 标识符**保持原文**(脚本/AI 按它们寻址)。
		std::string ComponentDisplayName(std::string_view type)
		{
			const UI::UiNodeTypeDesc* desc = UI::UiNodeRegistry::Find(type);
			const std::string componentId = desc != nullptr ? desc->ComponentId : std::string(type);
			const std::string typeName = desc != nullptr ? desc->Type : std::string(type);
			// 两层都走 Tr:内层给组件级兜底(可能命中),外层给类型级覆盖。
			const std::string componentName = Wui::Tr("wui.component." + componentId + ".name", typeName);
			return Wui::Tr("wui.node." + typeName + ".name", componentName);
		}

		// M44:同上的**描述**取值(`wui.node.<Type>.doc` → `wui.component.<id>.doc` → 引擎 `Doc`)。
		// 引擎的 `UiNodeTypeDesc::Doc` 本来就是**按 Type** 写的 ⇒ 它作最后一层兜底是对的;
		// 组件表里没有 `Doc` 时才退到 `SizeNotes`(技术说明,聊胜于无)。
		std::string ComponentDisplayDoc(std::string_view type)
		{
			const UI::UiNodeTypeDesc* desc = UI::UiNodeRegistry::Find(type);
			const std::string componentId = desc != nullptr ? desc->ComponentId : std::string(type);
			const std::string typeName = desc != nullptr ? desc->Type : std::string(type);
			const Wui::WuiComponentDesc* component = UI::UiNodeRegistry::Find(type) != nullptr
				? UI::UiNodeRegistry::Component(type) : nullptr;
			const std::string typeDoc = desc != nullptr ? desc->Doc : std::string();
			const std::string componentDoc = !typeDoc.empty()
				? typeDoc : (component != nullptr ? component->SizeNotes : std::string());
			const std::string shared = Wui::Tr("wui.component." + componentId + ".doc", componentDoc);
			return Wui::Tr("wui.node." + typeName + ".doc", shared);
		}

		// M44:状态选择器下方那行说明的**英文兜底**(键 `wui.state.<id>.doc`;中文由目录补)。
		// 常用状态逐一写清"什么时候出现";未登记的 id 用一句通用说明兜底(不留空 ——
		// 用户报的正是"State 解释不明确")。
		const char* StateDocFallback(std::string_view id)
		{
			if (id == "default")
				return "Base look when no other state is active; other states fall back to it.";
			if (id == "hover")
				return "Shown while the pointer is over the control.";
			if (id == "pressed")
				return "Shown while the control is being pressed down.";
			if (id == "focus")
				return "Shown while the control owns keyboard focus.";
			if (id == "disabled")
				return "Shown when the control is disabled and cannot be interacted with.";
			if (id == "checked")
				return "Shown when the option is on (checkbox / toggle).";
			if (id == "on")
				return "Shown when the switch is on.";
			if (id == "off")
				return "Shown when the switch is off.";
			if (id == "selected")
				return "Shown when this item is the selected one.";
			if (id == "open")
				return "Shown while the popup / list is open.";
			return "Visual state of this control; pick one to edit only the properties scoped to it.";
		}
	}

	// ---- 文档生命周期 ----

	void UiDesignerPanel::RequestOpenPath(const std::string& logicalPath)
	{
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		PendingOpenPath() = normalized;
	}

	void UiDesignerPanel::ConsumeOpenRequest()
	{
		std::string pending = PendingOpenPath();
		if (pending.empty())
			return;
		PendingOpenPath().clear();
		// 逻辑路径按内容根解析(与工具栏 Open 同一条 ResolveInputPath);解析后 LoadFrom
		// 负责读盘/报错/更新状态行。载入失败时同样清掉待办(不每帧重试同一条坏路径)。
		LoadFrom(ResolveInputPath(pending));
	}

	const std::filesystem::path& UiDesignerPanel::ContentRoot()
	{
		if (!m_ContentRootResolved)
		{
			m_ContentRootResolved = true;
			// 内容根 = **编辑器当前打开的项目**的内容根,与引擎其余部分同一口径:
			// `Paths::AssetRoot()` 已经处理了 `--project` / `WLD_PROJECT_DIR` / `projects/<名>`
			// 与"standard 清单的 content_root"四种来源。
			//
			// 为什么不能从 `current_path()` 反查清单(实测踩过):编辑器从仓库根启动 + 用
			// `WLD_PROJECT_DIR` 指定项目时,`current_path()` 是**仓库根**,那里没有清单 ——
			// 于是内容根解析失败,面板把 `assets/ui/untitled.wui` 落到了**引擎仓库**里
			// (用户的 .wui 会被写进 engine 仓库,而不是他的项目)。
			m_ContentRoot = World::Paths::AssetRoot();
			if (m_ContentRoot.empty())
			{
				std::error_code error;
				m_ContentRoot = std::filesystem::current_path(error);
			}
		}
		return m_ContentRoot;
	}

	std::filesystem::path UiDesignerPanel::ResolveInputPath(const std::string& text)
	{
		const std::string trimmed = TrimCopy(text);
		if (trimmed.empty())
			return {};
		std::filesystem::path path(trimmed);
		if (path.is_relative())
		{
			const std::filesystem::path& root = ContentRoot();
			std::error_code error;
			path = (root.empty() ? std::filesystem::current_path(error) : root) / path;
		}
		return path;
	}

	bool UiDesignerPanel::LoadFrom(const std::filesystem::path& path)
	{
		std::error_code error;
		if (!std::filesystem::exists(path, error))
		{
			m_Status = Wui::Tr("panel.ui_designer.load_missing", "File not found: ") + path.string();
			return false;
		}

		UI::UiDocument document;
		std::string parseError;
		if (!UI::UiDocumentIO::LoadFile(path, document, &parseError))
		{
			m_Status = Wui::Tr("panel.ui_designer.load_failed", "Load failed: ") + parseError;
			return false;
		}

		m_Document = std::move(document);
		m_Path = path;
		m_PathBuffer = path.string();
		m_SelectedId.clear();
		m_BufferNodeId.clear();
		// M12:换文档 = 撤销历史作废(跨文档撤销没有意义),进行中的拖动/编辑也一并收口。
		m_Undo.Clear();
		CancelDrag();
		CancelNodeEdit();
		CancelOutlineRename();
		m_Collapsed.clear();
		m_OutlineScroll = 0.0f;
		m_PropertyScroll = 0.0f;
		m_RecentScroll = 0.0f;
		m_HasDocument = true;
		m_ScreenDirty = true;
		m_Dirty = false;
		RebuildScreen();
		m_Status = Wui::Tr("panel.ui_designer.loaded", "Loaded ") + path.filename().string()
			+ " (" + std::to_string(m_Document.NodeCount()) + " nodes)";
		return true;
	}

	bool UiDesignerPanel::SaveTo(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			m_Status = Wui::Tr("panel.ui_designer.need_path", "Enter a .wui path first");
			return false;
		}
		std::error_code error;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), error);

		// M25:Bind / On 的空行不写盘 —— ValidateUiDocument(以及 UiDocumentIO::Parse)拒绝
		// 空 Target/Source/Event/Command 的条目。模型里保留这些"正在编辑"的草稿(UI 段内
		// 有可见提示),这里只过滤要写出去的那一份副本:文件永远能被 Parse 读回。
		UI::UiDocument outgoing = m_Document;
		std::size_t droppedRows = 0;
		outgoing.ForEachNode([&droppedRows](UI::UiNode& node, const std::string&)
		{
			const std::size_t before = node.Bind.size() + node.On.size();
			node.Bind.erase(std::remove_if(node.Bind.begin(), node.Bind.end(),
				[](const UI::UiBindingDecl& binding) { return BindingRowIncomplete(binding); }),
				node.Bind.end());
			node.On.erase(std::remove_if(node.On.begin(), node.On.end(),
				[](const UI::UiCommandDecl& command) { return CommandRowIncomplete(command); }),
				node.On.end());
			droppedRows += before - (node.Bind.size() + node.On.size());
		});

		std::string saveError;
		if (!UI::UiDocumentIO::SaveFile(path, outgoing, &saveError))
		{
			m_Status = Wui::Tr("panel.ui_designer.save_failed", "Save failed: ") + saveError;
			return false;
		}
		m_Path = path;
		m_PathBuffer = path.string();
		m_Dirty = false;
		m_Status = Wui::Tr("panel.ui_designer.saved", "Saved ") + path.filename().string();
		if (droppedRows > 0)
		{
			m_Status += " - " + std::to_string(droppedRows) + " " + Wui::Tr(
				"panel.ui_designer.saved_dropped", "empty binding/command row(s) not written");
		}
		return true;
	}

	void UiDesignerPanel::RebuildScreen()
	{
		m_ScreenDirty = false;
		std::string error;
		if (!m_Screen.Build(m_Document, &error))
			m_Status = Wui::Tr("panel.ui_designer.build_failed", "Cannot build screen: ") + error;
	}

	UI::UiNode* UiDesignerPanel::MutableSelectedNode()
	{
		if (m_SelectedId.empty())
			return nullptr;
		UI::UiNode* found = nullptr;
		m_Document.ForEachNode([&](UI::UiNode& node, const std::string&)
		{
			if (found == nullptr && node.Id == m_SelectedId)
				found = &node;
		});
		return found;
	}

	const UI::UiNode* UiDesignerPanel::SelectedNode() const
	{
		return m_SelectedId.empty() ? nullptr : m_Document.FindNode(m_SelectedId);
	}

	bool UiDesignerPanel::IsCollapsed(const std::string& nodeId) const
	{
		const auto found = m_Collapsed.find(nodeId);
		return found != m_Collapsed.end() && found->second;
	}

	// ---- 三栏渲染 ----

	void UiDesignerPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		if (!(rect.W > 1.0f) || !(rect.H > 1.0f))
			return;

		// M7a:先消费内容浏览器排队的"按路径打开"(取走后 LoadFrom),再画工具栏/画布 ——
		// 同一帧就能看到新文档,而不是等下一帧。
		ConsumeOpenRequest();

		// M12:面板级快捷键(Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z)。走 ctx 的输入快照 —— 真实键盘与
		// AI 注入(ui.key)是同一条路径;文本焦点活跃时让位给文本编辑(与 EditorShell W9-2 同口径)。
		HandleShortcuts(ctx);

		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.WindowBg, 0.0f);

		// M16:工具栏高度随宽度变化(够宽一排,排不下换两排);画布/状态行按同一结果切分。
		const float toolbarHeight = std::min(ToolbarNeededHeight(rect.W), rect.H);
		const float statusHeight = rect.H > 120.0f ? kStatusHeight : 0.0f;
		RenderToolbar(ctx, Wui::WuiRect { rect.X, rect.Y, rect.W, toolbarHeight }, host);

		const Wui::WuiRect body { rect.X, rect.Y + toolbarHeight, rect.W,
			std::max(0.0f, rect.H - toolbarHeight - statusHeight) };

		const float innerWidth = std::max(0.0f, body.W - kColumnPad * 2.0f - kSplitterWidth * 2.0f);
		if (innerWidth < kMinColumnWidth * 2.0f + kMinCanvasWidth || body.H < 48.0f)
		{
			// 面板太窄/太矮:三栏挤不出可读宽度,退化为只画画布(不画不可读的窄条)。
			RenderCanvas(ctx, body, host);
		}
		else
		{
			const float maxLeft = std::max(kMinColumnWidth, innerWidth - kMinCanvasWidth - kMinColumnWidth);
			const float leftWidth = std::clamp(m_LeftWidth, kMinColumnWidth, maxLeft);
			const float maxRight = std::max(kMinColumnWidth, innerWidth - kMinCanvasWidth - leftWidth);
			const float rightWidth = std::clamp(m_RightWidth, kMinColumnWidth, maxRight);
			const float canvasWidth = std::max(kMinCanvasWidth, innerWidth - leftWidth - rightWidth);

			const float columnTop = body.Y + kColumnPad;
			const float columnHeight = std::max(0.0f, body.H - kColumnPad * 2.0f);
			float x = body.X + kColumnPad;
			// M20:左栏拆成"大纲(上) + 节点面板(下)"。节点面板常驻可见 —— 这就是替代
			// "Add"下拉的交互:类型按钮一眼看全、一次点击命中,不用先展开弹层再选。
			// 高度取左栏的 42%(下限 140、上限 320);左栏太矮时只留大纲。
			const float paletteHeight = columnHeight > 260.0f
				? std::clamp(columnHeight * 0.42f, 140.0f, 320.0f) : 0.0f;
			const Wui::WuiRect outlineRect { x, columnTop, leftWidth,
				std::max(0.0f, columnHeight - paletteHeight) };
			const Wui::WuiRect paletteRect { x, columnTop + outlineRect.H + 4.0f, leftWidth,
				std::max(0.0f, paletteHeight - 4.0f) };
			x += leftWidth;
			const Wui::WuiRect leftSplitter { x, columnTop, kSplitterWidth, columnHeight };
			x += kSplitterWidth;
			const Wui::WuiRect canvasRect { x, columnTop, canvasWidth, columnHeight };
			x += canvasWidth;
			const Wui::WuiRect rightSplitter { x, columnTop, kSplitterWidth, columnHeight };
			x += kSplitterWidth;
			const Wui::WuiRect propertiesRect { x, columnTop, rightWidth, columnHeight };

			RenderOutline(ctx, outlineRect, host);
			if (paletteRect.H > 0.0f)
				RenderNodePalette(ctx, paletteRect, host);
			RenderCanvas(ctx, canvasRect, host);
			RenderProperties(ctx, propertiesRect, host);

			// 分隔条:拖动直接改像素宽度,越界由上面的 clamp 在每个帧收口。
			Wui::Splitter(ctx, Wui::HashId("ui_designer.split.left"), leftSplitter, true,
				m_LeftWidth, kMinColumnWidth, maxLeft, theme);
			// M51:右分隔条改的是**它右侧**的属性栏宽度 ⇒ reverse=true(否则往右拖属性栏反而变宽、
			// 边界往左跑 —— 用户报的"左右拉效果是反的")。
			Wui::Splitter(ctx, Wui::HashId("ui_designer.split.right"), rightSplitter, true,
				m_RightWidth, kMinColumnWidth, maxRight, theme, /*reverse=*/true);
		}

		if (statusHeight > 0.0f)
		{
			const Wui::WuiRect status { rect.X, rect.Y + rect.H - statusHeight, rect.W, statusHeight };
			Wui::PanelBackground(ctx, status, theme.PanelHeader, 0.0f);
			std::string text = m_Status;
			if (text.empty())
			{
				text = m_HasDocument
					? (m_Dirty ? std::string("* ") : std::string()) + m_Path.filename().string()
						+ "  ·  " + std::to_string(m_Document.NodeCount()) + " nodes"
					: Wui::Tr("panel.ui_designer.status_idle", "No document open");
			}
			Wui::Label(ctx, glm::vec2 { status.X + 8.0f, status.Y + 4.0f }, text,
				theme.TextMuted, theme.FontSizeSmall);
		}

		// M12:属性行编辑在鼠标抬起后落一条撤销记录(文本提交时鼠标本就抬起 → 同帧提交)。
		if (m_PendingEditValid && !ctx.Input().MouseDown[0])
			CommitNodeEdit();
		// M12:文本焦点快照 —— 下一帧的 Ctrl+Z/Y 判定要用"上一帧结束时"的状态(W9-2 同口径)。
		m_TextFocusLatched = Wui::WuiTextFocus::Get().Active();

		// M36:帧末真的落盘 —— 此刻属性面板(在三栏里、工具栏之后)已经渲染过,
		// 未回车/未失焦的文本缓冲已经在那一遍提交进文档。放在这里就不会丢最后一次编辑。
		if (m_PendingSave)
		{
			m_PendingSave = false;
			SaveTo(m_Path.empty() ? ResolveInputPath(m_PathBuffer) : m_Path);
		}
	}

	void UiDesignerPanel::RenderToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelHeader, 0.0f);

		constexpr float iconWidth = 30.0f;
		constexpr float buttonWidth = 62.0f;
		constexpr float deleteWidth = 70.0f;
		constexpr float duplicateWidth = 84.0f;
		constexpr float gap = 6.0f;

		const float height = kToolbarRowHeight;
		const float left = rect.X + 6.0f;
		const float right = rect.X + rect.W - 6.0f;
		const float row1Y = rect.Y + kToolbarPadY;
		// M16:排不下就把 New/Add/Delete/Duplicate 换到第二排(工具栏高度由
		// ToolbarNeededHeight 按同一阈值先算好,两边不会错位)。
		const bool twoRows = rect.W < kToolbarSingleRowMinWidth;
		const float row2Y = row1Y + (twoRows ? height + kToolbarRowGap : 0.0f);

		float x = left;

		// M12:撤销/重做(与 Ctrl+Z / Ctrl+Y 同一条面板本地栈)。
		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.undo"), Wui::WuiRect { x, row1Y, iconWidth, height },
			Wui::Tr("panel.ui_designer.undo", "Undo"), theme, m_Undo.CanUndo(), false,
			Wui::Tr("panel.ui_designer.undo.tip", "Undo the last designer edit (Ctrl+Z)")))
		{
			UndoDocument();
		}
		x += iconWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.redo"), Wui::WuiRect { x, row1Y, iconWidth, height },
			Wui::Tr("panel.ui_designer.redo", "Redo"), theme, m_Undo.CanRedo(), false,
			Wui::Tr("panel.ui_designer.redo.tip", "Redo the last undone edit (Ctrl+Y)")))
		{
			RedoDocument();
		}
		x += iconWidth + gap;

		// 行 1 尾部固定项:Open / Reload / Save / Fit(+ 单排时的 New / Delete / Duplicate)。
		float tailWidth = gap + buttonWidth * 4.0f + gap * 3.0f;
		if (!twoRows)
			tailWidth += gap + buttonWidth + gap + deleteWidth + gap + duplicateWidth;
		// 路径框拿走剩余宽度;面板极窄时钳到 80(后面的部件被画到边界外 = 截断,不互相叠)。
		const float fieldWidth = std::max(80.0f, right - x - tailWidth);

		Wui::TextFieldA11y pathA11y;
		pathA11y.Label = Wui::Tr("panel.ui_designer.path", "WUI file path");
		pathA11y.Placeholder = "assets/ui/hud.wui";
		Wui::TextField(ctx, Wui::HashId("ui_designer.path.field"),
			Wui::WuiRect { x, row1Y, fieldWidth, height }, m_PathBuffer, theme, nullptr, &pathA11y);
		x += fieldWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.open"), Wui::WuiRect { x, row1Y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.open", "Open"), theme, true, false,
			Wui::Tr("panel.ui_designer.open.tip", "Load the .wui document at this path")))
		{
			const std::filesystem::path path = ResolveInputPath(m_PathBuffer);
			if (path.empty())
				m_Status = Wui::Tr("panel.ui_designer.need_path", "Enter a .wui path first");
			else
				LoadFrom(path);
		}
		x += buttonWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.reload"), Wui::WuiRect { x, row1Y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.reload", "Reload"), theme, true, false,
			Wui::Tr("panel.ui_designer.reload.tip", "Discard in-memory edits and re-read the file from disk")))
		{
			if (m_Path.empty())
				m_Status = Wui::Tr("panel.ui_designer.need_path", "Enter a .wui path first");
			else
				LoadFrom(m_Path);
		}
		x += buttonWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.save"), Wui::WuiRect { x, row1Y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.save", "Save"), theme, m_HasDocument, true,
			Wui::Tr("panel.ui_designer.save.tip", "Write the document back to disk (atomic)")))
		{
			// M36:延后到帧末 —— 属性面板在工具栏之后渲染,那里才会提交未回车/未失焦的文本缓冲。
			m_PendingSave = true;
		}
		x += buttonWidth + gap;

		// M19:画布视图复位(F 键同路径)—— 放大看细节后一键回到"全貌适配"。
		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.fit"),
			Wui::WuiRect { x, row1Y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.fit", "Fit"), theme, m_HasDocument, false,
			Wui::Tr("panel.ui_designer.fit.tip",
				"Fit the whole design into the canvas (F); wheel zooms, middle/space drag pans")))
		{
			m_CanvasZoom = 1.0f;
			m_CanvasPan = glm::vec2 { 0.0f, 0.0f };
			m_Status = Wui::Tr("panel.ui_designer.view_reset", "Canvas view reset");
		}
		x += buttonWidth + gap;

		// ---- M16:New / Add / Delete / Duplicate(单排接在 Save 后;排不下换到第二排)----
		float editX = twoRows ? left : x;
		const float editY = twoRows ? row2Y : row1Y;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.new"),
			Wui::WuiRect { editX, editY, buttonWidth, height },
			Wui::Tr("panel.ui_designer.new", "New"), theme, true, false,
			Wui::Tr("panel.ui_designer.new.tip",
				"Create a minimal document (Panel + Label); uses the path box, else <content-root>/ui/untitled.wui")))
		{
			CreateNewDocument();
		}
		editX += buttonWidth + gap;

		// M20:原来的"Add"下拉已换成左栏的**节点面板**(搜索 + 类型网格 + 单击即插入)——
		// 用户反馈"下拉框选 UI 控件着实不便"。这里不再放快捷按钮:工具栏宽度有限,
		// 一排类型按钮会把 Delete/Duplicate 挤出面板(实测在默认 1000px 面板上真的出界)。

		const bool canDelete = CanDeleteSelected();
		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.delete"),
			Wui::WuiRect { editX, editY, deleteWidth, height },
			Wui::Tr("panel.ui_designer.delete", "Delete"), theme, canDelete, false,
			canDelete
				? Wui::Tr("panel.ui_designer.delete.tip", "Remove the selected node (and its children)")
				: Wui::Tr("panel.ui_designer.delete.disabled",
					"Select a node first; the last root node cannot be deleted")))
		{
			DeleteSelectedNode();
		}
		editX += deleteWidth + gap;

		const bool canDuplicate = CanDuplicateSelected();
		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.duplicate"),
			Wui::WuiRect { editX, editY, duplicateWidth, height },
			Wui::Tr("panel.ui_designer.duplicate", "Duplicate"), theme, canDuplicate, false,
			canDuplicate
				? Wui::Tr("panel.ui_designer.duplicate.tip", "Deep-copy the selected subtree and select the copy")
				: Wui::Tr("panel.ui_designer.duplicate.disabled", "Select a node first")))
		{
			DuplicateSelectedNode();
		}
	}

	void UiDesignerPanel::RenderOutline(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 2.0f);
		Wui::Label(ctx, glm::vec2 { rect.X + 6.0f, rect.Y + 4.0f },
			Wui::Tr("panel.ui_designer.outline", "Outline"), theme.TextMuted, theme.FontSizeCaption);

		// M12:同父内上移/下移(改 Children 顺序 = 改绘制顺序;保存后文档顺序真的变了)。
		// M29:再加 **缩进 / 提升** 一对(改**层级**)。
		// 为什么:此前只有 ↑/↓(同父换序)与"拖动重挂父"(鼠标拖动),用户找不到改层级的路
		// —— 反馈"无法修改控件层级"。现在四个按钮并列,层级改动是一等命令。
		{
			constexpr float headerHeight = 16.0f;
			constexpr float moveWidth = 20.0f;
			const float headerY = rect.Y + 1.0f;
			float buttonX = rect.X + rect.W - 4.0f - moveWidth;
			if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.outline.outdent"),
				Wui::WuiRect { buttonX, headerY, moveWidth, headerHeight },
				"\xE2\x86\x90", theme, CanOutdentSelected(), false,
				Wui::Tr("panel.ui_designer.outline.outdent.tip",
					"Outdent: become the next sibling of the parent (move up one level)")))
			{
				OutdentSelectedNode();
			}
			buttonX -= moveWidth + 3.0f;
			if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.outline.indent"),
				Wui::WuiRect { buttonX, headerY, moveWidth, headerHeight },
				"\xE2\x86\x92", theme, CanIndentSelected(), false,
				Wui::Tr("panel.ui_designer.outline.indent.tip",
					"Indent: become a child of the previous sibling (move down one level)")))
			{
				IndentSelectedNode();
			}
			buttonX -= moveWidth + 3.0f;
			if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.outline.down"),
				Wui::WuiRect { buttonX, headerY, moveWidth, headerHeight },
				"\xE2\x86\x93", theme, CanMoveSelected(1), false,
				Wui::Tr("panel.ui_designer.outline.down.tip", "Move node down (later in draw order)")))
			{
				MoveSelectedNode(1);
			}
			buttonX -= moveWidth + 3.0f;
			if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.outline.up"),
				Wui::WuiRect { buttonX, headerY, moveWidth, headerHeight },
				"\xE2\x86\x91", theme, CanMoveSelected(-1), false,
				Wui::Tr("panel.ui_designer.outline.up.tip", "Move node up (earlier in draw order)")))
			{
				MoveSelectedNode(-1);
			}
			// M47:改名入口(稳定 a11y id,`ui.invoke` 可点;双击行是鼠标路径)。按钮窄,
			// 可见文本用一个字母,完整名字进 tooltip 与 a11y label。
			buttonX -= moveWidth + 3.0f;
			if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.outline.rename"),
				Wui::WuiRect { buttonX, headerY, moveWidth, headerHeight },
				Wui::Tr("panel.ui_designer.outline.rename", "R"), theme, !m_SelectedId.empty(), false,
				Wui::Tr("panel.ui_designer.outline.rename.tip",
					"Rename the selected node (double-clicking its row does the same)")))
			{
				BeginOutlineRename(m_SelectedId);
			}
		}

		const Wui::WuiRect area { rect.X + 2.0f, rect.Y + 18.0f,
			std::max(0.0f, rect.W - 4.0f), std::max(0.0f, rect.H - 20.0f) };

		m_OutlineIds.clear();
		std::vector<Wui::TreeViewItem> items;
		if (m_HasDocument)
			AppendOutlineItems(m_Document.Nodes, 0, items);

		const Wui::TreeViewResult result = Wui::TreeView(ctx, area, items, kTreeRowHeight,
			m_OutlineScroll, theme, Wui::HashId("ui_designer.outline.tree"));

		const std::string selectionBefore = m_SelectedId;
		const auto nodeIdAt = [&](int index) -> const std::string*
		{
			if (index < 0 || static_cast<std::size_t>(index) >= m_OutlineIds.size())
				return nullptr;
			return &m_OutlineIds[static_cast<std::size_t>(index)];
		};
		if (const std::string* id = nodeIdAt(result.ClickedArrow))
			m_Collapsed[*id] = !IsCollapsed(*id);
		if (const std::string* id = nodeIdAt(result.KeyToggleExpand))
			m_Collapsed[*id] = !IsCollapsed(*id);
		if (const std::string* id = nodeIdAt(result.Clicked))
		{
			// M20:Ctrl+Click = 加选/取消加选(多选);普通点击 = 单选并清空加选。
			if (ctx.Input().Ctrl)
				ToggleSelected(*id);
			else
			{
				m_SelectedId = *id;
				m_SelectedIds.clear();
			}
			// M20:按下即武装"拖动重挂父"(位移超过阈值才算拖动,单纯点击不受影响)。
			// M47:改名编辑态下不武装拖动(否则在行内点选文字会被当成拖动重挂父)。
			if (!m_OutlineRenameActive)
			{
				m_OutlineDragActive = false;
				m_OutlineDragId = *id;
				m_OutlineDragStart = ctx.Input().MousePos;
			}
		}
		// M47:双击行 = 进入内联改名(TreeView 双击帧只报 DoubleClicked,所以选中也在这里补)。
		if (const std::string* id = nodeIdAt(result.DoubleClicked))
		{
			m_SelectedId = *id;
			m_SelectedIds.clear();
			BeginOutlineRename(*id);
		}
		if (const std::string* id = nodeIdAt(result.KeyActivate))
		{
			m_SelectedId = *id;
			m_SelectedIds.clear();
		}
		if (const std::string* id = nodeIdAt(result.KeyMoveTo))
		{
			m_SelectedId = *id;
			m_SelectedIds.clear();
		}
		if (m_SelectedId != selectionBefore)
			CommitNodeEdit();   // 选中变化前把上一节点的属性编辑落账

		// ---- M47:大纲行内联改名框(画在树之上,覆盖被改名行的文本)----
		// 与资源浏览器 RenameField 同一口径:TextFieldEx 行内报错;Escape 必须在控件之前
		// 判(TextFieldEx 没有 cancelled 回调,否则"取消"会被当成失焦提交)。
		if (m_OutlineRenameActive)
		{
			std::size_t renameIndex = m_OutlineIds.size();
			for (std::size_t i = 0; i < m_OutlineIds.size(); ++i)
			{
				if (m_OutlineIds[i] == m_OutlineRenameNodeId)
				{
					renameIndex = i;
					break;
				}
			}
			// 行可能已被撤销/删除/滚出视口 ⇒ 收掉编辑态(不留一个悬空的输入框)。
			const bool rowUsable = renameIndex < m_OutlineIds.size()
				&& renameIndex < result.ItemRects.size()
				&& result.ItemRects[renameIndex].Y + kTreeRowHeight * 0.5f >= area.Y
				&& result.ItemRects[renameIndex].Y + kTreeRowHeight * 0.5f <= area.Y + area.H;
			if (!rowUsable)
			{
				CancelOutlineRename();
			}
			else
			{
				const Wui::WuiRect row = result.ItemRects[renameIndex];
				const Wui::WuiRect field { row.X + 18.0f, row.Y + 1.0f,
					std::max(40.0f, row.W - 22.0f), std::max(10.0f, row.H - 2.0f) };
				const Wui::WuiId renameId = Wui::HashId("ui_designer.outline.rename.field");
				// M47 修:SetFocus 在**本帧**未必立刻生效(焦点可能在帧末才落到控件上)⇒ 请求焦点的那一帧
				// 不能当作"失焦",否则下面那条 `ctx.Focus() != renameId` 会在同一帧提交(缓冲 = 原名 ⇒ 无害
				// 提交 + CancelOutlineRename),输入框只存在一帧、永远改不了名(实测:点 R 后树里根本没有
				// 行内输入框;a11y 快照里也读不到)。这里记下"这一帧刚请求过焦点",跳过本帧的失焦分支。
				// M47 修(实测踩过):焦点请求要**连续几帧**重申,并且要和 "文本输入态" 一起给
				// —— 与内容浏览器 StartRename 的既有做法一致(`ctx.SetFocus(id)` + `SetTextInputActive(true)`)。
				// 只给一帧的话:该帧控件还没进焦点表,SetFocus 落空;下一帧按"失焦"提交 + 收口,
				// 输入框只活一帧,用户永远改不了名(a11y 树里也读不到那个框)。
				// 另外:只有**真正拿到过焦点**之后,失焦才算"提交"(否则第一帧就被判失焦)。
				bool focusRequestedThisFrame = false;
				if (ctx.Focus() == renameId)
					m_OutlineRenameHadFocus = true;
				if (m_OutlineRenameFocusPending || (!m_OutlineRenameHadFocus && m_OutlineRenameFocusTries < 4))
				{
					ctx.SetFocus(renameId);
					ctx.SetTextInputActive(true);
					m_OutlineRenameFocusPending = false;
					focusRequestedThisFrame = true;
					++m_OutlineRenameFocusTries;
				}
				const bool cancelRequested = ctx.Focus() == renameId
					&& ctx.IsKeyPressed(KeyCodes::Escape);
				const std::string error = OutlineRenameError(m_OutlineRenameNodeId, TrimCopy(m_OutlineRenameBuffer));
				const bool submitted = Wui::TextFieldEx(ctx, renameId, field,
					m_OutlineRenameBuffer, theme, error);
				if (cancelRequested)
				{
					CancelOutlineRename();
				}
				else if (submitted)
				{
					if (!CommitOutlineRename(m_OutlineRenameBuffer))
					{
						// 非法名字拒绝提交:留在编辑态、焦点还给输入框,好继续改。
						ctx.SetFocus(renameId);
						ctx.SetTextInputActive(true);
					}
				}
				else if (m_OutlineRenameHadFocus && ctx.Focus() != renameId)
				{
					// 失焦提交:非法名字不写文档(调用方会 CancelOutlineRename)。
					if (!CommitOutlineRename(m_OutlineRenameBuffer))
						CancelOutlineRename();
				}
			}
		}

		// ---- M20:大纲行拖动 = 重新挂父 ----
		// 阈值 4px(与 WuiContext 的拖拽判定同口径),避免把"点一下"误判成拖动;
		// 松开时按落点所在行决定新父:落在某行上 = 成为它的子节点,落在空白 = 移到根。
		const Wui::WuiInputState& input = ctx.Input();
		if (!m_OutlineDragId.empty() && input.MouseDown[0])
		{
			if (!m_OutlineDragActive &&
				glm::distance(input.MousePos, m_OutlineDragStart) > 4.0f)
				m_OutlineDragActive = true;
		}
		if (!m_OutlineDragId.empty() && !input.MouseDown[0])
		{
			if (m_OutlineDragActive)
			{
				const int hoveredRow = static_cast<int>(
					std::floor((input.MousePos.y - (area.Y - m_OutlineScroll)) / kTreeRowHeight));
				const std::string* dropTarget = nodeIdAt(hoveredRow);
				// 落点必须是**视口内**的行(滚出去的行不算)。
				const bool inside = input.MousePos.x >= area.X && input.MousePos.x <= area.X + area.W &&
					input.MousePos.y >= area.Y && input.MousePos.y <= area.Y + area.H;
				const std::string dragId = m_OutlineDragId;
				m_OutlineDragActive = false;
				m_OutlineDragId.clear();
				if (inside && dropTarget != nullptr && *dropTarget != dragId)
				{
					const std::string parent = *dropTarget;
					m_SelectedId = dragId;
					ReparentSelected(parent);
				}
			}
			else
			{
				m_OutlineDragActive = false;
				m_OutlineDragId.clear();
			}
		}
	}

	// ---- M20:节点面板(替代"Add"下拉)----
	//
	// 用户反馈"下拉框选 UI 控件着实不便":下拉要两步(展开弹层 → 选条目)、弹层盖住画布、
	// 11 种类型平铺一列且没有分类/搜索。这里改成**常驻面板**:
	//   `Wui::TextField` 搜索(按类型名或分类名过滤)+ **两列网格**的类型按钮,单击即插入
	//   (插到当前选中节点下;没有选中则作为新的根节点)。
	// 为什么是两列网格而不是"分类标题 + 单列":单列 11 种类型要滚两屏,而面板下半只有 ~190px;
	// 两列一次看全。(类型按分类排序 ⇒ 同类相邻,分组感仍在;分类名进按钮 tooltip。)
	// 类型列表来自 `UI::UiNodeRegistry::All()`(唯一事实源,新登记的类型自动出现,不用改这里)。
	void UiDesignerPanel::RenderNodePalette(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 2.0f);

		constexpr float pad = 6.0f;
		Wui::Label(ctx, glm::vec2 { rect.X + pad, rect.Y + 3.0f },
			Wui::Tr("panel.ui_designer.palette", "Add node"), theme.TextMuted, theme.FontSizeCaption);

		const float searchY = rect.Y + 18.0f;
		const Wui::WuiRect searchRect { rect.X + pad, searchY,
			std::max(40.0f, rect.W - pad * 2.0f), 20.0f };
		Wui::TextFieldA11y searchA11y;
		searchA11y.Label = Wui::Tr("panel.ui_designer.palette.search", "Search node types");
		searchA11y.Placeholder = Wui::Tr("panel.ui_designer.palette.hint", "Search types\u2026");
		Wui::TextField(ctx, Wui::HashId("ui_designer.palette.search"), searchRect, m_PaletteSearch,
			theme, nullptr, &searchA11y);

		// 组装(分类 + 过滤),再按"分类连续块"渲染 —— 同名分类只出现一次标题。
		const std::string needle = ToLowerAscii(m_PaletteSearch);
		// M42:每格 = 原始标识(排序/过滤/AddNode 用)+ 本地化文案(可见文本与 tooltip)。
		// 可见文案的键:`wui.component.<ComponentId>.name` / `.doc` 与 `wui.category.<分类>`,
		// 兜底 = 组件登记表的英文源文(DisplayName / Doc / SizeNotes)。
		struct PaletteEntry
		{
			std::string Category;       // 原始分类(空 = 未登记分类)
			std::string Type;           // 原始 `.wui` 类型名(按钮 id 与 AddNode 的输入,不改)
			std::string Label;          // 本地化控件名(按钮可见文本)
			std::string CategoryLabel;  // 本地化分类名(tooltip 用)
			std::string Doc;            // 本地化功能介绍(tooltip 用)
		};
		// M44:先建**全表**(未过滤)→ 按"可见名是否撞名"定型显示名 → 过滤 → 排序。
		// 为什么不在过滤后判歧义:搜索词会改变候选集,可见名不该随搜索文字变来变去。
		std::vector<PaletteEntry> allEntries;
		for (const UI::UiNodeTypeDesc& desc : UI::UiNodeRegistry::All())
		{
			const Wui::WuiComponentDesc* component = UI::UiNodeRegistry::Component(desc.Type);
			PaletteEntry entry;
			entry.Type = desc.Type;
			if (component != nullptr)
				entry.Category = component->Category;
			// `Slider` 的 ComponentId 是 `slider.float`(含点)⇒ 键里就带点,不做任何替换。
			// 兜底用**原始 `.wui` 类型名**:英文界面下按钮文案与改动前逐字相同。
			entry.Label = ComponentDisplayName(desc.Type);
			entry.CategoryLabel = entry.Category.empty()
				? Wui::Tr("panel.ui_designer.palette.other", "Other")
				: Wui::Tr("wui.category." + entry.Category, entry.Category);
			const std::string docSource = !desc.Doc.empty()
				? desc.Doc
				: (component != nullptr ? component->SizeNotes : std::string());
			entry.Doc = ComponentDisplayDoc(desc.Type);
			allEntries.push_back(std::move(entry));
		}
		// 消歧(可预测:撞名一律退回原始 `.wui` Type)。真因是登记表层面 List 与 Grid 的
		// ComponentId 都是 `listview` ⇒ 本地化名相同;退回 Type 后按钮分别显示 "List" / "Grid"
		// (Type 在登记表内唯一)。不在这里发明新的组件 id,也不加需要维护的后缀表。
		std::unordered_map<std::string, int> labelCounts;
		for (const PaletteEntry& entry : allEntries)
			++labelCounts[entry.Label];
		for (PaletteEntry& entry : allEntries)
		{
			if (labelCounts[entry.Label] > 1)
				entry.Label = entry.Type;
		}
		std::vector<PaletteEntry> entries;
		for (PaletteEntry& entry : allEntries)
		{
			// 过滤:原始 Type / 原始分类照旧(中文界面搜英文名仍然有效),再追加本地化名
			// 与本地化分类(中文界面也能搜中文);**过滤逻辑不跟随本地化文本改动**。
			if (!ContainsNoCase(entry.Type, needle) && !ContainsNoCase(entry.Category, needle)
				&& !ContainsNoCase(entry.Label, needle)
				&& !ContainsNoCase(entry.CategoryLabel, needle))
				continue;
			entries.push_back(std::move(entry));
		}
		std::stable_sort(entries.begin(), entries.end(),
			[](const PaletteEntry& a, const PaletteEntry& b) { return a.Category < b.Category; });

		const Wui::WuiRect listRect { rect.X + pad, searchY + 24.0f,
			std::max(40.0f, rect.W - pad * 2.0f),
			std::max(0.0f, rect.Y + rect.H - (searchY + 24.0f) - pad) };
		if (listRect.H < 12.0f)
			return;

		// 每格两行:第一行本地化名(按钮自己的文字),第二行**英文 `.wui` Type**(次要色)。
		// 为什么两行:格子只有 ~100px 宽,`中文名 (English)` 放不下;而英文名是写文档/提示词
		// 与 AI 交流时的正式名字(用户口径:"各 UI 控件的英文需要显示出来")。
		constexpr float rowHeight = 30.0f;
		constexpr float columnGap = 3.0f;
		const float cellWidth = std::max(40.0f, (listRect.W - columnGap) * 0.5f);
		const std::size_t rows = (entries.size() + 1) / 2;
		const float contentHeight = entries.empty()
			? 20.0f : (static_cast<float>(rows) * (rowHeight + 1.0f));

		Wui::BeginScrollArea(ctx, listRect, contentHeight, m_PaletteScroll, theme,
			Wui::HashId("ui_designer.palette.scroll"));
		float y = listRect.Y - m_PaletteScroll;
		std::string addType;
		for (std::size_t i = 0; i < entries.size(); ++i)
		{
			const std::size_t column = i % 2;
			const float cellX = listRect.X + static_cast<float>(column) * (cellWidth + columnGap);
			const Wui::WuiRect cell { cellX, y, cellWidth, rowHeight };
			if (column == 1)
				y += rowHeight + 1.0f;   // 每行两格:格子放完才推进行
			if (!ctx.ClipAllows(cell))
				continue;   // 滚出视口的行不画也不登记(与属性页滚动区同一口径)
			if (Wui::ButtonEx(ctx,
				// a11y id 用**原始** Type(语言无关的稳定寻址);可见文本与 tooltip 走本地化。
				Wui::HashId(("ui_designer.palette.add." + entries[i].Type).c_str()),
				cell, entries[i].Label, theme, true, false,
				// M44:分类名与控件名相同时省掉分类段(此前 "按钮 · 按钮 · 可点击的…" 首段冗余)。
				JoinTooltipParts(entries[i].CategoryLabel == entries[i].Label
					? std::string() : entries[i].CategoryLabel,
					entries[i].Label, entries[i].Doc)))
			{
				addType = entries[i].Type;
			}
			// 第二行:英文 `.wui` Type(次要色,不参与命中 —— 命中与 a11y 都归上面的按钮整格)。
			Wui::Label(ctx, glm::vec2 { cell.X + 6.0f, cell.Y + rowHeight - 12.0f },
				entries[i].Type, theme.TextDisabled, theme.FontSizeCaption);
			// 同一行文字也进无障碍树(kind=label):AI 按 `ui_designer.palette.type.<Type>` 能读到
			// 每个控件的英文名 —— 用户要"英文显示出来",AI 侧同样要能拿到它。
			{
				Wui::WuiAccessNode typeNode;
				typeNode.Id = Wui::HashId(("ui_designer.palette.type." + entries[i].Type).c_str());
				typeNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				typeNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				typeNode.Kind = "label";
				typeNode.Label = entries[i].Type;
				typeNode.Value = entries[i].Label;   // 本地化名(对照)
				typeNode.Rect = Wui::WuiRect { cell.X + 6.0f, cell.Y + rowHeight - 12.0f,
					std::max(8.0f, cell.W - 12.0f), 12.0f };
				typeNode.Enabled = true;
				typeNode.Interactive = false;
				typeNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(typeNode);
			}
		}
		if (entries.empty())
		{
			Wui::Label(ctx, glm::vec2 { listRect.X + 2.0f, y + 2.0f },
				Wui::Tr("panel.ui_designer.palette.none", "No matching type"),
				theme.TextDisabled, theme.FontSizeCaption);
		}
		Wui::EndScrollArea(ctx);

		// 在滚动区之后再加(避免改文档导致本帧的行与命中不一致)。
		if (!addType.empty())
			AddNode(addType);
	}

	void UiDesignerPanel::AppendOutlineItems(const std::vector<UI::UiNode>& nodes, int depth,
		std::vector<Wui::TreeViewItem>& out)
	{
		for (const UI::UiNode& node : nodes)
		{
			const bool hasChildren = !node.Children.empty();
			const bool collapsed = IsCollapsed(node.Id);
			Wui::TreeViewItem item;
			item.Id = Wui::HashId(("ui_designer.tree." + node.Id).c_str());
			// M44:行文本 = **本地化控件名** + "  " + Id。
			// `Id` 必须留在 label 里:既有验证脚本按 tree-item 的 label 含节点 Id(如 `btnA`)
			// 找行/寻址,而 Id 是稳定身份(Bind/热重载/`ui.invoke` 都按它定位)—— 本地化只换前半段。
			item.Label = ComponentDisplayName(node.Type) + "  " + node.Id;
			item.Depth = depth;
			item.HasChildren = hasChildren;
			item.Expanded = hasChildren && !collapsed;
			item.Selected = (node.Id == m_SelectedId);
			out.push_back(item);
			m_OutlineIds.push_back(node.Id);
			if (hasChildren && !collapsed)
				AppendOutlineItems(node.Children, depth + 1, out);
		}
	}

	// ---- M47:大纲行内联改名 ----
	//
	// 改名只碰 `Id` 一个字段 —— `.wui` 里**没有**任何字段按节点 Id 引用节点:
	//   · `Bind` 的**键**是该节点自己的属性名(如 `label`),值是数据源(`ecs:` / `service:` …);
	//   · `On` 的键是事件名(`Click`),值是命令(`ui.close` / 工程自定义命令);
	//   · `World.Target` 是宿主按**场景实体 Tag** 解析的名字 —— `GameHost::ResolveWorldPositionByName`
	//     用 `TagComponent` 建索引(`UiWorldAnchor.h` 的注释也写的是"实体名/路径"),不是节点 Id;
	//     误改它会把世界锚点指到不存在的实体。
	// 所以"同步改写 Bind/On/World 里的引用"在这里是**空集**;要跟着迁移的只有**面板自己**
	// 按 Id 建键的状态(选中集、折叠表、拖动/编辑缓冲)。将来若文档格式真的引入按 Id 的
	// 引用字段,必须在这里补改写 —— 那是公共格式决定,不能由面板单方面发明。
	void UiDesignerPanel::BeginOutlineRename(const std::string& nodeId)
	{
		const UI::UiNode* node = m_Document.FindNode(nodeId);
		if (node == nullptr)
			return;
		m_OutlineRenameActive = true;
		m_OutlineRenameNodeId = nodeId;
		m_OutlineRenameBuffer = node->Id;
		m_OutlineRenameFocusPending = true;
		// M47 修:每次进入编辑都重置"焦点看过没有 / 重申了几帧"(见 RenderOutline 的焦点块)。
		m_OutlineRenameHadFocus = false;
		m_OutlineRenameFocusTries = 0;
	}

	void UiDesignerPanel::CancelOutlineRename()
	{
		m_OutlineRenameActive = false;
		m_OutlineRenameNodeId.clear();
		m_OutlineRenameBuffer.clear();
		m_OutlineRenameFocusPending = false;
	}

	// M49:校验一个新名字相对 **oldId** 是否可用(合法字符 / 同层唯一 / 未改名)。
	// 为什么必须传 oldId:属性页 Node/Id 行的旧名是它自己的缓冲,不是大纲那套成员 ——
	// 用 m_OutlineRenameNodeId 判"没改名"会让属性页把"改成同名"误报成重名。
	std::string UiDesignerPanel::OutlineRenameError(const std::string& oldId, const std::string& newId) const
	{
		if (newId.empty())
			return Wui::Tr("panel.ui_designer.rename.error.empty", "Name cannot be empty");
		if (!UI::IsValidUiNodeId(newId))
			return Wui::Tr("panel.ui_designer.rename.error.illegal",
				"Use letters, digits, '_', '-', '.' or '#'; no spaces or '/'");
		if (newId == oldId)
			return std::string();   // 没改名 = 合法(提交时按"无变化"收口)
		if (m_Document.FindNode(newId) != nullptr)
			return Wui::Tr("panel.ui_designer.rename.error.duplicate",
				"Another node already uses this name");
		return std::string();
	}

	// M49:改名的**唯一实现** —— 大纲内联改名与属性页 Node/Id 行都调它。
	// 为什么抽出来:两处都要"校验 + 面板内部按 Id 索引的状态一起搬 + 一条撤销",各写一份迟早
	// 漂移(改了这里没改那里 = 从属性页改名会留下悬空的选中/折叠状态)。
	bool UiDesignerPanel::RenameNode(const std::string& oldId, const std::string& newId, std::string* error)
	{
		const std::string trimmed = TrimCopy(newId);
		const auto fail = [error](const std::string& message) {
			if (error)
				*error = message;
			return false;
		};
		if (trimmed == oldId)
			return true;   // 名字没变也算成功(不落空撤销记录)
		const std::string reason = OutlineRenameError(oldId, trimmed);
		if (!reason.empty())
		{
			m_Status = reason;
			return fail(reason);   // 保持原名(调用方把理由显示出来)
		}
		// 先把属性页上未提交的编辑落账,改名自己占一条撤销记录。
		CommitNodeEdit();
		const UI::UiDocument before = m_Document;
		UI::UiNode* node = FindNodeMutable(m_Document.Nodes, oldId);
		if (node == nullptr)
			return fail("the node no longer exists");
		node->Id = trimmed;
		node->IdWasGenerated = false;   // 手工设过名 = 显式 Id,不再由 MakeStableId 生成
		// 迁移面板内部按 Id 索引的状态(文档外的引用只有这些)。
		if (m_SelectedId == oldId)
			m_SelectedId = trimmed;
		for (std::string& selected : m_SelectedIds)
		{
			if (selected == oldId)
				selected = trimmed;
		}
		if (m_DragNodeId == oldId)
			m_DragNodeId = trimmed;
		if (m_OutlineDragId == oldId)
			m_OutlineDragId = trimmed;
		if (m_BufferNodeId == oldId)
			m_BufferNodeId.clear();   // 属性文本缓冲按 Id 建键 ⇒ 下一帧重建
		const auto collapsed = m_Collapsed.find(oldId);
		if (collapsed != m_Collapsed.end())
		{
			m_Collapsed[trimmed] = collapsed->second;
			m_Collapsed.erase(collapsed);
		}
		++m_RowBufferGen;   // Bind / On 行缓冲键含节点 Id ⇒ 换代
		PushDocumentUndo(Wui::Tr("panel.ui_designer.rename", "Rename node"), before);
		m_ScreenDirty = true;
		m_Dirty = true;
		m_Status = Wui::Tr("panel.ui_designer.renamed", "Renamed to ") + trimmed;
		if (error)
			error->clear();
		return true;
	}

	bool UiDesignerPanel::CommitOutlineRename(const std::string& newId)
	{
		if (!m_OutlineRenameActive)
			return false;
		std::string error;
		if (!RenameNode(m_OutlineRenameNodeId, newId, &error))
			return false;   // 保持原名与编辑态(调用方把焦点还给输入框)
		CancelOutlineRename();
		return true;
	}

	float UiDesignerPanel::PropertyContentHeight(const UI::UiNode* node) const
	{
		const float rowHeight = Wui::PropertyRowHeight();
		// M29:一行一属性 + 每换一个属性组多一个组标题行 —— 与 RenderProperties 共用
		// `CollectPropertyRows`,两边不会漂移(漂移就是"滚动区高度不对/最后一行的字段点不到")。
		float propertyRows = 1.0f;
		float previewRows = 0.0f;
		if (node != nullptr)
		{
			const std::vector<PropertyRowRef> rows = CollectPropertyRows(*node);
			propertyRows = rows.empty() ? 1.0f : static_cast<float>(rows.size());
			std::string lastGroup;
			for (const PropertyRowRef& ref : rows)
			{
				if (ref.Meta != nullptr)
				{
					const std::string groupName = PropertyGroupLabel(ref.Meta->Group);
					if (groupName != lastGroup)
					{
						lastGroup = groupName;
						propertyRows += 1.0f;   // 组标题行
					}
				}
				// M31:值为 @key 的文本类属性,字段下方多一行译文/缺键预览。
				const UI::UiProp* current = node->FindProp(ref.Name);
				const std::string value = current != nullptr
					? current->Value
					: (ref.Meta != nullptr ? PropertyDefaultText(*ref.Meta) : std::string());
				if (IsLocalizableProperty(ref.Meta, ref.Name) && ParseLocalizationRef(value).Valid)
					previewRows += 1.0f;
			}
		}
		// M31:两大块 —— Type 块头 + Common 块头 + 六个公共段头(Node/Anchor/Layout/World/Bind/On)。
		// M42:Type 块头下多一行**功能介绍**(见 RenderProperties 的 ui_designer.type.doc)。
		float rows = 9.0f;
		if (m_ShowNodeSection) rows += 2.0f;                // Id / Type
		rows += propertyRows;                               // Type 块的类型独有属性(恒显示)
		// M37/M44:Type 块的附加行 —— 状态选择器(类型登记了 ≥2 个状态时)+ 选中状态的
		// 一行说明(M44)与混选提示。与 RenderProperties 同一判定
		// (`PropStateOptions` / `SelectionMixedTypes`),不漂移。
		if (node != nullptr)
		{
			if (PropStateOptions(*node) != nullptr)
				rows += 2.0f;   // 状态选择器 + 选中状态的一行说明
			if (SelectionMixedTypes())
				rows += 1.0f;
		}
		if (m_ShowAnchorSection) rows += 11.0f;             // 10 个数值 + RelativeToSafeArea
		if (m_ShowWorldSection)
		{
			rows += 6.0f;                                   // Enabled + Target + 3×Offset + KeepOnScreen
			// M24:Enabled=true 且 Target 为空 = ValidateUiDocument 会报错 → 段内多一行可见提示。
			if (node != nullptr && node->World.Enabled && node->World.Target.empty())
				rows += 1.0f;
		}
		// M25:Bind / On —— 每条 = 两行(Target+Source / Event+Command)+ 段尾一个 Add 按钮,
		// 再加提示行(空段一行空态,非空段按 BindRowHints/CommandRowHints 的行数)。
		if (m_ShowBindSection)
		{
			rows += 1.0f;                                   // + Add binding
			if (node != nullptr)
			{
				rows += 2.0f * static_cast<float>(node->Bind.size());
				rows += node->Bind.empty() ? 1.0f : static_cast<float>(BindRowHints(*node).size());
			}
		}
		if (m_ShowOnSection)
		{
			rows += 1.0f;                                   // + Add command
			if (node != nullptr)
			{
				rows += 2.0f * static_cast<float>(node->On.size());
				rows += node->On.empty() ? 1.0f : static_cast<float>(CommandRowHints(*node).size());
			}
		}
		if (m_ShowLayoutSection) rows += 8.0f;              // Kind + Gap + 4×Padding + Columns + RowMajor
		return rowHeight * rows + kLocalizationPreviewHeight * previewRows + 8.0f;
	}

	// M29:属性面板的行来源 —— **类型登记的属性表**在前(按登记顺序,组内即作者顺序),
	// 文档里多出来的未登记属性跟在后面(仍然可编辑,不因为"没登记"就藏起来)。
	std::vector<UiDesignerPanel::PropertyRowRef> UiDesignerPanel::CollectPropertyRows(
		const UI::UiNode& node) const
	{
		std::vector<PropertyRowRef> rows;
		const Wui::WuiComponentDesc* component = UI::UiNodeRegistry::Component(node.Type);
		if (component != nullptr)
		{
			// M37:状态过滤 —— `StateScoped` 行(如 `bg.hover`)只在**状态选择器选中的那个
			// 状态**下出现;非 StateScoped 行(通道值 `bg`、`label`、字号…)恒显示。
			// `PropStateOptions` 为 nullptr(类型未登记 / 状态少于 2 个)= 不过滤,
			// 与引入选择器之前的行为逐字一致。
			const bool filterByState = PropStateOptions(node) != nullptr;
			rows.reserve(component->Properties.size());
			for (const Wui::WuiComponentProperty& meta : component->Properties)
			{
				if (filterByState && meta.StateScoped && StateScopeSuffix(meta.Name) != m_PropState)
					continue;
				// M50:内部字段(如 `image.textureId` 的宿主句柄)不出行 —— 设计师选的是 `.texture`
				// 逻辑路径;两个都列会让人以为要各填一遍(用户报障)。
				if (meta.EditorHidden)
					continue;
				// M51:`label` 在文本节点上是**遗留兜底**(`text` 优先,见 PaintLabel)—— 只在 Label 上
				// 出现,且写了 `text` 之后它就不生效。用户报障"label 与 text 两个控制的都是显示内容"
				// 正是这个:留一行就够,别让人以为要各填一遍。
				if (meta.Name == "label" && node.Type == "Label")
					continue;
				rows.push_back(PropertyRowRef { meta.Name, &meta });
			}
		}
		// 文档里的额外/未登记属性仍然可编辑;但**登记表里有的属性**绝不在这里重复出现 ——
		// 包括被状态过滤掉的那些(M37):它们是"当前状态不显示",不是"未登记"。
		// (判定与旧实现等价:旧实现比对的"已出行"集合本来就只由登记表填充。)
		for (const UI::UiProp& prop : node.Props)
		{
			const bool declared = (component != nullptr && std::any_of(component->Properties.begin(),
				component->Properties.end(),
				[&prop](const Wui::WuiComponentProperty& meta) { return meta.Name == prop.Name; }))
				|| std::any_of(rows.begin(), rows.end(), [&prop](const PropertyRowRef& ref)
					{ return ref.Name == prop.Name; });
			if (!declared)
				rows.push_back(PropertyRowRef { prop.Name, nullptr });
		}
		return rows;
	}

	void UiDesignerPanel::RenderProperties(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 2.0f);

		UI::UiNode* node = MutableSelectedNode();
		// 选中节点变化(含重新加载):清掉上一份属性文本编辑缓冲,避免 TextField 回显旧值。
		if (node == nullptr)
		{
			m_BufferNodeId.clear();
		}
		else if (node->Id != m_BufferNodeId)
		{
			m_BufferNodeId = node->Id;
			for (const UI::UiProp& prop : node->Props)
			{
				ctx.ErasePersist(Wui::HashId(
					("ui_designer.prop." + node->Id + "." + prop.Name + ".buf").c_str()));
			}
			// M24:World.Target 的文本编辑缓冲同样按选中节点重建(否则切回旧节点会回显旧值)。
			ctx.ErasePersist(Wui::HashId(
				("ui_designer.world.target." + node->Id + ".buf").c_str()));
			// M25:Bind / On 的行缓冲按下标键 —— 换节点(含撤销/重载/新建/复制后的重选)
			// 必须整体换代,否则后几行会回显上一个节点的文本。
			++m_RowBufferGen;
			// M37:换了节点就把状态选择器收回默认(否则 hover 会跨节点带过去)。
			m_PropState = "default";
		}

		// M37:状态选择器当前项必须属于当前节点的类型;换类型(含撤销/热重载)后回落默认。
		// 这一步在算高度(PropertyContentHeight)之前 —— 行清单与滚动高度同源。
		if (node != nullptr)
		{
			if (const std::vector<Wui::WuiComponentState>* states = PropStateOptions(*node))
			{
				const bool known = std::any_of(states->begin(), states->end(),
					[this](const Wui::WuiComponentState& state) { return state.Id == m_PropState; });
				if (!known)
					m_PropState = DefaultPropState(*states);
			}
		}

		// M12:没有待提交编辑时,每帧抓一份"本帧起点"节点快照 —— 属性行首次改动时拿它当
		// 撤销前像。拖动期间只在第一帧抓一次,所以"一次拖动 = 一条记录"。
		if (node != nullptr && !m_PendingEditValid)
			m_FrameNodeBefore = *node;

		const float rowHeight = Wui::PropertyRowHeight();
		const float innerWidth = std::max(0.0f, rect.W - 12.0f);
		const Wui::WuiRect inner { rect.X + 6.0f, rect.Y + 6.0f, innerWidth, std::max(0.0f, rect.H - 12.0f) };
		const float labelWidth = Wui::PropertyRowLabelWidth(Wui::WuiRect { 0.0f, 0.0f, innerWidth, rowHeight });

		Wui::BeginScrollArea(ctx, inner, PropertyContentHeight(node), m_PropertyScroll, theme,
			Wui::HashId("ui_designer.props.scroll"));

		float y = inner.Y - m_PropertyScroll;
		const auto nextRow = [&](float height) -> Wui::WuiRect
		{
			const Wui::WuiRect row { inner.X, y, inner.W, height };
			y += height;
			return row;
		};
		const auto groupHeader = [&](const char* key, const char* fallback, const std::string& trailing, bool& open)
		{
			Wui::PropertyGroupHeaderDesc desc;
			desc.Label = Wui::Tr(key, fallback);
			desc.A11yLabel = desc.Label;
			desc.Trailing = trailing;
			desc.Open = open;
			const Wui::PropertyGroupHeaderResult result =
				Wui::PropertyGroupHeader(ctx, Wui::HashId(key), nextRow(rowHeight), desc, theme);
			if (result.Toggled)
				open = !open;
		};
		// M24:enabled=false 时只画占位(值会保留在模型里,但关掉的 World 段不落盘),
		// 行内的禁用理由走 Tooltip / a11y,与库件"禁用行第二通道同源"口径一致。
		const auto floatRow = [&](const FloatRowDef& def, UI::UiNode& target, bool enabled = true)
		{
			float* value = FloatFieldPtr(target, def.Field);
			if (value == nullptr)
				return;
			Wui::PropertyRowDesc desc;
			desc.Label = Wui::Tr(def.Key, def.Label);
			desc.Term = def.Term;
			desc.A11yLabel = desc.Label;
			desc.A11yValue = FormatFloat(*value);
			desc.LabelWidth = labelWidth;
			desc.Enabled = enabled;
			desc.A11yEnabled = enabled;
			// M44:每行一句解释(键 = <Key> + ".doc");禁用行换成禁用理由(第二通道与悬停提示同源)。
			desc.Tooltip = Wui::Tr(std::string(def.Key) + ".doc", def.Doc);
			if (!enabled)
			{
				desc.FieldPlaceholder = Wui::Tr("panel.ui_designer.world.inactive", "\u2014");
				desc.Tooltip = Wui::Tr("panel.ui_designer.world.enabled_required",
					"Turn on World anchor to edit.");
			}
			const Wui::PropertyRowResult result =
				Wui::PropertyRow(ctx, Wui::HashId(def.Key), nextRow(rowHeight), desc, theme);
			if (!enabled)
				return;
			if (Wui::DragFloat(ctx, Wui::HashId((std::string(def.Key) + ".field").c_str()),
				result.FieldRect, *value, 0.25f, -1000000.0f, 1000000.0f, theme))
			{
				m_ScreenDirty = true;
				NoteNodeEdit(def.Label);
			}
		};

		// M25:属性页文本行(Target / Source / Event / Command 共用)。提交口径与 Props 逐字一致:
		// 回车 / 失焦提交,Escape 丢弃;首次改动经 NoteNodeEdit 记前像,鼠标抬起落一条撤销。
		// idBase = 稳定控件 id(只含节点 + 行下标 ⇒ a11y 树里的 id 稳定可寻址);
		// bufferBase = 编辑缓冲键(额外含 m_RowBufferGen,增删行/换节点后整体换代)。
		const auto textRow = [&](const std::string& idBase, const std::string& bufferBase,
			const std::string& label, const std::string& a11yLabel, const std::string& placeholder,
			const std::string& tooltip, const Wui::WuiRect& rowRect, std::string& value)
		{
			Wui::PropertyRowDesc desc;
			desc.Label = label;
			// 行节点用可见文案,字段节点的 a11y label 才是唯一的那条(如 "Bind 2 Source")——
			// 这样脚本按 label 找到的是可交互字段,不是标签行。
			desc.A11yLabel = label;
			desc.A11yValue = value;
			// 标签行不是控件(与 Props 行同一口径):可交互的是下面的 text-field 节点,
			// 避免脚本把"Target"标签行当按钮点。
			desc.A11yEnabled = false;
			desc.LabelWidth = labelWidth;
			desc.Tooltip = tooltip;
			const Wui::PropertyRowResult row = Wui::PropertyRow(ctx,
				Wui::HashId((idBase + ".row").c_str()), rowRect, desc, theme);
			std::string& buffer = ctx.Persist<std::string>(
				Wui::HashId((bufferBase + ".buf").c_str()), value);
			Wui::TextFieldA11y a11y;
			a11y.Label = a11yLabel;
			a11y.Placeholder = placeholder;
			const Wui::WuiId fieldId = Wui::HashId((idBase + ".field").c_str());
			bool cancelled = false;
			const bool committed = Wui::TextField(ctx, fieldId, row.FieldRect, buffer, theme, &cancelled, &a11y);
			if (cancelled)
			{
				buffer = value;
			}
			else if (committed || (ctx.Focus() != fieldId && buffer != value))
			{
				if (value != buffer)
				{
					value = buffer;
					m_ScreenDirty = true;
					NoteNodeEdit(label);
				}
			}
			// TextField 只登记 a11y 占位、不画占位文字(库件口径)—— 空值且未聚焦时补可见提示。
			if (buffer.empty() && ctx.Focus() != fieldId)
			{
				Wui::Label(ctx,
					glm::vec2 { row.FieldRect.X + 6.0f, row.FieldRect.Y + (row.FieldRect.H - 15.0f) * 0.5f },
					placeholder, theme.TextMuted, 15.0f);
			}
		};

		if (node == nullptr)
		{
			Wui::Label(ctx, glm::vec2 { inner.X + 2.0f, y + 2.0f },
				Wui::Tr("panel.ui_designer.no_selection", "Select a node in the outline to edit it."),
				theme.TextMuted, theme.FontSizeSmall);
			Wui::EndScrollArea(ctx);
			return;
		}

		// ---- Type:类型独有属性(M31)----
		// 两大块分区的上半:强调色 + 较大字号 + 1px 分隔线(全走库件),右侧给属性条数。
		const std::vector<PropertyRowRef> propRows = CollectPropertyRows(*node);
		// M37:多选批量改属性 —— 选中 >1 且同 Type 时,每个属性行同时写**全部**选中节点
		// (同一条 NoteNodeEdit/CommitNodeEdit ⇒ 一条撤销记录);混选时只写主选中。
		const bool batchEdit = BatchEditActive();
		const bool mixedTypes = SelectionMixedTypes();
		std::vector<UI::UiNode*> editTargets;
		if (batchEdit)
			editTargets = SelectedNodesMutable();
		if (editTargets.empty())
			editTargets.push_back(node);
		// M42:类型登记表的两个入口 —— `Find` 给 ComponentId/英文功能介绍(`Doc`),
		// `Component` 给 DisplayName/SizeNotes(本地化键的兜底)。
		const UI::UiNodeTypeDesc* typeDesc = UI::UiNodeRegistry::Find(node->Type);
		const Wui::WuiComponentDesc* typeMeta = UI::UiNodeRegistry::Component(node->Type);
		const std::string componentId = typeDesc != nullptr ? typeDesc->ComponentId : node->Type;
		{
			const Wui::WuiRect typeHeaderRow = nextRow(rowHeight);
			// a11y 稳定标识符:标签保持 "Type: <Type>"(既有验证脚本按它寻址,不随语言变化)。
			const std::string typeTitle = "Type: " + node->Type;
			// M42:可见文本 = 本地化控件名(键 `wui.component.<id>.name`,兜底 = DisplayName)。
			Wui::SectionHeader(ctx, typeHeaderRow, Wui::Tr("panel.ui_designer.type_header", "Type: ")
				+ Wui::Tr("wui.component." + componentId + ".name",
					typeMeta != nullptr && !typeMeta->DisplayName.empty()
					? typeMeta->DisplayName : node->Type),
			theme.Accent, theme, theme.FontSizeTitle);
			const std::string typeCount = "(" + std::to_string(propRows.size()) + ")";
			const float countWidth = ctx.MeasureTextWidth(typeCount, theme.FontSizeSmall);
			Wui::Label(ctx, glm::vec2 { typeHeaderRow.X + std::max(0.0f, typeHeaderRow.W - countWidth - 2.0f),
				typeHeaderRow.Y + 4.0f }, typeCount, theme.TextMuted, theme.FontSizeSmall);
			// a11y:标题节点(label = "Type: <TypeName>",value = 属性条数)—— 脚本按 label 可寻址。
			Wui::WuiAccessNode typeNode;
			typeNode.Id = Wui::HashId("ui_designer.type.header");
			typeNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			typeNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			typeNode.Kind = "heading";
			typeNode.Label = typeTitle;
			typeNode.Value = typeCount;
			typeNode.Rect = typeHeaderRow;
			typeNode.Enabled = true;
			typeNode.Interactive = false;
			typeNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(typeNode);
		}
		// M42:类型功能介绍一行(键 `wui.component.<id>.doc`,兜底 = `UiNodeTypeDesc::Doc`,
		// 空则用组件登记表的 SizeNotes)。a11y 稳定 id = `ui_designer.type.doc`、kind = label;
		// 可见文本走本地化,Value 保留英文源文(对照与审计用)。
		{
			const Wui::WuiRect typeDocRow = nextRow(rowHeight);
			const std::string docSource = typeDesc != nullptr && !typeDesc->Doc.empty()
				? typeDesc->Doc
				: (typeMeta != nullptr ? typeMeta->SizeNotes : std::string());
			const std::string docText = Wui::Tr("wui.component." + componentId + ".doc", docSource);
			Wui::Label(ctx, glm::vec2 { typeDocRow.X + 2.0f, typeDocRow.Y + 4.0f }, docText,
				theme.TextMuted, theme.FontSizeCaption);
			Wui::WuiAccessNode docNode;
			docNode.Id = Wui::HashId("ui_designer.type.doc");
			docNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			docNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			docNode.Kind = "label";
			docNode.Label = docText;
			docNode.Value = typeDesc != nullptr ? typeDesc->Doc : std::string();
			docNode.Rect = typeDocRow;
			docNode.Enabled = true;
			docNode.Interactive = false;
			docNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(docNode);
		}
		// M37:混选提示(块头一行)—— 不同 Type 的节点没有共同属性表,只改主选中。
		if (mixedTypes)
		{
			const Wui::WuiRect hintRow = nextRow(rowHeight);
			const std::string hintText = Wui::Tr("panel.ui_designer.props.mixed_types",
				"Mixed types: editing the primary selection");
			Wui::Label(ctx, glm::vec2 { inner.X + 2.0f, hintRow.Y + 4.0f }, hintText,
				theme.Warning, theme.FontSizeCaption);
			// a11y:非交互提示行(验证脚本可在 `ui.tree` 里按 id/label 找到它)。
			Wui::WuiAccessNode hintNode;
			hintNode.Id = Wui::HashId("ui_designer.props.mixed_types");
			hintNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			hintNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			hintNode.Kind = "label";
			hintNode.Label = hintText;
			hintNode.Rect = hintRow;
			hintNode.Enabled = true;
			hintNode.Interactive = false;
			hintNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(hintNode);
		}
		// M37:状态选择器 —— `StateScoped` 属性按状态分槽(`bg.hover`)。选中某状态后
		// Type 块只列该状态的 StateScoped 行 + 全部非状态行(过滤在 CollectPropertyRows)。
		// 切换**延后到帧末生效**:本帧的行清单/高度已按旧状态算过,立刻改会错位一帧。
		std::string pendingPropState = m_PropState;
		if (const std::vector<Wui::WuiComponentState>* states = PropStateOptions(*node))
		{
			std::vector<std::string> stateOptions;
			stateOptions.reserve(states->size());
			int stateSelected = -1;
			for (std::size_t i = 0; i < states->size(); ++i)
			{
				const Wui::WuiComponentState& state = (*states)[i];
				// M42:选项文本走本地化(键 `wui.state.<id>`),存值仍是 `state.Id`(别改)。
				// 用户口径:状态**选项文本不本地化**(保留登记表里的英文名,便于对照 .wui/文档),
				// 但**选项描述**要本地化 —— 描述在选中状态下方那行(`wui.state.<id>.doc`,见下)。
				stateOptions.push_back(state.Label.empty() ? state.Id : state.Label);
				if (state.Id == m_PropState)
					stateSelected = static_cast<int>(i);
			}
			if (stateSelected < 0)
				stateSelected = 0;

			Wui::PropertyRowDesc stateDesc;
			stateDesc.Label = Wui::Tr("panel.ui_designer.props.state", "State");
			stateDesc.Term = "State";
			// 行标签只做展示(a11y 关掉);可交互字段的 a11y label 固定为 "Props state"。
			stateDesc.A11yLabel = stateDesc.Label;
			stateDesc.A11yValue = stateOptions[static_cast<std::size_t>(stateSelected)];
			stateDesc.A11yEnabled = false;
			stateDesc.LabelWidth = labelWidth;
			// M44:这一行的解释(是什么、切了会怎样)。
			stateDesc.Tooltip = Wui::Tr("panel.ui_designer.props.state.doc",
				"Which visual states this type declares. Picking one narrows the property list "
				"below to that state's own properties (such as bg.hover); switching never edits "
				"the document.");
			const Wui::PropertyRowResult stateRow = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.props.state.row"), nextRow(rowHeight), stateDesc, theme);
			if (Wui::Combo(ctx, Wui::HashId("ui_designer.props.state.field"), stateRow.FieldRect,
				"Props state", stateOptions, stateSelected, theme))
			{
				pendingPropState = (*states)[static_cast<std::size_t>(stateSelected)].Id;
			}
			// M44:每个状态选项的说明。`Combo` 没有 per-option tooltip 口子(库件),按派工
			// 降级为"选中状态下方一行说明" —— 高度已计入 PropertyContentHeight(+1 行)。
			// 用本帧实际生效的 `m_PropState`(切换延后一帧),让说明与本帧显示的 StateScoped
			// 行清单一致。
			const std::string stateDoc = Wui::Tr("wui.state." + m_PropState + ".doc",
				StateDocFallback(m_PropState));
			const Wui::WuiRect stateDocRow = nextRow(rowHeight);
			Wui::Label(ctx, glm::vec2 { stateDocRow.X + 2.0f, stateDocRow.Y + 4.0f }, stateDoc,
				theme.TextMuted, theme.FontSizeCaption);
			Wui::WuiAccessNode stateDocNode;
			stateDocNode.Id = Wui::HashId("ui_designer.props.state.doc");
			stateDocNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			stateDocNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			stateDocNode.Kind = "label";
			stateDocNode.Label = stateDoc;
			stateDocNode.Value = m_PropState;
			stateDocNode.Rect = stateDocRow;
			stateDocNode.Enabled = true;
			stateDocNode.Interactive = false;
			stateDocNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(stateDocNode);
		}
		{
			if (node->Props.empty())
			{
				Wui::Label(ctx, glm::vec2 { inner.X + 2.0f, y + 2.0f },
					Wui::Tr("panel.ui_designer.no_props", "No properties are declared for this type."),
					theme.TextMuted, theme.FontSizeSmall);
				y += rowHeight;
			}
			// M29:按**类型登记的属性表**出行(不再只显示文档里已有的覆盖值)。
			//
			// 为什么改:此前 Props 段只列 `node->Props`,而新建的节点一个覆盖值都没有 ⇒
			// Button / Label / Panel 的面板长得一模一样,而且**连 label 都设不了**
			// (用户原话:"所有控件的属性都是相同的")。现在:类型登记表里的每个属性都出一行,
			// 有覆盖值就显示覆盖值,没有就显示登记的默认值(灰);编辑即写入覆盖,↺ 清除覆盖。
			std::string lastGroup;
			for (const PropertyRowRef& ref : propRows)
			{
				const Wui::WuiComponentProperty* meta = ref.Meta;
				// 分组小标题(只画不折叠:分组属于"展示",不是状态)。
				if (meta != nullptr)
				{
					const std::string groupName = PropertyGroupLabel(meta->Group);
					if (groupName != lastGroup)
					{
						lastGroup = groupName;
						Wui::Label(ctx, glm::vec2 { inner.X + 2.0f, y + 4.0f }, groupName,
							theme.TextMuted, theme.FontSizeCaption);
						y += rowHeight;
					}
				}

				const UI::UiProp* current = node->FindProp(ref.Name);
				const std::string value = current != nullptr
					? current->Value
					: (meta != nullptr ? PropertyDefaultText(*meta) : std::string());
				// M37:批量的"已覆盖"= 任一目标有覆盖(↺ 才亮);混选/单选 = 主选中自己。
				const bool overridden = current != nullptr;
				bool overriddenAny = overridden;
				if (batchEdit)
				{
					overriddenAny = std::any_of(editTargets.begin(), editTargets.end(),
						[&](const UI::UiNode* target) { return target->FindProp(ref.Name) != nullptr; });
				}
				const std::string base = "ui_designer.prop." + node->Id + "." + ref.Name;

				Wui::PropertyRowDesc desc;
				// M42:可见文案走本地化(键 `wui.prop.<名>.name` / `.doc`);A11yLabel 保持原始
				// 属性名(稳定标识符,脚本按它寻址),Term 仍是英文术语对照(既有 TrLabel 口径)。
				// M51:属性名/说明分**两层键** —— `wui.node.<Type>.prop.<名>.*` 优先,再回退到
				// `wui.prop.<名>.*`。为什么需要:同一个属性名在不同节点类型上含义不同 —— 文本节点的
				// `text` 是**内容**,按钮的 `text` 是**文字颜色**;共用一套键必然有一边被译错
				// (用户报的"两个控制的都是显示内容"就是文本节点上的 `text` 被标成了颜色)。
				const std::string propNameKey = "wui.node." + node->Type + ".prop." + ref.Name + ".name";
				const std::string propDocKey = "wui.node." + node->Type + ".prop." + ref.Name + ".doc";
				desc.Label = Wui::Tr(propNameKey,
					Wui::Tr("wui.prop." + ref.Name + ".name", ref.Name));
				desc.Term = ref.Name;
				desc.A11yLabel = ref.Name;
				desc.A11yValue = value;
				desc.LabelWidth = labelWidth;
				// M50:纹理行的值列里是 `WuiTexturePicker`(自带「定位」按钮)⇒ 字段再让出那一段,
				// 否则它会贴到/压住行尾的 ↺(用户报的"控件没有自适应所占区域"就是这个观感)。
				// 26(定位)+ 4(间距)+ 44(徽标常见宽度)。
				if (ref.Name == std::string("texture"))
					// 只让出与尾部复位键之间的间隙就够:选择器自己会按剩余宽度收缩 combo 与徽标(内部按 budget 切),预留过大反而把 combo 挤成 `<...>`(实测)。
					desc.TrailingReserve = 10.0f;
				desc.Enabled = true;
				desc.Tooltip = Wui::Tr(propDocKey,
					Wui::Tr("wui.prop." + ref.Name + ".doc", meta != nullptr ? meta->Doc : std::string()));
				if (meta != nullptr && !meta->Unit.empty())
					desc.Tooltip += (desc.Tooltip.empty() ? "" : "\n") + meta->Unit;
				// ↺ = 清除覆盖值(回到类型默认);只在**有覆盖**时出现(库件的 ResetModified 口径)。
				desc.ShowReset = true;
				desc.ResetId = Wui::HashId((base + ".reset").c_str());
				desc.ResetModified = overriddenAny;
				desc.ResetEnabled = overriddenAny;
				desc.ResetTooltip = Wui::Tr("panel.ui_designer.prop.reset",
					"Clear this override (back to the type default)");
				// (刻意不设 FieldPlaceholder:未覆盖时也要能**直接编辑**出覆盖值 ——
				//  占位文本会顶掉字段控件,那样"加一个覆盖"就没法做了。)
				// M31:值形如 @key 时,字段下方补一行译文/缺键预览,语言清单进 tooltip。
				bool previewMissing = false;
				std::string previewText;
				const LocalizationRef localization = ParseLocalizationRef(value);
				if (IsLocalizableProperty(meta, ref.Name) && localization.Valid)
				{
					previewText = LocalizationPreview(localization, previewMissing);
					const std::string languages = LocalizationLanguageList();
					if (!languages.empty())
						desc.Tooltip += (desc.Tooltip.empty() ? "" : "\n")
						+ Wui::Tr("panel.ui_designer.l10n.languages", "Languages") + ": " + languages;
					if (previewMissing)
						desc.Tooltip += (desc.Tooltip.empty() ? "" : "\n")
						+ Wui::Tr("panel.ui_designer.l10n.missing_hint",
						"No translation for this key - the runtime falls back to the inline text.");
				}
				const Wui::PropertyRowResult rowResult = Wui::PropertyRow(ctx,
					Wui::HashId((base + ".row").c_str()), nextRow(rowHeight), desc, theme);
				Wui::WuiRect previewRow {};
				if (!previewText.empty())
				{
					previewRow = nextRow(kLocalizationPreviewHeight);
					Wui::Label(ctx, glm::vec2 { rowResult.FieldRect.X + 2.0f, previewRow.Y + 1.0f },
						previewText, previewMissing ? theme.Danger : theme.TextMuted, theme.FontSizeCaption);
					Wui::WuiAccessNode previewNode;
					previewNode.Id = Wui::HashId((base + ".l10n").c_str());
					previewNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
					previewNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
					previewNode.Kind = "label";
					previewNode.Label = previewText;
					previewNode.Value = value;
					previewNode.Rect = previewRow;
					previewNode.Enabled = true;
					previewNode.Interactive = false;
					previewNode.Visible = true;
					Wui::WuiAccessibility::Get().Register(previewNode);
				}
				if (rowResult.ResetClicked && overriddenAny)
				{
					NoteNodeEdit(ref.Name, batchEdit);
					for (UI::UiNode* target : editTargets)
						ClearPropOverride(*target, ref.Name);
					m_ScreenDirty = true;
					continue;   // 本帧不再画字段(下一帧按"未覆盖"重画)
				}
				// ---- 按属性类型给编辑器;任何改动都写成"覆盖值" ----
				// 目标值(覆盖值,未覆盖时 = 类型默认)按**每个目标节点**求;"任一目标与
				// 新值不同"才算一次改动(批量下不会把已经等于新值的节点白写一遍)。
				const auto effectiveValue = [&](const UI::UiNode& target) -> std::string
				{
					const UI::UiProp* prop = target.FindProp(ref.Name);
					if (prop != nullptr)
						return prop->Value;
					return meta != nullptr ? PropertyDefaultText(*meta) : std::string();
				};
				const auto apply = [&](const std::string& text)
				{
					const bool anyChange = std::any_of(editTargets.begin(), editTargets.end(),
						[&](const UI::UiNode* target) { return effectiveValue(*target) != text; });
					if (!anyChange)
						return;
					NoteNodeEdit(ref.Name, batchEdit);
					for (UI::UiNode* target : editTargets)
						WritePropOverride(*target, ref.Name, text);
					m_ScreenDirty = true;
				};
				const Wui::WuiComponentProperty::Kind kind = meta != nullptr
					? meta->Type : Wui::WuiComponentProperty::Kind::Text;
				const Wui::WuiId fieldId = Wui::HashId((base + ".field").c_str());
				// M50:纹理引用行换 `Wui::WuiTexturePicker`(走 `Editor::TextureRefCatalog` 适配层,
				// 面板不自己扫盘/算徽标)。挂在 **`texture`(逻辑路径)** 上:这是用户与 AI 能选、
				// 引擎能解析(纹理解析钩子)的那一层;数值句柄 `textureId` 已标内部、面板不列。
				// 选中/清空即写覆盖值(一条撤销记录)。
				if (ref.Name == std::string("texture"))
				{
					std::string picked = value;
					auto options = Editor::TexturePickerOptions(picked,
						Wui::Tr("wui.prop.texture.name", "Texture"),
						"ui_designer.prop." + node->Id + "." + ref.Name, std::string());
					bool revealRequested = false;
					options.RevealRequested = &revealRequested;
					if (Wui::WuiTexturePicker(ctx, fieldId, rowResult.FieldRect, picked, options, theme))
						apply(picked);
					// "定位" = 复用宿主既有的"在内容浏览器选中"通道(与材质面板同一口径);
					// 面板不自己开资源管理器窗口。
					if (revealRequested)
					{
						const std::string shown = picked.empty() ? value : picked;
						if (host.SelectContentAsset(shown, "ui-designer-texture"))
							m_Status = Wui::Tr("panel.ui_designer.texture.located",
								"Selected in the Content Browser: ") + shown;
						else
							m_Status = Wui::Tr("panel.ui_designer.texture.locate_failed",
								"Cannot locate this texture in the Content Browser: ") + shown;
					}
					continue;   // 本行不再走下面的默认文本编辑器
				}
				switch (kind)
				{
				case Wui::WuiComponentProperty::Kind::Bool:
				{
					bool checked = value == "1" || value == "true" || value == "True"
						|| value == "yes" || value == "on";
					if (Wui::Checkbox(ctx, fieldId, rowResult.FieldRect, std::string(), checked, theme))
						apply(checked ? "true" : "false");
					break;
				}
				case Wui::WuiComponentProperty::Kind::Float:
				case Wui::WuiComponentProperty::Kind::Int:
				{
					float number = 0.0f;
					if (!value.empty())
					{
						try { number = std::stof(value); } catch (...) { number = 0.0f; }
					}
					const float minValue = meta != nullptr ? meta->Min : -1000000.0f;
					const float maxValue = meta != nullptr ? meta->Max : 1000000.0f;
					const float speed = meta != nullptr ? std::max(meta->Step, 0.001f) : 0.25f;
					const float step = meta != nullptr ? meta->Step : 1.0f;
					if (Wui::DragFloat(ctx, fieldId, rowResult.FieldRect, number, speed,
						minValue, maxValue, theme))
					{
						const float snapped = step > 0.0f ? std::round(number / step) * step : number;
						apply(kind == Wui::WuiComponentProperty::Kind::Int
							? std::to_string(static_cast<long long>(std::llround(snapped)))
							: FormatFloat(snapped));
					}
					break;
				}
				case Wui::WuiComponentProperty::Kind::Color:
				{
					Wui::WuiColor parsed;
					glm::vec4 rgba { 0.0f, 0.0f, 0.0f, 1.0f };
					if (Wui::ParseComponentColor(value, parsed))
						rgba = glm::vec4 { parsed.R, parsed.G, parsed.B, parsed.A };
					if (Wui::ColorField(ctx, fieldId, rowResult.FieldRect, rgba, theme))
					{
						Wui::WuiColor out { rgba.r, rgba.g, rgba.b, rgba.a };
						apply(Wui::FormatComponentColor(out));
					}
					break;
				}
				case Wui::WuiComponentProperty::Kind::Size2:
				{
					float width = 0.0f;
					float height = 0.0f;
					if (!Wui::ParseComponentSize(value, width, height))
					{
						width = 0.0f;
						height = 0.0f;
					}
					const float half = std::max(20.0f, (rowResult.FieldRect.W - 4.0f) * 0.5f);
					const Wui::WuiRect leftRect { rowResult.FieldRect.X, rowResult.FieldRect.Y,
						half, rowResult.FieldRect.H };
					const Wui::WuiRect rightRect { rowResult.FieldRect.X + half + 4.0f,
						rowResult.FieldRect.Y, rowResult.FieldRect.W - half - 4.0f, rowResult.FieldRect.H };
					const bool changedWidth = Wui::DragFloat(ctx,
						Wui::HashId((base + ".w").c_str()), leftRect, width, 0.5f, 0.0f, 100000.0f, theme);
					const bool changedHeight = Wui::DragFloat(ctx,
						Wui::HashId((base + ".h").c_str()), rightRect, height, 0.5f, 0.0f, 100000.0f, theme);
					if (changedWidth || changedHeight)
						apply(Wui::FormatComponentSize(width, height));
					break;
				}
				case Wui::WuiComponentProperty::Kind::Enum:
				{
					if (meta != nullptr && !meta->Options.empty())
					{
						int selected = -1;
						for (std::size_t i = 0; i < meta->Options.size(); ++i)
						{
							if (meta->Options[i] == value)
							{
								selected = static_cast<int>(i);
								break;
							}
						}
						if (Wui::Combo(ctx, fieldId, rowResult.FieldRect, ref.Name,
							meta->Options, selected, theme, value.empty() ? std::string() : value))
						{
							if (selected >= 0 && selected < static_cast<int>(meta->Options.size()))
								apply(meta->Options[static_cast<std::size_t>(selected)]);
						}
					}
					break;
				}
				default:
				{
					std::string& buffer = ctx.Persist<std::string>(
						Wui::HashId((base + ".buf").c_str()), value);
					Wui::TextFieldA11y a11y;
					a11y.Label = ref.Name;
					bool cancelled = false;
					const bool committed = Wui::TextField(ctx, fieldId, rowResult.FieldRect,
						buffer, theme, &cancelled, &a11y);
					if (cancelled)
						buffer = value;
					else if (committed || (ctx.Focus() != fieldId && buffer != value))
						apply(buffer);
					break;
				}
				}
			}
		}

		// ---- Common:所有类型都有的公共属性(M31)----
		{
			const Wui::WuiRect commonHeaderRow = nextRow(rowHeight);
			const std::string commonTitle = Wui::Tr("panel.ui_designer.section.common", "Common");
			Wui::SectionHeader(ctx, commonHeaderRow, commonTitle, theme.Text, theme, theme.FontSizeBody);
			Wui::WuiAccessNode commonNode;
			commonNode.Id = Wui::HashId("ui_designer.section.common");
			commonNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			commonNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			commonNode.Kind = "heading";
			commonNode.Label = commonTitle;
			commonNode.Rect = commonHeaderRow;
			commonNode.Enabled = true;
			commonNode.Interactive = false;
			commonNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(commonNode);
		}

		// ---- Node ----
		groupHeader("panel.ui_designer.section.node", "Node", std::string(), m_ShowNodeSection);
		if (m_ShowNodeSection)
		{
			// M44:只读内联行。`a11yLabel` 是**稳定标识符**(恒英文:Id / Type),不随本地化变化;
			// 可见标签与值才走本地化 —— 脚本/AI 按稳定标识符寻址,按 label 找不到中文行。
			// 用户口径"UI node 的标识没法自定义编辑"。编辑走与大纲改名**同一条**校验 + 迁移
			// (`RenameNode` 是唯一实现:合法性、同层唯一、面板内部按 Id 索引的状态一起搬),
			// 非法名拒绝并保持原值。值都是稳定标识(不本地化),只有行标签本地化。
			const auto inlineRow = [&](const char* key, const char* a11yLabel, const char* fallback,
				const std::string& value)
			{
				Wui::PropertyRowDesc desc;
				desc.Label = Wui::Tr(key, fallback);
				desc.A11yLabel = a11yLabel;
				desc.InlineValue = true;
				desc.InlineValueText = value;
				desc.LabelWidth = labelWidth;
				Wui::PropertyRow(ctx, Wui::HashId(key), nextRow(rowHeight), desc, theme);
			};
			// ---- Id:可编辑 ----
			// 行缓冲按"节点 Id + 代次"建键(与 Bind/On 行同一口径):换节点、改名、撤销之后
			// 必须换代,否则输入框会回显上一个节点的名字。
			if (m_IdRowNodeId != node->Id)
			{
				m_IdRowNodeId = node->Id;
				m_IdRowBuffer = node->Id;
			}
			{
				const std::string idRowError = OutlineRenameError(m_IdRowNodeId, TrimCopy(m_IdRowBuffer));
				Wui::PropertyRowDesc desc;
				desc.Label = Wui::Tr("panel.ui_designer.node.id", "Id");
				desc.Term = "Id";
				desc.A11yLabel = "Id";          // 稳定标识符:脚本/AI 按它寻址
				desc.A11yValue = node->Id;
				desc.LabelWidth = labelWidth;
				desc.Tooltip = Wui::Tr("panel.ui_designer.node.id.doc",
					"Stable identity of this node: bindings, events, hot reload and AI/script addressing "
					"all use it. Press Enter to apply; an invalid or duplicate name is refused.");
				const Wui::PropertyRowResult idRow = Wui::PropertyRow(ctx,
					Wui::HashId("panel.ui_designer.node.id.row"), nextRow(rowHeight), desc, theme);
				Wui::TextFieldA11y idA11y;
				idA11y.Label = "Id";
				if (Wui::TextFieldEx(ctx, Wui::HashId("panel.ui_designer.node.id.field"),
					idRow.FieldRect, m_IdRowBuffer, theme, idRowError, &idA11y))
				{
					std::string renameError;
					if (RenameNode(node->Id, TrimCopy(m_IdRowBuffer), &renameError))
					{
						m_IdRowNodeId = TrimCopy(m_IdRowBuffer);   // 提交成功:缓冲跟上新 Id
					}
					else
					{
						// 拒绝:保持原名与输入内容,理由进状态栏(用户能改完再回车)。
						m_Status = renameError;
					}
				}
			}
			// Id 的值是用户设的标识(原文,不本地化);Type 的值显示**本地化控件名**
			// (键 `wui.component.<id>.name`,兜底 = 原始 Type),但 a11y 标识符保持 "Type"。
			inlineRow("panel.ui_designer.node.type", "Type", "Type", ComponentDisplayName(node->Type));
		}

		// ---- Anchor ----
		groupHeader("panel.ui_designer.section.anchor", "Anchor", std::string(), m_ShowAnchorSection);
		if (m_ShowAnchorSection)
		{
			for (const FloatRowDef& def : kAnchorFloatRows)
				floatRow(def, *node);

			Wui::PropertyRowDesc desc;
			desc.Label = Wui::Tr("panel.ui_designer.anchor.relative_safe", "Relative To Safe Area");
			desc.Term = "RelativeToSafeArea";
			desc.A11yLabel = desc.Label;
			desc.LabelWidth = labelWidth;
			desc.Tooltip = Wui::Tr("panel.ui_designer.anchor.relative_safe.doc",
				"Anchor against the viewport content rect (safe area already removed) instead of the "
				"parent rect. Use it for HUD elements that must dodge screen notches.");
			const Wui::PropertyRowResult result = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.anchor.relative_safe"), nextRow(rowHeight), desc, theme);
			if (Wui::Checkbox(ctx, Wui::HashId("ui_designer.anchor.relative_safe.field"), result.FieldRect,
				std::string(), node->Anchor.RelativeToSafeArea, theme))
			{
				m_ScreenDirty = true;
				NoteNodeEdit(desc.Label);
			}
		}

		// ---- Layout ----
		groupHeader("panel.ui_designer.section.layout", "Layout", std::string(), m_ShowLayoutSection);
		if (m_ShowLayoutSection)
		{
			// M51:「自适应大小」开关(用户报障:文本框几个字却占很大地方)。勾上后布局在容器
			// 排布之后按**内容**收紧这个节点的框:文本按实测字宽、文本框按 padding+提示文字、
			// 复选框/开关按方框+标签、容器按子的包围盒。只影响运行态矩形,不改文档的 Anchor.Size。
			{
				Wui::PropertyRowDesc autoDesc;
				autoDesc.Label = Wui::Tr("panel.ui_designer.layout.autosize", "Fit content");
				autoDesc.Term = "AutoSize";
				autoDesc.A11yLabel = autoDesc.Label;
				autoDesc.LabelWidth = labelWidth;
				autoDesc.Tooltip = Wui::Tr("panel.ui_designer.layout.autosize.doc",
					"Shrink this node's box to fit its content (text width, or the children's bounding "
					"box) instead of using the authored size. Stretched anchors ignore it.");
				const Wui::PropertyRowResult autoRow = Wui::PropertyRow(ctx,
					Wui::HashId("ui_designer.layout.autosize"), nextRow(rowHeight), autoDesc, theme);
				if (Wui::Checkbox(ctx, Wui::HashId("ui_designer.layout.autosize.field"), autoRow.FieldRect,
					std::string(), node->Layout.AutoSize, theme))
				{
					// 注意:`Wui::Checkbox(..., bool& value, ...)` **自己就把值写回引用**了 ——
					// 这里绝不能再取反一次,否则两次翻转抵消、点了没反应(实测踩过)。
					NoteNodeEdit(autoDesc.Label);
					m_ScreenDirty = true;
				}
			}
			std::vector<std::string> options;
			options.reserve(sizeof(kLayoutKinds) / sizeof(kLayoutKinds[0]));
			for (const UI::UiLayoutKind kind : kLayoutKinds)
				options.push_back(UI::UiLayoutKindName(kind));

			int selected = 0;
			for (std::size_t i = 0; i < options.size(); ++i)
			{
				if (kLayoutKinds[i] == node->Layout.Kind)
				{
					selected = static_cast<int>(i);
					break;
				}
			}

			Wui::PropertyRowDesc kindDesc;
			kindDesc.Label = Wui::Tr("panel.ui_designer.layout.kind", "Kind");
			kindDesc.Term = "Kind";
			kindDesc.A11yLabel = kindDesc.Label;
			kindDesc.A11yValue = options.empty() ? std::string() : options[static_cast<std::size_t>(selected)];
			kindDesc.LabelWidth = labelWidth;
			// 选项文本 = `.wui` 的枚举字面量(UiLayoutKindName;序列化值,不本地化),
			// 只有行标签与解释走本地化 —— 改成中文会让"文档里写的值"对不上。
			kindDesc.Tooltip = Wui::Tr("panel.ui_designer.layout.kind.doc",
				"How this container arranges its children: Absolute (free, children use their own "
				"anchor), Column / Row (stack with Gap), Overlay (stack on top of each other), "
				"Grid (Columns wide) or Flex (Row Major picks the main axis).");
			const Wui::PropertyRowResult kindRow = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.layout.kind.row"), nextRow(rowHeight), kindDesc, theme);
			if (Wui::Combo(ctx, Wui::HashId("ui_designer.layout.kind.field"), kindRow.FieldRect,
				kindDesc.Label, options, selected, theme))
			{
				node->Layout.Kind = kLayoutKinds[static_cast<std::size_t>(selected)];
				m_ScreenDirty = true;
				NoteNodeEdit(kindDesc.Label);
			}

			for (const FloatRowDef& def : kLayoutFloatRows)
				floatRow(def, *node);

			Wui::PropertyRowDesc columnsDesc;
			columnsDesc.Label = Wui::Tr("panel.ui_designer.layout.columns", "Columns");
			columnsDesc.Term = "Columns";
			columnsDesc.A11yLabel = columnsDesc.Label;
			columnsDesc.A11yValue = std::to_string(node->Layout.Columns);
			columnsDesc.LabelWidth = labelWidth;
			columnsDesc.Tooltip = Wui::Tr("panel.ui_designer.layout.columns.doc",
				"Number of columns used by the Grid layout kind. Other kinds ignore it.");
			const Wui::PropertyRowResult columnsRow = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.layout.columns.row"), nextRow(rowHeight), columnsDesc, theme);
			int64_t columns = node->Layout.Columns;
			if (Wui::DragInt(ctx, Wui::HashId("ui_designer.layout.columns.field"), columnsRow.FieldRect,
				columns, 1, 64, theme))
			{
				node->Layout.Columns = static_cast<int>(columns);
				m_ScreenDirty = true;
				NoteNodeEdit(columnsDesc.Label);
			}

			Wui::PropertyRowDesc rowMajorDesc;
			rowMajorDesc.Label = Wui::Tr("panel.ui_designer.layout.row_major", "Row Major");
			rowMajorDesc.Term = "RowMajor";
			rowMajorDesc.A11yLabel = rowMajorDesc.Label;
			rowMajorDesc.LabelWidth = labelWidth;
			rowMajorDesc.Tooltip = Wui::Tr("panel.ui_designer.layout.row_major.doc",
				"Flex only: on = lay children out along the row (horizontal main axis), "
				"off = down the column.");
			const Wui::PropertyRowResult rowMajorRow = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.layout.row_major.row"), nextRow(rowHeight), rowMajorDesc, theme);
			if (Wui::Checkbox(ctx, Wui::HashId("ui_designer.layout.row_major.field"), rowMajorRow.FieldRect,
				std::string(), node->Layout.RowMajor, theme))
			{
				m_ScreenDirty = true;
				NoteNodeEdit(rowMajorDesc.Label);
			}
		}

		// ---- World(世界锚点,M24)----
		// `World:` 块 = 把节点钉在"目标世界位置投影到屏幕后的点"上(UiWorldProjector)。
		// Enabled=false = 序列化时整块不写(不是写 `Enabled: false`);Target 为空 + Enabled=true
		// 会被 ValidateUiDocument 判错、且保存出的文档 UiDocumentIO::Parse 读不回来 —— 段内必须先给可见提示。
		groupHeader("panel.ui_designer.section.world", "World",
			node->World.Enabled ? Wui::Tr("panel.ui_designer.world.on", "On") : std::string(),
			m_ShowWorldSection);
		if (m_ShowWorldSection)
		{
			Wui::PropertyRowDesc enabledDesc;
			enabledDesc.Label = Wui::Tr("panel.ui_designer.world.enabled", "Enabled");
			enabledDesc.Term = "Enabled";
			enabledDesc.A11yLabel = enabledDesc.Label;
			enabledDesc.A11yValue = node->World.Enabled ? "true" : "false";
			enabledDesc.LabelWidth = labelWidth;
			enabledDesc.Tooltip = Wui::Tr("panel.ui_designer.world.enabled.doc",
				"Pin this node to a world position projected onto the screen instead of the parent "
				"rect. Off = a plain screen-space node (the whole World block is not written on save).");
			const Wui::PropertyRowResult enabledRow = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.world.enabled.row"), nextRow(rowHeight), enabledDesc, theme);
			if (Wui::Checkbox(ctx, Wui::HashId("ui_designer.world.enabled.field"), enabledRow.FieldRect,
				std::string(), node->World.Enabled, theme))
			{
				m_ScreenDirty = true;
				NoteNodeEdit(enabledDesc.Label);
			}

			const bool targetMissing = node->World.Enabled && node->World.Target.empty();
			const std::string targetWarning = targetMissing
				? Wui::Tr("panel.ui_designer.world.target_required",
					"World anchor needs a non-empty Target.")
				: std::string();

			// Target:文本提交口径与 Props 一致(回车 / 失焦提交,Escape 丢弃)。
			{
				const std::string base = "ui_designer.world.target." + node->Id;
				Wui::PropertyRowDesc targetDesc;
				targetDesc.Label = Wui::Tr("panel.ui_designer.world.target", "Target");
				targetDesc.Term = "Target";
				targetDesc.A11yLabel = targetDesc.Label;
				targetDesc.A11yValue = targetMissing ? targetWarning : node->World.Target;
				targetDesc.LabelWidth = labelWidth;
				targetDesc.Enabled = node->World.Enabled;
				targetDesc.A11yEnabled = node->World.Enabled;
				if (node->World.Enabled)
				{
					// 空 Target 的红色提示保持不动(段内另有可见提示行);有值时给一句解释。
					targetDesc.Tooltip = targetWarning.empty()
						? Wui::Tr("panel.ui_designer.world.target.doc",
							"Name the host resolves to a world position (an entity tag such as "
							"'Mesh:Cube Green'), not a UI node id.")
						: targetWarning;
				}
				else
				{
					targetDesc.FieldPlaceholder = Wui::Tr("panel.ui_designer.world.inactive", "\u2014");
					targetDesc.Tooltip = Wui::Tr("panel.ui_designer.world.enabled_required",
						"Turn on World anchor to edit.");
				}
				const Wui::PropertyRowResult targetRow = Wui::PropertyRow(ctx,
					Wui::HashId((base + ".row").c_str()), nextRow(rowHeight), targetDesc, theme);
				if (node->World.Enabled)
				{
					std::string& buffer = ctx.Persist<std::string>(
						Wui::HashId((base + ".buf").c_str()), node->World.Target);
					Wui::TextFieldA11y a11y;
					a11y.Label = targetDesc.Label;
					const Wui::WuiId fieldId = Wui::HashId((base + ".field").c_str());
					bool cancelled = false;
					const bool committed =
						Wui::TextField(ctx, fieldId, targetRow.FieldRect, buffer, theme, &cancelled, &a11y);
					if (cancelled)
					{
						buffer = node->World.Target;
					}
					else if (committed || (ctx.Focus() != fieldId && buffer != node->World.Target))
					{
						if (node->World.Target != buffer)
						{
							node->World.Target = buffer;
							m_ScreenDirty = true;
							NoteNodeEdit(targetDesc.Label);
						}
					}
				}
			}

			if (targetMissing)
			{
				const Wui::WuiRect hintRow = nextRow(rowHeight);
				Wui::Label(ctx, glm::vec2 { hintRow.X + 2.0f, hintRow.Y + 4.0f }, targetWarning,
					theme.Danger, theme.FontSizeSmall);
			}

			for (const FloatRowDef& def : kWorldFloatRows)
				floatRow(def, *node, node->World.Enabled);

			Wui::PropertyRowDesc keepDesc;
			keepDesc.Label = Wui::Tr("panel.ui_designer.world.keep_on_screen", "Keep On Screen");
			keepDesc.Term = "KeepOnScreen";
			keepDesc.A11yLabel = keepDesc.Label;
			keepDesc.A11yValue = node->World.KeepOnScreen ? "true" : "false";
			keepDesc.LabelWidth = labelWidth;
			keepDesc.Enabled = node->World.Enabled;
			keepDesc.A11yEnabled = node->World.Enabled;
			if (node->World.Enabled)
			{
				keepDesc.Tooltip = Wui::Tr("panel.ui_designer.world.keep_on_screen.doc",
					"Clamp the projected point into the content rect when it leaves the screen.");
			}
			else
			{
				keepDesc.FieldPlaceholder = Wui::Tr("panel.ui_designer.world.inactive", "\u2014");
				keepDesc.Tooltip = Wui::Tr("panel.ui_designer.world.enabled_required",
					"Turn on World anchor to edit.");
			}
			const Wui::PropertyRowResult keepRow = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.world.keep_on_screen.row"), nextRow(rowHeight), keepDesc, theme);
			if (node->World.Enabled && Wui::Checkbox(ctx,
				Wui::HashId("ui_designer.world.keep_on_screen.field"), keepRow.FieldRect,
				std::string(), node->World.KeepOnScreen, theme))
			{
				m_ScreenDirty = true;
				NoteNodeEdit(keepDesc.Label);
			}
		}

		// ---- Bind(数据绑定;M25)----
		// `Bind:` 块 = "节点属性 ← 数据源"。源协议的唯一判据是引擎的 UiBinding::Parse;
		// 本段只做文本编辑 + 空行 / 缺 `:` 的可见提示。空 Target/Source 会被
		// ValidateUiDocument 拒绝 ⇒ 保存时整条不写盘(SaveTo 过滤写盘副本),模型里保留草稿。
		groupHeader("panel.ui_designer.section.bind", "Bind",
			"(" + std::to_string(node->Bind.size()) + ")", m_ShowBindSection);
		if (m_ShowBindSection)
		{
			const std::string bindPrefix = Wui::Tr("panel.ui_designer.bind.a11y", "Bind");
			const std::string targetLabel = Wui::Tr("panel.ui_designer.bind.target", "Target");
			const std::string sourceLabel = Wui::Tr("panel.ui_designer.bind.source", "Source");
			int removeBind = -1;
			for (std::size_t i = 0; i < node->Bind.size(); ++i)
			{
				UI::UiBindingDecl& binding = node->Bind[i];
				const std::string ordinal = std::to_string(i + 1);
				const std::string idBase = "ui_designer.bind." + node->Id + "." + std::to_string(i);
				const std::string bufferBase = idBase + ".g" + std::to_string(m_RowBufferGen);
				const Wui::WuiRect targetRow = nextRow(rowHeight);
				const Wui::WuiRect sourceRow = nextRow(rowHeight);
				// 两条行收窄到同一个动作列宽 ⇒ 字段列同宽、上下对齐。
				const Wui::WuiRect targetField { targetRow.X, targetRow.Y,
					std::max(0.0f, targetRow.W - kRowActionWidth - 2.0f), targetRow.H };
				const Wui::WuiRect sourceField { sourceRow.X, sourceRow.Y,
					std::max(0.0f, sourceRow.W - kRowActionWidth - 2.0f), sourceRow.H };
				textRow(idBase + ".target", bufferBase + ".target", targetLabel,
					bindPrefix + " " + ordinal + " " + targetLabel,
					Wui::Tr("panel.ui_designer.bind.target.hint", "Value"),
					Wui::Tr("panel.ui_designer.bind.target.tip",
						"Node property driven by the data source (Bind)."),
					targetField, binding.Target);
				textRow(idBase + ".source", bufferBase + ".source", sourceLabel,
					bindPrefix + " " + ordinal + " " + sourceLabel,
					Wui::Tr("panel.ui_designer.bind.source.hint", "service:player.hp"),
					Wui::Tr("panel.ui_designer.bind.source.tip",
						"Protocols: const: 1 / ecs: 0/Health/Current / script:player.hp / service:level"),
					sourceField, binding.Source);
				// 删除按钮在本条第二行(Source)右端;擦除推迟到整段画完之后(见下)。
				const Wui::WuiRect removeRect { sourceRow.X + sourceRow.W - kRowActionSize,
					sourceRow.Y + (sourceRow.H - kRowActionSize) * 0.5f, kRowActionSize, kRowActionSize };
				if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".remove").c_str()), removeRect, "-", theme,
					true, false,
					Wui::Tr("panel.ui_designer.bind.remove_tip", "Remove binding") + " " + ordinal))
				{
					removeBind = static_cast<int>(i);
				}
			}
			if (node->Bind.empty())
			{
				Wui::Label(ctx, glm::vec2 { inner.X + 2.0f, y + 2.0f },
					Wui::Tr("panel.ui_designer.bind.empty", "No bindings."),
					theme.TextMuted, theme.FontSizeSmall);
				y += rowHeight;
			}
			for (const std::string& hint : BindRowHints(*node))
			{
				const Wui::WuiRect hintRow = nextRow(rowHeight);
				Wui::Label(ctx, glm::vec2 { hintRow.X + 2.0f, hintRow.Y + 4.0f }, hint,
					theme.Warning, theme.FontSizeSmall);
			}
			const bool addBind = Wui::ButtonEx(ctx, Wui::HashId("ui_designer.bind.add"),
				nextRow(rowHeight), Wui::Tr("panel.ui_designer.bind.add", "+ Add binding"), theme,
				true, false, Wui::Tr("panel.ui_designer.bind.add_tip",
					"Append an empty binding row; empty rows are not written to disk"));
			// 增删都在本段画完之后落:本帧前面画过的 TextField 已把失焦提交写进模型,
			// 行下标/缓冲代次的变化不会吃掉未提交文本(与节点面板"画完再 AddNode"同一口径)。
			if (removeBind >= 0)
			{
				UI::UiBindingDecl& victim = node->Bind[static_cast<std::size_t>(removeBind)];
				if (victim.Target.empty() && victim.Source.empty())
				{
					// 本来就是空行:删它不产生撤销记录(派工口径)。
					node->Bind.erase(node->Bind.begin() + removeBind);
					m_ScreenDirty = true;
					m_Dirty = true;
					++m_RowBufferGen;
					m_Status = Wui::Tr("panel.ui_designer.bind.dropped_empty",
						"Removed an empty binding row (no undo step)");
				}
				else
				{
					CommitNodeEdit();   // 先收口上一条编辑,让删除自己占一条撤销记录
					m_FrameNodeBefore = *node;
					node->Bind.erase(node->Bind.begin() + removeBind);
					m_ScreenDirty = true;
					++m_RowBufferGen;
					NoteNodeEdit(Wui::Tr("panel.ui_designer.bind.remove", "Remove Binding"));
					m_Status = Wui::Tr("panel.ui_designer.bind.removed", "Removed binding ")
						+ std::to_string(removeBind + 1);
				}
			}
			if (addBind)
			{
				CommitNodeEdit();
				m_FrameNodeBefore = *node;
				node->Bind.push_back(UI::UiBindingDecl {});
				m_ScreenDirty = true;
				++m_RowBufferGen;
				NoteNodeEdit(Wui::Tr("panel.ui_designer.bind.add", "+ Add binding"));
				m_Status = Wui::Tr("panel.ui_designer.bind.added", "Added an empty binding row");
			}
		}

		// ---- On(命令;M25)----
		// `On:` 块 = "事件 → 命令"(如 Click → ui.close)。事件名 / 命令名由运行时消费端判定;
		// 本段只做文本编辑 + 空行可见提示(空 Event/Command 同样不写盘)。
		groupHeader("panel.ui_designer.section.on", "On",
			"(" + std::to_string(node->On.size()) + ")", m_ShowOnSection);
		if (m_ShowOnSection)
		{
			const std::string onPrefix = Wui::Tr("panel.ui_designer.on.a11y", "On");
			const std::string eventLabel = Wui::Tr("panel.ui_designer.on.event", "Event");
			const std::string commandLabel = Wui::Tr("panel.ui_designer.on.command", "Command");
			int removeOn = -1;
			for (std::size_t i = 0; i < node->On.size(); ++i)
			{
				UI::UiCommandDecl& command = node->On[i];
				const std::string ordinal = std::to_string(i + 1);
				const std::string idBase = "ui_designer.on." + node->Id + "." + std::to_string(i);
				const std::string bufferBase = idBase + ".g" + std::to_string(m_RowBufferGen);
				const Wui::WuiRect eventRow = nextRow(rowHeight);
				const Wui::WuiRect commandRow = nextRow(rowHeight);
				const Wui::WuiRect eventField { eventRow.X, eventRow.Y,
					std::max(0.0f, eventRow.W - kRowActionWidth - 2.0f), eventRow.H };
				const Wui::WuiRect commandField { commandRow.X, commandRow.Y,
					std::max(0.0f, commandRow.W - kRowActionWidth - 2.0f), commandRow.H };
				textRow(idBase + ".event", bufferBase + ".event", eventLabel,
					onPrefix + " " + ordinal + " " + eventLabel,
					Wui::Tr("panel.ui_designer.on.event.hint", "Click"),
					Wui::Tr("panel.ui_designer.on.event.tip",
						"UI event that triggers the command (e.g. Click)."),
					eventField, command.Event);
				textRow(idBase + ".command", bufferBase + ".command", commandLabel,
					onPrefix + " " + ordinal + " " + commandLabel,
					Wui::Tr("panel.ui_designer.on.command.hint", "ui.close"),
					Wui::Tr("panel.ui_designer.on.command.tip",
						"Command emitted when the event fires (ui.* runs through the host)."),
					commandField, command.Command);
				const Wui::WuiRect removeRect { commandRow.X + commandRow.W - kRowActionSize,
					commandRow.Y + (commandRow.H - kRowActionSize) * 0.5f, kRowActionSize, kRowActionSize };
				if (Wui::ButtonEx(ctx, Wui::HashId((idBase + ".remove").c_str()), removeRect, "-", theme,
					true, false,
					Wui::Tr("panel.ui_designer.on.remove_tip", "Remove command") + " " + ordinal))
				{
					removeOn = static_cast<int>(i);
				}
			}
			if (node->On.empty())
			{
				Wui::Label(ctx, glm::vec2 { inner.X + 2.0f, y + 2.0f },
					Wui::Tr("panel.ui_designer.on.empty", "No commands."),
					theme.TextMuted, theme.FontSizeSmall);
				y += rowHeight;
			}
			for (const std::string& hint : CommandRowHints(*node))
			{
				const Wui::WuiRect hintRow = nextRow(rowHeight);
				Wui::Label(ctx, glm::vec2 { hintRow.X + 2.0f, hintRow.Y + 4.0f }, hint,
					theme.Warning, theme.FontSizeSmall);
			}
			const bool addOn = Wui::ButtonEx(ctx, Wui::HashId("ui_designer.on.add"),
				nextRow(rowHeight), Wui::Tr("panel.ui_designer.on.add", "+ Add command"), theme,
				true, false, Wui::Tr("panel.ui_designer.on.add_tip",
					"Append an empty command row; empty rows are not written to disk"));
			if (removeOn >= 0)
			{
				UI::UiCommandDecl& victim = node->On[static_cast<std::size_t>(removeOn)];
				if (victim.Event.empty() && victim.Command.empty())
				{
					node->On.erase(node->On.begin() + removeOn);
					m_ScreenDirty = true;
					m_Dirty = true;
					++m_RowBufferGen;
					m_Status = Wui::Tr("panel.ui_designer.on.dropped_empty",
						"Removed an empty On row (no undo step)");
				}
				else
				{
					CommitNodeEdit();
					m_FrameNodeBefore = *node;
					node->On.erase(node->On.begin() + removeOn);
					m_ScreenDirty = true;
					++m_RowBufferGen;
					NoteNodeEdit(Wui::Tr("panel.ui_designer.on.remove", "Remove Command"));
					m_Status = Wui::Tr("panel.ui_designer.on.removed", "Removed command ")
						+ std::to_string(removeOn + 1);
				}
			}
			if (addOn)
			{
				CommitNodeEdit();
				m_FrameNodeBefore = *node;
				node->On.push_back(UI::UiCommandDecl {});
				m_ScreenDirty = true;
				++m_RowBufferGen;
				NoteNodeEdit(Wui::Tr("panel.ui_designer.on.add", "+ Add command"));
				m_Status = Wui::Tr("panel.ui_designer.on.added", "Added an empty command row");
			}
		}

		// M37:状态切换在帧末生效(本帧的行清单/高度已按旧状态算过;下一帧起按新状态出行)。
		m_PropState = pendingPropState;

		Wui::EndScrollArea(ctx);
	}

	// ---- 画布 ----

	void UiDesignerPanel::RenderCanvas(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.WindowBg, 2.0f);

		if (!m_HasDocument)
		{
			// M16:空态不再是"去工具栏打字"这一条死路 —— 直接列出内容根里扫到的 .wui
			// (每条一个按钮,点一条即 LoadFrom);一条都没有就提示用工具栏 New 新建。
			RenderDocumentBrowser(ctx, rect, host);
			return;
		}

		if (m_ScreenDirty)
			RebuildScreen();

		LayoutCanvas(rect);
		HandleCanvasInput(ctx, rect);
		{
			Wui::ClipScope clip(ctx, rect);
			DrawCanvas(ctx, rect, host);
		}
	}

	void UiDesignerPanel::LayoutCanvas(const Wui::WuiRect& rect)
	{
		const float designWidth = std::max(m_Document.Design.Resolution.x, 1.0f);
		const float designHeight = std::max(m_Document.Design.Resolution.y, 1.0f);
		const float fit = CanvasFitScale(rect);

		// 用"物理面 = 设计分辨率"(比例 = 1)调 ComputeUiViewport:安全区内缩在设计单位下
		// 换算逐字段正确(ContentRect = 设计矩形扣安全区);再把 Scale 换成画布适配比、
		// 把原点挪到画布中心 —— 设计器是设计视图,只做等比适配、不裁切。
		UI::UiViewport viewport = UI::ComputeUiViewport(m_Document.Design, m_Document.SafeArea,
			UI::UiSurface { glm::vec2 { designWidth, designHeight }, 1.0f });
		// M19:适配比 × 用户缩放(滚轮),再加平移量 —— 大设计稿也能放大看细节。
		// 视图中心仍然对齐画布中心,所以平移是"相对适配位置"的像素偏移,与缩放无关。
		viewport.Scale = std::max(0.02f, fit * m_CanvasZoom);
		viewport.PhysicalSize = glm::vec2 { rect.W, rect.H };
		viewport.PhysicalOrigin = glm::vec2 {
			rect.X + (rect.W - designWidth * viewport.Scale) * 0.5f + m_CanvasPan.x,
			rect.Y + (rect.H - designHeight * viewport.Scale) * 0.5f + m_CanvasPan.y };
		m_Viewport = viewport;
		m_Screen.Layout(viewport);
	}

	// M19:适配比 = 把整个设计分辨率塞进画布矩形(留 14px 边距)的等比缩放。
	// 单独抽出来,是因为"以光标为中心缩放"要按**同一个**适配比反推平移量。
	float UiDesignerPanel::CanvasFitScale(const Wui::WuiRect& rect) const
	{
		const float designWidth = std::max(m_Document.Design.Resolution.x, 1.0f);
		const float designHeight = std::max(m_Document.Design.Resolution.y, 1.0f);
		constexpr float pad = 14.0f;
		const float fitWidth = std::max(1.0f, rect.W - pad * 2.0f) / designWidth;
		const float fitHeight = std::max(1.0f, rect.H - pad * 2.0f) / designHeight;
		return std::max(0.01f, std::min(fitWidth, fitHeight));
	}

	void UiDesignerPanel::HandleCanvasInput(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		const Wui::WuiInputState& input = ctx.Input();

		// ---- M19:画布视图操作(缩放 / 平移)----
		// 滚轮落在画布上 = 以光标为中心缩放(与主流编辑器一致:放大时光标下的那个点不动)。
		if (ctx.IsHovered(rect) && input.Wheel != 0.0f)
		{
			const glm::vec2 anchorDesign = m_Viewport.PhysicalToDesign(input.MousePos);
			m_CanvasZoom = std::clamp(m_CanvasZoom * (input.Wheel > 0.0f ? 1.1f : (1.0f / 1.1f)), 0.25f, 8.0f);
			// 解 "PhysicalOrigin + anchorDesign*scale == mousePos":
			//   pan = mousePos - anchorDesign*scale - centerTerm
			// 其中 centerTerm 是"无平移时"的适配原点(与 LayoutCanvas 同一公式)。
			const float designWidth = std::max(m_Document.Design.Resolution.x, 1.0f);
			const float designHeight = std::max(m_Document.Design.Resolution.y, 1.0f);
			const float fit = CanvasFitScale(rect);
			const float scale = std::max(0.02f, fit * m_CanvasZoom);
			const glm::vec2 center {
				rect.X + (rect.W - designWidth * scale) * 0.5f,
				rect.Y + (rect.H - designHeight * scale) * 0.5f };
			m_CanvasPan = input.MousePos - anchorDesign * scale - center;
			m_Status = Wui::Tr("panel.ui_designer.zoom", "Zoom ")
				+ std::to_string(static_cast<int>(m_CanvasZoom * 100.0f)) + "%";
			// 滚轮被画布吃掉:不要让下面的滚动区再滚一次。
			ctx.ConsumePointerClick();
		}

		// 中键拖动 / 空格+左键拖动 = 平移画布。
		const bool panButton = input.MouseDown[2] || (input.MouseDown[0] && input.KeyDown.end() !=
			std::find(input.KeyDown.begin(), input.KeyDown.end(), static_cast<uint32_t>(KeyCodes::Space)));
		if (!m_CanvasPanning && ctx.IsHovered(rect) && panButton)
		{
			m_CanvasPanning = true;
			m_CanvasPanStart = m_CanvasPan;
			m_CanvasPanMouse = input.MousePos;
			CancelDrag();
		}
		if (m_CanvasPanning)
		{
			m_CanvasPan = m_CanvasPanStart + (input.MousePos - m_CanvasPanMouse);
			if (!input.MouseDown[2] && !input.MouseDown[0])
				m_CanvasPanning = false;
			return;   // 平移期间不参与选中/拖动,避免误改文档
		}

		// M12:选中节点的 8 个手柄悬停态(仅反馈用;拖动中不重算)。
		m_HoverHandle = HandleNone;
		Wui::WuiRect selectedBox;
		const bool hasSelectedBox = SelectedNodeBox(selectedBox);
		// M46:锚点预设按钮的命中区(与 DrawAnchorMarkers 共用同一份几何)。点预设不能
		// 被当成"点空白 = 取消选中" —— HandleCanvasInput 在 DrawCanvas **之前**跑,不拦
		// 这一下的话,预设还没画出来选中就被清掉,按钮永远点不中。
		bool overAnchorPreset = false;
		if (hasSelectedBox && ctx.IsHovered(rect))
		{
			for (const Wui::WuiRect& presetRect : LayoutAnchorPresets(selectedBox, rect))
			{
				if (ctx.IsHovered(presetRect))
				{
					overAnchorPreset = true;
					break;
				}
			}
		}
		if (hasSelectedBox && m_Drag == CanvasDrag::None)
		{
			for (const HandleDef& handle : kHandles)
			{
				if (ctx.IsHovered(HandlePhysicalRect(selectedBox, handle)))
				{
					m_HoverHandle = handle.Bits;
					break;
				}
			}
		}

		if (m_Drag == CanvasDrag::None && ctx.IsHovered(rect) && ctx.IsClicked(rect, 0)
			&& !overAnchorPreset)
		{
			// ① 手柄优先:命中 8 手柄之一 → 缩放当前选中节点(不改变选中)。
			UI::UiNode* node = m_HoverHandle != HandleNone ? MutableSelectedNode() : nullptr;
			if (node != nullptr)
			{
				m_Drag = CanvasDrag::Resize;
				m_DragHandle = m_HoverHandle;
				m_DragNodeId = node->Id;
				m_DragStartDesign = m_Viewport.PhysicalToDesign(input.MousePos);
				m_DragStartOffset = node->Anchor.Offset;
				m_DragStartSize = node->Anchor.Size;
				m_DragBefore = m_Document;
				m_DragBeforeValid = true;
			}
			else
			{
				// ② 否则节点命中 = 选中 + 拖动 Offset;命中空白 = 取消选中。
				const UI::UiNodeInstance* hit = m_Screen.HitTest(input.MousePos);
				if (hit != nullptr)
				{
					CommitNodeEdit();
					m_SelectedId = hit->Id;
					if (const UI::UiNode* source = m_Document.FindNode(hit->Id))
					{
						m_Drag = CanvasDrag::Move;
						m_DragHandle = HandleNone;
						m_DragNodeId = hit->Id;
						m_DragStartDesign = m_Viewport.PhysicalToDesign(input.MousePos);
						m_DragStartOffset = source->Anchor.Offset;
						m_DragStartSize = source->Anchor.Size;
						m_DragBefore = m_Document;
						m_DragBeforeValid = true;
					}
				}
				else
				{
					CommitNodeEdit();
					m_SelectedId.clear();
					CancelDrag();
				}
			}
		}

		if (m_Drag != CanvasDrag::None && input.MouseDown[0] && !m_DragNodeId.empty())
		{
			UI::UiNode* node = FindNodeMutable(m_Document.Nodes, m_DragNodeId);
			if (node != nullptr)
			{
				const glm::vec2 design = m_Viewport.PhysicalToDesign(input.MousePos);
				const glm::vec2 delta = design - m_DragStartDesign;
				if (m_Drag == CanvasDrag::Move)
				{
					// M19:吸附到整数设计单位(Alt = 临时关掉,做像素级微调)。
					glm::vec2 next { m_DragStartOffset.x + delta.x, m_DragStartOffset.y + delta.y };
					if (!input.Alt)
						next = SnapDesign(next);
					if (next.x != node->Anchor.Offset.x || next.y != node->Anchor.Offset.y)
					{
						node->Anchor.Offset = next;
						m_ScreenDirty = true;
						m_Status = Wui::Tr("panel.ui_designer.drag_offset", "Offset ")
							+ FormatFloat(next.x) + ", " + FormatFloat(next.y);
					}
				}
				else
				{
					// 手柄语义:拖哪条边就动哪条边,**对边钉住不动**(与主流 UI 编辑器一致)。
					//
					// 两种锚定模式下手柄必须做的事不同(M19 修的真 bug:此前只改 Size,
					// 于是拖左边/上边时对边跟着跑;M42 补齐"拖右/下"的对称补偿 —— 此前
					// 点锚定下拖右边会两边一起动):
					//
					//   点锚定(Min == Max):矩形完全由 Offset/Size 决定
					//     edgeLow0  = Offset.x - Pivot.x*W          (拖前左边缘)
					//     edgeHigh0 = Offset.x + (1-Pivot.x)*W      (拖前右边缘)
					//     拖左: Offset.x = edgeHigh0 - (1-Pivot.x)*size.x   (右边缘钉住)
					//     拖右: Offset.x = edgeLow0  + Pivot.x*size.x       (左边缘钉住)
					//   拉伸(Min != Max):Offset.x = offsetMin,Size.x 是相对锚框跨度的**增量**
					//     edgeLow0  = Offset.x                      (拖前左边缘)
					//     edgeHigh0 = Offset.x + Size.x             (拖前右边缘;同一常量跨度下比较)
					//     拖左: Offset.x = edgeHigh0 - size.x       (右边缘钉住)
					//     拖右: Offset.x 不变,只改 Size.x           (左边缘天然钉住)
					//
					// 两种模式的差异只在"钉住对边要反推什么":点锚定反推 Pivot 造成的那段位移,
					// 拉伸反推的是尺寸增量。**分开写清** —— 同一个量在两种模式下不是同一个意思
					// (此前 `right0 = Offset.x + Size.x` 在拉伸分支代数上恰好成立,但它不是右边缘)。
					//
					// Y 轴同理(屏幕 Y 向下,"上边"= Y 变小)。
					const bool pointX = node->Anchor.Min.x == node->Anchor.Max.x;
					const bool pointY = node->Anchor.Min.y == node->Anchor.Max.y;
					glm::vec2 size = m_DragStartSize;
					glm::vec2 offset = m_DragStartOffset;
					if ((m_DragHandle & HandleRight) != 0)
						size.x = m_DragStartSize.x + delta.x;
					if ((m_DragHandle & HandleLeft) != 0)
						size.x = m_DragStartSize.x - delta.x;
					if ((m_DragHandle & HandleBottom) != 0)
						size.y = m_DragStartSize.y + delta.y;
					if ((m_DragHandle & HandleTop) != 0)
						size.y = m_DragStartSize.y - delta.y;

					const bool corner = (m_DragHandle & (HandleLeft | HandleRight)) != 0
						&& (m_DragHandle & (HandleTop | HandleBottom)) != 0;
					if (corner && input.Shift && m_DragStartSize.x != 0.0f && m_DragStartSize.y != 0.0f)
					{
						// Shift = 等比:以变化更大的轴为准缩放两轴。
						const float sx = size.x / m_DragStartSize.x;
						const float sy = size.y / m_DragStartSize.y;
						const float scale = std::abs(sx) > std::abs(sy) ? sx : sy;
						size.x = m_DragStartSize.x * scale;
						size.y = m_DragStartSize.y * scale;
					}
					// 点锚定时 Size 就是实际尺寸 ⇒ 钳到 >= 0;拉伸时 Size 是尺寸增量,允许为负。
					if (pointX && size.x < 0.0f)
						size.x = 0.0f;
					if (pointY && size.y < 0.0f)
						size.y = 0.0f;

					// M19:尺寸也吸附到整数设计单位(Alt 关掉)。
					if (!input.Alt)
						size = SnapDesign(size);

					// 拖某条边时反推 Offset,使**对边**在任意钳位之后仍然钉住
					// (钳位改了 size,这里按钳后的 size 重算 ⇒ 缩到 0 时被拖的边正好停在
					//  对边上,而不是把整框推走)。
					if ((m_DragHandle & HandleLeft) != 0)
					{
						const float edgeHigh0 = pointX
							? m_DragStartOffset.x + (1.0f - node->Anchor.Pivot.x) * m_DragStartSize.x
							: m_DragStartOffset.x + m_DragStartSize.x;
						offset.x = pointX
							? edgeHigh0 - (1.0f - node->Anchor.Pivot.x) * size.x
							: edgeHigh0 - size.x;
					}
					if ((m_DragHandle & HandleRight) != 0 && pointX)
					{
						// 点锚定:拖右 = 钉住左边缘 ⇒ 反推 Pivot.x*size.x 那段位移。
						// (拉伸模式 Offset.x 保持不动 —— 左边缘天然钉住,只改 Size.x。)
						const float edgeLow0 =
							m_DragStartOffset.x - node->Anchor.Pivot.x * m_DragStartSize.x;
						offset.x = edgeLow0 + node->Anchor.Pivot.x * size.x;
					}
					if ((m_DragHandle & HandleTop) != 0)
					{
						const float edgeHigh0 = pointY
							? m_DragStartOffset.y + (1.0f - node->Anchor.Pivot.y) * m_DragStartSize.y
							: m_DragStartOffset.y + m_DragStartSize.y;
						offset.y = pointY
							? edgeHigh0 - (1.0f - node->Anchor.Pivot.y) * size.y
							: edgeHigh0 - size.y;
					}
					if ((m_DragHandle & HandleBottom) != 0 && pointY)
					{
						// 点锚定:拖下 = 钉住上边缘 ⇒ 反推 Pivot.y*size.y 那段位移。
						// (拉伸模式 Offset.y 保持不动 —— 上边缘天然钉住,只改 Size.y。)
						const float edgeLow0 =
							m_DragStartOffset.y - node->Anchor.Pivot.y * m_DragStartSize.y;
						offset.y = edgeLow0 + node->Anchor.Pivot.y * size.y;
					}

					if (size.x != node->Anchor.Size.x || size.y != node->Anchor.Size.y ||
						offset.x != node->Anchor.Offset.x || offset.y != node->Anchor.Offset.y)
					{
						node->Anchor.Size = size;
						node->Anchor.Offset = offset;
						m_ScreenDirty = true;
						m_Status = Wui::Tr("panel.ui_designer.drag_size", "Size ")
							+ FormatFloat(size.x) + ", " + FormatFloat(size.y);
					}
				}
			}
		}

		if (m_Drag != CanvasDrag::None && !input.MouseDown[0])
		{
			// 一次拖动 = 一条记录:只有真的改动了文档才入栈(单击选中不入栈)。
			if (m_DragBeforeValid && !UI::UiDocumentsEquivalent(m_DragBefore, m_Document))
			{
				const bool resize = m_Drag == CanvasDrag::Resize;
				PushDocumentUndo(resize
					? Wui::Tr("panel.ui_designer.resize", "Resize")
					: Wui::Tr("panel.ui_designer.move", "Move"),
					m_DragBefore);
				m_Dirty = true;
			}
			CancelDrag();
		}
	}

	void UiDesignerPanel::DrawCanvas(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const float designWidth = std::max(m_Document.Design.Resolution.x, 1.0f);
		const float designHeight = std::max(m_Document.Design.Resolution.y, 1.0f);

		const Wui::WuiRect designRect = m_Viewport.DesignRectToPhysical(
			Wui::WuiRect { 0.0f, 0.0f, designWidth, designHeight });
		Wui::PanelBackground(ctx, designRect, theme.ContentBg, 0.0f);
		Wui::HighlightOutline(ctx, designRect, theme.Border, 0.0f, 1.0f);

		// ---- M37:画布所见即运行所得 ----
		// 直接走运行时**同一个** `UiPainter::Paint`:同一份控件绘制实现、同一套主题令牌、
		// 同一套逐状态颜色 —— 画布上看到的 = 运行起来会看到的(不再是复刻的线框)。
		// 映射由 `viewport.DesignRectToPhysical` 完成,而画布视口就是 m_Screen 的视口
		// (见 LayoutCanvas),所以命令天然落在画布上,缩放/平移自动跟随。
		// `RegisterAccessibility=false`:设计器自己登记自己的控件 a11y,不能把一屏
		// 游戏 UI 节点混进编辑器无障碍树(否则 AI 通道按 label 寻址会撞名)。
		UI::UiPaintOptions paintOptions;
		paintOptions.RegisterAccessibility = false;
		const UI::UiPaintResult paint = UI::UiPainter::Paint(ctx, m_Screen, paintOptions);

		// M12:拖动中(移动/缩放)选中框换成警示色,给"正在拖动"的反馈。
		const bool dragging = m_Drag != CanvasDrag::None;

		// M35 的"文本/色块回退"保留为 **Paint 报错(未知 Type)时的兜底**:未知类型的节点
		// Paint 会跳过(绝不静默画空气),设计器在这里给它画回框 + 类型标签 + 红描边,
		// 并在画布角落给一行红字(见本函数末尾)。已知类型一律是 Paint 的真实外观 ——
		// 再画线框只会盖住控件。
		std::size_t fallbackNodes = 0;
		if (!paint.Ok())
		{
			for (const UI::UiNodeInstance& node : m_Screen.Nodes())
			{
				if (UI::UiNodeRegistry::Find(node.Type) != nullptr)
					continue;
				++fallbackNodes;
				const Wui::WuiRect box = NodeBoxPhysical(m_Viewport, node.Rect);
				Wui::WuiColor fill;
				if (CanvasNodeColor(node, "bg", "bg.default", fill))
					Wui::PanelBackground(ctx, box, fill, 0.0f);
				else
					Wui::PanelBackground(ctx, box, theme.Selection, 0.0f);
				Wui::HighlightOutline(ctx, box, theme.Danger, 0.0f, 2.0f);
				Wui::WuiColor textColor;
				if (!CanvasNodeColor(node, "color", "text.default", textColor))
					textColor = theme.Danger;
				std::string caption = CanvasNodeText(node);
				if (caption.empty())
					caption = node.Type + "  " + node.Id;
				Wui::Label(ctx, glm::vec2 { box.X + 3.0f, box.Y + 2.0f }, caption,
					textColor, CanvasNodeFontSize(node, theme.FontSizeCaption));
			}
		}

		// 选中高亮画在 Paint **之后**(真实控件之上):主选中 2px 强调色,其余加选项 1px。
		for (const UI::UiNodeInstance& node : m_Screen.Nodes())
		{
			if (!IsSelected(node.Id))
				continue;
			const bool primary = node.Id == m_SelectedId;
			const Wui::WuiColor outline = primary
				? (dragging ? theme.Warning : theme.Accent) : theme.Accent;
			Wui::HighlightOutline(ctx, NodeBoxPhysical(m_Viewport, node.Rect), outline,
				0.0f, primary ? 2.0f : 1.0f);
		}

		// M12:选中节点的 8 个缩放手柄(拖角改两轴、拖边改单轴;拖动手柄描边用强调色)。
		Wui::WuiRect selectedBox;
		if (SelectedNodeBox(selectedBox))
			DrawResizeHandles(ctx, theme, selectedBox);

		// M19:拖动/缩放中点亮对齐参考线(选中框的边或中心与"父框 / 视口内容矩形"的对应位置对齐时)。
		if (dragging && SelectedNodeBox(selectedBox))
			DrawAlignmentGuides(ctx, theme, selectedBox);

		// 安全区(虚线)与设计面区分。
		const Wui::WuiRect safeRect = m_Viewport.DesignRectToPhysical(m_Viewport.ContentRect);
		DashedRectOutline(ctx, safeRect, theme.Warning, 1.0f);
		Wui::Label(ctx, glm::vec2 { safeRect.X + 4.0f, safeRect.Y + 3.0f },
			Wui::Tr("panel.ui_designer.safe_area", "Safe Area"), theme.Warning, theme.FontSizeCaption);

		// 锚点标记:选中节点优先;没有选中时给根节点画,让"锚点在父矩形上的位置"始终看得见。
		// M46:9 宫格预设只给**选中**节点画(编辑对象明确,也不会一屏 11 个)。
		if (const UI::UiNodeInstance* selected = m_Screen.Find(m_SelectedId))
		{
			DrawAnchorMarkers(ctx, theme, rect, *selected, true);
		}
		else
		{
			for (const UI::UiNodeInstance& node : m_Screen.Nodes())
			{
				if (node.Parent == -1)
					DrawAnchorMarkers(ctx, theme, rect, node, false);
			}
		}

		// M37:Paint 报错(未知 Type / 未解析令牌)不能静默 —— 画布角落一行红字,
		// 带上条数与首条可读说明(令牌错误不阻断绘制,所以这里只是提示,不是"画不出来")。
		if (!paint.Ok())
		{
			std::string message = "Paint: " + std::to_string(paint.Errors.size()) + " error(s)";
			if (fallbackNodes > 0)
				message += " - " + std::to_string(fallbackNodes) + " unknown-type node(s)";
			message += ": " + paint.Errors.front().Message;
			constexpr std::size_t kMaxChars = 150;
			if (message.size() > kMaxChars)
				message = message.substr(0, kMaxChars) + "...";
			const float barWidth = std::min(std::max(rect.W - 12.0f, 0.0f),
				ctx.MeasureTextWidth(message, theme.FontSizeCaption) + 10.0f);
			const Wui::WuiRect bar { rect.X + 6.0f, rect.Y + rect.H - 20.0f, barWidth, 16.0f };
			Wui::PanelBackground(ctx, bar, theme.WindowBg, 2.0f);
			Wui::Label(ctx, glm::vec2 { bar.X + 4.0f, bar.Y + 1.0f }, message,
				theme.Danger, theme.FontSizeCaption);
		}

		Wui::HighlightOutline(ctx, rect, theme.Border, 2.0f, 1.0f);
	}

	void UiDesignerPanel::DrawAnchorMarkers(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& canvasRect, const UI::UiNodeInstance& node, bool allowPresets)
	{
		if (node.Source == nullptr)
			return;

		const Wui::WuiRect parentRect = node.Parent >= 0
			? m_Screen.Nodes()[static_cast<std::size_t>(node.Parent)].Rect
			: m_Viewport.ContentRect;
		const UI::UiAnchor& anchor = node.Source->Anchor;
		const glm::vec2 parentMin { parentRect.X, parentRect.Y };
		const glm::vec2 parentSize { parentRect.W, parentRect.H };

		const glm::vec2 minCanvas = m_Viewport.DesignToPhysical(parentMin + anchor.Min * parentSize);
		const glm::vec2 maxCanvas = m_Viewport.DesignToPhysical(parentMin + anchor.Max * parentSize);

		Wui::LineSegment(ctx, minCanvas, maxCanvas, theme.BorderStrong, 1.0f);
		// M46:拉伸锚定(任一轴 Min != Max)= 两点之间的"锚框"虚线矩形(与连线一起表达跨度)。
		const bool stretchX = anchor.Min.x != anchor.Max.x;
		const bool stretchY = anchor.Min.y != anchor.Max.y;
		if (stretchX || stretchY)
		{
			DashedRectOutline(ctx,
				Wui::WuiRect { std::min(minCanvas.x, maxCanvas.x), std::min(minCanvas.y, maxCanvas.y),
					std::abs(maxCanvas.x - minCanvas.x), std::abs(maxCanvas.y - minCanvas.y) },
				theme.BorderStrong, 1.0f);
		}
		MarkerCross(ctx, minCanvas, theme.Success);
		MarkerCross(ctx, maxCanvas, theme.Warning);
		Wui::PanelBackground(ctx, Wui::WuiRect { minCanvas.x - 2.0f, minCanvas.y - 2.0f, 4.0f, 4.0f },
			theme.Success, 0.0f);
		Wui::PanelBackground(ctx, Wui::WuiRect { maxCanvas.x - 2.0f, maxCanvas.y - 2.0f, 4.0f, 4.0f },
			theme.Warning, 0.0f);

		// M46:Min / Max 语义标签(截断预算 24px;颜色沿用既有令牌:Success = Min、Warning = Max)。
		const float markerFont = theme.FontSizeCaption;
		Wui::Label(ctx, glm::vec2 { minCanvas.x + 5.0f, minCanvas.y - 13.0f },
			Wui::EllipsizeMiddleToWidth(ctx, "Min", 24.0f, markerFont), theme.Success, markerFont);
		Wui::Label(ctx, glm::vec2 { maxCanvas.x + 5.0f, maxCanvas.y - 13.0f },
			Wui::EllipsizeMiddleToWidth(ctx, "Max", 24.0f, markerFont), theme.Warning, markerFont);
		// M46:相对安全区锚定在画布上不可见 ⇒ 十字旁加一个 "Safe" 角标(不给它新颜色)。
		if (anchor.RelativeToSafeArea)
		{
			Wui::Label(ctx, glm::vec2 { minCanvas.x + 5.0f, minCanvas.y + 4.0f }, "Safe",
				theme.Warning, markerFont);
		}

		if (!allowPresets)
			return;

		// M46:9 宫格锚点预设(点选即写 Min/Max,一条撤销记录;不改 Offset/Size/Pivot)。
		// 几何与 HandleCanvasInput 的"点在预设上"判定同源(见 LayoutAnchorPresets 的注释)。
		const std::array<Wui::WuiRect, kAnchorPresetCount> presetRects =
			LayoutAnchorPresets(NodeBoxPhysical(m_Viewport, node.Rect), canvasRect);
		UI::UiNode* target = nullptr;
		const std::string undoName = Wui::Tr("panel.ui_designer.anchor.preset", "Set anchor preset");
		for (std::size_t i = 0; i < kAnchorPresetCount; ++i)
		{
			const AnchorPresetDef& preset = kAnchorPresets[i];
			const Wui::WuiRect& presetRect = presetRects[i];
			if (presetRect.W <= 2.0f || presetRect.H <= 2.0f || !ctx.ClipAllows(presetRect))
				continue;   // 裁掉/滚出的预设不画也不登记(与面板其它滚动区同一口径)
			const std::string idKey = std::string("ui_designer.anchor.preset.") + preset.Id;
			const std::string label = Wui::Tr(preset.Key, preset.Label);
			const std::string tooltip = Wui::Tr(std::string(preset.Key) + ".tip", preset.Tip);
			// 已经等于该预设(只比该预设**固定**了的轴)⇒ 高亮,给"当前是哪一个"的反馈。
			const bool xFixed = !preset.KeepMinX || !preset.KeepMaxX;
			const bool yFixed = !preset.KeepMinY || !preset.KeepMaxY;
			const bool active = (!xFixed
					|| (anchor.Min.x == preset.Min.x && anchor.Max.x == preset.Max.x))
				&& (!yFixed
					|| (anchor.Min.y == preset.Min.y && anchor.Max.y == preset.Max.y));
			const bool hovered = ctx.IsHovered(presetRect);
			// 8x8 小按钮:不画文字(库件 ButtonEx 的标签排版放不下),名字进 a11y label 与 tooltip。
			Wui::PanelBackground(ctx, presetRect,
				active ? theme.Accent : (hovered ? theme.ButtonHover : theme.ButtonBg), 1.0f);
			Wui::HighlightOutline(ctx, presetRect, active ? theme.Accent : theme.Border, 1.0f, 1.0f);
			Wui::WuiAccessNode presetNode;
			presetNode.Id = Wui::HashId(idKey.c_str());
			presetNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			presetNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			presetNode.Kind = "button";
			// M46:标签带前缀 —— 裸 "Top Left" 在 a11y 树里看不出是「锚点预设」,AI 按 label 寻址会歧义。
			presetNode.Label = Wui::Tr("panel.ui_designer.anchor.preset.label_prefix", "Anchor preset: ") + label;
			presetNode.Value = active ? std::string("active") : std::string();
			presetNode.Tooltip = tooltip;
			presetNode.Rect = presetRect;
			presetNode.Enabled = true;
			presetNode.Interactive = true;
			presetNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(presetNode);
			if (hovered && !tooltip.empty())
				Wui::Tooltip(ctx, presetRect, tooltip);
			if (ctx.IsClicked(presetRect, 0))
			{
				if (target == nullptr)
					target = FindNodeMutable(m_Document.Nodes, node.Id);
				if (target != nullptr)
				{
					CommitNodeEdit();               // 收口上一条,让本次点选自己占一条记录
					m_FrameNodeBefore = *target;
					NoteNodeEdit(undoName);
					ApplyAnchorPreset(target->Anchor, preset);
					m_ScreenDirty = true;
					m_Dirty = true;
					CommitNodeEdit();
					m_Status = Wui::Tr("panel.ui_designer.anchor.preset_applied", "Anchor preset: ")
						+ label;
				}
			}
		}
	}

	// ---- M12:撤销/重做(面板本地 WuiUndoStack)----

	void UiDesignerPanel::HandleShortcuts(Wui::WuiContext& ctx)
	{
		if (!m_HasDocument || m_TextFocusLatched)
			return;
		const Wui::WuiInputState& input = ctx.Input();
		if (!input.Ctrl)
		{
			// ---- M19:不带修饰键的常用操作(只在没有文本焦点时生效)----
			// 方向键微调选中节点(Shift = ×10):改的是 Anchor.Offset,与鼠标拖动同一条模型路径。
			if (!m_SelectedId.empty() && m_Drag == CanvasDrag::None)
			{
				const float step = input.Shift ? 10.0f : 1.0f;
				if (ctx.WasKeyTriggered(KeyCodes::Left)) NudgeSelected(0, -step);
				else if (ctx.WasKeyTriggered(KeyCodes::Right)) NudgeSelected(0, step);
				else if (ctx.WasKeyTriggered(KeyCodes::Up)) NudgeSelected(1, -step);
				else if (ctx.WasKeyTriggered(KeyCodes::Down)) NudgeSelected(1, step);
			}
			// Delete/Backspace = 删除选中节点(与工具栏 Delete 同一条守卫)。
			if (ctx.WasKeyTriggered(KeyCodes::Delete) || ctx.WasKeyTriggered(KeyCodes::Backspace))
				DeleteSelectedNode();
			// F = 复位画布缩放/平移(看清全貌)。
			if (ctx.WasKeyTriggered(KeyCodes::F))
			{
				m_CanvasZoom = 1.0f;
				m_CanvasPan = glm::vec2 { 0.0f, 0.0f };
				m_Status = Wui::Tr("panel.ui_designer.view_reset", "Canvas view reset");
			}
			return;
		}
		// Ctrl+Z = 撤销;Ctrl+Y / Ctrl+Shift+Z = 重做(与编辑器其它面板同口径)。
		if (!input.Shift && ctx.WasKeyTriggered(KeyCodes::Z))
			UndoDocument();
		else if (ctx.WasKeyTriggered(KeyCodes::Y) || (input.Shift && ctx.WasKeyTriggered(KeyCodes::Z)))
			RedoDocument();
		// M19:Ctrl+D 复制 / Ctrl+S 保存(与工具栏按钮同一条路径)。
		else if (ctx.WasKeyTriggered(KeyCodes::D))
			DuplicateSelectedNode();
		else if (ctx.WasKeyTriggered(KeyCodes::S))
			m_PendingSave = true;   // M36:同上,统一在帧末落盘(那时缓冲已提交)
	}

	// M19:方向键微调 —— 与鼠标拖动共用"整档前像 + 一条撤销记录"的口径(连续微调合并成一条,
	// 直到指针/按键停下来:这里用 NoteNodeEdit/CommitNodeEdit 的同一套合并机制)。
	void UiDesignerPanel::NudgeSelected(int axis, float amount)
	{
		UI::UiNode* node = MutableSelectedNode();
		if (node == nullptr)
			return;
		glm::vec2 next = node->Anchor.Offset;
		(axis == 0 ? next.x : next.y) += amount;
		if (next == node->Anchor.Offset)
			return;
		NoteNodeEdit(Wui::Tr("panel.ui_designer.nudge", "Nudge"));
		node->Anchor.Offset = next;
		m_ScreenDirty = true;
		m_Status = Wui::Tr("panel.ui_designer.drag_offset", "Offset ")
			+ FormatFloat(next.x) + ", " + FormatFloat(next.y);
	}

	// M19:把移动/缩放的结果吸附到**整数设计单位**(像素级对齐;Alt 或缩放很小时跳过)。
	// 为什么是 1 而不是 8:设计单位通常就是像素,整数对齐能保证描边落在像素网格上;
	// 需要粗网格时按住 Shift(×10 微调)或直接用属性页输入精确值。
	glm::vec2 UiDesignerPanel::SnapDesign(glm::vec2 value) const
	{
		if (m_CanvasZoom < 0.6f)
			return value;   // 缩得太小,1 单位的吸附感会变成"跟不上鼠标"
		return glm::vec2 { std::round(value.x), std::round(value.y) };
	}

	void UiDesignerPanel::UndoDocument()
	{
		if (!m_Undo.CanUndo())
		{
			m_Status = Wui::Tr("panel.ui_designer.undo_empty", "Nothing to undo");
			return;
		}
		const std::string name = m_Undo.UndoName();
		if (m_Undo.Undo())
			m_Status = Wui::Tr("panel.ui_designer.undone", "Undo: ") + name;
	}

	void UiDesignerPanel::RedoDocument()
	{
		if (!m_Undo.CanRedo())
		{
			m_Status = Wui::Tr("panel.ui_designer.redo_empty", "Nothing to redo");
			return;
		}
		const std::string name = m_Undo.RedoName();
		if (m_Undo.Redo())
			m_Status = Wui::Tr("panel.ui_designer.redone", "Redo: ") + name;
	}

	void UiDesignerPanel::PushDocumentUndo(const std::string& name, const UI::UiDocument& before)
	{
		// 一条记录 = 前像 + 记录时的后像(整档)。面板本地,不往 WuiContext 加全局状态。
		UI::UiDocument after = m_Document;
		m_Undo.Push(name,
			[this, before]() { RestoreDocument(before); },
			[this, after]() { RestoreDocument(after); });
	}

	void UiDesignerPanel::RestoreDocument(const UI::UiDocument& document)
	{
		CancelDrag();
		CancelNodeEdit();
		CancelOutlineRename();   // M47:撤销/重做会把被改名的行换掉,编辑态一并收口
		m_Document = document;
		m_ScreenDirty = true;
		m_Dirty = true;
		m_BufferNodeId.clear();   // 属性文本缓冲按新模型重建
		if (!m_SelectedId.empty() && m_Document.FindNode(m_SelectedId) == nullptr)
			m_SelectedId.clear();
	}

	void UiDesignerPanel::NoteNodeEdit(const std::string& name, bool batchEdit)
	{
		if (m_PendingEditValid)
			return;   // 一次连续编辑只记第一条(拖动 = 一条记录)
		// 前像必须是"本次改动之前"的同一节点快照;对不上(不该发生)宁可不记,
		// 也不能拿一份空/别的节点当撤销前像(会把节点写坏)。
		if (m_FrameNodeBefore.Id != m_SelectedId)
			return;
		m_PendingEditValid = true;
		m_PendingEditNodeId = m_SelectedId;
		m_PendingEditBefore = m_FrameNodeBefore;
		m_PendingEditName = name;
		// M37:批量编辑(多选同 Type)一条撤销记录 = **整档前像** —— 单节点前像盖不住
		// 同时被改的其它选中节点,撤销会把它们留在改后的值上。此刻改动还没写进文档,
		// 所以这份快照就是"本次改动之前"的权威前像。
		m_PendingEditBatch = batchEdit;
		if (batchEdit)
			m_PendingEditBeforeDoc = m_Document;
	}

	void UiDesignerPanel::CommitNodeEdit()
	{
		if (!m_PendingEditValid)
			return;
		const std::string nodeId = m_PendingEditNodeId;
		const std::string name = m_PendingEditName;
		const UI::UiNode before = m_PendingEditBefore;
		// M37:批量前像要先取出来 —— CancelNodeEdit 会把它们清掉(见下)。
		const bool batchEdit = m_PendingEditBatch;
		UI::UiDocument batchBefore;
		if (batchEdit)
			batchBefore = m_PendingEditBeforeDoc;
		CancelNodeEdit();
		if (nodeId.empty())
			return;
		const UI::UiNode* current = m_Document.FindNode(nodeId);
		if (current == nullptr)
			return;
		// 后像 = 当前文档(节点就是 current);前像 = 当前文档把该节点换回 before。
		// 拖回原值 / 未真正改动时不落空记录。
		const UI::UiDocument after = m_Document;
		UI::UiDocument beforeDoc;
		if (batchEdit)
		{
			beforeDoc = batchBefore;
		}
		else
		{
			beforeDoc = m_Document;
			if (UI::UiNode* slot = FindNodeMutable(beforeDoc.Nodes, nodeId))
				*slot = before;
		}
		if (UI::UiDocumentsEquivalent(beforeDoc, after))
			return;
		PushDocumentUndo(name.empty() ? Wui::Tr("panel.ui_designer.edit", "Edit") : name, beforeDoc);
		m_Dirty = true;
	}

	void UiDesignerPanel::CancelNodeEdit()
	{
		m_PendingEditValid = false;
		m_PendingEditNodeId.clear();
		m_PendingEditName.clear();
		m_PendingEditBatch = false;
		m_PendingEditBeforeDoc = UI::UiDocument {};
	}

	void UiDesignerPanel::CancelDrag()
	{
		m_Drag = CanvasDrag::None;
		m_DragHandle = HandleNone;
		m_HoverHandle = HandleNone;
		m_DragNodeId.clear();
		m_DragBeforeValid = false;
	}

	// ---- M12:同父内上移/下移(改 Children 顺序 = 改绘制顺序)----

	// ---- M20:多选 ----
	// `m_SelectedId` 是"主选中"(属性页/手柄/方向键作用在它上);`m_SelectedIds` 是额外的加选项。
	// 删除/复制作用在整组(见 EffectiveSelection)。M37 起:同 Type 的多选支持**成套属性编辑**
	// (属性行同时写全部选中节点),混选只改主选中并在 Type 块头给可见提示。
	bool UiDesignerPanel::IsSelected(const std::string& nodeId) const
	{
		if (nodeId == m_SelectedId)
			return true;
		return std::find(m_SelectedIds.begin(), m_SelectedIds.end(), nodeId) != m_SelectedIds.end();
	}

	void UiDesignerPanel::ToggleSelected(const std::string& nodeId)
	{
		const auto found = std::find(m_SelectedIds.begin(), m_SelectedIds.end(), nodeId);
		if (found != m_SelectedIds.end())
		{
			m_SelectedIds.erase(found);
			return;
		}
		if (nodeId == m_SelectedId)
			return;   // 主选中不能被"加选"变成重复项
		m_SelectedIds.push_back(nodeId);
	}

	void UiDesignerPanel::ClearSelection()
	{
		m_SelectedId.clear();
		m_SelectedIds.clear();
	}

	std::vector<std::string> UiDesignerPanel::EffectiveSelection() const
	{
		std::vector<std::string> ids;
		if (!m_SelectedId.empty())
			ids.push_back(m_SelectedId);
		for (const std::string& id : m_SelectedIds)
		{
			if (m_Document.FindNode(id) != nullptr)
				ids.push_back(id);
		}
		return ids;
	}

	// M37:多选批量改属性 —— 选中的节点(主选中在前,顺序 = 大纲加选顺序)。
	std::vector<UI::UiNode*> UiDesignerPanel::SelectedNodesMutable()
	{
		std::vector<UI::UiNode*> nodes;
		for (const std::string& id : EffectiveSelection())
		{
			if (UI::UiNode* node = FindNodeMutable(m_Document.Nodes, id))
				nodes.push_back(node);
		}
		return nodes;
	}

	// M37:选中的 Type 是否不一致(混选)。选中 ≤1 个 / 节点不存在 = false。
	bool UiDesignerPanel::SelectionMixedTypes() const
	{
		std::string firstType;
		bool haveFirst = false;
		for (const std::string& id : EffectiveSelection())
		{
			const UI::UiNode* node = m_Document.FindNode(id);
			if (node == nullptr)
				continue;
			if (!haveFirst)
			{
				firstType = node->Type;
				haveFirst = true;
				continue;
			}
			if (node->Type != firstType)
				return true;
		}
		return false;
	}

	// M37:选中 >1 且全同 Type = 属性行批量写全部选中节点(否则只写主选中)。
	bool UiDesignerPanel::BatchEditActive() const
	{
		return EffectiveSelection().size() > 1 && !SelectionMixedTypes();
	}

	// M20:大纲拖动 = 重新挂父。守卫与"上移/下移"同一口径:
	//   * 不能挂到自己或自己的后代(会成环);
	//   * 目标父节点必须存在;
	//   * 已经在该父下 ⇒ 什么都不做(不入撤销栈)。
	void UiDesignerPanel::ReparentSelected(const std::string& newParentId)
	{
		if (m_SelectedId.empty() || newParentId.empty() || m_SelectedId == newParentId)
			return;
		if (FindNodeMutable(m_Document.Nodes, newParentId) == nullptr)
			return;

		// 环检测:新父是"被拖节点的后代"时就拒绝(否则树会被切断成不可达的循环)。
		if (const UI::UiNode* dragged = m_Document.FindNode(m_SelectedId))
		{
			bool isDescendant = false;
			std::function<void(const UI::UiNode&)> scan = [&](const UI::UiNode& node)
			{
				for (const UI::UiNode& child : node.Children)
				{
					if (child.Id == newParentId)
						isDescendant = true;
					scan(child);
				}
			};
			scan(*dragged);
			if (isDescendant)
			{
				m_Status = Wui::Tr("panel.ui_designer.reparent_cycle",
					"Cannot drop a node onto its own descendant");
				return;
			}
		}

		// 已经是该父的子节点 ⇒ 无操作(不产生撤销记录)。
		if (const UI::UiNode* parent = m_Document.FindNode(newParentId))
		{
			const bool alreadyChild = std::any_of(parent->Children.begin(), parent->Children.end(),
				[this](const UI::UiNode& child) { return child.Id == m_SelectedId; });
			if (alreadyChild)
				return;
		}

		UI::UiNode copy;
		{
			const UI::UiNode* dragged = m_Document.FindNode(m_SelectedId);
			if (dragged == nullptr)
				return;
			copy = *dragged;
		}

		const UI::UiDocument before = m_Document;
		// 从原位置摘掉(根或某个 Children)。
		NodeLocation location;
		if (LocateNode(m_Document.Nodes, m_SelectedId, std::string(), location) && location.List != nullptr)
			location.List->erase(location.List->begin() + static_cast<std::ptrdiff_t>(location.Index));
		// 挂到新父下(重新取指针:上面的 erase 可能让之前的指针失效)。
		UI::UiNode* target = FindNodeMutable(m_Document.Nodes, newParentId);
		if (target == nullptr)
		{
			m_Document = before;   // 极端情况:回滚,不留下"节点消失"的中间态
			return;
		}
		target->Children.push_back(std::move(copy));
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(Wui::Tr("panel.ui_designer.reparent", "Reparent"), before);
		m_Status = Wui::Tr("panel.ui_designer.reparented", "Moved under ") + newParentId;
	}

	bool UiDesignerPanel::CanMoveSelected(int direction) const
	{
		std::size_t index = 0;
		std::size_t count = 0;
		if (m_SelectedId.empty() || !LocateSiblingIndex(m_Document.Nodes, m_SelectedId, &index, &count))
			return false;
		return direction < 0 ? index > 0 : index + 1 < count;
	}

	// M29:改层级 —— 缩进 = 成为**前一个兄弟**的最后一个子节点。
	// 没有前一个兄弟(已经是第一个)就没有可挂的父 ⇒ 不可用。
	bool UiDesignerPanel::CanIndentSelected() const
	{
		std::size_t index = 0;
		std::size_t count = 0;
		if (m_SelectedId.empty() || !LocateSiblingIndex(m_Document.Nodes, m_SelectedId, &index, &count))
			return false;
		return index > 0;
	}

	// M29:提升 = 成为**父节点的下一个兄弟**;已经在根上(没有父)⇒ 不可用。
	bool UiDesignerPanel::CanOutdentSelected() const
	{
		if (m_SelectedId.empty() || !m_HasDocument)
			return false;
		// 根节点没有父,提升不了。
		std::size_t rootIndex = 0;
		if (LocateRootIndex(m_Document.Nodes, m_SelectedId, &rootIndex))
			return false;
		return m_Document.FindNode(m_SelectedId) != nullptr;
	}

	void UiDesignerPanel::IndentSelectedNode()
	{
		if (!CanIndentSelected())
		{
			m_Status = Wui::Tr("panel.ui_designer.indent_blocked",
				"Indent needs a previous sibling to become the new parent");
			return;
		}
		// 前一个兄弟(同父同层)的 id:把选中节点挂到它下面。
		std::vector<UI::UiNode>* list = nullptr;
		NodeLocation location;
		if (!LocateNode(m_Document.Nodes, m_SelectedId, std::string(), location)
			|| location.List == nullptr || location.Index == 0)
			return;
		list = location.List;
		const std::size_t index = location.Index;
		const std::string newParent = (*list)[index - 1].Id;
		ReparentSelected(newParent);
	}

	void UiDesignerPanel::OutdentSelectedNode()
	{
		if (!CanOutdentSelected())
		{
			m_Status = Wui::Tr("panel.ui_designer.outdent_blocked",
				"Outdent needs a parent node (root nodes cannot be outdented)");
			return;
		}
		// 找父节点:遍历时记录"谁的 Children 里有它"。
		UI::UiNode* parent = nullptr;
		std::function<void(std::vector<UI::UiNode>&)> findParent = [&](std::vector<UI::UiNode>& nodes)
		{
			for (UI::UiNode& node : nodes)
			{
				if (parent != nullptr)
					return;
				const bool isParent = std::any_of(node.Children.begin(), node.Children.end(),
					[this](const UI::UiNode& child) { return child.Id == m_SelectedId; });
				if (isParent)
				{
					parent = &node;
					return;
				}
				findParent(node.Children);
			}
		};
		findParent(m_Document.Nodes);
		if (parent == nullptr)
			return;
		const std::string parentId = parent->Id;

		// 提升 = 插到父节点**之后**(同层),所以先记下父的父与其下标。
		std::vector<UI::UiNode>* grandList = &m_Document.Nodes;
		std::size_t parentIndex = 0;
		{
			NodeLocation parentLocation;
			if (LocateNode(m_Document.Nodes, parentId, std::string(), parentLocation)
				&& parentLocation.List != nullptr)
			{
				grandList = parentLocation.List;
				parentIndex = parentLocation.Index;
			}
		}

		const UI::UiDocument before = m_Document;
		UI::UiNode moved;
		{
			const UI::UiNode* source = m_Document.FindNode(m_SelectedId);
			if (source == nullptr)
				return;
			moved = *source;
		}
		NodeLocation location;
		if (!LocateNode(m_Document.Nodes, m_SelectedId, std::string(), location)
			|| location.List == nullptr)
			return;
		location.List->erase(location.List->begin() + static_cast<std::ptrdiff_t>(location.Index));
		grandList->insert(grandList->begin() + static_cast<std::ptrdiff_t>(parentIndex + 1),
			std::move(moved));
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(Wui::Tr("panel.ui_designer.outdent", "Outdent"), before);
		m_Status = Wui::Tr("panel.ui_designer.outdented", "Outdented (moved up one level)");
	}

	void UiDesignerPanel::MoveSelectedNode(int direction)
	{
		if (m_SelectedId.empty())
			return;
		std::vector<UI::UiNode>* list = nullptr;
		std::size_t index = 0;
		if (!LocateSibling(m_Document.Nodes, m_SelectedId, &list, &index) || list == nullptr)
			return;
		if (direction < 0 && index == 0)
			return;
		const std::size_t target = direction < 0 ? index - 1 : index + 1;
		if (target >= list->size())
			return;

		CommitNodeEdit();
		const UI::UiDocument before = m_Document;
		std::swap((*list)[index], (*list)[target]);
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(direction < 0
			? Wui::Tr("panel.ui_designer.move_up", "Move Up")
			: Wui::Tr("panel.ui_designer.move_down", "Move Down"), before);
		m_Status = direction < 0
			? Wui::Tr("panel.ui_designer.moved_up", "Moved node up (earlier in draw order)")
			: Wui::Tr("panel.ui_designer.moved_down", "Moved node down (later in draw order)");
	}

	// ---- M16:空态文档浏览 / 节点增删复制 ----

	void UiDesignerPanel::ScanRecentDocuments()
	{
		m_RecentScanned = true;
		m_RecentDocuments.clear();
		m_RecentScroll = 0.0f;

		std::error_code error;
		std::filesystem::path root = ContentRoot();
		if (root.empty())
		{
			root = std::filesystem::current_path(error);
			error.clear();
		}
		if (root.empty())
			return;

		// 优先扫 <内容根>/ui;这个目录不存在才退回 <内容根> 全树(带访问量上限,
		// 无内容根时不会把 build 目录整棵扫穿)。
		//
		// 口径:`ContentRoot()` = `Paths::AssetRoot()` = 项目的**内容根**(即 `assets/`),
		// 所以 UI 目录是 `<内容根>/ui`,不是 `<内容根>/assets/ui`(后者会拼成 assets/assets)。
		const std::filesystem::path uiDirectory = root / "ui";
		const std::filesystem::path scanRoot =
			std::filesystem::is_directory(uiDirectory, error) && !error ? uiDirectory : root;

		constexpr std::size_t kRecentLimit = 50;
		constexpr std::size_t kCollectLimit = 200;
		constexpr std::size_t kVisitLimit = 20000;

		std::vector<std::filesystem::path> found;
		std::size_t visited = 0;
		std::filesystem::recursive_directory_iterator iterator(scanRoot,
			std::filesystem::directory_options::skip_permission_denied, error);
		const std::filesystem::recursive_directory_iterator finish;
		while (iterator != finish && visited < kVisitLimit && found.size() < kCollectLimit)
		{
			++visited;
			std::error_code fileError;
			if (iterator->is_regular_file(fileError) && !fileError)
			{
				std::string extension = iterator->path().extension().string();
				std::transform(extension.begin(), extension.end(), extension.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (extension == UI::kUiDocumentExtension)
					found.push_back(iterator->path());
			}
			iterator.increment(error);
			if (error)
				break;   // 遍历出错:拿到已扫到的就够(不抛异常)
		}

		std::sort(found.begin(), found.end());
		if (found.size() > kRecentLimit)
			found.resize(kRecentLimit);

		for (const std::filesystem::path& absolute : found)
		{
			std::error_code relativeError;
			const std::filesystem::path relative = std::filesystem::relative(absolute, root, relativeError);
			RecentDocument entry;
			entry.Path = absolute;
			entry.RelativePath = (relativeError ? absolute.filename() : relative).generic_string();
			m_RecentDocuments.push_back(std::move(entry));
		}
	}

	void UiDesignerPanel::RenderDocumentBrowser(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		if (!m_RecentScanned)
			ScanRecentDocuments();
		// M19:空态列表在面板打开期间也可能变(从内容浏览器新导出一份 .wui)——
		// 每 ~2 秒(120 帧)重扫一次,否则用户要关掉面板再打开才看得到。
		else if (ctx.Frame() >= m_RecentScanFrame + 120)
		{
			m_RecentScanFrame = ctx.Frame();
			ScanRecentDocuments();
		}

		const bool hasFiles = !m_RecentDocuments.empty();
		const std::string title = Wui::Tr("panel.ui_designer.no_document", "No UI document open");
		const std::string hint = hasFiles
			? Wui::Tr("panel.ui_designer.no_document_hint_browse",
				"Pick a .wui found in the content root below, or press New to start a blank one.")
			: Wui::Tr("panel.ui_designer.no_document_hint_none",
				"No .wui found under the content root - press New to create one.");

		const float headerHeight = std::min(std::max(96.0f, rect.H * 0.38f), rect.H);
		Wui::EmptyState(ctx, Wui::WuiRect { rect.X, rect.Y, rect.W, headerHeight }, std::string(),
			title, hint, std::string(), 0, theme);

		if (!hasFiles)
			return;

		const Wui::WuiRect listRect { rect.X + 10.0f, rect.Y + headerHeight + 4.0f,
			std::max(0.0f, rect.W - 20.0f), std::max(0.0f, rect.H - headerHeight - 14.0f) };
		if (!(listRect.H > 8.0f) || !(listRect.W > 40.0f))
			return;

		constexpr float rowHeight = 24.0f;
		constexpr float rowGap = 2.0f;
		const float contentHeight = static_cast<float>(m_RecentDocuments.size()) * (rowHeight + rowGap);
		Wui::BeginScrollArea(ctx, listRect, contentHeight, m_RecentScroll, theme,
			Wui::HashId("ui_designer.recent.scroll"));
		float rowY = listRect.Y - m_RecentScroll;
		for (std::size_t index = 0; index < m_RecentDocuments.size(); ++index)
		{
			const RecentDocument& entry = m_RecentDocuments[index];
			const Wui::WuiRect row { listRect.X, rowY, listRect.W, rowHeight };
			// a11y id 形如 ui_designer.recent.<n>(n = 按路径排序后的下标);按钮文案带相对路径。
			const Wui::WuiId id = Wui::HashId(("ui_designer.recent." + std::to_string(index)).c_str());
			const std::string label = Wui::Tr("panel.ui_designer.open", "Open") + " " + entry.RelativePath;
			if (Wui::ActionButton(ctx, id, row, label, theme, true,
				Wui::Tr("panel.ui_designer.recent.tip", "Load this .wui document")))
			{
				LoadFrom(entry.Path);
			}
			rowY += rowHeight + rowGap;
		}
		Wui::EndScrollArea(ctx);
	}

	bool UiDesignerPanel::CreateNewDocument()
	{
		std::error_code error;
		// M19(安全):`New` 只在"路径框为空 / 指向的文件不存在"时才用它。
		// 原实现无条件采用路径框现值 —— 先 Open 一份文档再点 New,就会以同一路径建出空文档,
		// 接着 Save 直接覆盖原文件(实测踩到的数据丢失路径)。
		std::filesystem::path target = ResolveInputPath(m_PathBuffer);
		if (!target.empty() && std::filesystem::exists(target, error))
		{
			m_Status = Wui::Tr("panel.ui_designer.new_refused",
				"New refuses to reuse an existing file — clear the path box or pick another name");
			return false;
		}
		if (target.empty())
		{
			// 路径框为空:落 <内容根>/ui/untitled.wui;已存在则加 -1、-2 后缀。
			// (`<内容根>` 已经是项目的 `assets/`;再加一层 `assets/` 会拼成 assets/assets。)
			std::filesystem::path root = ContentRoot();
			if (root.empty())
				root = std::filesystem::current_path(error);
			if (root.empty())
			{
				m_Status = Wui::Tr("panel.ui_designer.new_no_root",
					"Cannot resolve a content root for the new document");
				return false;
			}
			const std::filesystem::path directory = root / "ui";
			target = directory / "untitled.wui";
			for (int suffix = 1; suffix < 10000 && std::filesystem::exists(target, error); ++suffix)
				target = directory / ("untitled-" + std::to_string(suffix) + ".wui");
		}
		else if (!target.has_extension())
		{
			target += UI::kUiDocumentExtension;
		}

		// 最小可用文档:Panel 根 1920x1080 铺满 + 一个 Label 子节点。
		UI::UiDocument document;
		std::string screenName;
		for (const char c : target.stem().string())
		{
			if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-')
				screenName.push_back(c);
		}
		document.Screen = screenName.empty() ? std::string("Screen") : screenName;
		document.Design.Resolution = glm::vec2 { 1920.0f, 1080.0f };

		UI::UiNode root;
		root.Id = "root";
		root.Type = "Panel";
		root.Anchor.Min = glm::vec2 { 0.0f, 0.0f };
		root.Anchor.Max = glm::vec2 { 1.0f, 1.0f };
		root.Anchor.Pivot = glm::vec2 { 0.5f, 0.5f };
		root.Anchor.Offset = glm::vec2 { 0.0f, 0.0f };
		root.Anchor.Size = glm::vec2 { 0.0f, 0.0f };

		UI::UiNode label;
		label.Type = "Label";
		label.Id = UI::MakeStableId(root.Id, label.Type, 0);
		label.Props.push_back(UI::UiProp { "text", "Label" });
		label.Anchor.Min = glm::vec2 { 0.0f, 0.0f };
		label.Anchor.Max = glm::vec2 { 0.0f, 0.0f };
		label.Anchor.Pivot = glm::vec2 { 0.0f, 0.0f };
		label.Anchor.Offset = glm::vec2 { 40.0f, 40.0f };
		label.Anchor.Size = glm::vec2 { 240.0f, 40.0f };
		root.Children.push_back(std::move(label));
		document.Nodes.push_back(std::move(root));

		m_Document = std::move(document);
		m_Path = target;
		m_PathBuffer = target.string();
		// 根节点默认选中:画布上立刻有选择框/手柄,属性栏也不再是空的。
		m_SelectedId = m_Document.Nodes.empty() ? std::string() : m_Document.Nodes.front().Id;
		m_BufferNodeId.clear();
		m_Undo.Clear();          // 换文档 = 撤销历史作废(与 LoadFrom 同口径)
		CancelDrag();
		CancelNodeEdit();
		CancelOutlineRename();
		m_Collapsed.clear();
		m_OutlineScroll = 0.0f;
		m_PropertyScroll = 0.0f;
		m_RecentScroll = 0.0f;
		m_HasDocument = true;
		m_ScreenDirty = true;
		m_Dirty = true;          // 还没落盘:状态行显示未保存,Save 按钮可用
		RebuildScreen();
		m_Status = Wui::Tr("panel.ui_designer.new_unsaved", "New (unsaved): ")
			+ target.filename().string()
			+ Wui::Tr("panel.ui_designer.new_unsaved_hint", " - press Save to write it");
		return true;
	}

	bool UiDesignerPanel::AddNode(const std::string& type)
	{
		if (type.empty())
			return false;
		if (!m_HasDocument)
		{
			m_Status = Wui::Tr("panel.ui_designer.need_document",
				"Open or create a document first (New)");
			return false;
		}
		const UI::UiNodeTypeDesc* typeDesc = UI::UiNodeRegistry::Find(type);
		if (typeDesc == nullptr)
		{
			m_Status = Wui::Tr("panel.ui_designer.unknown_type", "Unknown node type: ") + type;
			return false;
		}
		CommitNodeEdit();

		// M29:插入位置按**选中节点是不是容器**决定(判据取自组件登记表的 Category,
		// 不是写死的类型名单):
		//   * 选中 = 容器(Panel / Box / Grid / List / ScrollArea …)⇒ 挂到它的 Children 末尾;
		//   * 选中 = 叶子(Label / Button / Image …)⇒ 加为它的**下一个兄弟**;
		//   * 没有选中 ⇒ 作为新的根节点。
		//
		// 为什么:此前一律"挂到选中节点的 Children",于是连点三次 Add 会得到
		// `Label#1 → Label#0 → Label#2` 这种三层链 —— 既不是用户意图,也难管理。
		std::vector<UI::UiNode>* targetList = &m_Document.Nodes;
		std::string parentPath;
		std::string parentId;   // 父容器 Id(空 = 根列表);M42 用它把新节点放进父内容框
		std::size_t insertIndex = m_Document.Nodes.size();
		if (!m_SelectedId.empty())
		{
			if (UI::UiNode* parent = FindNodeMutable(m_Document.Nodes, m_SelectedId))
			{
				const Wui::WuiComponentDesc* component = UI::UiNodeRegistry::Component(parent->Type);
				const bool isContainer = component != nullptr && component->Category == "Containers";
				if (isContainer)
				{
					targetList = &parent->Children;
					parentPath = FindNodePath(m_Document, m_SelectedId);
					parentId = parent->Id;
					insertIndex = targetList->size();
				}
				else
				{
					// 叶子:插到它自己后面(同层)。找到"谁的 Children 里有它"。
					NodeLocation location;
					if (LocateNode(m_Document.Nodes, m_SelectedId, std::string(), location)
						&& location.List != nullptr)
					{
						targetList = location.List;
						insertIndex = location.Index + 1;
						// 选中叶子的自身路径:末段是它自己,去掉末段 = 它的父路径
						// (新节点成为它的兄弟,落在同一个父下;根层叶子没有父 ⇒ 两处都空)。
						const std::string leafPath = FindNodePath(m_Document, m_SelectedId);
						const std::size_t lastDot = leafPath.rfind('.');
						if (lastDot != std::string::npos)
						{
							parentId = leafPath.substr(lastDot + 1);
							parentPath = leafPath.substr(0, lastDot);
						}
					}
				}
			}
		}

		const UI::UiDocument before = m_Document;
		std::set<std::string> reserved;
		UI::UiNode node;
		node.Type = type;
		node.Id = MakeUniqueNodeId(m_Document, reserved, parentPath, type, targetList->size());
		node.IdWasGenerated = false;
		// 默认锚点:父内容框左上角一个盒子(容器布局会覆盖,不影响)。
		node.Anchor.Min = glm::vec2 { 0.0f, 0.0f };
		node.Anchor.Max = glm::vec2 { 0.0f, 0.0f };
		node.Anchor.Pivot = glm::vec2 { 0.0f, 0.0f };
		// M42:尺寸取类型登记表的**首选尺寸**(`UiNodeTypeDesc::DefaultSize`,设计单位);
		// 任一轴 <= 0 回落 240 / 48(此前所有类型写死 240x48 ⇒ 复选框/进度条/标签
		// 因此比控件本体大好几倍)。字段由 we_engine 追加在 `UiNodeTypeDesc` 末尾。
		const glm::vec2 preferred = typeDesc->DefaultSize;
		const glm::vec2 size {
			preferred.x > 0.0f ? preferred.x : 240.0f,
			preferred.y > 0.0f ? preferred.y : 48.0f };
		// M42:新节点落在**父容器内容框**内(固定 24,24 在小容器里会跑到容器外)。
		// 父矩形查不到(m_Screen 未布局 / 根层)⇒ 保持 24,24(与旧行为一致)。
		glm::vec2 offset { 24.0f, 24.0f };
		if (!parentId.empty())
		{
			const Wui::WuiRect parentRect = m_Screen.RectOf(parentId);
			if (parentRect.W > 0.0f && parentRect.H > 0.0f)
			{
				offset.x = std::clamp(offset.x, 0.0f, std::max(0.0f, parentRect.W - size.x));
				offset.y = std::clamp(offset.y, 0.0f, std::max(0.0f, parentRect.H - size.y));
			}
		}
		node.Anchor.Offset = offset;
		node.Anchor.Size = size;
		const std::string newId = node.Id;
		insertIndex = std::min(insertIndex, targetList->size());
		targetList->insert(targetList->begin() + static_cast<std::ptrdiff_t>(insertIndex), std::move(node));

		m_SelectedId = newId;
		m_BufferNodeId.clear();
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(Wui::Tr("panel.ui_designer.add_node", "Add Node"), before);
		m_Status = Wui::Tr("panel.ui_designer.added", "Added ") + newId;
		return true;
	}

	void UiDesignerPanel::DeleteSelectedNode()
	{
		if (!CanDeleteSelected())
		{
			m_Status = Wui::Tr("panel.ui_designer.delete_blocked",
				"Select a node first; the last root node cannot be deleted");
			return;
		}
		CommitNodeEdit();

		// M20:多选时删除整组(`EffectiveSelection()` 已过滤掉不存在的 id)。
		// 逐个查位置再删:每次 erase 都会让上一次的下标失效,所以每删一个都重新定位。
		const std::vector<std::string> victims = EffectiveSelection();
		if (victims.empty())
			return;
		const UI::UiDocument before = m_Document;
		std::size_t removed = 0;
		for (const std::string& id : victims)
		{
			// 父节点也在删除集合里时,子节点会随父一起消失 ⇒ 重定位失败就跳过(已删过)。
			NodeLocation location;
			if (!LocateNode(m_Document.Nodes, id, std::string(), location) || location.List == nullptr)
				continue;
			// 最后一个根节点不能删(与 CanDeleteSelected 同一守卫)。
			if (location.List == &m_Document.Nodes && m_Document.Nodes.size() <= 1)
				continue;
			location.List->erase(location.List->begin() + static_cast<std::ptrdiff_t>(location.Index));
			++removed;
		}
		CancelDrag();
		CancelNodeEdit();
		ClearSelection();
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(Wui::Tr("panel.ui_designer.delete_node", "Delete Node"), before);
		m_Status = Wui::Tr("panel.ui_designer.deleted", "Deleted ") +
			(removed == 1 ? victims.front() : (std::to_string(removed) + " nodes"));
	}

	// M19:对齐参考线 —— 拖动/缩放时,选中框的左/中/右、上/中/下与**父框**(没有父则视口内容矩形)
	// 的对应位置在 1 设计单位内对齐时,画一条横跨父框的虚线。只在拖动期间画,零常驻开销。
	// 与吸附的关系:吸附给"手感",参考线给"看得见的理由" —— 两者共用同一容差。
	void UiDesignerPanel::DrawAlignmentGuides(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& box)
	{
		const UI::UiNodeInstance* selected = m_Screen.Find(m_SelectedId);
		if (selected == nullptr)
			return;
		Wui::WuiRect reference = m_Viewport.ContentRect;
		if (selected->Parent >= 0 && selected->Parent < static_cast<int>(m_Screen.Nodes().size()))
			reference = m_Screen.Nodes()[static_cast<std::size_t>(selected->Parent)].Rect;

		constexpr float kTolerance = 1.0f;
		const float boxXs[3] = { box.X, box.X + box.W * 0.5f, box.X + box.W };
		const float boxYs[3] = { box.Y, box.Y + box.H * 0.5f, box.Y + box.H };
		const float refXs[3] = { reference.X, reference.X + reference.W * 0.5f, reference.X + reference.W };
		const float refYs[3] = { reference.Y, reference.Y + reference.H * 0.5f, reference.Y + reference.H };

		for (const float bx : boxXs)
		{
			for (const float rx : refXs)
			{
				if (std::abs(bx - rx) <= kTolerance)
				{
					Wui::LineSegment(ctx, glm::vec2 { rx, reference.Y }, glm::vec2 { rx, reference.Y + reference.H },
						theme.Accent, 1.0f);
					break;
				}
			}
		}
		for (const float by : boxYs)
		{
			for (const float ry : refYs)
			{
				if (std::abs(by - ry) <= kTolerance)
				{
					Wui::LineSegment(ctx, glm::vec2 { reference.X, ry }, glm::vec2 { reference.X + reference.W, ry },
						theme.Accent, 1.0f);
					break;
				}
			}
		}
	}

	void UiDesignerPanel::DuplicateSelectedNode()
	{
		if (!CanDuplicateSelected())
		{
			m_Status = Wui::Tr("panel.ui_designer.duplicate_blocked", "Select a node first");
			return;
		}
		CommitNodeEdit();

		NodeLocation location;
		if (!LocateNode(m_Document.Nodes, m_SelectedId, std::string(), location)
			|| location.List == nullptr)
			return;
		const UI::UiDocument before = m_Document;

		// 深拷贝整棵子树(值语义),再给子树里每个节点重发唯一 Id。
		UI::UiNode copy = (*location.List)[location.Index];
		std::set<std::string> reserved;
		RegenerateSubtreeIds(m_Document, reserved, copy, location.ParentPath, location.Index + 1);
		const std::string copyId = copy.Id;
		location.List->insert(
			location.List->begin() + static_cast<std::ptrdiff_t>(location.Index + 1), std::move(copy));

		m_SelectedId = copyId;
		m_BufferNodeId.clear();
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(Wui::Tr("panel.ui_designer.duplicate_node", "Duplicate Node"), before);
		m_Status = Wui::Tr("panel.ui_designer.duplicated", "Duplicated to ") + copyId;
	}

	bool UiDesignerPanel::CanDeleteSelected() const
	{
		if (!CanDuplicateSelected())
			return false;
		std::size_t rootIndex = 0;
		// 最后一个根节点删掉就是空文档:这一步禁用并给理由(tooltip)。
		return !(LocateRootIndex(m_Document.Nodes, m_SelectedId, &rootIndex)
			&& m_Document.Nodes.size() <= 1);
	}

	bool UiDesignerPanel::CanDuplicateSelected() const
	{
		return m_HasDocument && !m_SelectedId.empty()
			&& m_Document.FindNode(m_SelectedId) != nullptr;
	}

	// ---- M12:画布手柄 ----

	bool UiDesignerPanel::SelectedNodeBox(Wui::WuiRect& out) const
	{
		if (!m_HasDocument || m_SelectedId.empty())
			return false;
		const UI::UiNodeInstance* instance = m_Screen.Find(m_SelectedId);
		if (instance == nullptr)
			return false;
		out = NodeBoxPhysical(m_Viewport, instance->Rect);
		return true;
	}

	void UiDesignerPanel::DrawResizeHandles(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& box)
	{
		for (const HandleDef& handle : kHandles)
		{
			const Wui::WuiRect rect = HandlePhysicalRect(box, handle);
			const bool active = m_Drag == CanvasDrag::Resize && m_DragHandle == handle.Bits;
			const bool hovered = active || m_HoverHandle == handle.Bits;
			const Wui::WuiColor fill = active
				? theme.Accent : (hovered ? theme.ActiveBg : theme.PanelHeader);
			const Wui::WuiColor border = active || hovered ? theme.Accent : theme.BorderStrong;
			Wui::PanelBackground(ctx, rect, fill, 0.0f);
			Wui::HighlightOutline(ctx, rect, border, 0.0f, 1.0f);
		}
	}
}
