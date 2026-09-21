#include "wldpch.h"
#include "HierarchyPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Gameplay/Prefab.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiModal.h"

#include <algorithm>
#include <array>
#include <functional>
#include <unordered_set>

namespace World
{
	namespace
	{
		// P4-U13b:实例行左侧的标记列宽度。根行画 accent 徽标、子树内的其它行画淡色点;
		// 行文本按 Indent 偏移,所以徽标列不会压到"折叠标记 + 名字"(不需要往文本里塞空格)。
		constexpr float kPrefabMarkerSlot = 20.0f;

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

		// 面板内无障碍登记(与 WuiWidgets.cpp 的 RegisterAccessNode 同一格式):
		// 徽标/菜单说明这类"看得见但读不到"的信息统一进树。
		void RegisterAccessNode(Wui::WuiId id, const char* kind, const Wui::WuiRect& rect,
			const std::string& label, const std::string& value, bool enabled = true,
			const std::string& tooltip = std::string(), bool interactive = false)
		{
			if (id == 0)
				return;
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = interactive;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// 实例行的悬停说明:来源(逻辑路径)+ 覆盖计数;成员行额外说清它属于哪棵子树。
		std::string PrefabRowTooltip(const Gameplay::PrefabInstanceRecord& record, bool isRoot)
		{
			const std::string count = std::to_string(Gameplay::GetOverrideCount(record));
			return std::string(isRoot
					? Wui::Tr("panel.hierarchy.prefab.tooltip", "Prefab instance: ")
					: Wui::Tr("panel.hierarchy.prefab.member.tooltip", "Prefab instance subtree: "))
				+ record.PrefabPath
				+ " (" + count + ") "
				+ Wui::Tr("panel.hierarchy.prefab.overrides", "override(s)");
		}

		// 沿父链找实例记录(成员行画点、菜单动作都用它;深度上限防非法层级死循环)。
		const Gameplay::PrefabInstanceRecord* OwningRecord(const Scene& scene, entt::entity entity)
		{
			constexpr int kMaxAncestorDepth = 64;
			entt::entity current = entity;
			for (int depth = 0; depth < kMaxAncestorDepth && current != entt::null; ++depth)
			{
				if (const auto* record = scene.FindPrefabInstance(current))
					return record;
				const entt::registry& registry = scene.GetRegistry();
				if (!registry.valid(current))
					break;
				const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
				if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
					break;
				current = hierarchy->Parent;
			}
			return nullptr;
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
		// P4-U13b:每行的实例角色(0 = 普通实体,1 = 实例根,2 = 子树成员)与悬停说明。
		// 每帧重算:覆盖计数会变,而它改了不一定要重建行。
		std::vector<uint8_t> prefabRoles;
		std::vector<std::string> prefabTips;
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
		// enclosingRecord = 最近的祖先实例根记录(根行自己就是实例根时用它自己那条)。
		std::function<void(entt::entity, uint32_t, const Gameplay::PrefabInstanceRecord*)> visit =
			[&](entt::entity handle, uint32_t depth, const Gameplay::PrefabInstanceRecord* enclosingRecord)
		{
			constexpr uint32_t kMaxDisplayDepth = 64;
			if (depth > kMaxDisplayDepth || !visited.insert(static_cast<uint32_t>(handle)).second)
				return;
			entities.push_back(Entity(scene.get(), handle));
			depths.push_back(depth);
			const Gameplay::PrefabInstanceRecord* ownRecord = scene->FindPrefabInstance(handle);
			const Gameplay::PrefabInstanceRecord* activeRecord = ownRecord ? ownRecord : enclosingRecord;
			prefabRoles.push_back(activeRecord ? (ownRecord ? 1 : 2) : 0);
			prefabTips.push_back(activeRecord ? PrefabRowTooltip(*activeRecord, ownRecord != nullptr)
				: std::string());

			const auto* hierarchy = registry.try_get<HierarchyComponent>(handle);
			if (!hierarchy)
				return;
			// 折叠节点不展开子节点(但仍显示自身)。
			if (m_Collapsed.find(static_cast<uint32_t>(handle)) != m_Collapsed.end())
				return;
			// 子节点顺序即 HierarchyComponent::Children 的顺序(同级重排会改它),不再按名字排序。
			for (const entt::entity child : hierarchy->Children)
				if (registry.valid(child))
					visit(child, depth + 1, activeRecord);
		};
		for (const entt::entity root : roots)
			visit(root, 0, nullptr);
		for (auto handle : registry.view<UUIDComponent>())
			if (visited.find(static_cast<uint32_t>(handle)) == visited.end())
			{
				entities.push_back(Entity(scene.get(), handle));
				depths.push_back(0);
				const Gameplay::PrefabInstanceRecord* record = scene->FindPrefabInstance(handle);
				prefabRoles.push_back(record ? 1 : 0);
				prefabTips.push_back(record ? PrefabRowTooltip(*record, true) : std::string());
			}

		std::string orderKey;
		for (size_t i = 0; i < entities.size(); ++i)
		{
			// 实例角色进排序键:实例被断开/新建(实体集合没变)时也要重建行(缩进列会变)。
			orderKey += std::to_string(depths[i]) + ":" + std::to_string(prefabRoles[i]) + ":"
				+ labelOf(entities[i]) + '\n';
		}

		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_Scroll = std::make_shared<Wui::WuiScrollArea>();
			m_Root->Add(m_Scroll, { 0, 1e30f, 0, 1e30f, 1 });
		}

		// 实体集合/排序变化时重建行;否则只更新文本与选中态。
		// 场景对象本身更换(Edit↔Play:Play 走 CopyScene 的播放副本,是另一个 Scene)也必须重建:
		// 副本的名字与层级和编辑场景完全相同,orderKey 一样,但实体归属的 Scene* 不同。
		// 沿用旧行会让点击把"上一个场景的实体"设为选择,EditorLayer 的归属校验(实体必须属于
		// 活动场景)随即清空选择 —— 表现就是 Play 下点层级行,属性面板只有 "No entity selected"。
		bool rowsMatch = m_RowEntities.size() == entities.size();
		if (rowsMatch)
		{
			for (size_t i = 0; i < entities.size(); ++i)
				if (m_RowEntities[i] != entities[i])
				{
					rowsMatch = false;
					break;
				}
		}
		if (!rowsMatch || orderKey != m_LastOrderKey)
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
				// P4-U13b:实例行(根 + 子树成员)整体让出一列画徽标/成员点 —— 普通行不变。
				const bool prefabRow = index < prefabRoles.size() && prefabRoles[index] != 0;
				row->Indent = static_cast<float>(depth) * 14.0f + (prefabRow ? kPrefabMarkerSlot : 0.0f);
				// 折叠标记直接放进文本(避免额外的行内热区绘制):有子节点显示 +/-。
				const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
				const bool hasChildren = hierarchy && !hierarchy->Children.empty();
				const bool collapsed = m_Collapsed.find(static_cast<uint32_t>(static_cast<entt::entity>(entity)))
					!= m_Collapsed.end();
				row->Text = std::string(hasChildren ? (collapsed ? "+ " : "- ") : "  ") + labelOf(entity);
				// P4-U5a:行进无障碍树 —— id 用实体 handle(稳定,重排后仍是同一行),
				// value 给脚本一个可以直接喂给 scene.select / 属性面板的句柄。
				row->SetId(Wui::HashId(("hierarchy.row." + std::to_string(
					static_cast<uint32_t>(static_cast<entt::entity>(entity)))).c_str()));
				row->AccessValue = std::to_string(static_cast<uint32_t>(static_cast<entt::entity>(entity)));
				// 实例行的悬停说明必须写清"来自哪个 prefab、覆盖了几处":徽标只是视觉提示,
				// 说不清来源的话用户仍然要猜。
				const std::string selectionHint = Wui::Tr("panel.hierarchy.row.tooltip",
					"Click to select this entity (shows in the Properties panel)");
				row->AccessTooltip = prefabRow && index < prefabTips.size() && !prefabTips[index].empty()
					? prefabTips[index] + " — " + selectionHint : selectionHint;
				row->OnClick = [&host, entity]
				{
					if (std::getenv("WLD_TRACE_UI"))
					{
						// GetScene() 对无效句柄会抛异常,先判有效再取。
						const void* rowScene = entity.IsValid()
							? static_cast<const void*>(entity.GetScene()) : nullptr;
						WLD_CORE_INFO("[ui] hierarchy row clicked: handle={0} rowScene={1} activeScene={2}",
							static_cast<uint32_t>(static_cast<entt::entity>(entity)),
							rowScene,
							static_cast<const void*>(host.GetActiveScene().get()));
					}
					host.SetSelectedEntity(entity);
				};
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
		// 实例行的无障碍悬停说明每帧刷新(覆盖计数会变,而它不一定触发行重建)。
		for (size_t i = 0; i < m_Rows.size() && i < prefabTips.size(); ++i)
			if (!prefabTips[i].empty())
				m_Rows[i]->AccessTooltip = prefabTips[i] + " — " + Wui::Tr("panel.hierarchy.row.tooltip",
					"Click to select this entity (shows in the Properties panel)");
		m_Scroll->ContentHeight = entities.size() * 22.0f + 8.0f;

		Wui::LayoutWidgetTree(m_Root, { rect.X + 4, rect.Y + 4, rect.W - 8, rect.H - 8 });
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);

		// ---- P4-U13b:实例标记列(根行 = accent 徽标,实例子树内的其它行 = 淡色点)----
		// 画在行文本之前留出的标记列里(见 kPrefabMarkerSlot),不遮挡折叠标记与实体名。
		for (size_t i = 0; i < m_Rows.size() && i < prefabRoles.size(); ++i)
		{
			if (prefabRoles[i] == 0 || !m_Rows[i])
				continue;
			const Wui::WuiRect rowRect = m_Rows[i]->Rect();
			const float slotX = rowRect.X + 6.0f + (m_Rows[i]->Indent - kPrefabMarkerSlot);
			const std::string tip = i < prefabTips.size() ? prefabTips[i] : std::string();
			const uint32_t handle = static_cast<uint32_t>(static_cast<entt::entity>(m_RowEntities[i]));
			if (prefabRoles[i] == 1)
			{
				const Wui::WuiRect badge { slotX, rowRect.Y + 2.0f, 16.0f, 16.0f };
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, badge, theme.Accent, badge.H * 0.5f });
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
					{ badge.X + 3.0f, badge.Y + 2.0f, 0.0f, 0.0f },
					Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }, 0.0f, 1.0f,
					Wui::Tr("panel.hierarchy.prefab.badge", "预"), 11.0f, true });
				RegisterAccessNode(Wui::HashId(("hierarchy.prefab.badge." + std::to_string(handle)).c_str()),
					"prefab-badge", badge,
					Wui::Tr("panel.hierarchy.prefab.badge.label", "Prefab instance"), tip, true, tip, false);
			}
			else
			{
				// 淡色点:只表达"这一行属于某棵实例子树",不抢行文本的注意力。
				const Wui::WuiRect dot { slotX + 5.5f, rowRect.Y + rowRect.H * 0.5f - 2.5f, 5.0f, 5.0f };
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, dot,
					Wui::WuiColor { theme.TextMuted.R, theme.TextMuted.G, theme.TextMuted.B, 0.55f },
					dot.H * 0.5f });
			}
			if (!tip.empty())
				Wui::Tooltip(ctx, rowRect, tip);
		}

		// ---- U2d:场景里没有任何实体 → 统一空状态 ----
		// 只替换"列表内容"的表达;拖拽设父/拖入 .wprefab/空白处右键等既有入口全部保留在下面。
		if (entities.empty())
		{
			const Wui::WuiRect emptyRect { rect.X + 8.0f, rect.Y + 8.0f,
				std::max(0.0f, rect.W - 16.0f), std::max(0.0f, rect.H - 16.0f) };
			const bool createRequested = Wui::EmptyState(ctx, emptyRect, std::string(),
				Wui::Tr("panel.hierarchy.empty.title", "No entities in this scene"),
				Wui::Tr("panel.hierarchy.empty.hint",
					"Drag a model in from the Content Browser, or create an entity here."),
				Wui::Tr("panel.hierarchy.empty.action", "Create Empty Entity"),
				Wui::HashId("hierarchy.empty.create"), theme);
			if (createRequested)
			{
				// 与空白处右键菜单的 "Create Empty Entity" 同一条路径(结构写走延迟命令)。
				Entity created;
				if (scene->DeferStructuralChange([&created](Scene& s)
					{ created = Entity::CreateEntity(&s, "Empty Entity"); }) && created.IsValid())
				{
					host.SetSelectedEntity(created);
					host.MarkDocumentDirty();
				}
			}
		}

		// ---- W3b-2:拖拽设父(目标行 = 成为其子节点;非法目标由内核环路检测拒绝)----
		// 交互范式与 ContentBrowser 一致:按下即 BeginDrag(移动 >4px 才真正激活),
		// 悬停在候选行上时登记落点并高亮,拖拽结束后统一提交。
		std::string dragPayload;
		const bool dragging = ctx.IsDragActive(&dragPayload);
		for (size_t i = 0; i < m_Rows.size(); ++i)
		{
			const Wui::WuiRect rowRect = m_Rows[i]->Rect();
			const bool hovered = ctx.IsHovered(rowRect);
			// P4-U7:弹层上的按下不会落到这里 —— WuiContext 的捕获层/覆盖层矩形统一处理穿透。
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
		// 文档化路径:拖拽的"释放帧"由 AcceptDrop 交付载荷(此时 IsDragActive 已变 false),
		// 前置条件是悬停期间调用过 DropTarget 完成武装。这里同时兜住两条路径。
		std::string acceptedPayload;
		if (ctx.AcceptDrop(&acceptedPayload, "file:"))
		{
			WLD_CORE_INFO("[drop] hierarchy accepted payload '{0}'", acceptedPayload);
			if (acceptedPayload.size() > 13 &&
				acceptedPayload.compare(acceptedPayload.size() - 8, 8, ".wprefab") == 0)
				m_PendingPrefabFile = acceptedPayload.substr(5);
		}
		if (dragging && dragPayload.rfind("file:", 0) == 0)
		{
			static uint64_t s_LastLoggedHash = 0;
			const uint64_t hash = std::hash<std::string> {}(dragPayload);
			if (hash != s_LastLoggedHash)
			{
				s_LastLoggedHash = hash;
				WLD_CORE_INFO("[drop] hierarchy sees drag payload '{0}' (hovering panel={1})",
					dragPayload, static_cast<int>(ctx.IsHovered(rect)));
			}
		}
		if (!dragging && !m_PendingPrefabFile.empty())
		{
			// 内容浏览器载荷是"相对内容根"的路径:先按相对路径尝试,失败再用项目清单解析内容根。
			// 两者都不可用时仍把原始载荷交给引擎(其 VFS/磁盘解析链路可能认得该路径),
			// 由 InstantiateFromFile 反序列化的成败决定最终结果。
			std::filesystem::path prefabPath = m_PendingPrefabFile;
			if (!std::filesystem::exists(prefabPath))
			{
				bool resolved = false;
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
						{
							prefabPath = candidate;
							resolved = true;
						}
					}
				}
				if (!resolved)
					WLD_CORE_WARN("[drop] prefab path not resolved on disk, falling back to engine lookup: {0}",
						m_PendingPrefabFile);
			}

			const entt::entity parent = m_PendingDropHandle.IsValid()
				? static_cast<entt::entity>(m_PendingDropHandle) : entt::null;
			const Gameplay::PrefabInstanceResult instance =
				Gameplay::InstantiateFromFile(prefabPath, *scene, parent);
			if (instance.IsValid())
			{
				// P4-U13b:实例记录交给 Scene 持有(随 .wd 存档一起走,重开场景不再丢链接)。
				// 记录里存**逻辑路径**(内容浏览器载荷原本就是相对内容根的路径):存档可移植,
				// Revert/Apply 交给引擎的路径解析;只有"这一刻的实例化"才需要解析后的磁盘路径。
				scene->AddPrefabInstance(m_PendingPrefabFile, static_cast<entt::entity>(instance.Root));
				host.SetSelectedEntity(instance.Root);
				host.MarkDocumentDirty();
				WLD_CORE_INFO("Prefab '{0}' instantiated from hierarchy drop ({1} entities)",
					m_PendingPrefabFile, instance.EntityCount);
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
			// P4-U13b:实例三个动作常驻在属性面板实例条里,但右键菜单保留老入口 ——
			// 不再"不满足 CanRevert 就整块消失",而是灰态 + 可读原因(用户看得出为什么不可用)。
			const entt::entity contextHandle = static_cast<entt::entity>(m_Context);
			const Gameplay::PrefabInstanceRecord* contextRecord = OwningRecord(*scene, contextHandle);
			// 菜单高度 = 条目数 × 22 + 8:实例子树多出"来源说明 + 3 个动作"四行,
			// 旧实现把这几行画在声明高度之外(面板底图盖不住)。
			const size_t itemCount = 5u + (contextRecord ? 4u : 0u);
			Wui::WuiRect panel;
			if (Wui::BeginContextMenu(ctx, popup, m_MenuPos, 190.0f, itemCount, &panel, theme))
			{
				float itemY = panel.Y + 4.0f;
				const auto nextItem = [&itemY, &panel]()
				{
					const Wui::WuiRect item { panel.X + 4.0f, itemY, panel.W - 8.0f, 22.0f };
					itemY += 22.0f;
					return item;
				};
				if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.duplicate"),
					nextItem(), "Duplicate", theme))
				{
					host.DuplicateSelectedEntity();
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.delete"),
					nextItem(), "Delete", theme))
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
					nextItem(),
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
					nextItem(), "Unparent", theme))
				{
					const entt::entity child = m_Context;
					scene->DeferStructuralChange([child](Scene& s) { Hierarchy::ClearParent(s.GetRegistry(), child); });
					host.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
				// W4-2c:把选中实体的子树导出为 .wprefab(落在内容根 prefabs/ 下)。
				// 说明:首版不做文件对话框,固定目录 + 以 Tag 命名,便于立刻验证拖拽实例化链路。
				if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.exportprefab"),
					nextItem(), "Export as Prefab (.wprefab)", theme))
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
				// P4-U13b:实例行 —— 来源说明 + 三个动作(记录由 Scene 持有,不再有面板私有表)。
				if (contextRecord)
				{
					const Wui::WuiRect infoItem = nextItem();
					const std::string source = std::filesystem::path(contextRecord->PrefabPath).filename().string();
					const std::string infoText = Wui::Tr("panel.hierarchy.prefab.menu.source", "Prefab instance: ")
						+ (source.empty() ? Wui::Tr("panel.hierarchy.prefab.menu.nosource", "(no source)") : source)
						+ " · " + std::to_string(Gameplay::GetOverrideCount(*contextRecord)) + " "
						+ Wui::Tr("panel.hierarchy.prefab.overrides", "override(s)");
					Wui::Label(ctx, { infoItem.X + 8.0f, infoItem.Y + 3.0f },
						infoText, theme.TextMuted, 12.0f);
					RegisterAccessNode(Wui::HashId("hierarchy.prefab.menu.source"), "text", infoItem,
						infoText, std::string(), false, infoText, false);

					// 实例动作作用在**整棵子树**上:右键子节点也按它所属的实例根处理。
					const Entity instanceRoot(scene.get(), contextRecord->Root);
					const bool sourceMissing = contextRecord->PrefabPath.empty();
					const bool canRevert = !sourceMissing && Gameplay::CanRevert(*contextRecord, *scene);
					const std::string revertHint = canRevert
						? Wui::Tr("panel.hierarchy.prefab.revert.tooltip",
							"Discard overrides and restore this subtree from the prefab asset")
						: Wui::Tr("panel.hierarchy.prefab.revert.blocked",
							"Unavailable: the source asset path is missing or the instance root is gone");
					const Wui::WuiRect revertItem = nextItem();
					if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.prefabrevert"), revertItem,
						canRevert ? Wui::Tr("panel.hierarchy.prefab.revert", "Revert to Asset")
							: Wui::Tr("panel.hierarchy.prefab.revert", "Revert to Asset") + " — "
								+ Wui::Tr("panel.hierarchy.prefab.blocked_short", "unavailable"),
						theme, canRevert))
					{
						std::string message;
						if (!host.PrefabInstanceRevert(instanceRoot, &message))
							host.Notify(message);
						ctx.CloseAllPopups();
					}
					else if (!canRevert && ctx.IsHovered(revertItem))
						ctx.SetTooltip(revertHint);

					const std::string applyHint = sourceMissing
						? Wui::Tr("panel.hierarchy.prefab.apply.blocked",
							"Unavailable: the instance has no source asset path")
						: Wui::Tr("panel.hierarchy.prefab.apply.tooltip",
							"Write this subtree back to the prefab asset (overwrites the asset)");
					const Wui::WuiRect applyItem = nextItem();
					if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.prefabapply"), applyItem,
						Wui::Tr("panel.hierarchy.prefab.apply", "Apply to Asset"), theme, !sourceMissing))
					{
						OpenPrefabConfirm(ctx, host, PrefabConfirmAction::Apply, instanceRoot,
							contextRecord->PrefabPath);
						ctx.CloseAllPopups();
					}
					else if (sourceMissing && ctx.IsHovered(applyItem))
						ctx.SetTooltip(applyHint);

					const Wui::WuiRect unpackItem = nextItem();
					if (Wui::ContextMenuItem(ctx, Wui::HashId("hierarchy.prefabunpack"), unpackItem,
						Wui::Tr("panel.hierarchy.prefab.unpack", "Unpack (break link)"), theme))
					{
						OpenPrefabConfirm(ctx, host, PrefabConfirmAction::Unpack, instanceRoot,
							contextRecord->PrefabPath);
						ctx.CloseAllPopups();
					}
					else if (ctx.IsHovered(unpackItem))
						ctx.SetTooltip(Wui::Tr("panel.hierarchy.prefab.unpack.tooltip",
							"Turn this subtree into plain entities: it no longer follows the asset "
							"(existing values stay)"));
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

		// ---- P4-U13b:实例破坏性动作的确认模态(与属性面板"移除组件"同一套模态通道)----
		if (m_PrefabConfirm != PrefabConfirmAction::None)
			DrawPrefabConfirm(ctx, host);
	}

	void HierarchyPanel::OpenPrefabConfirm(Wui::WuiContext& ctx, PanelHost& host,
		PrefabConfirmAction action, Entity root, const std::string& source)
	{
		m_PrefabConfirm = action;
		m_PrefabConfirmRoot = root;
		m_PrefabConfirmSource = source;
		m_PrefabConfirmModal = Wui::HashId(action == PrefabConfirmAction::Apply
			? "hierarchy.prefab.apply.modal" : "hierarchy.prefab.unpack.modal");
		ctx.SetModal(m_PrefabConfirmModal);
		// 面板级模态:宿主帧初封锁整窗输入,渲染本面板前解开(与属性面板同一条路径)。
		host.SetPanelModalOwner(Id());
		ctx.RecordOp("hierarchy", action == PrefabConfirmAction::Apply
			? "prefab-apply-ask" : "prefab-unpack-ask", source, std::string());
	}

	void HierarchyPanel::ClosePrefabConfirm(Wui::WuiContext& ctx, PanelHost& host)
	{
		m_PrefabConfirm = PrefabConfirmAction::None;
		m_PrefabConfirmRoot = Entity();
		m_PrefabConfirmSource.clear();
		m_PrefabConfirmModal = 0;
		ctx.ClearModal();
		host.SetPanelModalOwner(std::string());
	}

	void HierarchyPanel::DrawPrefabConfirm(Wui::WuiContext& ctx, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const bool apply = m_PrefabConfirm == PrefabConfirmAction::Apply;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = m_PrefabConfirmModal;
		frameDesc.Title = apply
			? Wui::Tr("panel.hierarchy.prefab.apply.confirm_title", "Apply to Prefab Asset")
			: Wui::Tr("panel.hierarchy.prefab.unpack.confirm_title", "Unpack (Break Prefab Link)");
		frameDesc.Size = { 470.0f, 180.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
			return;

		// 正文说清后果:会覆盖资产 / 之后不再跟随资产 —— 破坏性操作不能只给一个按钮。
		// 逐行给(Wui::Label 不换行),不让说明被省略号吃掉。
		const std::array<std::string, 3> bodyLines = apply
			? std::array<std::string, 3> {
				Wui::Tr("panel.hierarchy.prefab.apply.confirm_body",
					"Write this instance back to the prefab asset?"),
				Wui::Tr("panel.hierarchy.prefab.apply.confirm_body2",
					"The asset file will be overwritten, and its other instances"),
				Wui::Tr("panel.hierarchy.prefab.apply.confirm_body3",
					"will follow the new values.") }
			: std::array<std::string, 3> {
				Wui::Tr("panel.hierarchy.prefab.unpack.confirm_body",
					"Break the link to the prefab asset?"),
				Wui::Tr("panel.hierarchy.prefab.unpack.confirm_body2",
					"These entities stay as they are now, but they"),
				Wui::Tr("panel.hierarchy.prefab.unpack.confirm_body3",
					"stop following the asset (revert/apply go away).") };
		for (int line = 0; line < 3; ++line)
			Wui::Label(ctx, { frame.X + 16.0f, frame.Y + 50.0f + 18.0f * static_cast<float>(line) },
				bodyLines[line], theme.Text, 13.0f);
		Wui::LabelWithTerm(ctx, { frame.X + 16.0f, frame.Y + 108.0f },
			std::filesystem::path(m_PrefabConfirmSource).filename().string(), std::string(),
			theme.Warning, 13.0f, theme, frame.W - 32.0f);

		const Wui::ModalButtonDesc buttons[2] = {
			{ Wui::Tr("panel.hierarchy.prefab.confirm_cancel", "Cancel"),
				Wui::HashId("hierarchy.prefab.confirm.cancel"), true },
			{ apply ? Wui::Tr("panel.hierarchy.prefab.apply.confirm", "Apply to Asset")
				: Wui::Tr("panel.hierarchy.prefab.unpack.confirm", "Unpack"),
				Wui::HashId("hierarchy.prefab.confirm.ok"), true },
		};
		const int clicked = Wui::ModalButtons(ctx, frame, buttons, 2, theme);
		bool closeRequested = false;
		if (clicked == 1)
		{
			std::string message;
			const bool ok = apply
				? host.PrefabInstanceApply(m_PrefabConfirmRoot, &message)
				: host.PrefabInstanceUnpack(m_PrefabConfirmRoot, &message);
			if (!message.empty())
				host.Notify(message);
			if (ok)
				ctx.RecordOp("hierarchy", apply ? "prefab-apply" : "prefab-unpack",
					m_PrefabConfirmSource, message);
			else
				WLD_CORE_WARN("Prefab {0} failed: {1}", apply ? "apply" : "unpack", message);
			closeRequested = true;
		}
		else if (clicked == 0 || escapePressed)
			closeRequested = true;
		// 先收 overlay 再清模态态(BeginModalFrame/EndModalFrame 必须成对)。
		Wui::EndModalFrame(ctx);
		if (closeRequested && ctx.Modal() == frameDesc.Id)
			ClosePrefabConfirm(ctx, host);
	}

	bool HierarchyPanel::DebugInvokeRowClick(size_t index)
	{
		// 直接调用行控件保存的 OnClick:与鼠标点击走完全相同的回调(含其捕获的实体)。
		if (index >= m_Rows.size() || !m_Rows[index] || !m_Rows[index]->OnClick)
			return false;
		m_Rows[index]->OnClick();
		return true;
	}
}
