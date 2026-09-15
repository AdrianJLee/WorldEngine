#include "wldpch.h"
#include "HierarchyPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Gameplay/Prefab.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace World
{
	namespace
	{
		// 解析内容根(开发布局为 Game/assets,打包布局由清单决定);失败时返回空路径。
		std::filesystem::path ResolveContentRoot()
		{
			std::filesystem::path manifestPath;
			if (!World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
				return {};
			std::string error;
			World::Asset::ProjectManifest manifest;
			if (!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
				return {};
			return manifest.ResolveContentRoot(manifestPath);
		}
	}

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
			// 折叠节点不展开子节点(但仍显示自身)。
			if (m_Collapsed.find(static_cast<uint32_t>(handle)) != m_Collapsed.end())
				return;
			// 子节点顺序即 HierarchyComponent::Children 的顺序(同级重排会改它),不再按名字排序。
			for (const entt::entity child : hierarchy->Children)
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
				const size_t index = m_Rows.size();
				const uint32_t depth = index < depths.size() ? depths[index] : 0;
				row->Indent = static_cast<float>(depth) * 14.0f;
				// 折叠标记直接放进文本(避免额外的行内热区绘制):有子节点显示 +/-。
				const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
				const bool hasChildren = hierarchy && !hierarchy->Children.empty();
				const bool collapsed = m_Collapsed.find(static_cast<uint32_t>(static_cast<entt::entity>(entity)))
					!= m_Collapsed.end();
				row->Text = std::string(hasChildren ? (collapsed ? "+ " : "- ") : "  ") + labelOf(entity);
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

		// ---- W3b-2:拖拽设父(目标行 = 成为其子节点;非法目标由内核环路检测拒绝)----
		// 交互范式与 ContentBrowser 一致:按下即 BeginDrag(移动 >4px 才真正激活),
		// 悬停在候选行上时登记落点并高亮,拖拽结束后统一提交。
		std::string dragPayload;
		const bool dragging = ctx.IsDragActive(&dragPayload);
		for (size_t i = 0; i < m_Rows.size(); ++i)
		{
			const Wui::WuiRect rowRect = m_Rows[i]->Rect();
			const bool hovered = ctx.IsHovered(rowRect);
			if (ctx.Input().MouseDown[0] && hovered)
			{
				const uint32_t handle = static_cast<uint32_t>(static_cast<entt::entity>(m_RowEntities[i]));
				ctx.BeginDrag(Wui::HashId(("hierarchy.drag." + std::to_string(handle)).c_str()),
					"entity:" + std::to_string(handle));
				ctx.SetCursor(Wui::WuiCursor::Hand);
			}
			if (dragging && hovered && dragPayload.rfind("entity:", 0) == 0)
			{
				const uint32_t source = static_cast<uint32_t>(std::stoul(dragPayload.substr(7)));
				const uint32_t target = static_cast<uint32_t>(static_cast<entt::entity>(m_RowEntities[i]));
				if (source != target)
				{
					ctx.DropTarget(rowRect, "entity:");
					m_PendingDropHandle = m_RowEntities[i];
					m_PendingDropSource = source;
					// 三区落点:上 25% 插到目标之前、下 25% 插到之后、中间 50% 成为目标子节点。
					const float local = (ctx.Input().MousePos.y - rowRect.Y) / std::max(1.0f, rowRect.H);
					m_PendingDropZone = local < 0.25f ? 0 : (local > 0.75f ? 2 : 1);
					if (m_PendingDropZone == 1)
					{
						Wui::HighlightOutline(ctx, rowRect, theme.Accent, 2.0f, 2.0f);
					}
					else
					{
						const float lineY = m_PendingDropZone == 0 ? rowRect.Y : rowRect.Y + rowRect.H - 2.0f;
						ctx.Commands().push_back({ Wui::WuiDrawKind::Rect,
							{ rowRect.X, lineY, rowRect.W, 2.0f }, theme.Accent, 0.0f });
					}
				}
			}
			// W4-2b:内容浏览器拖来的 .wprefab 落在某一行 = 成为该实体的子节点。
			if (dragging && hovered && dragPayload.rfind("file:", 0) == 0)
			{
				const std::string file = dragPayload.substr(5);
				if (file.size() > 8 && file.compare(file.size() - 8, 8, ".wprefab") == 0)
				{
					ctx.DropTarget(rowRect, "file:");
					m_PendingPrefabFile = file;
					m_PendingDropHandle = m_RowEntities[i];
					Wui::HighlightOutline(ctx, rowRect, theme.Accent, 2.0f, 2.0f);
				}
			}
		}
		// 拖到面板空白处:实例化为场景根。
		if (dragging && ctx.IsHovered(rect) && dragPayload.rfind("file:", 0) == 0 &&
			dragPayload.size() > 13 && dragPayload.compare(dragPayload.size() - 8, 8, ".wprefab") == 0)
		{
			ctx.DropTarget(rect, "file:");
			m_PendingPrefabFile = dragPayload.substr(5);
		}
		if (!dragging && !m_PendingPrefabFile.empty())
		{
			// 内容浏览器载荷是"相对内容根"的路径:先按相对路径尝试,失败再用项目清单解析内容根。
			std::filesystem::path prefabPath = m_PendingPrefabFile;
			if (!std::filesystem::exists(prefabPath))
			{
				std::filesystem::path manifestPath;
				if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
				{
					std::string manifestError;
					World::Asset::ProjectManifest manifest;
					if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &manifestError))
					{
						const std::filesystem::path candidate =
							manifest.ResolveContentRoot(manifestPath) / m_PendingPrefabFile;
						if (std::filesystem::exists(candidate))
							prefabPath = candidate;
					}
				}
			}

			const entt::entity parent = m_PendingDropHandle.IsValid()
				? static_cast<entt::entity>(m_PendingDropHandle) : entt::null;
			const Gameplay::PrefabInstanceResult instance =
				Gameplay::InstantiateFromFile(prefabPath, *scene, parent);
			if (instance.IsValid())
			{
				host.SetSelectedEntity(instance.Root);
				host.MarkDocumentDirty();
				WLD_CORE_INFO("Prefab '{0}' instantiated from hierarchy drop ({1} entities)",
					prefabPath.generic_string(), instance.EntityCount);
			}
			else
			{
				WLD_CORE_WARN("Prefab drop rejected: '{0}'", prefabPath.generic_string());
			}
			m_PendingPrefabFile.clear();
			m_PendingDropHandle = Entity();
		}
		if (!dragging && m_PendingDropHandle.IsValid() && m_PendingDropSource != 0)
		{
			const entt::entity child = static_cast<entt::entity>(m_PendingDropSource);
			const entt::entity parent = m_PendingDropHandle;
			const uint32_t zone = m_PendingDropZone;
			if (scene->DeferStructuralChange([child, parent, zone](Scene& s)
				{
					auto& registry = s.GetRegistry();
					if (zone == 1)
					{
						if (!Hierarchy::SetParent(registry, child, parent))
							WLD_CORE_WARN("Hierarchy: drag-drop rejected (cycle or invalid target)");
						return;
					}
					// 同级重排:插到目标之前/之后;目标是根节点时退回为"挂到同一层"(即设为根)。
					const auto* targetHierarchy = registry.try_get<HierarchyComponent>(parent);
					const entt::entity newParent = targetHierarchy ? targetHierarchy->Parent : entt::null;
					const int32_t targetIndex = Hierarchy::GetChildIndex(registry, parent);
					const size_t insertIndex = targetIndex < 0 ? 0u
						: static_cast<size_t>(targetIndex + (zone == 2 ? 1 : 0));
					if (!Hierarchy::InsertChild(registry, child, newParent, insertIndex))
						WLD_CORE_WARN("Hierarchy: reorder rejected (cycle or invalid target)");
				}))
				host.MarkDocumentDirty();
			m_PendingDropHandle = Entity();
			m_PendingDropSource = 0;
			m_PendingDropZone = 1;
		}

		// ---- 折叠/展开:点击行首标记区(标记 14px + 缩进)切换,并把列表标脏以便重建行 ----
		for (size_t i = 0; i < m_Rows.size(); ++i)
		{
			const Entity entity = m_RowEntities[i];
			const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
			if (!hierarchy || hierarchy->Children.empty())
				continue;
			const Wui::WuiRect rowRect = m_Rows[i]->Rect();
			const Wui::WuiRect toggle { rowRect.X + m_Rows[i]->Indent, rowRect.Y, 16.0f, rowRect.H };
			if (ctx.IsClicked(toggle))
			{
				const uint32_t handle = static_cast<uint32_t>(static_cast<entt::entity>(entity));
				if (m_Collapsed.find(handle) != m_Collapsed.end())
					m_Collapsed.erase(handle);
				else
					m_Collapsed.insert(handle);
				m_LastOrderKey.clear();   // 强制下一帧重建行(折叠会改变可见行集合)
			}
		}

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
			if (Wui::BeginContextMenu(ctx, popup, m_MenuPos, 170.0f, 5, &panel, theme))
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
				// W4-2c:把选中实体的子树导出为 .wprefab(落在内容根 prefabs/ 下)。
				// 说明:首版不做文件对话框,固定目录 + 以 Tag 命名,便于立刻验证拖拽实例化链路。
				if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.exportprefab"),
					{ panel.X + 4, panel.Y + 92, panel.W - 8, 22 }, "Export as Prefab (.wprefab)", theme))
				{
					const std::filesystem::path contentRoot = ResolveContentRoot();
					if (contentRoot.empty())
					{
						WLD_CORE_WARN("Export Prefab: content root not found (project.we.yaml?)");
					}
					else
					{
						std::string tag = m_Context.GetComponent<TagComponent>().Tag;
						if (tag.empty())
							tag = "Prefab";
						for (char& ch : tag)
							if (ch == ' ' || ch == '/' || ch == '\\' || ch == ':')
								ch = '_';
						const std::filesystem::path output =
							contentRoot / "prefabs" / (tag + ".wprefab");
						std::filesystem::create_directories(output.parent_path());
						std::string error;
						if (Gameplay::SaveFromScene(*scene, m_Context, output, &error))
							WLD_CORE_INFO("Prefab exported: {0}", output.generic_string());
						else
							WLD_CORE_WARN("Export Prefab failed: {0}", error);
					}
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
