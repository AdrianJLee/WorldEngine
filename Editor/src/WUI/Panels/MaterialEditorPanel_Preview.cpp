#include "MaterialEditorPanel_Internal.h"

namespace World
{

using namespace MaterialEditorPanelDetail;


	// ---- 预览区:卡片(图像 + 预览设置标签/控件 + "仅影响预览显示"标注 + 读数) ----
	//
	// U23(用户 2026-09-22):「预览这个大分类应该和其他的分开。因为预览只是为了看,它并不影响
	// 实际效果呀。」→ 预览设置(网格/背景/光照/显示)搬进**预览区自己的卡片与标签条**,
	// 参数列只剩会影响材质本身的字段;卡片有底色 + 描边,与参数列明确分隔。
float MaterialEditorPanel::DrawPreview(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		const Wui::WuiTheme& theme = host.Theme();
		const float pad = theme.Pad;
		const float gap = theme.PadSmall;
		const float innerW = std::max(80.0f, rect.W - 2.0f * pad);
		// 预览行:与参数区同一条"窄列换行"口径(宽窗左列只有 220..340 宽)。
		// U23:预览列比参数列窄得多,单行的"标签 + 控件"在 240 设计单位以上都能放下
		// (再窄才改成上下两行),这样图像能拿到更多高度。
		// U27:卡片够宽(分隔条拖开 / 极宽窗口)时同一套响应式网格生效 —— 光照的"强度 +
		// 方位"、显示开关这类短字段成对同行;不够宽仍然一行一项(既有口径)。
		const int previewColumns = GridColumnCount(innerW, theme);
		const float previewCellWidth = std::max(80.0f, (innerW
			- theme.PadSmall * static_cast<float>(previewColumns - 1))
			/ static_cast<float>(previewColumns));
		const float previewLabelWidth = previewColumns >= 2
			? std::clamp(previewCellWidth * 0.42f, 64.0f, 110.0f) : -1.0f;
		const bool stacked = previewColumns == 1 && innerW < kPreviewInlineMinWidth;
		const float rowH = stacked ? kRowHeightStacked : kRowHeight;
		// 当前标签的行计划(网格切分与绘制共用同一份构造,避免"预留高度"和"实际画的东西"不一致)。
		const auto previewPlans = [this, rowH](int tab)
		{
			std::vector<RowPlan> plans;
			for (int index = 0; index < kRowSpecCount; ++index)
			{
				const RowSpec& spec = kRowSpecs[index];
				if (std::string(spec.Group) != std::string("preview") || PreviewTabFor(spec.Key) != tab)
					continue;
				RowPlan plan;
				plan.Group = spec.Group;
				plan.Key = spec.Key;
				plan.ControlId = std::string("material.") + spec.Key;
				plan.Label = Wui::Tr(spec.LabelKey, spec.LabelEn);
				plan.Doc = Wui::Tr(spec.DocKey, spec.DocEn);
				plan.ReadOnly = spec.ReadOnly != 0;
				plan.HasReset = spec.HasReset != 0;
				plan.Modified = plan.HasReset && !plan.ReadOnly && PreviewOptionModified(plan.Key);
				plan.Height = rowH;
				plans.push_back(std::move(plan));
			}
			return plans;
		};
		const auto packedHeight = [](const std::vector<std::vector<const RowPlan*>>& lines)
		{
			float height = 0.0f;
			for (const std::vector<const RowPlan*>& line : lines)
			{
				float lineHeight = 0.0f;
				for (const RowPlan* plan : line)
					lineHeight = std::max(lineHeight, plan->Height);
				height += lineHeight;
			}
			return height;
		};
		// 标签条高度按**当前最高的标签**预留(光照 4 项 / 显示 3 项 / 网格·背景 1 项):
		// 切标签时预览画面与卡片尺寸都不变 → 离屏目标不重建、切标签不闪。
		float tabRowsH[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (int tab = 0; tab < 4; ++tab)
		{
			const std::vector<RowPlan> plans = previewPlans(tab);
			std::vector<const RowPlan*> refs;
			refs.reserve(plans.size());
			for (const RowPlan& plan : plans)
				refs.push_back(&plan);
			tabRowsH[tab] = packedHeight(BuildGridLines(refs, previewColumns));
		}
		const float rowsReserveH = std::max(std::max(tabRowsH[0], tabRowsH[1]),
			std::max(tabRowsH[2], tabRowsH[3]));
		const float tabRowH = kPreviewTabHeight - 6.0f;
		const float stripH = (tabRowH + gap) + (kPreviewNoteHeight + gap) + rowsReserveH;
		// 图像(正方形)占满可用宽,但给下面的"标签 + 标注 + 控件"留出空间:空间不足时图像先变小。
		const float side = std::clamp(innerW, kPreviewImageMinSide,
			std::max(kPreviewImageMinSide, rect.H - 2.0f * pad - stripH));
		const float cardH = std::min(rect.H, 2.0f * pad + side + stripH);
		const Wui::WuiRect card { rect.X, rect.Y, rect.W, cardH };
		m_PreviewZoneRect = card;
		// 预览区 = 一张卡片(ContentBg 底 + BorderStrong 描边):与右侧参数列"明确分隔"。
		Wui::PanelBackground(ctx, card, theme.ContentBg, theme.Radius + 2.0f);
		Wui::HighlightOutline(ctx, card, theme.BorderStrong, theme.Radius + 2.0f, 1.0f);

		const Wui::WuiRect view { card.X + pad, card.Y + pad, side, side };
		m_PreviewRect = view;
		UpdatePreviewTargetSize(view);
		const uint64_t textureId = RenderPreview();
		const std::string hint = Wui::Tr("panel.material.preview.hint",
			"Drag = orbit, wheel = zoom, double-click or F = frame");
		if (textureId != 0)
		{
			// 贴图按**物理像素网格**对齐:目标尺寸 = 这段物理尺寸时,每个屏幕像素正好采样
			// 一个纹素(1:1),线性过滤也不会糊。
			const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
			const Wui::WuiRect pixelView {
				std::round(view.X * uiScale) / uiScale,
				std::round(view.Y * uiScale) / uiScale,
				static_cast<float>(std::lround(view.W * uiScale)) / uiScale,
				static_cast<float>(std::lround(view.H * uiScale)) / uiScale };
			// 渐变背景:预览目标清成透明,先在下面画一层 WUI 渐变(2D 通道,不需要改 3D)。
			if (m_PreviewBackground == PreviewBackground::Gradient)
			{
				// W3.5:改走库件 P1c-LIB2 的 Wui::GradientFill(四角重载)—— 库实现就是
				// Kind=Gradient + Rect + Corners 三项 + push_back,与这里原来手搓的命令逐字段等价
				// (见 Engine/src/World/WUI/WuiWidgets.cpp 的同名重载);
				// 面板不再需要自己拼 WuiDrawCommand(本面板最后一条即时绘制)。
				Wui::GradientFill(ctx, pixelView,
					Wui::WuiColor { 0.24f, 0.27f, 0.33f, 1.0f }, Wui::WuiColor { 0.24f, 0.27f, 0.33f, 1.0f },
					Wui::WuiColor { 0.05f, 0.06f, 0.08f, 1.0f }, Wui::WuiColor { 0.05f, 0.06f, 0.08f, 1.0f });
			}
			// U22:离屏预览按引擎统一口径贴({0,1,1,-1},与主视口同一行序约定)。
			// 之前用 {0,0,1,1} 会上下镜像:主光(仰角 +45°)会画到球的下方,
			// 用户看到的就是"上下反、左右反"(实测:与 capture.texture 的 flipV 相关度 0.98)。
			Wui::Image(ctx, pixelView, textureId, ViewChrome::kPreviewImageUv, theme);
			// 相机:左键轨道旋转 / 滚轮推拉 / 双击或 F 取景(与模型/预制体面板同一手感)。
			const bool hovered = ctx.IsHovered(pixelView);
			if (ctx.Input().Wheel != 0.0f && hovered)
			{
				const float step = std::max(0.05f, m_CameraDistance * 0.1f);
				m_CameraDistance = std::clamp(m_CameraDistance - ctx.Input().Wheel * step,
					m_CameraMinDistance, m_CameraMaxDistance);
			}
			if (hovered && ctx.Input().MouseClicked[0])
			{
				m_Orbiting = true;
				m_LastMouse = ctx.Input().MousePos;
			}
			if (m_Orbiting && ctx.Input().MouseDown[0] && !ctx.IsDoubleClicked(pixelView))
			{
				const glm::vec2 delta = ctx.Input().MousePos - m_LastMouse;
				m_LastMouse = ctx.Input().MousePos;
				// U22:符号只有一处定义(三个预览面板 + 主视口共用 ViewChrome::ApplyOrbitDrag)。
				// 2026-09-22 用户实测材质预览"上下反、左右反"的真因是**画面上下镜像**
				// (离屏纹理按 {0,1,1,-1} 才正立,见上面 Image 的 UV),不是拖拽符号本身。
				ViewChrome::ApplyOrbitDrag(m_OrbitYaw, m_OrbitPitch, delta, kPitchLimit);
			}
			if (m_Orbiting && !ctx.Input().MouseDown[0])
				m_Orbiting = false;
			if (ctx.IsDoubleClicked(pixelView) || (hovered && ctx.WasKeyPressed(KeyCodes::F)))
				FramePreview();
			if (hovered)
				ctx.SetCursor(m_Orbiting ? Wui::WuiCursor::Hand : Wui::WuiCursor::Arrow);
			// U22:坐标系指示器(方案 §5.5)—— 右下角 48px,跟随预览相机朝向。
			// 画在这里 = 画在预览图像之后,天然压在图像之上;轴顶点用与渲染**同一份**
			// 基向量算(OrbitBasis),不另写一套投影,避免"图正了、轴标还是反的"。
			glm::vec3 axisRight { 1.0f, 0.0f, 0.0f };
			glm::vec3 axisUp { 0.0f, 1.0f, 0.0f };
			ViewChrome::OrbitBasis(m_OrbitYaw, m_OrbitPitch, &axisRight, &axisUp, nullptr);
			m_AxisRect = ViewChrome::DrawAxisIndicator(ctx, pixelView, axisRight, axisUp);
			// 角标:实际离屏目标分辨率(与预制体面板同一口径);移到右上,给轴标让位。
			const std::string targetLabel = std::to_string(m_PreviewTargetW) + "×"
				+ std::to_string(m_PreviewTargetH) + "px";
			const float labelWidth = ctx.MeasureTextWidth(targetLabel, 10.0f);
			Wui::Label(ctx, { pixelView.X + std::max(4.0f, pixelView.W - labelWidth - 6.0f),
				pixelView.Y + 4.0f }, targetLabel, theme.TextDisabled, 10.0f);
			Wui::Label(ctx, { pixelView.X + 6.0f, pixelView.Y + pixelView.H - 14.0f },
				EllipsizeToWidth(ctx, hint, std::max(40.0f, pixelView.W - 90.0f), 11.0f),
				theme.TextDisabled, 11.0f);
			Wui::Tooltip(ctx, pixelView, hint);
		}
		else
		{
			Wui::Label(ctx, { view.X + pad, view.Y + pad },
				Wui::Tr("panel.material.preview_unavailable", "Preview unavailable (RHI device not ready)"),
				theme.TextMuted, 12.0f);
		}
		// ---- 预览设置:分段标签(网格|背景|光照|显示)+ "仅影响预览显示"标注 + 当前标签的控件 ----
		const char* tabKeys[4] = { "mesh", "bg", "light", "display" };
		const std::vector<std::string> tabLabels {
			Wui::Tr("material.preview.tab.mesh", "Mesh"),
			Wui::Tr("material.preview.tab.bg", "Background"),
			Wui::Tr("material.preview.tab.light", "Lighting"),
			Wui::Tr("material.preview.tab.display", "Display") };
		const int activeTab = std::clamp(m_PreviewTab, 0, 3);
		const Wui::WuiRect tabRect { card.X + pad, view.Y + view.H + gap, innerW, tabRowH };
		int tabSelection = activeTab;
		if (Wui::TabBar(ctx, Wui::HashId("material.preview.tabs"), tabRect, tabLabels,
			tabSelection, theme))
			m_PreviewTab = tabSelection;
		// 额外登记**稳定 id**(material.preview.tab.<key>):脚本按 id 点击/断言,
		// 不依赖本地化文案,也不用去猜 TabBar 的派生 id。
		const float tabWidth = tabRect.W / 4.0f;
		for (int index = 0; index < 4; ++index)
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId((std::string("material.preview.tab.") + tabKeys[index]).c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "tab";
			node.Label = tabLabels[static_cast<size_t>(index)];
			node.Value = index == activeTab ? "true" : "false";
			node.Tooltip = Wui::Tr("panel.material.preview.tab.tooltip",
				"Preview settings are split into tabs: the preview scene never changes, "
				"only which options you are looking at.");
			node.Rect = { tabRect.X + tabWidth * static_cast<float>(index), tabRect.Y, tabWidth,
				tabRect.H };
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		// 归属标注:预览区是"看"的设置,不写进 .wmat —— 用户 2026-09-22 的原话就是这个分界线。
		const std::string previewNote = Wui::Tr("panel.material.preview.note",
			"Preview display only — never written into the material");
		const std::string previewNoteDoc = Wui::Tr("panel.material.preview.note.tooltip",
			"Preview settings change how you look at the material (mesh, background, lighting, "
			"display overlays). They are never saved into the .wmat and never change the material "
			"that objects render with.");
		const Wui::WuiRect noteRect { card.X + pad, tabRect.Y + tabRect.H + gap, innerW,
			kPreviewNoteHeight };
		Wui::Label(ctx, { noteRect.X, noteRect.Y },
			EllipsizeToWidth(ctx, previewNote, noteRect.W, 11.0f), theme.TextDisabled, 11.0f);
		Wui::Tooltip(ctx, noteRect, previewNoteDoc);
		RegisterReadOnlyNode(Wui::HashId("material.preview.note"), previewNote, "preview-only",
			noteRect, previewNoteDoc);
		// 当前标签的控件:与参数行同一个绘制函数(同一套密度与"恢复默认"口径)。
		float rowY = noteRect.Y + noteRect.H + gap;
		{
			const std::vector<RowPlan> plans = previewPlans(activeTab);
			std::vector<const RowPlan*> refs;
			refs.reserve(plans.size());
			for (const RowPlan& plan : plans)
				refs.push_back(&plan);
			for (const std::vector<const RowPlan*>& line : BuildGridLines(refs, previewColumns))
			{
				float lineHeight = 0.0f;
				for (const RowPlan* plan : line)
					lineHeight = std::max(lineHeight, plan->Height);
				// 窗口高度极小时宁可少画一行,也不让控件越出卡片(卡片外面是参数列)。
				if (rowY + lineHeight > card.Y + cardH + 0.5f)
					break;
				for (size_t cell = 0; cell < line.size(); ++cell)
				{
					const float cellX = card.X + pad
						+ static_cast<float>(cell) * (previewCellWidth + gap);
					DrawParameterRow(ctx, host, theme, *line[cell], cellX, rowY, previewCellWidth,
						stacked, previewLabelWidth, previewColumns >= 2);
				}
				rowY += lineHeight;
			}
		}
		// 无障碍:预览区节点 + 两条可断言的读数(target / camera)。
		{
			Wui::WuiAccessNode zone;
			zone.Id = Wui::HashId("material.preview.zone");
			zone.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			zone.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			zone.Kind = "group";
			zone.Label = Wui::Tr("panel.material.preview.zone", "Preview");
			zone.Value = "preview-only";
			zone.Tooltip = previewNoteDoc;
			zone.Rect = card;
			zone.Enabled = true;
			zone.Interactive = false;
			zone.Visible = true;
			Wui::WuiAccessibility::Get().Register(zone);

			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.preview");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "image";
			node.Label = Wui::Tr("panel.material.preview", "Preview");
			node.Value = textureId != 0
				? (Wui::Tr("panel.material.preview.of", "preview of ") + m_Path + " ("
					+ std::to_string(m_PreviewTargetW) + "x"
					+ std::to_string(m_PreviewTargetH) + ")")
				: Wui::Tr("panel.material.preview_unavailable", "Preview unavailable (RHI device not ready)");
			node.Tooltip = hint;
			node.Rect = view;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		char targetText[256] = {};
		std::snprintf(targetText, sizeof(targetText),
			"target=%ux%u view=%.0fx%.0f uiScale=%.2f renderScale=%.2f (echo only)",
			m_PreviewTargetW, m_PreviewTargetH, static_cast<double>(m_PreviewViewW),
			static_cast<double>(m_PreviewViewH), static_cast<double>(m_PreviewUiScale),
			static_cast<double>(RenderSettings::RenderScale()));
		RegisterReadOnlyNode(Wui::HashId("material.preview.target"),
			Wui::Tr("panel.material.preview.target", "Preview render target"), targetText,
			{ view.X, view.Y, std::max(20.0f, view.W), 14.0f },
			Wui::Tr("material.prop.preview.target.doc",
				"Preview render target = preview rect in physical pixels, long side clamped to [128, 2048]. "
				"Independent of rendering.render_scale."));
		char cameraText[256] = {};
		std::snprintf(cameraText, sizeof(cameraText),
			"yaw=%.2f pitch=%.2f pitchLimitDeg=89 dist=%.3f minDist=%.3f maxDist=%.3f mesh=%d",
			static_cast<double>(m_OrbitYaw), static_cast<double>(m_OrbitPitch),
			static_cast<double>(m_CameraDistance), static_cast<double>(m_CameraMinDistance),
			static_cast<double>(m_CameraMaxDistance), static_cast<int>(m_PreviewMesh));
		RegisterReadOnlyNode(Wui::HashId("material.preview.camera"),
			Wui::Tr("panel.material.preview.camera", "Preview camera"), cameraText,
			{ view.X, view.Y + 14.0f, std::max(20.0f, view.W), 14.0f }, hint);
		// U22:坐标系指示器读数(探针按它断言"拖拽后 yaw/pitch 的符号"与方向一致性)。
		ViewChrome::RegisterAxisNode(m_AxisRect, "material.axis",
			Wui::Tr("panel.material.axis", "Preview axes"),
			ViewChrome::AxisReadout(glm::degrees(m_OrbitYaw), glm::degrees(m_OrbitPitch), false),
			Wui::Tr("panel.material.axis.tooltip",
				"World axes drawn in the preview's corner: X red, Y green, Z blue. "
				"They follow the preview camera; yaw/pitch of that camera are in the value."));
		return cardH;
	}

}
