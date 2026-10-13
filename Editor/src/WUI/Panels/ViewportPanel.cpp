#include "WUI/Panels/ViewportPanel_ToolGizmos.h"
#include "wldpch.h"
#include "WUI/Panels/ViewportPanel.h"

#include "World/WUI/WuiGizmo.h"
#include "World/Renderer/Tilemap.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/Core/KeyCodes.h"
#include "World/Physics/Physics3D.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiWidgets.h"
#include "Core/EditorPreferences.h"

namespace World
{
	namespace
	{
		static const char* OrthoAxisShortName(World::OrthographicAxis axis)
		{
			switch (axis)
			{
			case World::OrthographicAxis::Top:    return "Top";
			case World::OrthographicAxis::Bottom: return "Bottom";
			case World::OrthographicAxis::Front:  return "Front";
			case World::OrthographicAxis::Back:   return "Back";
			case World::OrthographicAxis::Right:  return "Right";
			case World::OrthographicAxis::Left:   return "Left";
			}
			return "Front";
		}

		// Liang-Barsky 严格二维线段裁剪: 确保裁剪后线段严格保持原始斜率与方向, 绝不因轴向暴力 clamp 产生几何畸变
		static bool ClipLineSegment2D(glm::vec2& p0, glm::vec2& p1, float minX, float maxX, float minY, float maxY)
		{
			float t0 = 0.0f, t1 = 1.0f;
			const float dx = p1.x - p0.x;
			const float dy = p1.y - p0.y;

			const auto clipTest = [&](float p, float q) -> bool
			{
				if (p < 0.0f)
				{
					const float r = q / p;
					if (r > t1) return false;
					if (r > t0) t0 = r;
				}
				else if (p > 0.0f)
				{
					const float r = q / p;
					if (r < t0) return false;
					if (r < t1) t1 = r;
				}
				else if (q < 0.0f)
				{
					return false;
				}
				return true;
			};

			if (clipTest(-dx, p0.x - minX) &&
				clipTest(dx, maxX - p0.x) &&
				clipTest(-dy, p0.y - minY) &&
				clipTest(dy, maxY - p0.y))
			{
				const glm::vec2 origP0 = p0;
				p0 = origP0 + t0 * glm::vec2(dx, dy);
				p1 = origP0 + t1 * glm::vec2(dx, dy);
				return true;
			}
			return false;
		}

		// 投影 + 近平面裁剪 + 线段四边形:相机视锥等覆盖层线段共用。
		void PushProjectedSegment(Wui::WuiContext& ctx, const glm::mat4& viewProjection,
			const Wui::WuiRect& viewport, const glm::vec4& clipA, const glm::vec4& clipB,
			const Wui::WuiColor& color, float thickness,
			glm::vec2* boundsMin = nullptr, glm::vec2* boundsMax = nullptr)
		{
			constexpr float kMinW = 0.15f;
			glm::vec4 a = clipA;
			glm::vec4 b = clipB;
			if (a.w < kMinW && b.w < kMinW)
				return;
			if (a.w < kMinW)
			{
				const float t = (kMinW - a.w) / (b.w - a.w);
				a = glm::mix(a, b, t);
			}
			else if (b.w < kMinW)
			{
				const float t = (kMinW - b.w) / (a.w - b.w);
				b = glm::mix(b, a, t);
			}
			const auto project = [&viewport](const glm::vec4& clip)
			{
				const glm::vec3 ndc = glm::vec3(clip) / clip.w;
				return glm::vec2 { viewport.X + (ndc.x + 1.0f) * 0.5f * viewport.W,
					viewport.Y + (1.0f - ndc.y) * 0.5f * viewport.H };
			};
			glm::vec2 from = project(a);
			glm::vec2 to = project(b);

			if (std::isnan(from.x) || std::isnan(from.y) || std::isnan(to.x) || std::isnan(to.y))
				return;

			// 使用 Liang-Barsky 严格保持几何斜率地裁剪至视口矩形
			const float pad = 4.0f;
			if (!ClipLineSegment2D(from, to, viewport.X - pad, viewport.X + viewport.W + pad,
				viewport.Y - pad, viewport.Y + viewport.H + pad))
			{
				return;
			}

			if (boundsMin)
				*boundsMin = glm::min(*boundsMin, glm::min(from, to));
			if (boundsMax)
				*boundsMax = glm::max(*boundsMax, glm::max(from, to));
			const glm::vec2 delta = to - from;
			const float length = glm::length(delta);
			if (length < 0.5f)
				return;
			Wui::LineSegment(ctx, from, to, color, thickness);
		}

		// 相机预览小窗位置:场景图左下角,宽取视口的 28%,16:9。
		Wui::WuiRect CameraPreviewRect(const Wui::WuiRect& sceneRect)
		{
			const float width = std::max(160.0f, sceneRect.W * 0.28f);
			const float height = width * 9.0f / 16.0f;
			const float margin = 10.0f;
			return { sceneRect.X + margin, sceneRect.Y + sceneRect.H - height - margin, width, height };
		}

		// 只读节点(status):AI 能读到,ui.invoke 不会去点它(与脚本面板同一约定)。
		// ---- P4-U6:范围可视化辅助(光源范围 / 碰撞体轮廓共用) ----

		// 世界变换:优先 WorldTransformComponent(带父级链),否则退回本地矩阵。
		glm::mat4 WorldMatrixOf(const entt::registry& registry, entt::entity handle)
		{
			if (const auto* world = registry.try_get<WorldTransformComponent>(handle))
				return world->Matrix;
			if (const auto* transform = registry.try_get<TransformComponent>(handle))
				return transform->GetLocalMatrix();
			return glm::mat4(1.0f);
		}

		// 世界空间线段(两端点)→ 投影成屏幕线段。
		void PushWorldSegment(Wui::WuiContext& ctx, const glm::mat4& viewProjection, const Wui::WuiRect& viewport,
			const glm::mat4& world, const glm::vec3& from, const glm::vec3& to,
			const Wui::WuiColor& color, float thickness, glm::vec2* boundsMin, glm::vec2* boundsMax)
		{
			PushProjectedSegment(ctx, viewProjection, viewport,
				viewProjection * (world * glm::vec4 { from, 1.0f }),
				viewProjection * (world * glm::vec4 { to, 1.0f }),
				color, thickness, boundsMin, boundsMax);
		}

		// 世界空间圆(axis:0=XY 平面 1=XZ 2=YZ),按 40 段折线画成"圆环"。
		// 点光范围 / 球形碰撞体 / 胶囊端面都用它 —— 一套几何代码,不各写一遍。
		void PushWorldCircle(Wui::WuiContext& ctx, const glm::mat4& viewProjection, const Wui::WuiRect& viewport,
			const glm::mat4& world, const glm::vec3& center, float radius, int axis,
			const Wui::WuiColor& color, float thickness, glm::vec2* boundsMin, glm::vec2* boundsMax)
		{
			constexpr int kSegments = 40;
			constexpr float kTwoPi = 6.28318530718f;
			glm::vec4 previous {};
			for (int i = 0; i <= kSegments; ++i)
			{
				const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kSegments);
				glm::vec3 offset(0.0f);
				if (axis == 0)
					offset = { std::cos(angle) * radius, std::sin(angle) * radius, 0.0f };
				else if (axis == 1)
					offset = { std::cos(angle) * radius, 0.0f, std::sin(angle) * radius };
				else
					offset = { 0.0f, std::cos(angle) * radius, std::sin(angle) * radius };
				const glm::vec4 clip = viewProjection * (world * glm::vec4 { center + offset, 1.0f });
				if (i > 0)
					PushProjectedSegment(ctx, viewProjection, viewport, previous, clip, color, thickness,
						boundsMin, boundsMax);
				previous = clip;
			}
		}

		// P4-U8a:视口"悬浮层"布局 —— 运行控制 / 视图菜单 / 状态徽标全部**浮在画面上**,
		// 不占布局(否则场景图被挤扁,用户 2026-09-21:「play 三个按钮使视口变形了」)。
		constexpr float kChromePad = 8.0f;     // 悬浮层与画面边缘的距离
		constexpr float kIslandH = 30.0f;      // 悬浮岛屿底板高度
		constexpr float kBtnH = 22.0f;         // 悬浮岛内按钮与控件高度
		constexpr float kPillH = 36.0f;        // 居中药丸(运行控制)的高度
		// 运行控制按钮的稳定无障碍 id:脚本按 id 点,不靠坐标(与菜单/其它按钮同一条口径)。
		const char* const kViewportToolIds[3] = {
			"viewport.tool.play", "viewport.tool.simulate", "viewport.tool.pause" };

		void RegisterReadonlyNode(Wui::WuiId id, const char* kind, const std::string& label,
			const std::string& value, const Wui::WuiRect& rect, const std::string& tooltip = std::string())
		{
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		}
	}

	void ViewportPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost&)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		m_Host.SetViewportRect(rect);

		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_SceneImage = std::make_shared<Wui::WuiImage>();
			m_SceneImage->Uv = { 0, 1, 1, -1 };
			// 场景图**占满整块面板**:视口尺寸 = 面板尺寸,渲染目标不再被任何工具条挤掉一块。
			m_Root->Add(m_SceneImage, { 0, 1e30f, 0, 1e30f, 1 });

			// 悬浮的运行控制(居中药丸):独立的小布局树,按**绝对矩形**摆放(见 OnRender),
			// 不进场景图布局 —— 悬浮层可以有,但不能改变画面大小。
			m_ToolPill = std::make_shared<Wui::WuiBox>();
			m_ToolPill->Direction = Wui::WuiDirection::Row;
			m_ToolPill->Gap = 4;
			m_ToolPill->AlignMain = Wui::WuiAlign::Center;
			m_ToolPill->AlignCross = Wui::WuiAlign::Center;
			for (int i = 0; i < 3; ++i)
			{
				auto button = std::make_shared<Wui::WuiImageButton>();
				button->Uv = { 0, 1, 1, -1 };
				button->SetId(Wui::HashId(kViewportToolIds[i]));
				m_ToolPill->Add(button, { 28, 28, 28, 28, 0 });
				m_Tools.push_back(button);
			}
		}

		const bool play = m_Host.IsPlaying();
		const bool simulate = m_Host.IsSimulating();
		const bool paused = m_Host.IsPaused();
		const int icons[3] = {
			play ? 1 : 0,
			simulate ? 5 : 4,
			paused ? (simulate ? 7 : 3) : (simulate ? 6 : 2),
		};
		const bool dim[3] = { simulate, play, !play && !simulate };
		// 文案随状态变化:同一个按钮既是"开始"也是"停止",无障碍/提示必须说清当前能做什么。
		const std::string labels[3] = {
			play ? Wui::Tr("viewport.tool.stop_play", "Stop") : Wui::Tr("viewport.tool.play", "Play"),
			simulate ? Wui::Tr("viewport.tool.stop_simulate", "Stop Simulate")
				: Wui::Tr("viewport.tool.simulate", "Simulate"),
			paused ? Wui::Tr("viewport.tool.resume", "Resume") : Wui::Tr("viewport.tool.pause", "Pause"),
		};
		const std::string tooltips[3] = {
			Wui::Tr("viewport.tool.play.tooltip", "Play the scene (F5). Play again to stop."),
			Wui::Tr("viewport.tool.simulate.tooltip",
				"Simulate: run physics/animation without game scripts. Simulate again to stop."),
			Wui::Tr("viewport.tool.pause.tooltip", "Pause / resume the running scene."),
		};
		m_Tools[0]->OnClick = [this] { m_Host.TogglePlay(); };
		m_Tools[1]->OnClick = [this] { m_Host.ToggleSimulate(); };
		m_Tools[2]->OnClick = [this] { m_Host.TogglePause(); };
		for (int i = 0; i < 3; ++i)
		{
			m_Tools[i]->TextureId = m_Host.GetIconId(icons[i]);
			m_Tools[i]->Dim = dim[i];
			m_Tools[i]->Label = labels[i];
			m_Tools[i]->Tooltip = tooltips[i];
		}
		m_SceneImage->TextureId = m_Host.GetSceneTextureId();

		// 面板底色:纯绘制,不进布局树。
		Wui::PanelBackground(ctx, rect, { 0.06f, 0.06f, 0.07f, 1 });
		Wui::LayoutWidgetTree(m_Root, rect);
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);

		const Wui::WuiRect sceneRect = m_SceneImage->Rect();
		// 画面区进无障碍树:脚本/验收可以断言"视口 = 整块面板"(悬浮层不许改变画面尺寸),
		// 也能拿它算视口内坐标给拾取/gizmo 自动化用。
		RegisterReadonlyNode(Wui::HashId("viewport.surface"), "viewport",
			Wui::Tr("viewport.surface", "Viewport"),
			std::to_string(static_cast<int>(sceneRect.W)) + "x" + std::to_string(static_cast<int>(sceneRect.H)),
			sceneRect, Wui::Tr("viewport.surface.tooltip",
				"The rendered surface fills the whole panel; run controls and the view menu float above it."));

		// ---- 悬浮层(chrome):全部画在画面之上,不占布局 ----
		// P4-U7 的覆盖层机制在这里同样适用:整层进 overlay 通道 + 登记覆盖层矩形 →
		// 悬浮控件自己照常命中,而**它下面的视口拾取/gizmo 不会同时吃到这一下点击**。
		ctx.PushOverlay();

		// ① 顶部居中: 运行控制药丸 (播放 / 模拟 / 暂停) 作为中央绝对安全禁区
		const float pillW = 3 * 28.0f + 2 * 4.0f + 20.0f; // 112px
		const Wui::WuiRect pillRect { sceneRect.X + (sceneRect.W - pillW) * 0.5f,
			sceneRect.Y + kChromePad, pillW, kPillH };
		const bool pillHovered = ctx.IsHovered(pillRect);
		Wui::Toolbar(ctx, pillRect, theme, kPillH * 0.5f,
			pillHovered ? 0.95f : (play || simulate ? 0.85f : 0.65f));
		Wui::LayoutWidgetTree(m_ToolPill, pillRect);
		m_ToolPill->Paint(paint);
		ctx.RegisterOverlayRect(pillRect);

		// 药丸安全边界防护: 左右各预留 10px 空隙，严禁任何两侧岛屿穿透碰撞
		const float pillLeftBound = pillRect.X - 10.0f;
		const float pillRightBound = pillRect.X + pillRect.W + 10.0f;

		// ② 左岛 (Left Island - 视图与变换工具组): [视图下拉] + [分割线] + [Q / W / E / R / T]
		const World::ViewportProjection currentProj = m_Host.GetViewportProjection();
		const World::ViewportViewpoint currentVp = m_Host.GetViewportViewpoint();
		const bool isOrtho = (currentProj == World::ViewportProjection::Orthographic);
		const bool camera3D = !isOrtho;
		const char* projName = isOrtho ? "正交" : "透视";
		const char* vpShort = "自由";
		switch (currentVp)
		{
		case World::ViewportViewpoint::Free:   vpShort = "自由"; break;
		case World::ViewportViewpoint::Top:    vpShort = "Top"; break;
		case World::ViewportViewpoint::Bottom: vpShort = "Bottom"; break;
		case World::ViewportViewpoint::Front:  vpShort = "Front"; break;
		case World::ViewportViewpoint::Back:   vpShort = "Back"; break;
		case World::ViewportViewpoint::Right:  vpShort = "Right"; break;
		case World::ViewportViewpoint::Left:   vpShort = "Left"; break;
		}
		const std::string cameraText = std::string(projName) + " · " + vpShort;
		const std::string viewLabel = Wui::Tr("viewport.view.button", "View") + ": "
			+ projName + " · " + vpShort + "  ▼";
		// 固定最小宽度 140px，防止切换 Lock 或切换 Top/Front/Bottom 时工具栏发生水平位移跳动
		const float viewButtonW = std::max(140.0f, ctx.MeasureTextWidth(viewLabel, 12.0f) + 16.0f);

		// Gizmo 工具组规格 (Q: 选 / W: 移 / E: 转 / R: 缩 / T: 2D 矩形手柄，22×22px)
		const float gizmoBtnW = 22.0f;
		const float gizmoBtnH = 22.0f;
		struct GizmoBtnSpec { const char* id; const char* label; Wui::GizmoOperation op; const char* tip; };
		const GizmoBtnSpec gizmoBtns[5] = {
			{ "viewport.btn.gizmo_q", "Q", Wui::GizmoOperation::None, "选择模式 (None) [快捷键 Q]" },
			{ "viewport.btn.gizmo_w", "W", Wui::GizmoOperation::Translate, "平移工具 (Move) [快捷键 W]" },
			{ "viewport.btn.gizmo_e", "E", Wui::GizmoOperation::Rotate, "旋转工具 (Rotate) [快捷键 E]" },
			{ "viewport.btn.gizmo_r", "R", Wui::GizmoOperation::Scale, "缩放工具 (Scale) [快捷键 R]" },
			{ "viewport.btn.gizmo_t", "T", Wui::GizmoOperation::Rect, "2D 矩形变换手柄 (Rect Transform) [快捷键 T]" }
		};
		const Wui::GizmoOperation activeGizmo = m_Host.GetGizmoOperation();

		// 左岛全宽: 内衬4 + viewButtonW + 间隙3 + 分割线8 + 间隙3 + 5*22 + 4*2 + 内衬4
		const float fullLeftIslandW = 4.0f + viewButtonW + 3.0f + 8.0f + 3.0f + (5 * gizmoBtnW + 4 * 2.0f) + 4.0f;
		const float leftIslandX = sceneRect.X + kChromePad;
		const float leftIslandY = sceneRect.Y + kChromePad;
		// 受中岛左边界严格防护
		const float maxLeftIslandW = std::max(viewButtonW + 8.0f, pillLeftBound - leftIslandX);
		const float leftIslandW = std::min(fullLeftIslandW, maxLeftIslandW);

		const Wui::WuiRect leftIslandRect { leftIslandX, leftIslandY, leftIslandW, kIslandH };
		Wui::Toolbar(ctx, leftIslandRect, theme, 6.0f, 0.85f);
		ctx.RegisterOverlayRect(leftIslandRect);

		// 左岛内部内容填充
		const float leftBtnY = leftIslandRect.Y + 4.0f;
		const Wui::WuiRect viewButton { leftIslandRect.X + 4.0f, leftBtnY, viewButtonW, kBtnH };
		const std::string viewHint = Wui::Tr("viewport.view.hint",
			"View options: view mode (3D/2D), camera preview, light ranges, collider outlines.");
		Wui::Tooltip(ctx, viewButton, viewHint);
		if (Wui::Button(ctx, Wui::HashId("viewport.view"), viewButton, viewLabel, theme))
			ctx.OpenPopup(Wui::HashId("viewport.view.popup"));
		RegisterReadonlyNode(Wui::HashId("viewport.camera.mode"), "text", cameraText,
			camera3D ? "3d" : "2d", viewButton,
			Wui::Tr("viewport.camera.hint", "Viewport view mode — switch in the View menu (top-left)."));
		ctx.RegisterOverlayRect(viewButton);

		float leftCursor = viewButton.X + viewButton.W + 3.0f;
		if (leftCursor + 8.0f + gizmoBtnW <= leftIslandRect.X + leftIslandRect.W - 4.0f)
		{
			const Wui::WuiRect sepRect { leftCursor, leftIslandRect.Y, 8.0f, kIslandH };
			Wui::ToolbarSeparator(ctx, sepRect, theme);
			leftCursor += 8.0f + 3.0f;
		}

		for (int i = 0; i < 5; ++i)
		{
			if (leftCursor + gizmoBtnW > leftIslandRect.X + leftIslandRect.W - 4.0f)
				break;

			const Wui::WuiRect gBtnRect { leftCursor, leftBtnY, gizmoBtnW, gizmoBtnH };
			Wui::Tooltip(ctx, gBtnRect, gizmoBtns[i].tip);

			auto gizmoTheme = theme;
			if (activeGizmo == gizmoBtns[i].op)
			{
				gizmoTheme.ButtonBg = theme.ActiveBg;
				gizmoTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId(gizmoBtns[i].id), gBtnRect, gizmoBtns[i].label, gizmoTheme))
			{
				m_Host.SetGizmoOperation(gizmoBtns[i].op);
			}
			ctx.RegisterOverlayRect(gBtnRect);
			leftCursor += gizmoBtnW + 2.0f;
		}

		// ③ 右岛 (Right Island - 覆盖层与状态徽章)
		// 从左至右内含: [图层下拉] + [分割线] + [Grid] + [Part] + [Lock] + [分割线] + [状态徽章]
		const std::vector<Level::DataLayerDefinition>& dataLayers = m_Host.GetLevelDataLayers();
		const uint32_t activeLayers = m_Host.GetActiveDataLayers();
		const bool hasDataLayers = !dataLayers.empty();
		size_t activeCount = 0;
		if (hasDataLayers)
		{
			for (const auto& layer : dataLayers)
			{
				if (!layer.Name.empty() && (layer.LayerMask & activeLayers) != 0)
					activeCount++;
			}
		}
		char layersLabel[64] = {};
		std::snprintf(layersLabel, sizeof(layersLabel), "%s: %zu/%zu ▼",
			Wui::Tr("viewport.layers.dropdown", "图层").c_str(), activeCount, dataLayers.size());
		const float layersBtnW = hasDataLayers ? (ctx.MeasureTextWidth(layersLabel, 12.0f) + 16.0f) : 0.0f;

		const std::string gridLabel = Wui::Tr("viewport.grid.short", "Grid");
		const float gridBtnW = std::max(42.0f, ctx.MeasureTextWidth(gridLabel, 12.0f) + 14.0f);

		const std::string partLabel = Wui::Tr("viewport.partition.short", "Part");
		const float partBtnW = std::max(42.0f, ctx.MeasureTextWidth(partLabel, 12.0f) + 14.0f);

		const std::string lockLabel = Wui::Tr("viewport.2d.short", "Lock");
		const float lockBtnW = std::max(42.0f, ctx.MeasureTextWidth(lockLabel, 12.0f) + 14.0f);

		const char* const modeKey = simulate ? (paused ? "paused" : "simulate")
			: (play ? (paused ? "paused" : "play") : "edit");
		const std::string modeText = paused ? Wui::Tr("viewport.mode.paused", "Paused")
			: (simulate ? Wui::Tr("viewport.mode.simulate", "Simulating")
				: (play ? Wui::Tr("viewport.mode.play", "Playing")
					: Wui::Tr("viewport.mode.edit", "Edit Mode")));
		const Wui::WuiColor modeColor = paused ? theme.Warning
			: (simulate ? theme.Accent : (play ? theme.Success : theme.TextDisabled));
		const float modeWidth = ctx.MeasureTextWidth(modeText, 11.5f) + 18.0f;

		// 计算右岛理想总宽度
		float naturalRightIslandW = 4.0f;
		if (hasDataLayers)
		{
			naturalRightIslandW += layersBtnW + 3.0f + 8.0f + 3.0f;
		}
		naturalRightIslandW += gridBtnW + 3.0f;
		naturalRightIslandW += partBtnW + 3.0f;
		naturalRightIslandW += lockBtnW + 3.0f;
		const std::string snapLabel = Wui::Tr("viewport.snap.short", "Snap");
		const float snapBtnW = std::max(42.0f, ctx.MeasureTextWidth(snapLabel, 12.0f) + 14.0f);
		naturalRightIslandW += snapBtnW + 3.0f;
		naturalRightIslandW += 8.0f + 3.0f; // 分割线
		naturalRightIslandW += modeWidth;
		naturalRightIslandW += 4.0f;

		float rightIslandW = naturalRightIslandW;
		float rightIslandX = sceneRect.X + sceneRect.W - kChromePad - rightIslandW;

		// 严格受中岛右边界 pillRightBound 防护
		if (rightIslandX < pillRightBound)
		{
			rightIslandX = pillRightBound;
			rightIslandW = std::max(0.0f, (sceneRect.X + sceneRect.W - kChromePad) - rightIslandX);
		}

		const Wui::WuiRect rightIslandRect { rightIslandX, sceneRect.Y + kChromePad, rightIslandW, kIslandH };
		Wui::Toolbar(ctx, rightIslandRect, theme, 6.0f, 0.85f);
		ctx.RegisterOverlayRect(rightIslandRect);

		// 右岛内部控件由左向右整齐排布
		float rightCursor = rightIslandRect.X + 4.0f;
		const float rightBtnY = rightIslandRect.Y + 4.0f;
		const float rightLimit = rightIslandRect.X + rightIslandRect.W - 4.0f;

		// 1) 数据层多选下拉按钮
		Wui::WuiRect layersBtnRect {};
		if (hasDataLayers && rightCursor + layersBtnW <= rightLimit)
		{
			layersBtnRect = { rightCursor, rightBtnY, layersBtnW, kBtnH };
			Wui::Tooltip(ctx, layersBtnRect, Wui::Tr("viewport.layers.hint", "Manage data layer visibility."));
			if (Wui::Button(ctx, Wui::HashId("viewport.layers_dropdown"), layersBtnRect, layersLabel, theme))
			{
				ctx.OpenPopup(Wui::HashId("viewport.layers.popup"));
			}
			ctx.RegisterOverlayRect(layersBtnRect);
			rightCursor += layersBtnW + 3.0f;

			if (rightCursor + 8.0f <= rightLimit)
			{
				const Wui::WuiRect sepRect { rightCursor, rightIslandRect.Y, 8.0f, kIslandH };
				Wui::ToolbarSeparator(ctx, sepRect, theme);
				rightCursor += 8.0f + 3.0f;
			}
		}

		// 2) 网格开关 (Grid) - 开启时背景变亮 (ActiveBg + Accent 边框)，严禁带有生硬的 : ON / : OFF
		if (rightCursor + gridBtnW <= rightLimit)
		{
			const Wui::WuiRect gridBtn { rightCursor, rightBtnY, gridBtnW, kBtnH };
			const bool gridOn = m_Host.IsReferenceGridEnabled();
			Wui::Tooltip(ctx, gridBtn, Wui::Tr("viewport.grid.hint", "Toggle ground reference grid (1m/10m)."));
			auto gridTheme = theme;
			if (gridOn)
			{
				gridTheme.ButtonBg = theme.ActiveBg;
				gridTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId("viewport.stream_grid"), gridBtn, gridLabel, gridTheme))
			{
				m_Host.SetReferenceGridEnabled(!gridOn);
			}
			ctx.RegisterOverlayRect(gridBtn);
			rightCursor += gridBtnW + 3.0f;
		}

		// 3) 分区开关 (Part) - 开启时高亮，严禁带有 : ON / : OFF
		if (rightCursor + partBtnW <= rightLimit)
		{
			const Wui::WuiRect partBtn { rightCursor, rightBtnY, partBtnW, kBtnH };
			Wui::Tooltip(ctx, partBtn, Wui::Tr("viewport.partition.hint", "Toggle world partition & streaming chunk overlay."));
			auto partTheme = theme;
			if (m_ShowWorldPartition)
			{
				partTheme.ButtonBg = theme.ActiveBg;
				partTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId("viewport.partition_grid"), partBtn, partLabel, partTheme))
			{
				m_ShowWorldPartition = !m_ShowWorldPartition;
			}
			ctx.RegisterOverlayRect(partBtn);
			rightCursor += partBtnW + 3.0f;
		}

		// 4) 视角旋转锁定 (Lock) - 开启时高亮，严禁带有 : ON / : OFF
		if (rightCursor + lockBtnW <= rightLimit)
		{
			const Wui::WuiRect mode2dBtn { rightCursor, rightBtnY, lockBtnW, kBtnH };
			const bool is2D = m_Host.Is2DMode();
			Wui::Tooltip(ctx, mode2dBtn, Wui::Tr("viewport.2d.hint", "锁定/解锁鼠标旋转视角 (锁定后禁止鼠标旋转、右键拖拽变为平移；左上角方位菜单仍可正常切换方向) (viewport.btn.toggle_2d)"));
			auto lockTheme = theme;
			if (is2D)
			{
				lockTheme.ButtonBg = theme.ActiveBg;
				lockTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId("viewport.btn.toggle_2d"), mode2dBtn, lockLabel, lockTheme))
			{
				m_Host.Set2DMode(!is2D);
			}
			ctx.RegisterOverlayRect(mode2dBtn);
			rightCursor += lockBtnW + 3.0f;
		}

		// 4b) 2D 像素网格吸附开关 (Snap)
		if (rightCursor + snapBtnW <= rightLimit)
		{
			const Wui::WuiRect snapBtn { rightCursor, rightBtnY, snapBtnW, kBtnH };
			const bool isSnap = m_Host.IsPixelSnapEnabled();
			Wui::Tooltip(ctx, snapBtn, Wui::Tr("viewport.snap.hint", "2D 像素网格吸附开关 (按 1/PPU 步长吸附坐标，杜绝亚像素插值撕裂与接缝抖动) (viewport.btn.toggle_snap)"));
			auto snapTheme = theme;
			if (isSnap)
			{
				snapTheme.ButtonBg = theme.ActiveBg;
				snapTheme.Border = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId("viewport.btn.toggle_snap"), snapBtn, snapLabel, snapTheme))
			{
				m_Host.SetPixelSnapEnabled(!isSnap);
			}
			ctx.RegisterOverlayRect(snapBtn);
			rightCursor += snapBtnW + 3.0f;
		}

		// 5) 分割线
		if (rightCursor + 8.0f <= rightLimit)
		{
			const Wui::WuiRect sepRect { rightCursor, rightIslandRect.Y, 8.0f, kIslandH };
			Wui::ToolbarSeparator(ctx, sepRect, theme);
			rightCursor += 8.0f + 3.0f;
		}

		// 6) 运行状态徽章 (viewport.mode)
		if (rightCursor + modeWidth <= rightIslandRect.X + rightIslandRect.W + 2.0f)
		{
			const Wui::WuiRect modeRect { rightCursor, rightBtnY, modeWidth, kBtnH };
			Wui::WuiColor modeFill = modeColor;
			modeFill.A = modeKey[0] == 'e' ? 0.12f : 0.22f;
			Wui::Badge(ctx, modeRect, modeText, modeFill, modeColor, theme, 11.5f, true, 4.0f);
			RegisterReadonlyNode(Wui::HashId("viewport.mode"), "text", modeText, modeKey, modeRect);
			ctx.RegisterOverlayRect(modeRect);
		}

		// ④ 视图菜单弹层(下拉选项: 视图模式 Combo + 视图方位 Combo + 覆盖层开关)。
		const Wui::WuiId viewPopup = Wui::HashId("viewport.view.popup");
		if (ctx.IsPopupOpen(viewPopup))
		{
			const float rowH = 22.0f;
			const float comboH = 24.0f;
			// 精确计算弹层高度: 顶部留白(6) + 2个Combo(2*comboH + 14) + 旋转锁定项(rowH + 4) + 分割线(7) + 标题(rowH) + 3个Overlays菜单项(3*rowH) + 底部留白(6)
			const float panelH = 6.0f + (comboH * 2.0f + 14.0f) + (rowH + 4.0f) + 7.0f + rowH * 6.0f + 6.0f;
			const Wui::WuiRect panel { viewButton.X, leftIslandRect.Y + leftIslandRect.H + 3.0f, 250.0f, panelH };
			Wui::DrawPanelSurface(ctx, panel, theme);
			ctx.RegisterOverlayRect(panel);
			float rowY = panel.Y + 6.0f;

			// ① 投影方式下拉选项 (Projection: 屏幕映射方式)
			{
				const Wui::WuiRect comboRect { panel.X + 8.0f, rowY, panel.W - 16.0f, comboH };
				rowY += comboH + 6.0f;
				const std::vector<std::string> projOptions = {
					Wui::Tr("viewport.view.proj_persp", "透视 (Perspective)"),
					Wui::Tr("viewport.view.proj_ortho", "正交 (Orthographic)")
				};
				int selectedProj = (currentProj == World::ViewportProjection::Orthographic) ? 1 : 0;
				if (Wui::Combo(ctx, Wui::HashId("viewport.combo.proj"), comboRect,
					Wui::Tr("viewport.view.proj_label", "投影"), projOptions, selectedProj, theme))
				{
					m_Host.SetViewportProjection(selectedProj == 1 ? World::ViewportProjection::Orthographic : World::ViewportProjection::Perspective);
				}
				ctx.RegisterOverlayRect(comboRect);
			}

			// ② 观察方位下拉选项 (Viewpoint: 空间朝向，与投影方式完全解耦)
			{
				const Wui::WuiRect comboRect { panel.X + 8.0f, rowY, panel.W - 16.0f, comboH };
				rowY += comboH + 8.0f;
				const std::vector<std::string> vpOptions = {
					Wui::Tr("viewport.vp.free", "自由视角 (Free Orbit)"),
					Wui::Tr("viewport.vp.top", "顶视图 Top (XZ [俯视])"),
					Wui::Tr("viewport.vp.front", "前视图 Front (XY [立面])"),
					Wui::Tr("viewport.vp.right", "右视图 Right (ZY [侧面])"),
					Wui::Tr("viewport.vp.bottom", "底视图 Bottom (XZ [仰视])"),
					Wui::Tr("viewport.vp.back", "后视图 Back (XY [背面])"),
					Wui::Tr("viewport.vp.left", "左视图 Left (ZY [侧面])")
				};
				int selectedVp = 0;
				switch (currentVp)
				{
				case World::ViewportViewpoint::Free:   selectedVp = 0; break;
				case World::ViewportViewpoint::Top:    selectedVp = 1; break;
				case World::ViewportViewpoint::Front:  selectedVp = 2; break;
				case World::ViewportViewpoint::Right:  selectedVp = 3; break;
				case World::ViewportViewpoint::Bottom: selectedVp = 4; break;
				case World::ViewportViewpoint::Back:   selectedVp = 5; break;
				case World::ViewportViewpoint::Left:   selectedVp = 6; break;
				}
				if (Wui::Combo(ctx, Wui::HashId("viewport.combo.vp"), comboRect,
					Wui::Tr("viewport.view.vp_label", "方位"), vpOptions, selectedVp, theme))
				{
					World::ViewportViewpoint newVp = World::ViewportViewpoint::Free;
					switch (selectedVp)
					{
					case 0: newVp = World::ViewportViewpoint::Free; break;
					case 1: newVp = World::ViewportViewpoint::Top; break;
					case 2: newVp = World::ViewportViewpoint::Front; break;
					case 3: newVp = World::ViewportViewpoint::Right; break;
					case 4: newVp = World::ViewportViewpoint::Bottom; break;
					case 5: newVp = World::ViewportViewpoint::Back; break;
					case 6: newVp = World::ViewportViewpoint::Left; break;
					}
					m_Host.SetViewportViewpoint(newVp);
				}
				ctx.RegisterOverlayRect(comboRect);
			}



			// ③ 视角鼠标旋转锁定开关 (配合正交/透视与任意方位使用)
			{
				const Wui::WuiRect lockRow { panel.X + 4.0f, rowY, panel.W - 8.0f, rowH };
				rowY += rowH + 4.0f;
				const bool locked = m_Host.Is2DMode();
				const std::string lockTip = Wui::Tr("viewport.view.lock_rotation.tooltip",
					"锁定鼠标旋转视角 (右键拖拽改为平移画布); 左上角『方位』下拉菜单仍可正常切换方向。");
				Wui::Tooltip(ctx, lockRow, lockTip);
				if (Wui::MenuItem(ctx, Wui::HashId("viewport.view.lock_rotation"), lockRow,
					Wui::Tr("viewport.view.lock_rotation", "锁定鼠标旋转 (Lock Rotation)"), locked, true, theme))
				{
					m_Host.Set2DMode(!locked);
				}
				ctx.RegisterOverlayRect(lockRow);
			}

			// 分割线
			Wui::PanelBackground(ctx, { panel.X + 8.0f, rowY, panel.W - 16.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 6.0f;

			const auto header = [&](const std::string& text)
			{
				const Wui::WuiRect row { panel.X + 8.0f, rowY, panel.W - 16.0f, rowH };
				rowY += rowH;
				Wui::Label(ctx, { row.X, row.Y + 4.0f }, text, theme.TextMuted, 12.0f);
				RegisterReadonlyNode(Wui::HashId(("viewport.view.group." + text).c_str()), "text",
					text, std::string(), row);
			};
			const auto item = [&](const char* id, const std::string& label, bool checked,
				const std::string& tooltip, const std::function<void()>& action)
			{
				const Wui::WuiRect row { panel.X + 4.0f, rowY, panel.W - 8.0f, rowH };
				rowY += rowH;
				if (!tooltip.empty())
					Wui::Tooltip(ctx, row, tooltip);
				if (Wui::MenuItem(ctx, Wui::HashId(id), row, label, checked, true, theme))
					action();
				if (!tooltip.empty())
				{
					Wui::WuiAccessNode node;
					node.Id = Wui::HashId(id);
					node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
					node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
					node.Kind = "menu-item";
					node.Label = label;
					node.Value = checked ? "checked" : "unchecked";
					node.Tooltip = tooltip;
					node.Rect = row;
					Wui::WuiAccessibility::Get().Register(node);
				}
			};

			header(Wui::Tr("viewport.view.group.overlays", "Overlays"));
			item("viewport.view.camera_preview", Wui::Tr("viewport.view.camera_preview", "Camera Preview"),
				m_Host.IsCameraPreviewEnabled(),
				Wui::Tr("viewport.view.camera_preview.tooltip",
					"Show a picture-in-picture preview of the selected camera entity (bottom-left)."),
				[this] { m_Host.ToggleCameraPreview(); });

			const bool rangesAll = Editor::EditorPreferences::Get().Data().ViewportLightRangesAll;
			item("viewport.view.light_ranges",
				Wui::Tr("viewport.view.light_ranges", "Light Ranges: All Lights"), rangesAll,
				Wui::Tr("viewport.overlays.light_ranges.tooltip",
					"Draw light extents: point light = Range sphere, directional light = Direction arrow.\n"
					"Off = only the selected entity."),
				[] {
					Editor::EditorPreferences& prefs = Editor::EditorPreferences::Get();
					prefs.SetViewportLightRangesAll(!prefs.Data().ViewportLightRangesAll);
				});
			const bool collidersAll = Editor::EditorPreferences::Get().Data().ViewportColliderOutlinesAll;
			item("viewport.view.collider_outlines",
				Wui::Tr("viewport.view.collider_outlines", "Collider Outlines: All"), collidersAll,
				Wui::Tr("viewport.overlays.collider_outlines.tooltip",
					"Draw collider shapes in edit mode (2D box/circle, 3D box/sphere/capsule).\n"
					"Off = only the selected entity. MeshCollider3D has no analytic shape."),
				[] {
					Editor::EditorPreferences& prefs = Editor::EditorPreferences::Get();
					prefs.SetViewportColliderOutlinesAll(!prefs.Data().ViewportColliderOutlinesAll);
				});
			item("viewport.view.reference_grid",
				Wui::Tr("viewport.view.reference_grid", "Ground Grid (1m/10m)"), m_Host.IsReferenceGridEnabled(),
				Wui::Tr("viewport.grid.hint", "Toggle ground reference grid (1m/10m)."),
				[this] { m_Host.SetReferenceGridEnabled(!m_Host.IsReferenceGridEnabled()); });
			item("viewport.view.world_partition",
				Wui::Tr("viewport.view.world_partition", "World Partition Grid"), m_ShowWorldPartition,
				Wui::Tr("viewport.partition.hint", "Toggle world partition & streaming chunk overlay."),
				[this] { m_ShowWorldPartition = !m_ShowWorldPartition; });
			ctx.ClosePopupsOnOutsideClick({ viewPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(viewPopup);
		}

		// ⑤ 数据层多选弹层 (Layers Popup: 无论有多少数据层，绝不挤爆视口)
		const Wui::WuiId layersPopup = Wui::HashId("viewport.layers.popup");
		if (ctx.IsPopupOpen(layersPopup) && !dataLayers.empty())
		{
			const float rowH = 22.0f;
			const float popupW = 200.0f;
			const float popupH = 30.0f + static_cast<float>(dataLayers.size()) * rowH + 34.0f;
			const float popupX = layersBtnRect.W > 0.0f
				? std::min(layersBtnRect.X, sceneRect.X + sceneRect.W - popupW - kChromePad)
				: (sceneRect.X + sceneRect.W - popupW - kChromePad);
			const float popupY = rightIslandRect.Y + rightIslandRect.H + 3.0f;
			const Wui::WuiRect panel { popupX, popupY, popupW, popupH };
			Wui::DrawPanelSurface(ctx, panel, theme);
			ctx.RegisterOverlayRect(panel);

			float rowY = panel.Y + 6.0f;
			Wui::Label(ctx, { panel.X + 8.0f, rowY + 3.0f }, Wui::Tr("viewport.layers.title", "关卡数据层 (Data Layers)"), theme.TextMuted, 12.0f);
			rowY += 20.0f;
			Wui::PanelBackground(ctx, { panel.X + 6.0f, rowY, panel.W - 12.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 4.0f;

			for (const Level::DataLayerDefinition& layer : dataLayers)
			{
				if (layer.Name.empty())
					continue;
				const bool isOn = (layer.LayerMask & activeLayers) != 0;
				const Wui::WuiRect row { panel.X + 4.0f, rowY, panel.W - 8.0f, rowH };
				rowY += rowH;

				const std::string layerHint = Wui::TrFormat("viewport.datalayer.hint",
					"Toggle data layer '{0}' (mask {1}).", { { "name", layer.Name },
						{ "mask", std::to_string(layer.LayerMask) } });
				Wui::Tooltip(ctx, row, layerHint);

				if (Wui::MenuItem(ctx, Wui::HashId(("viewport.layer." + layer.Name).c_str()), row, layer.Name, isOn, true, theme))
				{
					const uint32_t next = isOn ? (activeLayers & ~layer.LayerMask) : (activeLayers | layer.LayerMask);
					m_Host.SetActiveDataLayers(next);
				}
			}

			// 底部快捷操作行: [全部开启] / [仅基础层]
			rowY += 2.0f;
			Wui::PanelBackground(ctx, { panel.X + 6.0f, rowY, panel.W - 12.0f, 1.0f }, theme.Border, 0.0f);
			rowY += 4.0f;
			const float btnHalfW = (panel.W - 16.0f) * 0.5f;
			const Wui::WuiRect allBtn { panel.X + 6.0f, rowY, btnHalfW, 20.0f };
			const Wui::WuiRect baseBtn { panel.X + 10.0f + btnHalfW, rowY, btnHalfW, 20.0f };
			if (Wui::Button(ctx, Wui::HashId("viewport.layers.all"), allBtn, "全部显示", theme))
			{
				uint32_t allMask = 0;
				for (const auto& l : dataLayers) allMask |= l.LayerMask;
				m_Host.SetActiveDataLayers(allMask);
			}
			if (Wui::Button(ctx, Wui::HashId("viewport.layers.base_only"), baseBtn, "仅Base", theme))
			{
				m_Host.SetActiveDataLayers(1u);
			}

			ctx.ClosePopupsOnOutsideClick({ layersPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(layersPopup);
		}
		// U22(用户 §5.5「所有视口类窗口都要有坐标系来表明方向」):主视口右下角
		// 半透明三轴指示(X 红 / Y 绿 / Z 蓝),朝向取**当前画面用的相机基** ——
		// Play 下是场景主相机、2D 下是正交相机(此时 Z 投影为点,自然只剩 X/Y)。
		{
			const Wui::GizmoCamera axisCamera = m_Host.GetGizmoCamera();
			const Wui::WuiRect axisRect = ViewChrome::DrawAxisIndicator(ctx, sceneRect,
				axisCamera.Right, axisCamera.Up);
			float axisYaw = 0.0f;
			float axisPitch = 0.0f;
			ViewChrome::BasisYawPitch(axisCamera.Forward, &axisYaw, &axisPitch);
			ViewChrome::RegisterAxisNode(axisRect, "viewport.axis",
				Wui::Tr("viewport.axis", "Viewport axes"),
				ViewChrome::AxisReadout(axisYaw, axisPitch, !m_Host.IsViewportCamera3D()),
				Wui::Tr("viewport.axis.tooltip",
					"World axes drawn in the corner of the viewport: X red, Y green, Z blue. "
					"They follow the viewport camera; in 2D orthographic views only X and Y are visible."));
			// 与其它悬浮层同一条口径:指示器下面的拾取/gizmo 不吃这一下的点击。
			ctx.RegisterOverlayRect(axisRect);
		}
		ctx.PopOverlay();

		const bool hovered = ctx.IsHovered(rect);
		if (ctx.IsClicked(rect))
			ctx.SetFocus(Wui::HashId("viewport"));
		const bool focused = ctx.Focus() == Wui::HashId("viewport");
		// 视口尺寸/边界取**场景图像区域**(sceneRect)= 整块面板(悬浮层不占布局)。
		// 拾取与 gizmo 的屏幕↔世界映射都按 sceneRect 算,所以它必须等于真正显示画面的矩形。
		glm::vec2 bounds[2] = { { sceneRect.X, sceneRect.Y },
			{ sceneRect.X + sceneRect.W, sceneRect.Y + sceneRect.H } };
		m_Host.SetViewportState(focused, hovered, { sceneRect.W, sceneRect.H }, bounds);

		// W5-L1:磁盘场景改动提示条(顶部居中)。先算矩形:它要参与"点提示条不吃实体拾取"的判定;
		// 真正绘制在 OnRender 末尾,保证盖在选中框/视锥覆盖层之上。
		const bool bannerVisible = m_Host.ExternalSceneChanged() && !m_Host.IsPlaying() && !m_Host.IsSimulating();
		const float bannerWidth = std::min(sceneRect.W - 24.0f, 470.0f);
		const Wui::WuiRect bannerRect { sceneRect.X + (sceneRect.W - bannerWidth) * 0.5f, sceneRect.Y + 10.0f,
			bannerWidth, 26.0f };
		const bool overBanner = bannerVisible && ctx.IsHovered(bannerRect);

		// 先跑 gizmo:它有"鼠标占用"语义(拖拽中/悬停在手柄上),必须优先于实体拾取 ——
		// 否则点箭头会被"点空白清空选择"吃掉,表现为"拖不动箭头"。
		bool gizmoEngaged = false;
		Entity selected = m_Host.GetSelectedEntity();
		if (selected.IsValid() && selected.GetScene() == m_Host.GetActiveScene().get() &&
			selected.HasComponent<TransformComponent>() && m_Host.HasRenderedScene())
		{
			auto& transform = selected.GetComponent<TransformComponent>();
			const TransformComponent before = transform;
			bool nowUsing = false;

			if (m_Host.IsEditingCollider())
			{
				if (selected.HasComponent<BoxCollider2DComponent>())
				{
					auto& box = selected.GetComponent<BoxCollider2DComponent>();
					nowUsing = Wui::ManipulateBoxCollider2D(m_Host.GetGizmoCamera(), transform, box.Offset, box.Size, sceneRect, ctx, !m_Host.IsReadOnlyMode());
				}
				else if (selected.HasComponent<CircleCollider2DComponent>())
				{
					auto& circle = selected.GetComponent<CircleCollider2DComponent>();
					nowUsing = Wui::ManipulateCircleCollider2D(m_Host.GetGizmoCamera(), transform, circle.Offset, circle.Radius, sceneRect, ctx, !m_Host.IsReadOnlyMode());
				}
				else if (selected.HasComponent<CapsuleCollider2DComponent>())
				{
					auto& cap = selected.GetComponent<CapsuleCollider2DComponent>();
					nowUsing = Wui::ManipulateCapsuleCollider2D(m_Host.GetGizmoCamera(), transform, cap.Offset, cap.Radius, cap.Length, cap.Direction, sceneRect, ctx, !m_Host.IsReadOnlyMode());
				}
			}
			else if (m_Host.GetGizmoOperation() == Wui::GizmoOperation::Rect)
			{
				glm::vec2 pivot { 0.5f, 0.5f };
				glm::vec2 baseSize { 1.0f, 1.0f };
				if (selected.HasComponent<SpriteComponent>())
				{
					auto& spr = selected.GetComponent<SpriteComponent>();
					nowUsing = Wui::ManipulateRect2D(m_Host.GetGizmoCamera(), transform, spr.Pivot, baseSize, sceneRect, ctx, !m_Host.IsReadOnlyMode());
				}
				else if (selected.HasComponent<TilemapComponent>())
				{
					auto& tmComp = selected.GetComponent<TilemapComponent>();
					float mapW = 16.0f, mapH = 16.0f;
					if (tmComp.Tilemap.HasPath())
					{
						const std::string tmP = StringPool::Get().PathOf(tmComp.Tilemap.Path);
						if (auto tm = Tilemap::GetOrLoad(tmP))
						{
							mapW = static_cast<float>(tm->GetWidth()) * (tmComp.CellSize.x > 0.0f ? tmComp.CellSize.x : tm->GetCellSize().x);
							mapH = static_cast<float>(tm->GetHeight()) * (tmComp.CellSize.y > 0.0f ? tmComp.CellSize.y : tm->GetCellSize().y);
						}
					}
					nowUsing = Wui::ManipulateRect2D(m_Host.GetGizmoCamera(), transform, pivot, { mapW, mapH }, sceneRect, ctx, !m_Host.IsReadOnlyMode());
				}
				else
				{
					nowUsing = Wui::ManipulateRect2D(m_Host.GetGizmoCamera(), transform, pivot, baseSize, sceneRect, ctx, !m_Host.IsReadOnlyMode());
				}
			}
			else
			{
				nowUsing = Wui::ManipulateGizmo(m_Host.GetGizmoCamera(),
					m_Host.GetGizmoOperation(), transform, sceneRect, ctx,
					/*allowManipulation=*/!m_Host.IsReadOnlyMode());
			}
			if (nowUsing && m_Host.IsPixelSnapEnabled())
			{
				const float ppu = m_Host.GetPixelSnapPpu();
				transform.Location.x = std::round(transform.Location.x * ppu) / ppu;
				transform.Location.y = std::round(transform.Location.y * ppu) / ppu;
			}
			if (selected.HasComponent<FreeformLight2DComponent>())
			{
				auto& light = selected.GetComponent<FreeformLight2DComponent>();
				const bool lightUsing = ManipulateFreeformLight2D(m_Host.GetGizmoCamera(), transform, light, sceneRect, ctx, !m_Host.IsReadOnlyMode());
				if (lightUsing) nowUsing = true;
			}
			gizmoEngaged = nowUsing;
			if (!m_GizmoActive && nowUsing)
			{
				m_GizmoActive = true;
				m_GizmoBefore = before;
			}
			else if (m_GizmoActive && !nowUsing)
			{
				m_GizmoActive = false;
				const auto& after = selected.GetComponent<TransformComponent>();
				if (after.Location != m_GizmoBefore.Location || after.Rotation != m_GizmoBefore.Rotation || after.Scale != m_GizmoBefore.Scale)
					m_Host.MarkDocumentDirty();
			}
		}
		const Wui::WuiRect previewRect = CameraPreviewRect(sceneRect);
		// 预览小窗只在**选中相机**时才有内容(CameraPreviewLabel 为空 = 没有相机预览)。
		const std::string previewLabel = m_Host.CameraPreviewLabel();
		const bool previewVisible = m_Host.IsCameraPreviewEnabled() &&
			m_Host.GetCameraPreviewTextureId() != 0 && !previewLabel.empty();
		// 相机预览小窗挡住的位置不参与拾取(避免"点预览把背后物体选走")。
		const bool overPreview = previewVisible && ctx.IsHovered(previewRect);
		if (ctx.IsClicked(sceneRect) && !m_GizmoActive && !gizmoEngaged && !overPreview && !overBanner)
		{
			const glm::vec2 local = ctx.Input().MousePos - glm::vec2 { sceneRect.X, sceneRect.Y };
			const Entity picked = m_Host.PickEntityAt(local);
			if (std::getenv("WLD_TRACE_UI"))
				WLD_CORE_INFO("[ui] viewport click local=({0},{1}) sceneRect=({2},{3},{4},{5}) -> handle={6} valid={7}",
					local.x, local.y, sceneRect.X, sceneRect.Y, sceneRect.W, sceneRect.H,
					picked.IsValid() ? static_cast<uint32_t>(static_cast<entt::entity>(picked)) : 0u,
					picked.IsValid() ? 1 : 0);
			m_Host.SetSelectedEntity(picked);
		}

		// 3D 网格实体的选中框:投影单位网格包围盒的 12 条棱(真 3D 线框盒),画在场景图之上。
		// 近的棱更实、远的棱更淡,按相机深度排序(远的先画);整体用 ClipPush 夹在场景图区域内。
		// 旧实现是 SceneRenderer 里用 Renderer2D 按实体 Transform 画的一个 2D 方块 ——
		// 在 3D 视口里看着就是"一个跟方块无关的方形,位置还不对"(用户 2026-09-16 反馈)。

		// ---- 相机可视化:选中相机的视锥线框(带方向指示) ----
		// 只有**选中相机实体**时才画(用户 2026-09-16:"编辑器中的相机范围区域也是")。
		// 方向指示:近面亮 / 远面淡 + 从相机原点指向远面中心的**中轴**与远端十字 ——
		// 正交(2D)相机因此也能一眼看出朝向(旧版只有两个矩形,用户反馈"不知道方向")。
		Entity selectedCamera;
		if (selected.IsValid() && selected.HasComponent<CameraComponent>() &&
			selected.HasComponent<TransformComponent>())
			selectedCamera = selected;
		if (m_Host.HasRenderedScene() && selectedCamera.IsValid())
		{
			const Wui::GizmoCamera frustumCamera = m_Host.GetGizmoCamera();
			const Ref<Scene> frustumScene = m_Host.GetActiveScene();
			const entt::registry& frustumRegistry = static_cast<const Scene*>(frustumScene.get())->GetRegistry();
			int frustumCount = 0;
			glm::vec2 frustumMin { 1e30f, 1e30f };
			glm::vec2 frustumMax { -1e30f, -1e30f };
			{
				// P1c-W3.4:裁剪作用域走库件 Wui::ClipScope(RAII):进口一条 ClipPush、出口一条 ClipPop
				// (与手写命令逐字段等价,含 rect),并把 ctx 的裁剪栈同进同出。本作用域里不含控件绘制,
				// 所以 ClipAllows(焦点环/条目剔除)没有可作用的调用点 —— 像素后果应当为 0。
				Wui::ClipScope frustumClip(ctx, sceneRect);
				for (const entt::entity handle : frustumRegistry.view<CameraComponent, TransformComponent>())
				{
					if (static_cast<entt::entity>(selectedCamera) != handle)
						continue;   // 只画选中的那台相机
					// PECS(相机):直接读权威参数字段;有效宽高比优先取 CameraSystem 缓存下来的
					// LastAspectRatio(与投影/拾取同一口径),没有缓存时回退到参数里的值。
					const CameraSettings& camera = frustumRegistry.get<CameraComponent>(handle).Camera;
					const auto* cameraView = frustumRegistry.try_get<CameraViewComponent>(handle);
					const float aspectRatio = (cameraView && cameraView->Valid)
						? cameraView->LastAspectRatio : camera.m_AspectRatio;
					glm::mat4 world = frustumRegistry.get<TransformComponent>(handle).GetLocalMatrix();
					if (const auto* worldTransform = frustumRegistry.try_get<WorldTransformComponent>(handle))
						world = worldTransform->Matrix;
					// 相机看向 -Z(与 glm::perspective / glm::ortho 的约定一致)。
					float hNear = 0.0f, wNear = 0.0f, hFar = 0.0f, wFar = 0.0f, zNear = 0.0f, zFar = 0.0f;
					if (camera.m_ProjectionType == CameraSettings::ProjectionType::Perspective)
					{
						const float tanHalf = std::tan(glm::radians(camera.m_PerspectiveFOV) * 0.5f);
						zNear = -camera.m_PerspectiveNearClip;
						zFar = -camera.m_PerspectiveFarClip;
						hNear = tanHalf * std::abs(zNear);
						wNear = hNear * aspectRatio;
						hFar = tanHalf * std::abs(zFar);
						wFar = hFar * aspectRatio;
					}
					else
					{
						zNear = -camera.m_OrthographicNearClip;
						zFar = -camera.m_OrthographicFarClip;
						hNear = hFar = camera.m_OrthographicZoom;
						wNear = wFar = camera.m_OrthographicZoom * aspectRatio;
					}
					const glm::vec3 corners[8] = {
						{ -wNear, -hNear, zNear }, { wNear, -hNear, zNear }, { wNear, hNear, zNear }, { -wNear, hNear, zNear },
						{ -wFar, -hFar, zFar }, { wFar, -hFar, zFar }, { wFar, hFar, zFar }, { -wFar, hFar, zFar },
					};
					glm::vec4 clip[8];
					for (int i = 0; i < 8; ++i)
						clip[i] = frustumCamera.ViewProjection * (world * glm::vec4 { corners[i], 1.0f });
					const Wui::WuiColor nearColor { 1.0f, 0.55f, 0.12f, 0.95f };   // 近面(相机所在端)
					const Wui::WuiColor farColor { 1.0f, 0.55f, 0.12f, 0.38f };    // 远面
					const Wui::WuiColor axisColor { 1.0f, 0.78f, 0.28f, 0.85f };   // 中轴/方向
					for (int i = 0; i < 4; ++i)
					{
						PushProjectedSegment(ctx, frustumCamera.ViewProjection, sceneRect, clip[i], clip[(i + 1) % 4],
							nearColor, 2.4f, &frustumMin, &frustumMax);
						PushProjectedSegment(ctx, frustumCamera.ViewProjection, sceneRect, clip[4 + i], clip[4 + (i + 1) % 4],
							farColor, 1.2f, &frustumMin, &frustumMax);
						PushProjectedSegment(ctx, frustumCamera.ViewProjection, sceneRect, clip[i], clip[4 + i],
							nearColor, 1.6f, &frustumMin, &frustumMax);
					}
					// 中轴:相机原点 → 远面中心;远端画一个小十字,远处也看得出"这是朝向哪儿"。
					const glm::vec4 eye = frustumCamera.ViewProjection * (world * glm::vec4 { 0.0f, 0.0f, 0.0f, 1.0f });
					const glm::vec4 farCenter = frustumCamera.ViewProjection * (world * glm::vec4 { 0.0f, 0.0f, zFar, 1.0f });
					PushProjectedSegment(ctx, frustumCamera.ViewProjection, sceneRect, eye, farCenter,
						axisColor, 2.2f, &frustumMin, &frustumMax);
					const float crossSize = std::max(0.08f * std::max(wFar, hFar), 0.02f);
					PushProjectedSegment(ctx, frustumCamera.ViewProjection, sceneRect,
						frustumCamera.ViewProjection * (world * glm::vec4 { crossSize, 0.0f, zFar, 1.0f }), farCenter,
						axisColor, 1.8f, &frustumMin, &frustumMax);
					PushProjectedSegment(ctx, frustumCamera.ViewProjection, sceneRect,
						frustumCamera.ViewProjection * (world * glm::vec4 { -crossSize, 0.0f, zFar, 1.0f }), farCenter,
						axisColor, 1.8f, &frustumMin, &frustumMax);
					PushProjectedSegment(ctx, frustumCamera.ViewProjection, sceneRect,
						frustumCamera.ViewProjection * (world * glm::vec4 { 0.0f, crossSize, zFar, 1.0f }), farCenter,
						axisColor, 1.8f, &frustumMin, &frustumMax);
					PushProjectedSegment(ctx, frustumCamera.ViewProjection, sceneRect,
						frustumCamera.ViewProjection * (world * glm::vec4 { 0.0f, -crossSize, zFar, 1.0f }), farCenter,
						axisColor, 1.8f, &frustumMin, &frustumMax);
					// 相机位置标记:原点 → 上方 0.25(相机本地 +Y)一小段,便于找到相机实体。
					PushProjectedSegment(ctx, frustumCamera.ViewProjection, sceneRect,
						eye,
						frustumCamera.ViewProjection * (world * glm::vec4 { 0.0f, 0.25f, 0.0f, 1.0f }),
						nearColor, 2.8f, &frustumMin, &frustumMax);
					++frustumCount;
				}
			}
			if (frustumCount > 0 && std::getenv("WLD_TRACE_UI"))
			{
				// 每 ~2 秒打一行(帧号节流):自动化/人工都能核对"视锥覆盖范围"。
				static uint64_t lastFrustumLog = ~0ull;
				const uint64_t stamp = ctx.Frame() / 120;
				if (stamp != lastFrustumLog)
				{
					lastFrustumLog = stamp;
					WLD_CORE_INFO("[ui] camera frustum count={0} bbox=({1},{2},{3},{4}) viewport=({5},{6},{7},{8})",
						frustumCount, frustumMin.x, frustumMin.y, frustumMax.x - frustumMin.x, frustumMax.y - frustumMin.y,
						sceneRect.X, sceneRect.Y, sceneRect.W, sceneRect.H);
				}
			}
		}

		// ---- P4-U6:范围可视化(光源范围 + 碰撞体轮廓) ----
		//
		// 用户 2026-09-21:「像光照这种需要标明范围的目前没有范围显示」+
		// 「应该在 view 中有个选项可以选是否显示范围;如果这个选项没开应该只画选中」。
		// 口径:开关(编辑器偏好 ▸ Viewport / 视口工具栏 Overlays)= 画全部还是只画选中;
		// 选中实体的范围**永远**画(与相机视锥一致:选中就看得见自己的影响范围)。
		{
			const Editor::EditorPreferencesData& prefs = Editor::EditorPreferences::Get().Data();
			const bool showAllLights = prefs.ViewportLightRangesAll;
			const bool showAllColliders = prefs.ViewportColliderOutlinesAll;
			const Ref<Scene> overlayScene = m_Host.GetActiveScene();
			if (overlayScene && m_Host.HasRenderedScene())
			{
				const entt::registry& registry = static_cast<const Scene*>(overlayScene.get())->GetRegistry();
				const Wui::GizmoCamera overlayCamera = m_Host.GetGizmoCamera();
				const entt::entity selectedHandle = selected.IsValid()
					&& selected.GetScene() == overlayScene.get()
					? static_cast<entt::entity>(selected) : entt::null;

				// 平行光箭头长度:按场景实体位置的包围盒对角线取 25%(下限 1.0,上限 50),
				// 这样小场景大场景都看得清;没有实体时退回 2.0。
				float sceneDiagonal = 0.0f;
				{
					glm::vec3 minPos(1e30f), maxPos(-1e30f);
					int transformCount = 0;
					for (const entt::entity handle : registry.view<TransformComponent>())
					{
						const glm::vec3 position = glm::vec3(WorldMatrixOf(registry, handle)[3]);
						minPos = glm::min(minPos, position);
						maxPos = glm::max(maxPos, position);
						++transformCount;
					}
					if (transformCount > 0)
						sceneDiagonal = glm::length(maxPos - minPos);
				}
				const float arrowLength = glm::clamp(sceneDiagonal * 0.25f, 1.0f, 50.0f);

				int lightCount = 0;
				int colliderCount = 0;
				glm::vec2 overlayMin { 1e30f, 1e30f };
				glm::vec2 overlayMax { -1e30f, -1e30f };
				{
					// P1c-W3.4:范围覆盖层(光源/碰撞体)的裁剪走库件 Wui::ClipScope(同相机视锥那条)。
					Wui::ClipScope overlayClip(ctx, sceneRect);

					// ---- 光源 ----
					for (const entt::entity handle : registry.view<TransformComponent>())
					{
						const bool isSelected = handle == selectedHandle;
						if (!isSelected && !showAllLights)
							continue;
						const glm::mat4 world = WorldMatrixOf(registry, handle);
						const float alpha = isSelected ? 0.95f : 0.45f;
						bool drew = false;
						if (const auto* point = registry.try_get<PointLightComponent>(handle))
						{
							// 点光:半径 = Range 的三个大圆(2D 视口只画 XY 那一个,避免看起来像乱线)。
							const Wui::WuiColor color { glm::clamp(point->Color.r + point->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(point->Color.g + point->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(point->Color.b + point->Intensity * 0.05f, 0.0f, 1.0f), alpha };
							if (point->Range > 0.0f)
							{
								// 用户 2026-09-21:「绘制的光源范围怎么是 2d 的,要 3d 的」→
								// 三个大圆**始终**画(2D 正交视口下它同样投影成一个球(圆环),不再降级成单圈)。
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
									point->Range, 0, color, isSelected ? 1.6f : 1.1f, &overlayMin, &overlayMax);
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
									point->Range, 1, color, isSelected ? 1.3f : 0.9f, &overlayMin, &overlayMax);
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
									point->Range, 2, color, isSelected ? 1.3f : 0.9f, &overlayMin, &overlayMax);
								drew = true;
							}
							// 位置十字(即使 Range=0 也能看到光源在哪)。
							const float cross = glm::clamp(point->Range * 0.1f, 0.05f, 0.5f);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								{ -cross, 0.0f, 0.0f }, { cross, 0.0f, 0.0f }, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								{ 0.0f, -cross, 0.0f }, { 0.0f, cross, 0.0f }, color, 2.0f, &overlayMin, &overlayMax);
							drew = true;
						}
						if (const auto* directional = registry.try_get<DirectionalLightComponent>(handle))
						{
							// 平行光:沿 Direction 的箭头 + 末端四根短射线(太阳标记)。方向是世界空间,不受实体缩放影响。
							const Wui::WuiColor color { glm::clamp(directional->Color.r + directional->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(directional->Color.g + directional->Intensity * 0.05f, 0.0f, 1.0f),
								glm::clamp(directional->Color.b + directional->Intensity * 0.05f, 0.0f, 1.0f), alpha };
							glm::vec3 direction = directional->Direction;
							if (glm::length(direction) < 1e-4f)
								direction = { 0.0f, -1.0f, 0.0f };
							direction = glm::normalize(direction);
							const float length = arrowLength;
							const glm::vec3 tip = direction * length;
							const glm::vec3 side = glm::normalize(glm::cross(direction, glm::vec3(0.0f, 1.0f, 0.0f))
								+ glm::vec3(1e-4f, 0.0f, 0.0f));
							const glm::vec3 side2 = glm::normalize(glm::cross(direction, side));
							const float wing = length * 0.12f;
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f), tip,
								color, isSelected ? 2.2f : 1.4f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								tip, tip - direction * wing + side * wing, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								tip, tip - direction * wing - side * wing, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								tip, tip - direction * wing + side2 * wing, color, 2.0f, &overlayMin, &overlayMax);
							PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
								tip, tip - direction * wing - side2 * wing, color, 2.0f, &overlayMin, &overlayMax);
							drew = true;
						}
						// AmbientLightComponent 是全局语义(没有空间范围)→ 不画,属性行 tooltip 已写明。
						if (drew)
							++lightCount;
					}

					// ---- 碰撞体轮廓(编辑期;MeshCollider3D 无解析形状,按设计不画) ----
					const Wui::WuiColor colliderColor { 0.20f, 0.90f, 0.40f, 0.55f };
					const Wui::WuiColor colliderSelected { 0.35f, 1.0f, 0.55f, 0.95f };
					for (const entt::entity handle : registry.view<TransformComponent>())
					{
						const bool isSelected = handle == selectedHandle;
						if (!isSelected && !showAllColliders)
							continue;
						// 口径:与物理**同源** —— 2D(Box2D)与 3D(Jolt)都是用实体的**本地** Transform
						// 建刚体/形状(见 Scene.cpp 的 b2MakeOffsetBox 与 Physics3D.cpp:415),所以轮廓也走本地矩阵;
						// 用世界矩阵在"有父级"的实体上会和真实碰撞体错位。
						const glm::mat4 world = registry.try_get<TransformComponent>(handle)
							? registry.get<TransformComponent>(handle).GetLocalMatrix() : glm::mat4(1.0f);
						const Wui::WuiColor color = isSelected ? colliderSelected : colliderColor;
						const float thickness = isSelected ? 1.8f : 1.1f;
						bool drew = false;
						if (const auto* box2d = registry.try_get<BoxCollider2DComponent>(handle))
						{
							// 注意:`BoxCollider2DComponent::Size` 是**半尺寸**(运行时直接喂
							// b2MakeOffsetBox(halfWidth, halfHeight, …)),不要再乘 0.5 ——
							// 乘了会把轮廓画成真实碰撞体的一半(we_engine 在 U6 复核时抓到的口径矛盾)。
							const float hx = std::max(0.0f, box2d->Size.x);
							const float hy = std::max(0.0f, box2d->Size.y);
							const glm::vec3 c { box2d->Offset.x, box2d->Offset.y, 0.0f };
							const glm::vec3 corners[4] = {
								c + glm::vec3 { -hx, -hy, 0.0f }, c + glm::vec3 { hx, -hy, 0.0f },
								c + glm::vec3 { hx, hy, 0.0f }, c + glm::vec3 { -hx, hy, 0.0f } };
							for (int i = 0; i < 4; ++i)
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									corners[i], corners[(i + 1) % 4], color, thickness, &overlayMin, &overlayMax);
							drew = true;
						}
						if (const auto* circle2d = registry.try_get<CircleCollider2DComponent>(handle))
						{
							if (circle2d->Radius > 0.0f)
								PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
									{ circle2d->Offset.x, circle2d->Offset.y, 0.0f }, circle2d->Radius, 0,
									color, thickness, &overlayMin, &overlayMax);
							drew = true;
						}
						if (const auto* box3d = registry.try_get<BoxCollider3DComponent>(handle))
						{
							// 12 条棱:两个角点索引按位异或一个轴位就得到一条棱。
							glm::vec3 corners[8];
							for (int i = 0; i < 8; ++i)
								corners[i] = box3d->Offset + glm::vec3 {
									(i & 1) ? box3d->HalfExtents.x : -box3d->HalfExtents.x,
									(i & 2) ? box3d->HalfExtents.y : -box3d->HalfExtents.y,
									(i & 4) ? box3d->HalfExtents.z : -box3d->HalfExtents.z };
							for (int i = 0; i < 8; ++i)
								for (int axis = 0; axis < 3; ++axis)
								{
									const int other = i ^ (1 << axis);
									if (other > i)
										PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
											corners[i], corners[other], color, thickness, &overlayMin, &overlayMax);
								}
							drew = true;
						}
						if (const auto* sphere = registry.try_get<SphereCollider3DComponent>(handle))
						{
							if (sphere->Radius > 0.0f)
								for (int axis = 0; axis < 3; ++axis)
									PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, sphere->Offset,
										sphere->Radius, axis, color, thickness, &overlayMin, &overlayMax);
							drew = true;
						}
						if (const auto* audioSrc = registry.try_get<AudioSourceComponent>(handle))
						{
							if (audioSrc->Spatial3D)
							{
								if (audioSrc->MinDistance > 0.0f)
								{
									const Wui::WuiColor minColor { 0.20f, 0.85f, 0.95f, 0.85f };
									for (int axis = 0; axis < 3; ++axis)
										PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
											audioSrc->MinDistance, axis, minColor, 1.2f, &overlayMin, &overlayMax);
								}
								if (audioSrc->MaxDistance > audioSrc->MinDistance)
								{
									const Wui::WuiColor maxColor { 0.35f, 0.50f, 0.95f, 0.65f };
									for (int axis = 0; axis < 3; ++axis)
										PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world, glm::vec3(0.0f),
											audioSrc->MaxDistance, axis, maxColor, 1.0f, &overlayMin, &overlayMax);
								}
								drew = true;
							}
						}
						if (const auto* capsule = registry.try_get<CapsuleCollider3DComponent>(handle))
						{
							// 近似:上下两个端面圆 + 两条母线(沿本地 Y 轴)。
							const float radius = std::max(0.0f, capsule->Radius);
							const float half = std::max(0.0f, capsule->HalfHeight);
							if (radius > 0.0f)
							{
								const glm::vec3 base = capsule->Offset;
								for (int axis = 0; axis < 3; ++axis)
								{
									PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
										base + glm::vec3 { 0.0f, half, 0.0f }, radius, axis, color, thickness,
										&overlayMin, &overlayMax);
									PushWorldCircle(ctx, overlayCamera.ViewProjection, sceneRect, world,
										base - glm::vec3 { 0.0f, half, 0.0f }, radius, axis, color, thickness,
										&overlayMin, &overlayMax);
								}
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									base + glm::vec3 { radius, -half, 0.0f }, base + glm::vec3 { radius, half, 0.0f },
									color, thickness, &overlayMin, &overlayMax);
								PushWorldSegment(ctx, overlayCamera.ViewProjection, sceneRect, world,
									base + glm::vec3 { -radius, -half, 0.0f }, base + glm::vec3 { -radius, half, 0.0f },
									color, thickness, &overlayMin, &overlayMax);
							}
							drew = true;
						}
						if (drew)
							++colliderCount;
					}

				}
				// 取证(与相机视锥同口径):脚本可断言"画了几个、覆盖到哪",不靠像素。
				if ((lightCount > 0 || colliderCount > 0) && std::getenv("WLD_TRACE_UI"))
				{
					static uint64_t lastOverlayLog = ~0ull;
					const uint64_t stamp = ctx.Frame() / 120;
					if (stamp != lastOverlayLog)
					{
						lastOverlayLog = stamp;
						WLD_CORE_INFO("[ui] overlays lights={0} colliders={1} allLights={2} allColliders={3} "
							"bbox=({4},{5},{6},{7})",
							lightCount, colliderCount, showAllLights ? 1 : 0, showAllColliders ? 1 : 0,
							overlayMin.x, overlayMin.y, overlayMax.x - overlayMin.x, overlayMax.y - overlayMin.y);
					}
				}
			}
		}

		if (selected.IsValid() && selected.HasComponent<MeshRendererComponent>() &&
			selected.HasComponent<TransformComponent>() && m_Host.HasRenderedScene())
		{
			const Wui::GizmoCamera gizmoCamera = m_Host.GetGizmoCamera();
			glm::mat4 world = selected.GetComponent<TransformComponent>().GetLocalMatrix();
			if (selected.HasComponent<WorldTransformComponent>())
				world = selected.GetComponent<WorldTransformComponent>().Matrix;
			const bool plane = selected.GetComponent<MeshRendererComponent>().Primitive == MeshRendererComponent::PrimitiveShape::Plane;

			// 单位网格角点: bit0 = X、bit1 = Y、bit2 = Z; plane 只有 y = 0 的一层
			glm::vec4 clipPos[8];
			for (int i = 0; i < 8; ++i)
			{
				const glm::vec3 corner { (i & 1) ? 0.5f : -0.5f, plane ? 0.0f : ((i & 2) ? 0.5f : -0.5f),
					(i & 4) ? 0.5f : -0.5f };
				clipPos[i] = gizmoCamera.ViewProjection * (world * glm::vec4 { corner, 1.0f });
			}

			// 面朝向判定:只画"至少依附一个朝向相机的面"的棱 —— 物体后面的棱不再出现
			const glm::mat3 normalMatrix = glm::mat3(world);
			const glm::vec3 faceNormals[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
				{ 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
			const glm::vec3 faceCenters[6] = { { 0.5f, 0, 0 }, { -0.5f, 0, 0 }, { 0, 0.5f, 0 },
				{ 0, -0.5f, 0 }, { 0, 0, 0.5f }, { 0, 0, -0.5f } };
			bool faceVisible[6] = {};
			for (int i = 0; i < 6; ++i)
			{
				const glm::vec3 centerWorld = glm::vec3(world * glm::vec4 { faceCenters[i], 1.0f });
				const glm::vec3 normalWorld = glm::normalize(normalMatrix * faceNormals[i]);
				faceVisible[i] = glm::dot(normalWorld, gizmoCamera.Position - centerWorld) > 0.0f;
			}

			const Wui::WuiColor selectOutlineColor { 1.0f, 0.55f, 0.12f, 1.0f };
			Wui::ClipScope selectionClip(ctx, sceneRect);

			if (plane)
			{
				// 平面 4 条边: {0,1}, {1,5}, {5,4}, {4,0} (走 Liang-Barsky 严格几何裁剪, 绝无折断飞线与除零畸变)
				const int planeEdges[4][2] = { { 0, 1 }, { 1, 5 }, { 5, 4 }, { 4, 0 } };
				for (const auto& edge : planeEdges)
				{
					PushProjectedSegment(ctx, gizmoCamera.ViewProjection, sceneRect,
						clipPos[edge[0]], clipPos[edge[1]], selectOutlineColor, 1.8f);
				}
			}
			else
			{
				// 12 条棱 + 每条棱的两个相邻面
				const int cubeEdges[12][4] = {
					{ 0, 1, 3, 5 }, { 2, 3, 2, 5 }, { 4, 5, 3, 4 }, { 6, 7, 2, 4 },   // 沿 X
					{ 0, 2, 1, 5 }, { 1, 3, 0, 5 }, { 4, 6, 1, 4 }, { 5, 7, 0, 4 },   // 沿 Y
					{ 0, 4, 1, 3 }, { 1, 5, 0, 3 }, { 2, 6, 1, 2 }, { 3, 7, 0, 2 },   // 沿 Z
				};
				for (const auto& edge : cubeEdges)
				{
					if (faceVisible[edge[2]] || faceVisible[edge[3]])
					{
						PushProjectedSegment(ctx, gizmoCamera.ViewProjection, sceneRect,
							clipPos[edge[0]], clipPos[edge[1]], selectOutlineColor, 1.8f);
					}
				}
			}
		}

		// ---- 双轨制视口辅助系统: 轨道一 (地面参考标尺 1m/10m) + 轨道二 (世界分区宏观沙盘 64m) ----
		const Wui::GizmoCamera levelCam = m_Host.GetGizmoCamera();
		const glm::vec3 camPos = levelCam.Position;
		// 复用前面已获取的 currentVp, currentProj, isOrtho

		// ==================== 轨道一：场景地面参考网格 (Reference Grid) ====================
		// 1. Free 自由漫游、Top 顶视、Bottom 底视下：地面位于 XZ 平面，由 SceneRenderer 原生 GPU 深度通道渲染，保证 100% 铺满视口、绝无半截黑屏截断；
		// 2. 纯立面视角 (Front/Back/Right/Left) 下：视线正对立面，由 WUI 在 XY 或 ZY 竖直平面绘制铺满视口的参考坐标图纸。
		const bool showGrid = m_Host.IsReferenceGridEnabled();
		const bool isElevationView = (currentVp == World::ViewportViewpoint::Front || currentVp == World::ViewportViewpoint::Back ||
			currentVp == World::ViewportViewpoint::Right || currentVp == World::ViewportViewpoint::Left);
		if (showGrid && isOrtho && isElevationView)
		{
			Wui::ClipScope gridClip(ctx, sceneRect);
			const float orthoSpan = std::max(0.1f, levelCam.OrthographicSize);

			const float worldPerPixel = orthoSpan / std::max(1.0f, sceneRect.H);
			const float targetStep = worldPerPixel * 75.0f;
			const float exponent = std::floor(std::log10(std::max(1e-4f, targetStep)));
			const float baseStep = std::pow(10.0f, exponent);
			const float fracStep = targetStep / baseStep;
			const float cellSize = (fracStep >= 5.0f) ? (baseStep * 5.0f) : ((fracStep >= 2.0f) ? (baseStep * 2.0f) : baseStep);

			const glm::mat4 invVP = glm::inverse(levelCam.ViewProjection);
			const glm::vec4 ndcCorners[4] = {
				{ -1.0f, -1.0f, 0.0f, 1.0f },
				{  1.0f, -1.0f, 0.0f, 1.0f },
				{  1.0f,  1.0f, 0.0f, 1.0f },
				{ -1.0f,  1.0f, 0.0f, 1.0f }
			};
			glm::vec3 wc[4];
			for (int i = 0; i < 4; ++i)
			{
				const glm::vec4 p = invVP * ndcCorners[i];
				wc[i] = glm::vec3(p) / (std::abs(p.w) > 1e-6f ? p.w : 1.0f);
			}

			const Wui::WuiColor minorColor { 0.45f, 0.52f, 0.62f, 0.22f };
			const Wui::WuiColor majorColor { 0.55f, 0.65f, 0.78f, 0.40f };
			const Wui::WuiColor xAxisColor { 0.68f, 0.38f, 0.38f, 0.55f };
			const Wui::WuiColor yAxisColor { 0.38f, 0.68f, 0.38f, 0.55f };
			const Wui::WuiColor zAxisColor { 0.38f, 0.48f, 0.70f, 0.55f };

			if (currentVp == World::ViewportViewpoint::Front || currentVp == World::ViewportViewpoint::Back)
			{
				// XY 立面参考网格: 水平 X, 垂直 Y
				const float rawMinX = std::min({ wc[0].x, wc[1].x, wc[2].x, wc[3].x });
				const float rawMaxX = std::max({ wc[0].x, wc[1].x, wc[2].x, wc[3].x });
				const float rawMinY = std::min({ wc[0].y, wc[1].y, wc[2].y, wc[3].y });
				const float rawMaxY = std::max({ wc[0].y, wc[1].y, wc[2].y, wc[3].y });

				const float minX = std::floor((rawMinX - cellSize * 2.0f) / cellSize) * cellSize;
				const float maxX = std::ceil((rawMaxX + cellSize * 2.0f) / cellSize) * cellSize;
				const float minY = std::floor((rawMinY - cellSize * 2.0f) / cellSize) * cellSize;
				const float maxY = std::ceil((rawMaxY + cellSize * 2.0f) / cellSize) * cellSize;

				for (float x = minX; x <= maxX + 0.01f; x += cellSize)
				{
					const bool isOrigin = std::abs(x) < (cellSize * 0.25f);
					const bool isMajor = std::fmod(std::abs(x), cellSize * 5.0f) < (cellSize * 0.25f);
					PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
						levelCam.ViewProjection * glm::vec4(x, minY, 0.0f, 1.0f),
						levelCam.ViewProjection * glm::vec4(x, maxY, 0.0f, 1.0f),
						isOrigin ? yAxisColor : (isMajor ? majorColor : minorColor), isOrigin ? 1.8f : 1.0f);
				}
				for (float y = minY; y <= maxY + 0.01f; y += cellSize)
				{
					const bool isOrigin = std::abs(y) < (cellSize * 0.25f);
					const bool isMajor = std::fmod(std::abs(y), cellSize * 5.0f) < (cellSize * 0.25f);
					PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
						levelCam.ViewProjection * glm::vec4(minX, y, 0.0f, 1.0f),
						levelCam.ViewProjection * glm::vec4(maxX, y, 0.0f, 1.0f),
						isOrigin ? xAxisColor : (isMajor ? majorColor : minorColor), isOrigin ? 1.8f : 1.0f);
				}
			}
			else if (currentVp == World::ViewportViewpoint::Right || currentVp == World::ViewportViewpoint::Left)
			{
				// ZY 立面参考网格: 水平 Z, 垂直 Y
				const float rawMinZ = std::min({ wc[0].z, wc[1].z, wc[2].z, wc[3].z });
				const float rawMaxZ = std::max({ wc[0].z, wc[1].z, wc[2].z, wc[3].z });
				const float rawMinY = std::min({ wc[0].y, wc[1].y, wc[2].y, wc[3].y });
				const float rawMaxY = std::max({ wc[0].y, wc[1].y, wc[2].y, wc[3].y });

				const float minZ = std::floor((rawMinZ - cellSize * 2.0f) / cellSize) * cellSize;
				const float maxZ = std::ceil((rawMaxZ + cellSize * 2.0f) / cellSize) * cellSize;
				const float minY = std::floor((rawMinY - cellSize * 2.0f) / cellSize) * cellSize;
				const float maxY = std::ceil((rawMaxY + cellSize * 2.0f) / cellSize) * cellSize;

				for (float z = minZ; z <= maxZ + 0.01f; z += cellSize)
				{
					const bool isOrigin = std::abs(z) < (cellSize * 0.25f);
					const bool isMajor = std::fmod(std::abs(z), cellSize * 5.0f) < (cellSize * 0.25f);
					PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
						levelCam.ViewProjection * glm::vec4(0.0f, minY, z, 1.0f),
						levelCam.ViewProjection * glm::vec4(0.0f, maxY, z, 1.0f),
						isOrigin ? yAxisColor : (isMajor ? majorColor : minorColor), isOrigin ? 1.8f : 1.0f);
				}
				for (float y = minY; y <= maxY + 0.01f; y += cellSize)
				{
					const bool isOrigin = std::abs(y) < (cellSize * 0.25f);
					const bool isMajor = std::fmod(std::abs(y), cellSize * 5.0f) < (cellSize * 0.25f);
					PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
						levelCam.ViewProjection * glm::vec4(0.0f, y, minZ, 1.0f),
						levelCam.ViewProjection * glm::vec4(0.0f, y, maxZ, 1.0f),
						isOrigin ? zAxisColor : (isMajor ? majorColor : minorColor), isOrigin ? 1.8f : 1.0f);
				}
			}
		}

		// 视口右上角 HUD 基础数据声明
		size_t entityCount = 0;
		if (const Ref<Scene> gridScene = m_Host.GetActiveScene())
		{
			const entt::registry& gridRegistry = std::as_const(*gridScene).GetRegistry();
			entityCount = gridRegistry.view<UUIDComponent>().size();
		}
		char hudLine1[160] = {};
		char hudLine2[160] = {};

		// ==================== 轨道二：世界分区系统 (World Partition / Streaming Grid) ====================
		int32_t activeChunkX = 0;
		int32_t activeChunkY = 0;
		int32_t activeChunkZ = 0;
		size_t authoredChunkCount = 0;
		if (m_ShowWorldPartition)
		{
			const Level::LevelPartitionSettings partSettings = m_Host.GetLevelPartitionSettings();
			const float chunkSize = static_cast<float>(partSettings.GridCellSize > 0 ? partSettings.GridCellSize : 64);
			const Level::PartitionGridPlane planeMode = partSettings.GridPlane;

			activeChunkX = static_cast<int32_t>(std::floor(camPos.x / chunkSize));
			activeChunkY = static_cast<int32_t>(std::floor(camPos.y / chunkSize));
			activeChunkZ = static_cast<int32_t>(std::floor(camPos.z / chunkSize));

			// 统计当前关卡在各 Chunk 的实体分布热度
			if (const Ref<Scene> activeScene = m_Host.GetActiveScene())
			{
				std::unordered_set<int64_t> chunkSet;
				const entt::registry& reg = std::as_const(*activeScene).GetRegistry();
				auto transformView = reg.view<TransformComponent>();
				for (auto entity : transformView)
				{
					const auto& tc = transformView.get<TransformComponent>(entity);
					int32_t cA = static_cast<int32_t>(std::floor(tc.Location.x / chunkSize));
					int32_t cB = static_cast<int32_t>(std::floor(tc.Location.z / chunkSize));
					if (planeMode == Level::PartitionGridPlane::VerticalXY)
					{
						cA = static_cast<int32_t>(std::floor(tc.Location.x / chunkSize));
						cB = static_cast<int32_t>(std::floor(tc.Location.y / chunkSize));
					}
					else if (planeMode == Level::PartitionGridPlane::VerticalZY)
					{
						cA = static_cast<int32_t>(std::floor(tc.Location.z / chunkSize));
						cB = static_cast<int32_t>(std::floor(tc.Location.y / chunkSize));
					}
					const int64_t key = (static_cast<int64_t>(cA) << 32) ^ static_cast<uint32_t>(cB);
					chunkSet.insert(key);
				}
				authoredChunkCount = chunkSet.size();
			}

			const float viewSpan = isOrtho ? levelCam.OrthographicSize : std::max(20.0f, levelCam.Distance);
			const float proximityWeight = isOrtho ? 1.0f : std::clamp((viewSpan - 40.0f) / 60.0f, 0.0f, 1.0f);

			if (proximityWeight > 0.01f)
			{
				Wui::ClipScope partClip(ctx, sceneRect);
				const int32_t gridRadius = std::clamp(static_cast<int32_t>(std::ceil((viewSpan * 1.5f) / chunkSize)), 4, 16);
				const Wui::WuiColor chunkBorderColor { 0.40f, 0.50f, 0.65f, 0.22f * proximityWeight };

				// 1. 全局大世界空间分区棋盘 (支持 XZ / XY / ZY / Isometric / Full3D)
				for (int32_t da = -gridRadius; da <= gridRadius; ++da)
				{
					for (int32_t db = -gridRadius; db <= gridRadius; ++db)
					{
						const float distSq = static_cast<float>(da * da + db * db);
						const float normDist = std::sqrt(distSq) / static_cast<float>(gridRadius);
						const float fadeAlpha = std::clamp(1.0f - normDist * normDist, 0.0f, 1.0f);
						if (fadeAlpha <= 0.02f)
							continue;

						Wui::WuiColor borderColor = chunkBorderColor;
						borderColor.A *= fadeAlpha;

						if (planeMode == Level::PartitionGridPlane::VerticalXY)
						{
							const float minX = (activeChunkX + da) * chunkSize;
							const float minY = (activeChunkY + db) * chunkSize;
							const float maxX = minX + chunkSize;
							const float maxY = minY + chunkSize;
							const glm::vec3 corners[4] = {
								{ minX, minY, 0.05f }, { maxX, minY, 0.05f },
								{ maxX, maxY, 0.05f }, { minX, maxY, 0.05f }
							};
							for (int i = 0; i < 4; ++i)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(corners[i], 1.0f),
									levelCam.ViewProjection * glm::vec4(corners[(i + 1) % 4], 1.0f),
									borderColor, 1.0f);
						}
						else if (planeMode == Level::PartitionGridPlane::VerticalZY)
						{
							const float minZ = (activeChunkZ + da) * chunkSize;
							const float minY = (activeChunkY + db) * chunkSize;
							const float maxZ = minZ + chunkSize;
							const float maxY = minY + chunkSize;
							const glm::vec3 corners[4] = {
								{ 0.05f, minY, minZ }, { 0.05f, minY, maxZ },
								{ 0.05f, maxY, maxZ }, { 0.05f, maxY, minZ }
							};
							for (int i = 0; i < 4; ++i)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(corners[i], 1.0f),
									levelCam.ViewProjection * glm::vec4(corners[(i + 1) % 4], 1.0f),
									borderColor, 1.0f);
						}
						else if (planeMode == Level::PartitionGridPlane::Isometric)
						{
							const float ca = (activeChunkX + da) * chunkSize;
							const float cb = (activeChunkZ + db) * chunkSize;
							const glm::vec3 corners[4] = {
								{ (ca) * 0.7071f + (cb) * 0.7071f, 0.05f, (ca) * 0.7071f - (cb) * 0.7071f },
								{ (ca + chunkSize) * 0.7071f + (cb) * 0.7071f, 0.05f, (ca + chunkSize) * 0.7071f - (cb) * 0.7071f },
								{ (ca + chunkSize) * 0.7071f + (cb + chunkSize) * 0.7071f, 0.05f, (ca + chunkSize) * 0.7071f - (cb + chunkSize) * 0.7071f },
								{ (ca) * 0.7071f + (cb + chunkSize) * 0.7071f, 0.05f, (ca) * 0.7071f - (cb + chunkSize) * 0.7071f },
							};
							for (int i = 0; i < 4; ++i)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(corners[i], 1.0f),
									levelCam.ViewProjection * glm::vec4(corners[(i + 1) % 4], 1.0f),
									borderColor, 1.0f);
						}
						else if (planeMode == Level::PartitionGridPlane::Full3D)
						{
							const float minX = (activeChunkX + da) * chunkSize;
							const float minZ = (activeChunkZ + db) * chunkSize;
							const float maxX = minX + chunkSize;
							const float maxZ = minZ + chunkSize;
							const float minY = 0.0f;
							const float maxY = chunkSize;
							const glm::vec3 bCorners[8] = {
								{ minX, minY, minZ }, { maxX, minY, minZ }, { maxX, minY, maxZ }, { minX, minY, maxZ },
								{ minX, maxY, minZ }, { maxX, maxY, minZ }, { maxX, maxY, maxZ }, { minX, maxY, maxZ },
							};
							const int bEdges[12][2] = {
								{0,1}, {1,2}, {2,3}, {3,0}, {4,5}, {5,6}, {6,7}, {7,4},
								{0,4}, {1,5}, {2,6}, {3,7}
							};
							for (int e = 0; e < 12; ++e)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(bCorners[bEdges[e][0]], 1.0f),
									levelCam.ViewProjection * glm::vec4(bCorners[bEdges[e][1]], 1.0f),
									borderColor, 1.0f);
						}
						else // HorizontalXZ
						{
							const float minX = (activeChunkX + da) * chunkSize;
							const float minZ = (activeChunkZ + db) * chunkSize;
							const float maxX = minX + chunkSize;
							const float maxZ = minZ + chunkSize;
							const glm::vec3 corners[4] = {
								{ minX, 0.05f, minZ }, { maxX, 0.05f, minZ },
								{ maxX, 0.05f, maxZ }, { minX, 0.05f, maxZ }
							};
							for (int i = 0; i < 4; ++i)
								PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
									levelCam.ViewProjection * glm::vec4(corners[i], 1.0f),
									levelCam.ViewProjection * glm::vec4(corners[(i + 1) % 4], 1.0f),
									borderColor, 1.0f);
						}
					}
				}

				// 2. 特定区域局部覆盖呈现 (PartitionVolumeComponent 3D包围盒与细网格)
				if (const Ref<Scene> activeScene = m_Host.GetActiveScene())
				{
					const entt::registry& reg = std::as_const(*activeScene).GetRegistry();
					auto volumeView = reg.view<PartitionVolumeComponent, TransformComponent>();
					for (auto entity : volumeView)
					{
						const auto& pvc = volumeView.get<PartitionVolumeComponent>(entity);
						const auto& tc = volumeView.get<TransformComponent>(entity);
						if (!pvc.Active)
							continue;

						const glm::vec3 center = tc.Location;
						const glm::vec3 h = pvc.HalfExtents;
						const Wui::WuiColor volumeBoxColor { 0.95f, 0.72f, 0.28f, 0.65f };

						const glm::vec3 bCorners[8] = {
							center + glm::vec3(-h.x, -h.y, -h.z),
							center + glm::vec3( h.x, -h.y, -h.z),
							center + glm::vec3( h.x, -h.y,  h.z),
							center + glm::vec3(-h.x, -h.y,  h.z),
							center + glm::vec3(-h.x,  h.y, -h.z),
							center + glm::vec3( h.x,  h.y, -h.z),
							center + glm::vec3( h.x,  h.y,  h.z),
							center + glm::vec3(-h.x,  h.y,  h.z),
						};
						const int edges[12][2] = {
							{0,1}, {1,2}, {2,3}, {3,0},
							{4,5}, {5,6}, {6,7}, {7,4},
							{0,4}, {1,5}, {2,6}, {3,7}
						};
						for (int e = 0; e < 12; ++e)
						{
							PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
								levelCam.ViewProjection * glm::vec4(bCorners[edges[e][0]], 1.0f),
								levelCam.ViewProjection * glm::vec4(bCorners[edges[e][1]], 1.0f),
								volumeBoxColor, 1.8f);
						}

						// 盒内地表细分网格呈现
						const float subCell = static_cast<float>(pvc.OverrideCellSize > 0 ? pvc.OverrideCellSize : 16);
						const Wui::WuiColor subGridColor { 0.95f, 0.80f, 0.35f, 0.28f };
						const float startX = std::floor((center.x - h.x) / subCell) * subCell;
						const float endX = std::ceil((center.x + h.x) / subCell) * subCell;
						const float startZ = std::floor((center.z - h.z) / subCell) * subCell;
						const float endZ = std::ceil((center.z + h.z) / subCell) * subCell;

						for (float x = startX; x <= endX; x += subCell)
						{
							PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
								levelCam.ViewProjection * glm::vec4(x, center.y - h.y + 0.05f, center.z - h.z, 1.0f),
								levelCam.ViewProjection * glm::vec4(x, center.y - h.y + 0.05f, center.z + h.z, 1.0f),
								subGridColor, 1.2f);
						}
						for (float z = startZ; z <= endZ; z += subCell)
						{
							PushProjectedSegment(ctx, levelCam.ViewProjection, sceneRect,
								levelCam.ViewProjection * glm::vec4(center.x - h.x, center.y - h.y + 0.05f, z, 1.0f),
								levelCam.ViewProjection * glm::vec4(center.x + h.x, center.y - h.y + 0.05f, z, 1.0f),
								subGridColor, 1.2f);
						}
					}
				}
			}

			const char* planeTag = "XZ";
			if (planeMode == Level::PartitionGridPlane::VerticalXY) planeTag = "XY";
			else if (planeMode == Level::PartitionGridPlane::VerticalZY) planeTag = "ZY";
			else if (planeMode == Level::PartitionGridPlane::Isometric) planeTag = "ISO";
			else if (planeMode == Level::PartitionGridPlane::Full3D) planeTag = "3D";

			std::snprintf(hudLine1, sizeof(hudLine1),
				"Partition %um [%s] | Chunk (%d, %d) | Authored: %zu Chunks",
				partSettings.GridCellSize, planeTag,
				activeChunkX, (planeMode == Level::PartitionGridPlane::VerticalXY || planeMode == Level::PartitionGridPlane::VerticalZY) ? activeChunkY : activeChunkZ, authoredChunkCount);

			if (m_Host.IsFlyNavigating() || m_Host.GetFlySpeedTimer() > 0.0f)
			{
				char flyInfo[48] = {};
				const float curSpeed = 12.0f * m_Host.GetFlySpeedScale();
				std::snprintf(flyInfo, sizeof(flyInfo), " | Fly: %.1f m/s (%.1fx)", curSpeed, m_Host.GetFlySpeedScale());
				std::strncat(hudLine1, flyInfo, sizeof(hudLine1) - std::strlen(hudLine1) - 1);
			}
		}
		else if (m_ShowReferenceGrid)
		{
			const char* axisTag = "Free (XZ)";
			switch (currentVp)
			{
			case World::ViewportViewpoint::Free:   axisTag = "Free (XZ)"; break;
			case World::ViewportViewpoint::Top:    axisTag = "Top (XZ)"; break;
			case World::ViewportViewpoint::Bottom: axisTag = "Bottom (XZ)"; break;
			case World::ViewportViewpoint::Front:  axisTag = "Front (XY)"; break;
			case World::ViewportViewpoint::Back:   axisTag = "Back (XY)"; break;
			case World::ViewportViewpoint::Right:  axisTag = "Right (ZY)"; break;
			case World::ViewportViewpoint::Left:   axisTag = "Left (ZY)"; break;
			}
			const float scaleValue = isOrtho ? levelCam.OrthographicSize : levelCam.Distance;
			const char* gridStepLabel = "Grid 1m/10m";
			if (scaleValue > 300.0f)
				gridStepLabel = "Grid 50m/100m";
			else if (scaleValue > 60.0f)
				gridStepLabel = "Grid 10m/50m";

			std::snprintf(hudLine1, sizeof(hudLine1),
				"%s | %s | Scale: %.1fu", gridStepLabel, axisTag, scaleValue);

			if (m_Host.IsFlyNavigating() || m_Host.GetFlySpeedTimer() > 0.0f)
			{
				char flyInfo[48] = {};
				const float curSpeed = 12.0f * m_Host.GetFlySpeedScale();
				std::snprintf(flyInfo, sizeof(flyInfo), " | Fly: %.1f m/s (%.1fx)", curSpeed, m_Host.GetFlySpeedScale());
				std::strncat(hudLine1, flyInfo, sizeof(hudLine1) - std::strlen(hudLine1) - 1);
			}
		}
			std::string layersText;
			for (const Level::DataLayerDefinition& layer : m_Host.GetLevelDataLayers())
			{
				if (layer.Name.empty())
					continue;
				if ((layer.LayerMask & m_Host.GetActiveDataLayers()) == 0)
					continue;
				if (!layersText.empty())
					layersText += "+";
				layersText += layer.Name;
			}
			if (layersText.empty())
				layersText = "all";
			std::snprintf(hudLine2, sizeof(hudLine2),
				"Layers: [%s] | Entities: %zu", layersText.c_str(), entityCount);

		// ⑤ 视口状态信息卡片下沉至右下角 (避免遮挡顶部工具栏与 3D AxisGizmo 坐标系)
		const bool hasHudData = (hudLine1[0] != '\0' || entityCount > 0 || !m_Host.GetLevelDataLayers().empty());
		if (hasHudData)
		{
			const float text1W = ctx.MeasureTextWidth(hudLine1, 11.5f);
			const float text2W = ctx.MeasureTextWidth(hudLine2, 11.5f);
			const float hudW = std::max(220.0f, std::max(text1W, text2W) + 20.0f);
			const float hudH = 42.0f;

			// 避让右下角 48px 坐标轴指示器 (AxisGizmo), 视口宽裕时留出 56px 避让空间
			const float axisAvoid = (sceneRect.W >= 420.0f) ? 56.0f : 0.0f;
			const Wui::WuiRect hudRect {
				sceneRect.X + sceneRect.W - hudW - 12.0f - axisAvoid,
				sceneRect.Y + sceneRect.H - hudH - 12.0f,
				hudW,
				hudH
			};

			Wui::PanelBackground(ctx, hudRect, Wui::WuiColor { 0.08f, 0.10f, 0.14f, 0.85f }, 6.0f);
			Wui::HighlightOutline(ctx, hudRect, Wui::WuiColor { 0.20f, 0.25f, 0.32f, 0.70f }, 6.0f, 1.0f);

			if (hudLine1[0] != '\0')
			{
				Wui::Label(ctx, { hudRect.X + 10.0f, hudRect.Y + 4.0f }, hudLine1,
					Wui::WuiColor { 0.38f, 0.82f, 0.95f, 0.95f }, 11.5f);
				Wui::Label(ctx, { hudRect.X + 10.0f, hudRect.Y + 22.0f }, hudLine2,
					Wui::WuiColor { 0.75f, 0.78f, 0.84f, 0.90f }, 11.5f);
			}
			else
			{
				Wui::Label(ctx, { hudRect.X + 10.0f, hudRect.Y + 12.0f }, hudLine2,
					Wui::WuiColor { 0.75f, 0.78f, 0.84f, 0.90f }, 11.5f);
			}

			ctx.RegisterOverlayRect(hudRect);
		}

		// ⑥ 飞行漫游实时速度浮动胶囊 (对标 UE5 Camera Speed Floating Badge, 居中高亮醒目呈现)
		if (m_Host.IsFlyNavigating() || m_Host.GetFlySpeedTimer() > 0.0f)
		{
			const float speedScale = m_Host.GetFlySpeedScale();
			const float curSpeed = 12.0f * speedScale;
			char speedText[64] = {};
			std::snprintf(speedText, sizeof(speedText), "飞行速度: %.1f m/s  (%.1fx)", curSpeed, speedScale);

			const float badgeTextW = ctx.MeasureTextWidth(speedText, 13.0f);
			const float badgeW = badgeTextW + 36.0f;
			const float badgeH = 28.0f;
			const Wui::WuiRect badgeRect {
				sceneRect.X + (sceneRect.W - badgeW) * 0.5f,
				sceneRect.Y + kChromePad + kPillH + 8.0f,
				badgeW,
				badgeH
			};

			const Wui::WuiColor badgeBg { 0.06f, 0.10f, 0.16f, 0.92f };
			const Wui::WuiColor badgeBorder { 0.20f, 0.85f, 0.90f, 0.85f };
			const Wui::WuiColor badgeTextCol { 0.35f, 0.92f, 1.0f, 1.0f };

			Wui::PanelBackground(ctx, badgeRect, badgeBg, 14.0f);
			Wui::HighlightOutline(ctx, badgeRect, badgeBorder, 14.0f, 1.2f);
			Wui::Label(ctx, { badgeRect.X + 18.0f, badgeRect.Y + 7.0f }, speedText, badgeTextCol, 13.0f);
			ctx.RegisterOverlayRect(badgeRect);
		}

		// ---- 相机预览小窗(PiP):直接显示"场景相机看到的东西" ----
		//
		// 物理调试线框(P1b D6)也画在这一段之前:P4-U4 起开关有两处来源 ——
		// 场景头 `World.physics_debug`(设置面板可改、随场景保存)或环境变量
		// `WLD_PHYSICS_DEBUG`(自动化/一次性诊断,保持向后兼容)。
		const Ref<Scene> debugToggleScene = m_Host.GetActiveScene();
		const bool physicsDebugEnabled = std::getenv("WLD_PHYSICS_DEBUG") != nullptr
			|| (debugToggleScene && debugToggleScene->GetWorldSettings().PhysicsDebug);
		if (physicsDebugEnabled)
		{
			// P1b D6:3D 物理调试线框(碰撞体世界空间线段,复用视锥那套投影/裁剪)。
			// 只在世界已启动(Play/Simulate)时存在;缓冲按帧复用,避免每帧分配。
			const Ref<Scene> physicsScene = m_Host.GetActiveScene();
			if (physicsScene && physicsScene->IsPhysics3DRunning())
			{
				static std::vector<DebugLine> physicsLines;
				physicsLines.clear();
				physicsScene->GetPhysics3DWorld()->CollectDebugLines(physicsLines);
				if (!physicsLines.empty())
				{
					const Wui::GizmoCamera debugCamera = m_Host.GetGizmoCamera();
					const Wui::WuiColor physicsColor { 0.20f, 0.90f, 0.40f, 0.90f };
					{
						// P1c-W3.4:物理调试线框的裁剪走库件 Wui::ClipScope。
						Wui::ClipScope physicsClip(ctx, sceneRect);
						for (const DebugLine& line : physicsLines)
						{
							PushProjectedSegment(ctx, debugCamera.ViewProjection, sceneRect,
								debugCamera.ViewProjection * glm::vec4 { line.Begin, 1.0f },
								debugCamera.ViewProjection * glm::vec4 { line.End, 1.0f },
								physicsColor, 1.4f);
						}
					}
					if (std::getenv("WLD_TRACE_UI"))
					{
						static uint64_t lastPhysicsLog = ~0ull;
						const uint64_t stamp = ctx.Frame() / 120;
						if (stamp != lastPhysicsLog)
						{
							lastPhysicsLog = stamp;
							WLD_CORE_INFO("[ui] physics debug lines={0} viewport=({1},{2},{3},{4})",
								physicsLines.size(), sceneRect.X, sceneRect.Y, sceneRect.W, sceneRect.H);
						}
					}
				}
			}
		}

		if (previewVisible)
		{
			const uint64_t previewTexture = m_Host.GetCameraPreviewTextureId();
			Wui::PanelBackground(ctx, previewRect, { 0.05f, 0.05f, 0.06f, 0.92f }, 3.0f);
			Label(ctx, { previewRect.X + 6.0f, previewRect.Y + 3.0f },
				"相机: " + previewLabel, theme.Text, 12.0f);
			// UV 与主视口一致(场景纹理按 {0,1,1,-1} 贴,预览渲染器同一条路径)。
			Image(ctx, { previewRect.X + 1.0f, previewRect.Y + 18.0f, previewRect.W - 2.0f, previewRect.H - 19.0f },
				previewTexture, { 0, 1, 1, -1 }, theme);
			// P1c-W3.4:描边走库件(与手写 RectOutline 逐字段等价:色/圆角 3/厚度 1)。
			Wui::HighlightOutline(ctx, previewRect, theme.Border, 3.0f, 1.0f);
		}

		// ---- W5-L1:文档场景外部改动提示:只提示 + 一键重开,**不自动替换**文档 ----
		if (bannerVisible)
		{
			Wui::PanelBackground(ctx, bannerRect, { 0.30f, 0.24f, 0.08f, 0.96f }, 3.0f);
			RegisterReadonlyNode(Wui::HashId("scene.external.changed"), "status", "scene external change",
				"场景文件已在磁盘上被外部修改(未自动替换)", bannerRect);
			Label(ctx, { bannerRect.X + 10.0f, bannerRect.Y + 6.0f },
				"场景已在磁盘上改动", Wui::WuiColor { 1.0f, 0.86f, 0.55f, 1.0f }, 12.0f);
			if (Button(ctx, Wui::HashId("scene.external.reopen"),
				{ bannerRect.X + bannerRect.W - 104.0f, bannerRect.Y + 3.0f, 96.0f, 20.0f }, "重新打开", theme))
				m_Host.ReopenExternalScene();
			// P1c-W3.4:描边走库件(与手写 RectOutline 逐字段等价)。注:该横幅要"场景文件被外部改"
			// 才出现,本片未驱动该状态(报告 §5.4 未做项);同一条命令的另一处(预览小窗边框)已实测。
			Wui::HighlightOutline(ctx, bannerRect, theme.Border, 3.0f, 1.0f);
		}
		(void)theme;
	}
}
