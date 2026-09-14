#include "wldpch.h"
#include "HierarchyPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/Scene/Components.h"
#include "World/WUI/WuiWidget.h"
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
				return a.GetComponent<TagComponent>().Tag < b.GetComponent<TagComponent>().Tag;
			});

		std::string orderKey;
		for (Entity entity : entities)
		{
			orderKey += entity.GetComponent<TagComponent>().Tag;
			orderKey += '\n';
		}

		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_Scroll = std::make_shared<Wui::WuiScrollArea>();
			m_Root->Add(m_Scroll, { 0, 1e30f, 0, 1e30f, 1 });
		}

		// 实体集合/排序变化时重建行;否则只更新文本与选中态。
		if (m_RowEntities.size() != entities.size() || orderKey != m_LastOrderKey)
		{
			m_LastOrderKey = std::move(orderKey);
			m_Rows.clear();
			m_RowEntities = entities;
			auto content = std::make_shared<Wui::WuiBox>();
			for (Entity entity : entities)
			{
				auto row = std::make_shared<Wui::WuiListRow>();
				row->Text = entity.GetComponent<TagComponent>().Tag.empty() ? "Empty Entity" : entity.GetComponent<TagComponent>().Tag;
				row->OnClick = [&host, entity] { host.SetSelectedEntity(entity); };
				content->Add(row, { 0, 1e30f, 0, 22, 0 });
				m_Rows.push_back(row);
			}
			m_Scroll->Child = content;
			m_Root->Invalidate();
		}
		else
		{
			for (size_t i = 0; i < m_Rows.size(); ++i)
				m_Rows[i]->Selected = host.GetSelectedEntity() == m_RowEntities[i];
		}
		m_Scroll->ContentHeight = entities.size() * 22.0f + 8.0f;

		Wui::LayoutWidgetTree(m_Root, { rect.X + 4, rect.Y + 4, rect.W - 8, rect.H - 8 });
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);

		// ---- 右键菜单(瞬态 overlay,不参与布局)----
		bool itemRightClicked = false;
		for (size_t i = 0; i < m_Rows.size(); ++i)
		{
			if (ctx.Input().MouseClicked[1] && ctx.IsHovered(m_Rows[i]->Rect()))
			{
				itemRightClicked = true;
				host.SetSelectedEntity(m_RowEntities[i]);
				m_Context = m_RowEntities[i];
				m_MenuPos = ctx.Input().MousePos;
				ctx.OpenPopup(Wui::HashId("hierarchy.context"));
			}
		}

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
