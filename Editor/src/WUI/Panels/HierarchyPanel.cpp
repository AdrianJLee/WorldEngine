#include "wldpch.h"
#include "HierarchyPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

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
		std::vector<uint32_t> depths;
		// 只读遍历必须走 const registry:运行中的场景拒绝非 const 访问(结构写保护)。
		const entt::registry& registry = static_cast<const Scene*>(scene.get())->GetRegistry();

		// W3b-2:按层级展平(根在前、子节点紧随父节点),而不是按名字平铺排序。
		// 带访问集与深度上限,非法层级(环/断链)也能安全显示且不丢行。
		const auto labelOf = [&registry](entt::entity handle) -> std::string
		{
			if (const auto* tag = registry.try_get<TagComponent>(handle))
				return tag->Tag.empty() ? "Empty Entity" : tag->Tag;
			return "Empty Entity";
		};
		std::vector<entt::entity> roots;
		for (auto handle : registry.view<UUIDComponent>())
		{
			const auto* hierarchy = registry.try_get<HierarchyComponent>(handle);
			if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
				roots.push_back(handle);
		}
		auto byLabel = [&labelOf](entt::entity a, entt::entity b) { return labelOf(a) < labelOf(b); };
		std::sort(roots.begin(), roots.end(), byLabel);

		std::unordered_set<uint32_t> visited;
		std::function<void(entt::entity, uint32_t)> visit = [&](entt::entity handle, uint32_t depth)
		{
			constexpr uint32_t kMaxDisplayDepth = 64;
			if (depth > kMaxDisplayDepth || !visited.insert(static_cast<uint32_t>(handle)).second)
				return;
			entities.push_back(Entity(scene.get(), handle));
			depths.push_back(depth);

			const auto* hierarchy = registry.try_get<HierarchyComponent>(handle);
			if (!hierarchy)
				return;
			std::vector<entt::entity> children = hierarchy->Children;
			std::sort(children.begin(), children.end(), byLabel);
			for (const entt::entity child : children)
				if (registry.valid(child))
					visit(child, depth + 1);
		};
		for (const entt::entity root : roots)
			visit(root, 0);
		for (auto handle : registry.view<UUIDComponent>())
			if (visited.find(static_cast<uint32_t>(handle)) == visited.end())
			{
				entities.push_back(Entity(scene.get(), handle));
				depths.push_back(0);
			}

		std::string orderKey;
		for (size_t i = 0; i < entities.size(); ++i)
		{
			orderKey += std::to_string(depths[i]) + ":" + labelOf(entities[i]) + '\n';
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
				// 缩进用空格前缀:WuiListRow 暂无 Indent 字段,加字段属 UI 组件库改动,单独排期。
				const size_t index = m_Rows.size();
				const uint32_t depth = index < depths.size() ? depths[index] : 0;
				row->Text = std::string(depth * 4, ' ') + labelOf(entity);
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
			// 右键菜单走组件(ContextMenu):位置钉住 + 外部点击/Esc 关闭统一由组件处理。
			Wui::WuiRect panel;
			if (Wui::BeginContextMenu(ctx, popup, m_MenuPos, 150.0f, 4, &panel, theme))
			{
				if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.duplicate"),
					{ panel.X + 4, panel.Y + 4, panel.W - 8, 22 }, "Duplicate", theme))
				{
					host.DuplicateSelectedEntity();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.delete"),
					{ panel.X + 4, panel.Y + 26, panel.W - 8, 22 }, "Delete", theme))
				{
					Entity::DestroyEntity(scene.get(), m_Context);
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				// W3b-2:重设父级/解挂。拖拽交互需要行控件支持拖拽(UI 组件库改动,单独排期),
				// 这里先提供等价入口:右键实体 -> 设为「当前选中实体」的子节点,或解除父子关系。
				const Entity selected = host.GetSelectedEntity();
				const bool canParent = selected.IsValid() && selected.GetScene() == scene.get() &&
					static_cast<entt::entity>(selected) != static_cast<entt::entity>(m_Context);
				if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.setparent"),
					{ panel.X + 4, panel.Y + 48, panel.W - 8, 22 },
					canParent ? "Set Parent (Selected)" : "Set Parent (select another first)", theme))
				{
					if (canParent)
					{
						const entt::entity child = m_Context;
						const entt::entity parent = selected;
						// 结构写必须走延迟命令:循环依赖由 Hierarchy::SetParent 内核侧拒绝并告警。
						scene->DeferStructuralChange([child, parent](Scene& s)
							{
								if (Hierarchy::SetParent(s.GetRegistry(), child, parent))
									WLD_CORE_INFO("Hierarchy: '{0}' is now a child of '{1}'",
										static_cast<uint32_t>(child), static_cast<uint32_t>(parent));
							});
						host.MarkDocumentDirty();
					}
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.unparent"),
					{ panel.X + 4, panel.Y + 70, panel.W - 8, 22 }, "Unparent", theme))
				{
					const entt::entity child = m_Context;
					scene->DeferStructuralChange([child](Scene& s) { Hierarchy::ClearParent(s.GetRegistry(), child); });
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				if (ctx.IsKeyPressed(KeyCodes::Escape))
					ctx.ClosePopup(popup);
				Wui::EndContextMenu(ctx, popup, panel, theme);
			}
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
