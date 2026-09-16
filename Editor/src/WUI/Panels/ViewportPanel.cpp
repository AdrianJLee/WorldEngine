#include "wldpch.h"
#include "ViewportPanel.h"

#include "World/WUI/WuiGizmo.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
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
		if (ctx.IsClicked(sceneRect) && !m_GizmoActive && !gizmoEngaged)
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
		if (selected.IsValid() && selected.HasComponent<MeshRendererComponent>() &&
			selected.HasComponent<TransformComponent>() && m_Host.HasRenderedScene())
		{
			const Wui::GizmoCamera gizmoCamera = m_Host.GetGizmoCamera();
			glm::mat4 world = selected.GetComponent<TransformComponent>().Transform;
			if (selected.HasComponent<WorldTransformComponent>())
				world = selected.GetComponent<WorldTransformComponent>().Matrix;
			const bool plane = selected.GetComponent<MeshRendererComponent>().Primitive == "plane";

			// 单位网格角点:bit0 = X、bit1 = Y、bit2 = Z;plane 只有 y = 0 的一层(z/x 四角)。
			glm::vec2 screen[8];
			float depth[8] = {};
			bool valid[8] = {};
			for (int i = 0; i < 8; ++i)
			{
				const glm::vec3 corner { (i & 1) ? 0.5f : -0.5f, plane ? 0.0f : ((i & 2) ? 0.5f : -0.5f),
					(i & 4) ? 0.5f : -0.5f };
				const glm::vec4 clip = gizmoCamera.ViewProjection * (world * glm::vec4 { corner, 1.0f });
				if (clip.w <= 0.0001f)
					continue; // 角点在相机背后:跳过(避免投影爆炸)
				valid[i] = true;
				const glm::vec3 ndc = glm::vec3(clip) / clip.w;
				screen[i] = { sceneRect.X + (ndc.x + 1.0f) * 0.5f * sceneRect.W,
					sceneRect.Y + (1.0f - ndc.y) * 0.5f * sceneRect.H };
				depth[i] = clip.w; // 视深度:同一投影下 w 与相机距离同序,用来排序/淡出
			}

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

			struct Edge { int A = 0; int B = 0; float Depth = 0.0f; };
			std::vector<Edge> edges;
			const auto addEdge = [&](int a, int b, int faceA, int faceB)
			{
				// 两条相邻面都背向相机 = 这条棱在物体后面,跳过。
				if (valid[a] && valid[b] && (faceVisible[faceA] || faceVisible[faceB]))
					edges.push_back({ a, b, (depth[a] + depth[b]) * 0.5f });
			};
			if (plane)
			{
				// 平面只有 4 个角:i = 0/1(x)、4/5(z)→ {0,1},{1,5},{5,4},{4,0}
				// 平面没有体积,不做面剔除(始终画它的 4 条边)。
				const int planeEdges[4][2] = { { 0, 1 }, { 1, 5 }, { 5, 4 }, { 4, 0 } };
				for (const auto& edge : planeEdges)
					if (valid[edge[0]] && valid[edge[1]])
						edges.push_back({ edge[0], edge[1], (depth[edge[0]] + depth[edge[1]]) * 0.5f });
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
					minScreen = glm::min(minScreen, glm::min(screen[edge.A], screen[edge.B]));
					maxScreen = glm::max(maxScreen, glm::max(screen[edge.A], screen[edge.B]));
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
					const glm::vec2 from = screen[edge.A];
					const glm::vec2 to = screen[edge.B];
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
					static uint32_t lastHandle = 0;
					const uint32_t handle = static_cast<uint32_t>(static_cast<entt::entity>(selected));
					if (handle != lastHandle)
					{
						lastHandle = handle;
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
		(void)theme;
	}
}
