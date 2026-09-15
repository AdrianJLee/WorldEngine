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
		glm::vec2 bounds[2] = { { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } };
		m_Host.SetViewportState(focused, hovered, { rect.W, rect.H }, bounds);

		if (ctx.IsClicked(sceneRect) && !m_GizmoActive)
		{
			const glm::vec2 local = ctx.Input().MousePos - glm::vec2 { sceneRect.X, sceneRect.Y };
			m_Host.SetSelectedEntity(m_Host.PickEntityAt(local));
		}

		Entity selected = m_Host.GetSelectedEntity();
		if (selected.IsValid() && selected.GetScene() == m_Host.GetActiveScene().get() &&
			selected.HasComponent<TransformComponent>() && m_Host.HasRenderedScene())
		{
			auto& transform = selected.GetComponent<TransformComponent>();
			const TransformComponent before = transform;
			const bool nowUsing = Wui::ManipulateGizmo(m_Host.GetEditorCamera(),
				m_Host.GetGizmoOperation(), transform, sceneRect, ctx,
				/*allowManipulation=*/!m_Host.IsReadOnlyMode());
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
		(void)theme;
	}
}
