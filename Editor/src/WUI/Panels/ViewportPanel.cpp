#include "wldpch.h"
#include "ViewportPanel.h"

#include "World/ImGui/ImGuiDrawLibrary.h"
#include "World/WUI/WuiWidgets.h"

#include <ImGuizmo.h>

namespace World
{
	void ViewportPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost&)
	{
		const Wui::WuiTheme& theme = m_Host.Theme();
		m_Host.SetViewportRect(rect);
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, { 0.06f, 0.06f, 0.07f, 1 }, 0.0f });
		if (m_Host.HasRenderedScene() && m_Host.GetSceneRenderer())
		{
			const uint64_t texture = m_Host.GetSceneRenderer()->GetTargetFramebuffer()->GetColorAttachmentRendererID();
			Image(ctx, rect, texture, { 0, 1, 1, -1 }, theme);
		}

		const bool hovered = ctx.IsHovered(rect);
		if (ctx.IsClicked(rect))
			ctx.SetFocus(Wui::HashId("viewport"));
		const bool focused = ctx.Focus() == Wui::HashId("viewport");
		glm::vec2 bounds[2] = { { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } };
		m_Host.SetViewportState(focused, hovered, { rect.W, rect.H }, bounds);

		// 悬浮工具栏:Play / Simulate / Pause
		const float panelWidth = 3 * 28.0f + 2 * 8.0f + 16.0f;
		const Wui::WuiRect bar { rect.X + (rect.W - panelWidth) * 0.5f, rect.Y + 14, panelWidth, 44 };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, bar, { 0.12f, 0.12f, 0.12f, 0.85f }, 6.0f });
		const bool play = m_Host.IsPlaying();
		const bool simulate = m_Host.IsSimulating();
		const bool paused = m_Host.IsPaused();
		struct Tool { int Icon; std::function<void()> Action; bool Dim; };
		const Tool tools[] = {
			{ play ? 1 : 0, [this] { m_Host.TogglePlay(); }, simulate },
			{ simulate ? 5 : 4, [this] { m_Host.ToggleSimulate(); }, play },
			{ paused ? (simulate ? 7 : 3) : (simulate ? 6 : 2), [this] { m_Host.TogglePause(); }, !play && !simulate },
		};
		for (int i = 0; i < 3; ++i)
		{
			const Wui::WuiRect button { bar.X + 8 + i * 36, bar.Y + 8, 28, 28 };
			Ref<Texture2D> icon = m_Host.GetIcon(tools[i].Icon);
			if (icon)
			{
				Image(ctx, button, icon->GetRendererID(), { 0, 1, 1, -1 }, theme);
				if (!tools[i].Dim && ctx.IsClicked(button))
					tools[i].Action();
			}
		}

		if (ctx.IsClicked(rect) && !ImGuiDrawLibrary::GizmoIsOver())
		{
			const glm::vec2 local = ctx.Input().MousePos - glm::vec2 { rect.X, rect.Y };
			m_Host.SetSelectedEntity(m_Host.PickEntityAt(local));
		}

		Entity selected = m_Host.GetSelectedEntity();
		if (selected.IsValid() && selected.GetScene() == m_Host.GetActiveScene().get() &&
			selected.HasComponent<TransformComponent>() && m_Host.HasRenderedScene())
		{
			auto& transform = selected.GetComponent<TransformComponent>();
			const bool wasUsing = ImGuiDrawLibrary::GizmoIsUsing();
			const TransformComponent before = transform;
			ImGuiDrawLibrary::DrawGizmo(m_Host.GetEditorCamera(), selected, static_cast<ImGuizmo::OPERATION>(m_Host.GetGizmoOperation()), rect);
			const bool nowUsing = ImGuiDrawLibrary::GizmoIsUsing();
			if (!wasUsing && nowUsing)
			{
				m_GizmoActive = true;
				m_GizmoBefore = before;
			}
			else if (wasUsing && !nowUsing && m_GizmoActive)
			{
				m_GizmoActive = false;
				const auto& after = selected.GetComponent<TransformComponent>();
				if (after.Location != m_GizmoBefore.Location || after.Rotation != m_GizmoBefore.Rotation || after.Scale != m_GizmoBefore.Scale)
					m_Host.MarkDocumentDirty();
			}
		}
	}
}
