#include "wldpch.h"
#include "WUI/Panels/ViewportPanel.h"

#include "World/WUI/WuiGizmo.h"
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
		constexpr float kChromeRowH = 24.0f;   // 左侧视图按钮 / 右侧状态徽标的高度
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

		// ① 左上:`视图 ▼`(投影方式 / 观察方位 / 相机预览 / 光照 / 碰撞体 的唯一入口)
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
		const float viewButtonW = ctx.MeasureTextWidth(viewLabel, 13.0f) + 20.0f;
		const Wui::WuiRect viewButton { sceneRect.X + kChromePad, sceneRect.Y + kChromePad, viewButtonW, kChromeRowH };
		const std::string viewHint = Wui::Tr("viewport.view.hint",
			"View options: view mode (3D/2D), camera preview, light ranges, collider outlines.");
		Wui::Tooltip(ctx, viewButton, viewHint);
		if (Wui::Button(ctx, Wui::HashId("viewport.view"), viewButton, viewLabel, theme))
			ctx.OpenPopup(Wui::HashId("viewport.view.popup"));
		RegisterReadonlyNode(Wui::HashId("viewport.camera.mode"), "text", cameraText,
			camera3D ? "3d" : "2d", viewButton,
			Wui::Tr("viewport.camera.hint", "Viewport view mode — switch in the View menu (top-left)."));
		ctx.RegisterOverlayRect(viewButton);

		// Level 2.0: 视口工具栏 —— 数据层开关(改的是**真实**渲染剔除掩码)与网格开关
		float toolbarX = viewButton.X + viewButton.W + 8.0f;

		// 每个声明的数据层一个开关:点亮 = 该层实体参与本帧渲染。
		// 这是昼夜/战损这类"同一空间多套内容"的实际切换点(SceneRenderer 按掩码剔除)。
		const std::vector<Level::DataLayerDefinition>& dataLayers = m_Host.GetLevelDataLayers();
		const uint32_t activeLayers = m_Host.GetActiveDataLayers();
		for (const Level::DataLayerDefinition& layer : dataLayers)
		{
			if (layer.Name.empty())
				continue;
			const bool isOn = (layer.LayerMask & activeLayers) != 0;
			const std::string label = (isOn ? "* " : "  ") + layer.Name;
			const float layerW = ctx.MeasureTextWidth(label, 12.0f) + 16.0f;
			const Wui::WuiRect layerBtn { toolbarX, sceneRect.Y + kChromePad, layerW, kChromeRowH };
			const std::string layerHint = Wui::TrFormat("viewport.datalayer.hint",
				"Toggle data layer '{0}' (mask {1}).", { { "name", layer.Name },
					{ "mask", std::to_string(layer.LayerMask) } });
			Wui::Tooltip(ctx, layerBtn, layerHint);
			if (Wui::Button(ctx, Wui::HashId(("viewport.layer." + layer.Name).c_str()), layerBtn, label, theme))
			{
				const uint32_t next = isOn ? (activeLayers & ~layer.LayerMask) : (activeLayers | layer.LayerMask);
				m_Host.SetActiveDataLayers(next);
			}
			ctx.RegisterOverlayRect(layerBtn);
			toolbarX += layerW + 4.0f;
		}

		const float gridBtnW = 88.0f;
		const Wui::WuiRect gridBtn { toolbarX, sceneRect.Y + kChromePad, gridBtnW, kChromeRowH };
		const bool gridOn = m_Host.IsReferenceGridEnabled();
		const std::string gridLabel = gridOn ? "Grid: ON" : "Grid: OFF";
		Wui::Tooltip(ctx, gridBtn, Wui::Tr("viewport.grid.hint", "Toggle ground reference grid (1m/10m)."));
		if (Wui::Button(ctx, Wui::HashId("viewport.stream_grid"), gridBtn, gridLabel, theme))
		{
			m_Host.SetReferenceGridEnabled(!gridOn);
		}
		ctx.RegisterOverlayRect(gridBtn);
		toolbarX += gridBtnW + 4.0f;

		const float partBtnW = 100.0f;
		const Wui::WuiRect partBtn { toolbarX, sceneRect.Y + kChromePad, partBtnW, kChromeRowH };
		const std::string partLabel = m_ShowWorldPartition ? "Partition: ON" : "Partition: OFF";
		Wui::Tooltip(ctx, partBtn, Wui::Tr("viewport.partition.hint", "Toggle world partition & streaming chunk overlay."));
		if (Wui::Button(ctx, Wui::HashId("viewport.partition_grid"), partBtn, partLabel, theme))
		{
			m_ShowWorldPartition = !m_ShowWorldPartition;
		}
		ctx.RegisterOverlayRect(partBtn);

		// ② 顶部居中:运行控制药丸(播放 / 模拟 / 暂停)。用户 2026-09-21:「悬浮在视口上居中」——
		// 深色半透明圆角底板,悬停时提亮;不悬停时更淡,尽量少挡画面。
		const float pillW = 3 * 28.0f + 2 * 4.0f + 20.0f;
		const Wui::WuiRect pillRect { sceneRect.X + (sceneRect.W - pillW) * 0.5f,
			sceneRect.Y + kChromePad, pillW, kPillH };
		const bool pillHovered = ctx.IsHovered(pillRect);
		Wui::Toolbar(ctx, pillRect, theme, kPillH * 0.5f,
			pillHovered ? 0.95f : (play || simulate ? 0.85f : 0.65f));
		Wui::LayoutWidgetTree(m_ToolPill, pillRect);
		m_ToolPill->Paint(paint);
		ctx.RegisterOverlayRect(pillRect);

		// ③ 右上:运行状态徽标(只读)。编辑态压到最低对比度,运行/暂停时给颜色 —— 状态是
		// "看图时最需要知道的一件事",但不该在编辑态抢视线。
		const char* const modeKey = simulate ? (paused ? "paused" : "simulate")
			: (play ? (paused ? "paused" : "play") : "edit");
		const std::string modeText = paused ? Wui::Tr("viewport.mode.paused", "Paused")
			: (simulate ? Wui::Tr("viewport.mode.simulate", "Simulating")
				: (play ? Wui::Tr("viewport.mode.play", "Playing")
					: Wui::Tr("viewport.mode.edit", "Edit Mode")));
		const Wui::WuiColor modeColor = paused ? theme.Warning
			: (simulate ? theme.Accent : (play ? theme.Success : theme.TextDisabled));
		const float modeWidth = ctx.MeasureTextWidth(modeText, 12.0f) + 18.0f;
		const Wui::WuiRect modeRect { sceneRect.X + sceneRect.W - kChromePad - modeWidth,
			sceneRect.Y + kChromePad + (kChromeRowH - 20.0f) * 0.5f, modeWidth, 20.0f };
		if (modeKey[0] != 'e' || rect.W >= 360.0f)
		{
			Wui::WuiColor modeFill = modeColor;
			modeFill.A = modeKey[0] == 'e' ? 0.10f : 0.20f;
			// P1c-W3.4:徽标走库件 Wui::Badge(填充 + 居中粗体字;radius 10 与原来手写一致)。
			// 逐字段:填充 rect/颜色/圆角 10、字色/字号 12/粗体 —— 全部相同;**唯一差异** = 文字
			// 纵向位置按库件居中口径 (H-12)/2 = +4(原手写 +3)⇒ 整条徽标文字下移 1px。
			// 这是本片唯一预期像素差,已在报告 §3 把它定位到 modeRect 之内(bbox 与像素数)。
			Wui::Badge(ctx, modeRect, modeText, modeFill, modeColor, theme, 12.0f, true, 10.0f);
			RegisterReadonlyNode(Wui::HashId("viewport.mode"), "text", modeText, modeKey, modeRect);
			ctx.RegisterOverlayRect(modeRect);
		}

		// ④ 视图菜单弹层(下拉选项: 视图模式 Combo + 视图方位 Combo + 覆盖层开关)。
		const Wui::WuiId viewPopup = Wui::HashId("viewport.view.popup");
		if (ctx.IsPopupOpen(viewPopup))
		{
			const float rowH = 22.0f;
			const float comboH = 24.0f;
			const Wui::WuiRect panel { viewButton.X, viewButton.Y + viewButton.H + 2.0f, 260.0f, comboH * 2.0f + rowH * 4.0f + 24.0f };
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
			item("viewport.view.world_partition", Wui::Tr("viewport.view.world_partition", "World Partition (流送区块)"),
				m_ShowWorldPartition,
				Wui::Tr("viewport.overlays.partition.tooltip", "Draw 64m world partition chunks, streaming radius circle, and active chunk highlight."),
				[this] { m_ShowWorldPartition = !m_ShowWorldPartition; });
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
			ctx.ClosePopupsOnOutsideClick({ viewPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(viewPopup);
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
			const bool nowUsing = Wui::ManipulateGizmo(m_Host.GetGizmoCamera(),
				m_Host.GetGizmoOperation(), transform, sceneRect, ctx,
				/*allowManipulation=*/!m_Host.IsReadOnlyMode());
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

			// 单位网格角点:bit0 = X、bit1 = Y、bit2 = Z;plane 只有 y = 0 的一层(z/x 四角)。
			// 保留**裁剪空间**坐标:大物体(例如地面平面)的角点会跑到相机后面,直接丢角点会让
			// 框缺边/变形,这里按近平面 w 裁剪线段后再投影。
			glm::vec4 clipPos[8];
			for (int i = 0; i < 8; ++i)
			{
				const glm::vec3 corner { (i & 1) ? 0.5f : -0.5f, plane ? 0.0f : ((i & 2) ? 0.5f : -0.5f),
					(i & 4) ? 0.5f : -0.5f };
				clipPos[i] = gizmoCamera.ViewProjection * (world * glm::vec4 { corner, 1.0f });
			}
			const auto projectClip = [&sceneRect](const glm::vec4& clip)
			{
				const glm::vec3 ndc = glm::vec3(clip) / clip.w;
				return glm::vec2 { sceneRect.X + (ndc.x + 1.0f) * 0.5f * sceneRect.W,
					sceneRect.Y + (1.0f - ndc.y) * 0.5f * sceneRect.H };
			};
			// 近平面裁剪后的端点可能落在极远处(大平面):夹到视口周围一个安全范围,
			// 避免 WUI 批处理里出现 1e7 级坐标(方向几乎不变,绘制结果由 ClipPush 决定)。
			const float clampPad = 10.0f * std::max(sceneRect.W, sceneRect.H);
			const auto clampScreen = [&sceneRect, clampPad](const glm::vec2& point)
			{
				return glm::vec2 { glm::clamp(point.x, sceneRect.X - clampPad, sceneRect.X + sceneRect.W + clampPad),
					glm::clamp(point.y, sceneRect.Y - clampPad, sceneRect.Y + sceneRect.H + clampPad) };
			};

			// 面朝向判定:只画"至少依附一个朝向相机的面"的棱 —— 物体后面的棱不再出现
			// (用户 2026-09-16:不要透视效果、别让我看到盒子后面的框)。
			// 面索引:0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z。
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

			struct Edge { glm::vec2 From { 0.0f, 0.0f }; glm::vec2 To { 0.0f, 0.0f }; float Depth = 0.0f; };
			std::vector<Edge> edges;
			const auto addEdge = [&](int a, int b, int faceA, int faceB)
			{
				// 两条相邻面都背向相机 = 这条棱在物体后面,跳过。
				if (!(faceVisible[faceA] || faceVisible[faceB]))
					return;
				glm::vec4 p0 = clipPos[a];
				glm::vec4 p1 = clipPos[b];
				constexpr float kMinW = 0.0001f;
				if (p0.w < kMinW && p1.w < kMinW)
					return; // 整条棱在相机后面
				if (p0.w < kMinW)
				{
					const float t = (kMinW - p0.w) / (p1.w - p0.w);
					p0 = glm::mix(p0, p1, t);
				}
				else if (p1.w < kMinW)
				{
					const float t = (kMinW - p1.w) / (p0.w - p1.w);
					p1 = glm::mix(p1, p0, t);
				}
				edges.push_back({ clampScreen(projectClip(p0)), clampScreen(projectClip(p1)),
					std::max(p0.w, p1.w) });
			};
			if (plane)
			{
				// 平面只有 4 个角:i = 0/1(x)、4/5(z)→ {0,1},{1,5},{5,4},{4,0}
				// 平面没有体积,不做面剔除(始终画它的 4 条边)。
				const int planeEdges[4][2] = { { 0, 1 }, { 1, 5 }, { 5, 4 }, { 4, 0 } };
				for (const auto& edge : planeEdges)
				{
					glm::vec4 p0 = clipPos[edge[0]];
					glm::vec4 p1 = clipPos[edge[1]];
					constexpr float kMinW = 0.0001f;
					if (p0.w < kMinW && p1.w < kMinW)
						continue;
					if (p0.w < kMinW)
					{
						const float t = (kMinW - p0.w) / (p1.w - p0.w);
						p0 = glm::mix(p0, p1, t);
					}
					else if (p1.w < kMinW)
					{
						const float t = (kMinW - p1.w) / (p0.w - p1.w);
						p1 = glm::mix(p1, p0, t);
					}
					edges.push_back({ clampScreen(projectClip(p0)), clampScreen(projectClip(p1)), std::max(p0.w, p1.w) });
				}
			}
			else
			{
				// 12 条棱 + 每条棱的两个相邻面(见上面的面索引)。
				const int cubeEdges[12][4] = {
					{ 0, 1, 3, 5 }, { 2, 3, 2, 5 }, { 4, 5, 3, 4 }, { 6, 7, 2, 4 },   // 沿 X(角点 y/z 固定)
					{ 0, 2, 1, 5 }, { 1, 3, 0, 5 }, { 4, 6, 1, 4 }, { 5, 7, 0, 4 },   // 沿 Y(角点 x/z 固定)
					{ 0, 4, 1, 3 }, { 1, 5, 0, 3 }, { 2, 6, 1, 2 }, { 3, 7, 0, 2 },   // 沿 Z(角点 x/y 固定)
				};
				for (const auto& edge : cubeEdges)
					addEdge(edge[0], edge[1], edge[2], edge[3]);
			}

			if (!edges.empty())
			{
				glm::vec2 minScreen { 1e30f, 1e30f };
				glm::vec2 maxScreen { -1e30f, -1e30f };
				float minDepth = edges[0].Depth, maxDepth = edges[0].Depth;
				for (const Edge& edge : edges)
				{
					minScreen = glm::min(minScreen, glm::min(edge.From, edge.To));
					maxScreen = glm::max(maxScreen, glm::max(edge.From, edge.To));
					minDepth = std::min(minDepth, edge.Depth);
					maxDepth = std::max(maxDepth, edge.Depth);
				}
				// 远的先画(先画的被后画的盖住),并按视深度淡出:近实远淡。
				std::sort(edges.begin(), edges.end(),
					[](const Edge& a, const Edge& b) { return a.Depth > b.Depth; });
				{
					// P1c-W3.4:选中框线框的裁剪走库件 Wui::ClipScope。
					Wui::ClipScope selectionClip(ctx, sceneRect);
					for (const Edge& edge : edges)
					{
						// 可见棱统一实色(不再做远近淡出:背面棱已经被剔除,不需要透视暗示)。
						const Wui::WuiColor color { 1.0f, 0.55f, 0.12f, 1.0f };
						const glm::vec2 from = edge.From;
						const glm::vec2 to = edge.To;
						// P1c-W3.6:可见棱同理走库件(1.8px 宽 = 半点偏移 0.9)。
						Wui::LineSegment(ctx, from, to, color, 1.8f);
					}
				}
				if (std::getenv("WLD_TRACE_UI"))
				{
					// 句柄或矩形变化时打一行(上限 40 行):Play↔暂停会换相机,矩形必须跟着变,
					// 自动化要能同时看到两种状态的框。
					static uint32_t lastHandle = 0;
					static Wui::WuiRect lastRect { 0, 0, 0, 0 };
					static int traced = 0;
					const uint32_t handle = static_cast<uint32_t>(static_cast<entt::entity>(selected));
					const Wui::WuiRect rect { minScreen.x, minScreen.y, maxScreen.x - minScreen.x, maxScreen.y - minScreen.y };
					const bool moved = std::abs(rect.X - lastRect.X) > 1.0f || std::abs(rect.Y - lastRect.Y) > 1.0f ||
						std::abs(rect.W - lastRect.W) > 1.0f || std::abs(rect.H - lastRect.H) > 1.0f;
					if ((handle != lastHandle || moved) && traced < 40)
					{
						lastHandle = handle;
						lastRect = rect;
						++traced;
						const int visibleFaces = (faceVisible[0] ? 1 : 0) + (faceVisible[1] ? 1 : 0) +
							(faceVisible[2] ? 1 : 0) + (faceVisible[3] ? 1 : 0) +
							(faceVisible[4] ? 1 : 0) + (faceVisible[5] ? 1 : 0);
						WLD_CORE_INFO("[ui] selection box(3d) handle={0} bbox=({1},{2},{3},{4}) edges={5} faces={6}",
							handle, minScreen.x, minScreen.y, maxScreen.x - minScreen.x, maxScreen.y - minScreen.y,
							edges.size(), visibleFaces);
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

			const float hudW = 240.0f;
			const float hudH = 46.0f;
			const Wui::WuiRect hudRect { sceneRect.X + sceneRect.W - hudW - 12.0f, sceneRect.Y + 12.0f, hudW, hudH };
			Wui::PanelBackground(ctx, hudRect, Wui::WuiColor { 0.07f, 0.09f, 0.13f, 0.88f }, 4.0f);
			Wui::HighlightOutline(ctx, hudRect, Wui::WuiColor { 0.18f, 0.65f, 0.90f, 0.70f }, 4.0f, 1.0f);
			Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 5.0f }, hudLine1, Wui::WuiColor { 0.30f, 0.90f, 1.0f, 1.0f }, 12.0f);
			Wui::Label(ctx, { hudRect.X + 8.0f, hudRect.Y + 24.0f }, hudLine2, Wui::WuiColor { 0.85f, 0.85f, 0.90f, 0.90f }, 12.0f);

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
