#include "wldpch.h"
#include "HierarchyPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/Scene/Components.h"
#include "World/WUI/WuiWidgets.h"

#include <algorithm>

namespace World
{
	void HierarchyPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Ref<Scene> scene = host.GetActiveScene();
		if (!scene)
		{
			Label(ctx, { rect.X + 8, rect.Y + 8 }, "(no scene)", theme.TextMuted, 14.0f);
			return;
		}
		std::vector<Entity> entities;
		for (auto handle : scene->GetRegistry().view<UUIDComponent>())
			entities.push_back(Entity(scene.get(), handle));
		std::sort(entities.begin(), entities.end(), [](Entity a, Entity b)
			{
				const std::string& at = a.GetComponent<TagComponent>().Tag;
				const std::string& bt = b.GetComponent<TagComponent>().Tag;
				return at < bt;
			});

		float scrollY = 0;
		BeginScrollArea(ctx, rect, entities.size() * 22.0f + 8.0f, scrollY, theme);
		bool itemRightClicked = false;
		for (size_t i = 0; i < entities.size(); ++i)
		{
			const Wui::WuiRect row { rect.X + 4, rect.Y + 4 + i * 22.0f - scrollY, rect.W - 8, 22 };
			const bool selected = host.GetSelectedEntity() == entities[i];
			if (selected || ctx.IsHovered(row))
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, selected ? theme.Accent : theme.ButtonHover, 2.0f });
			const std::string tag = entities[i].GetComponent<TagComponent>().Tag;
			Label(ctx, { row.X + 6, row.Y + 3 }, tag.empty() ? "Empty Entity" : tag, theme.Text, 14.0f);
			if (ctx.IsClicked(row))
				host.SetSelectedEntity(entities[i]);
			if (ctx.Input().MouseClicked[1] && ctx.IsHovered(row))
			{
				itemRightClicked = true;
				host.SetSelectedEntity(entities[i]);
				m_Context = entities[i];
				m_MenuPos = ctx.Input().MousePos;
				ctx.OpenPopup(Wui::HashId("hierarchy.context"));
			}
		}
		EndScrollArea(ctx);

		const Wui::WuiId popup = Wui::HashId("hierarchy.context");
		if (ctx.IsPopupOpen(popup) && m_Context.IsValid())
		{
			ctx.PushOverlay();
			const Wui::WuiRect panel { m_MenuPos.x, m_MenuPos.y, 150, 2 * 22 + 8 };
			DrawPanelSurface(ctx, panel, theme);
			if (MenuItem(ctx, Wui::HashId("hierarchy.duplicate"), { panel.X + 4, panel.Y + 4, panel.W - 8, 22 }, "Duplicate", true, theme))
			{
				host.DuplicateSelectedEntity();
				ctx.CloseAllPopups();
			}
			if (MenuItem(ctx, Wui::HashId("hierarchy.delete"), { panel.X + 4, panel.Y + 26, panel.W - 8, 22 }, "Delete", true, theme))
			{
				Entity::DestroyEntity(scene.get(), m_Context);
				host.MarkDocumentDirty();
				ctx.CloseAllPopups();
			}
			ctx.ClosePopupsOnOutsideClick({ popup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(popup);
			ctx.PopOverlay();
		}
		else
		{
			// 菜单关闭后清空目标,避免残留;残留的不可见弹窗一并收起。
			m_Context = Entity();
			if (ctx.IsPopupOpen(popup))
				ctx.ClosePopup(popup);
		}

		// ---- 空白处右键 ----
		if (ctx.Input().MouseClicked[1] && ctx.IsHovered(rect) && !itemRightClicked)
		{
			m_BlankMenuPos = ctx.Input().MousePos;
			ctx.OpenPopup(Wui::HashId("hierarchy.blankcontext"));
		}
		const Wui::WuiId blankPopup = Wui::HashId("hierarchy.blankcontext");
		if (ctx.IsPopupOpen(blankPopup))
		{
			ctx.PushOverlay();
			const Wui::WuiRect panel { m_BlankMenuPos.x, m_BlankMenuPos.y, 190, 4 * 22 + 8 };
			DrawPanelSurface(ctx, panel, theme);
			const bool hasSelection = host.GetSelectedEntity().IsValid() && host.GetSelectedEntity().GetScene() == scene.get();
			if (MenuItem(ctx, Wui::HashId("hierarchy.create"), { panel.X + 4, panel.Y + 4, panel.W - 8, 22 }, "Create Empty Entity", true, theme))
			{
				Entity created;
				if (scene->DeferStructuralChange([&created](Scene& s) { created = Entity::CreateEntity(&s, "Empty Entity"); }) && created.IsValid())
				{
					host.SetSelectedEntity(created);
					host.MarkDocumentDirty();
				}
				ctx.CloseAllPopups();
			}
			if (MenuItem(ctx, Wui::HashId("hierarchy.createcamera"), { panel.X + 4, panel.Y + 26, panel.W - 8, 22 }, "Create Camera", true, theme))
			{
				Entity created;
				if (scene->DeferStructuralChange([&created](Scene& s)
					{
						created = Entity::CreateEntity(&s, "Camera");
						created.AddComponent<TransformComponent>();
						auto& camera = created.AddComponent<CameraComponent>();
						camera.Primary = true;
					}) && created.IsValid())
				{
					host.SetSelectedEntity(created);
					host.MarkDocumentDirty();
				}
				ctx.CloseAllPopups();
			}
			if (MenuItem(ctx, Wui::HashId("hierarchy.duplicatesel"), { panel.X + 4, panel.Y + 48, panel.W - 8, 22 }, "Duplicate Selected", hasSelection, theme))
			{
				if (hasSelection)
				{
					host.DuplicateSelectedEntity();
					host.MarkDocumentDirty();
				}
				ctx.CloseAllPopups();
			}
			if (MenuItem(ctx, Wui::HashId("hierarchy.deletesel"), { panel.X + 4, panel.Y + 70, panel.W - 8, 22 }, "Delete Selected", hasSelection, theme))
			{
				if (hasSelection)
				{
					Entity::DestroyEntity(scene.get(), host.GetSelectedEntity());
					host.MarkDocumentDirty();
				}
				ctx.CloseAllPopups();
			}
			ctx.ClosePopupsOnOutsideClick({ blankPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(blankPopup);
			ctx.PopOverlay();
		}
	}
}
