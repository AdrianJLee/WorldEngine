#include "wldpch.h"
#include "ViewportPanel.h"

#include "World/WUI/WuiGizmo.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
	namespace
	{
		// 投影 + 近平面裁剪 + 线段四边形:相机视锥等覆盖层线段共用。
		void PushProjectedSegment(Wui::WuiContext& ctx, const glm::mat4& viewProjection,
			const Wui::WuiRect& viewport, const glm::vec4& clipA, const glm::vec4& clipB,
			const Wui::WuiColor& color, float thickness,
			glm::vec2* boundsMin = nullptr, glm::vec2* boundsMax = nullptr)
		{
			constexpr float kMinW = 0.0001f;
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
			const float pad = 10.0f * std::max(viewport.W, viewport.H);
			const auto clampScreen = [&viewport, pad](const glm::vec2& point)
			{
				return glm::vec2 { glm::clamp(point.x, viewport.X - pad, viewport.X + viewport.W + pad),
					glm::clamp(point.y, viewport.Y - pad, viewport.Y + viewport.H + pad) };
			};
			const glm::vec2 from = clampScreen(project(a));
			const glm::vec2 to = clampScreen(project(b));
			// 供"视锥覆盖范围"日志/自动化核对:统计端点与视口矩形的交叠范围(夹到视口内,
			// 反映"用户在视口里能看到的这部分视锥"),而不是夹到 pad 之外的原始端点。
			const auto clampToViewport = [&viewport](const glm::vec2& point)
			{
				return glm::vec2 { glm::clamp(point.x, viewport.X, viewport.X + viewport.W),
					glm::clamp(point.y, viewport.Y, viewport.Y + viewport.H) };
			};
			const glm::vec2 visibleFrom = clampToViewport(from);
			const glm::vec2 visibleTo = clampToViewport(to);
			if (boundsMin)
				*boundsMin = glm::min(*boundsMin, glm::min(visibleFrom, visibleTo));
			if (boundsMax)
				*boundsMax = glm::max(*boundsMax, glm::max(visibleFrom, visibleTo));
			const glm::vec2 delta = to - from;
			const float length = glm::length(delta);
			if (length < 0.5f)
				return;
			const glm::vec2 normal { -delta.y / length, delta.x / length };
			const glm::vec2 offset = normal * (thickness * 0.5f);
			Wui::WuiDrawCommand command;
			command.Kind = Wui::WuiDrawKind::Quad;
			command.Color = color;
			command.Vertices = { from + offset, to + offset, to - offset, from - offset };
			ctx.Commands().push_back(std::move(command));
		}

		// 相机预览小窗位置:场景图左下角,宽取视口的 28%,16:9。
		Wui::WuiRect CameraPreviewRect(const Wui::WuiRect& sceneRect)
		{
			const float width = std::max(160.0f, sceneRect.W * 0.28f);
			const float height = width * 9.0f / 16.0f;
			const float margin = 10.0f;
			return { sceneRect.X + margin, sceneRect.Y + sceneRect.H - height - margin, width, height };
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
			m_Root->Add(m_SceneImage, { 0, 1e30f, 0, 1e30f, 1 });

			auto toolbar = std::make_shared<Wui::WuiBox>();
			toolbar->Direction = Wui::WuiDirection::Row;
			toolbar->Gap = 8;
			toolbar->AlignMain = Wui::WuiAlign::Center;
			toolbar->AlignCross = Wui::WuiAlign::Center;
			for (int i = 0; i < 3; ++i)
			{
				auto button = std::make_shared<Wui::WuiImageButton>();
				button->Uv = { 0, 1, 1, -1 };
				toolbar->Add(button, { 28, 28, 28, 28, 0 });
				m_Tools.push_back(button);
			}
			m_Root->Add(toolbar, { 0, 1e30f, 0, 44, 0 });
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
		m_Tools[0]->OnClick = [this] { m_Host.TogglePlay(); };
		m_Tools[1]->OnClick = [this] { m_Host.ToggleSimulate(); };
		m_Tools[2]->OnClick = [this] { m_Host.TogglePause(); };
		for (int i = 0; i < 3; ++i)
		{
			m_Tools[i]->TextureId = m_Host.GetIconId(icons[i]);
			m_Tools[i]->Dim = dim[i];
		}
		m_SceneImage->TextureId = m_Host.GetSceneTextureId();

		// 面板底色与工具栏底板:纯绘制,不进布局树。
		Wui::PanelBackground(ctx, rect, { 0.06f, 0.06f, 0.07f, 1 });
		const float panelWidth = 3 * 28.0f + 2 * 8.0f + 16.0f;
		const Wui::WuiRect bar { rect.X + (rect.W - panelWidth) * 0.5f, rect.Y + 14, panelWidth, 44 };
		// 工具栏底板走组件(Toolbar):与其它工具栏样式统一。
		Wui::Toolbar(ctx, bar, theme, 6.0f, 0.85f);

		// D7-1a:视口相机模式切换(2D 正视 / 3D 轨道)。放在工具栏左侧,不与播放按钮混排。
		const bool camera3D = m_Host.IsViewportCamera3D();
		if (Button(ctx, Wui::HashId("viewport.camera.mode"),
			{ rect.X + 10.0f, rect.Y + 14.0f, 46.0f, 24.0f }, camera3D ? "3D" : "2D", theme))
			m_Host.ToggleViewportCamera3D();
		// 相机可视化开关:开关相机预览小窗(视锥线框始终显示)。
		if (Button(ctx, Wui::HashId("viewport.camera.preview"),
			{ rect.X + 62.0f, rect.Y + 14.0f, 52.0f, 24.0f },
			m_Host.IsCameraPreviewEnabled() ? "Cam*" : "Cam", theme))
			m_Host.ToggleCameraPreview();

		Wui::LayoutWidgetTree(m_Root, rect);
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);

		const Wui::WuiRect sceneRect = m_SceneImage->Rect();
		const bool hovered = ctx.IsHovered(rect);
		if (ctx.IsClicked(rect))
			ctx.SetFocus(Wui::HashId("viewport"));
		const bool focused = ctx.Focus() == Wui::HashId("viewport");
		// 视口尺寸/边界取**场景图像区域**(sceneRect),不是整块面板:面板底部还有 44px 工具栏,
		// 用面板尺寸会让渲染目标比实际显示区域高(场景被纵向拉伸),拾取与 gizmo 的
		// 屏幕↔世界映射也会跟画面错开(它们都按 sceneRect 算)。
		glm::vec2 bounds[2] = { { sceneRect.X, sceneRect.Y },
			{ sceneRect.X + sceneRect.W, sceneRect.Y + sceneRect.H } };
		m_Host.SetViewportState(focused, hovered, { sceneRect.W, sceneRect.H }, bounds);

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
		if (ctx.IsClicked(sceneRect) && !m_GizmoActive && !gizmoEngaged && !overPreview)
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
			ctx.Commands().push_back({ Wui::WuiDrawKind::ClipPush, sceneRect, {} });
			for (const entt::entity handle : frustumRegistry.view<CameraComponent, TransformComponent>())
			{
				if (static_cast<entt::entity>(selectedCamera) != handle)
					continue;   // 只画选中的那台相机
				const SceneCamera& camera = frustumRegistry.get<CameraComponent>(handle).Camera;
				glm::mat4 world = frustumRegistry.get<TransformComponent>(handle).Transform;
				if (const auto* worldTransform = frustumRegistry.try_get<WorldTransformComponent>(handle))
					world = worldTransform->Matrix;
				// 相机看向 -Z(与 glm::perspective / glm::ortho 的约定一致)。
				float hNear = 0.0f, wNear = 0.0f, hFar = 0.0f, wFar = 0.0f, zNear = 0.0f, zFar = 0.0f;
				if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective)
				{
					const float tanHalf = std::tan(glm::radians(camera.GetPerspectiveFOV()) * 0.5f);
					zNear = -camera.GetPerspectiveNearClip();
					zFar = -camera.GetPerspectiveFarClip();
					hNear = tanHalf * std::abs(zNear);
					wNear = hNear * camera.GetAspectRatio();
					hFar = tanHalf * std::abs(zFar);
					wFar = hFar * camera.GetAspectRatio();
				}
				else
				{
					zNear = -camera.GetOrthographicNearClip();
					zFar = -camera.GetOrthographicFarClip();
					hNear = hFar = camera.GetOrthographicZoom();
					wNear = wFar = camera.GetOrthographicZoom() * camera.GetAspectRatio();
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
			ctx.Commands().push_back({ Wui::WuiDrawKind::ClipPop });
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

		if (selected.IsValid() && selected.HasComponent<MeshRendererComponent>() &&
			selected.HasComponent<TransformComponent>() && m_Host.HasRenderedScene())
		{
			const Wui::GizmoCamera gizmoCamera = m_Host.GetGizmoCamera();
			glm::mat4 world = selected.GetComponent<TransformComponent>().Transform;
			if (selected.HasComponent<WorldTransformComponent>())
				world = selected.GetComponent<WorldTransformComponent>().Matrix;
			const bool plane = selected.GetComponent<MeshRendererComponent>().Primitive == "plane";

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
				ctx.Commands().push_back({ Wui::WuiDrawKind::ClipPush, sceneRect, {} });
				for (const Edge& edge : edges)
				{
					// 可见棱统一实色(不再做远近淡出:背面棱已经被剔除,不需要透视暗示)。
					const Wui::WuiColor color { 1.0f, 0.55f, 0.12f, 1.0f };
					const glm::vec2 from = edge.From;
					const glm::vec2 to = edge.To;
					const glm::vec2 delta = to - from;
					const float length = glm::length(delta);
					if (length < 0.5f)
						continue;
					const glm::vec2 normal { -delta.y / length, delta.x / length };
					const glm::vec2 offset = normal * 0.9f; // 1.8px 宽
					Wui::WuiDrawCommand command;
					command.Kind = Wui::WuiDrawKind::Quad;
					command.Color = color;
					command.Vertices = { from + offset, to + offset, to - offset, from - offset };
					ctx.Commands().push_back(std::move(command));
				}
				ctx.Commands().push_back({ Wui::WuiDrawKind::ClipPop });
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

		// ---- 相机预览小窗(PiP):直接显示"场景相机看到的东西" ----
		if (previewVisible)
		{
			const uint64_t previewTexture = m_Host.GetCameraPreviewTextureId();
			Wui::PanelBackground(ctx, previewRect, { 0.05f, 0.05f, 0.06f, 0.92f }, 3.0f);
			Label(ctx, { previewRect.X + 6.0f, previewRect.Y + 3.0f },
				"相机: " + previewLabel, theme.Text, 12.0f);
			// UV 与主视口一致(场景纹理按 {0,1,1,-1} 贴,预览渲染器同一条路径)。
			Image(ctx, { previewRect.X + 1.0f, previewRect.Y + 18.0f, previewRect.W - 2.0f, previewRect.H - 19.0f },
				previewTexture, { 0, 1, 1, -1 }, theme);
			ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, previewRect, theme.Border, 3.0f, 1.0f });
		}
		(void)theme;
	}
}
