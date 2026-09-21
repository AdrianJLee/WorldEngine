#include "wldpch.h"
#include "PrefabPanel.h"

#include "World/Core/Application.h"
#include "World/Gameplay/PrefabTypes.h"
#include "World/Scene/Components.h"
#include "World/Scene/SceneSerializer.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <unordered_map>

namespace World
{
	namespace
	{
		// 行/段间距与列表行高:与其它资产面板(Scripts/Properties)同一密度。
		constexpr float kRowHeight = 22.0f;
		constexpr float kSectionHeaderHeight = 20.0f;
		// 左树右详情会退化成一列的门槛(与材质/模型面板的窄布局口径一致)。
		constexpr float kTwoColumnMinWidth = 560.0f;
		// 读盘节流:同一份资产 2s 内不重复反序列化(不做逐帧读盘)。
		constexpr double kDiskScanInterval = 2.0;

		double NowSeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		std::string NormalizePath(std::string path)
		{
			std::replace(path.begin(), path.end(), '\\', '/');
			return path;
		}

		// 按 UTF-8 码点边界截断(直接按字节切会切碎中文,文本渲染拿到半个序列)。
		std::string TruncateUtf8(const std::string& text, std::size_t maxBytes)
		{
			if (text.size() <= maxBytes)
				return text;
			std::size_t cut = maxBytes;
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
				--cut;
			return text.substr(0, cut) + "…";
		}

		std::string FormatVec3(const glm::vec3& value)
		{
			char buffer[96] = {};
			std::snprintf(buffer, sizeof(buffer), "%.3g, %.3g, %.3g", value.x, value.y, value.z);
			return buffer;
		}

		std::string FormatFloat(float value, int decimals = 3)
		{
			char buffer[48] = {};
			std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
			return buffer;
		}

		// ---- 无障碍登记(与 WuiWidgets.cpp / PropertiesPanel 同一格式) ----
		void RegisterNode(Wui::WuiId id, const char* kind, const Wui::WuiRect& rect, const std::string& label,
			const std::string& value, bool enabled, const std::string& tooltip, bool interactive)
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
			// 不可用的控件不可被 ui.invoke 点击(与真实鼠标路径一致)。
			node.Interactive = enabled && interactive;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// 动作按钮:与属性面板实例条同一套画法。不可用时弱化绘制,并把"为什么不可用"
		// 同时写进无障碍节点(tooltip)与悬停提示 —— 灰按钮不能没有理由。
		bool PanelActionButton(Wui::WuiContext& ctx, const char* idText, const Wui::WuiRect& rect,
			const std::string& label, const std::string& tooltip, bool enabled, const Wui::WuiTheme& theme)
		{
			const bool hovered = ctx.IsHovered(rect);
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect,
				enabled ? (hovered ? theme.ButtonHover : theme.ButtonBg) : theme.PanelBg, 3.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, rect,
				hovered && enabled ? theme.Accent : theme.Border, 3.0f, 1.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
				{ rect.X + 9.0f, rect.Y + (rect.H - 13.0f) * 0.5f, 0.0f, 0.0f },
				enabled ? theme.Text : theme.TextDisabled, 0.0f, 1.0f, label, 13.0f, false });
			RegisterNode(Wui::HashId(idText), "button", rect, label, tooltip, enabled, tooltip, true);
			if (hovered)
			{
				if (enabled)
					ctx.SetCursor(Wui::WuiCursor::Hand);
				if (!tooltip.empty())
					ctx.SetTooltip(tooltip);
			}
			return enabled && ctx.IsClicked(rect);
		}

		struct RowSpec
		{
			std::string Id;
			std::string Label;        // 无障碍名字 = 行文本的语义内容(脚本/读屏读这一份)
			std::string DisplayText;  // 画出来的文本(空 = 与 Label 相同;实体树用缩进表达层级)
			std::string Value;
			std::string Tooltip;
			bool Interactive = false;
		};

		// 只读清单/树的统一画法:滚动裁剪 + 行底 + 选中高亮 + 无障碍节点。
		// 返回被点中的行下标(-1 = 没有点击)。空表画 emptyText 并**不**登记行节点。
		int DrawRows(Wui::WuiContext& ctx, const Wui::WuiRect& viewport, float& scroll,
			const std::vector<RowSpec>& rows, int selected, const Wui::WuiTheme& theme,
			const std::string& emptyText)
		{
			if (rows.empty())
			{
				Wui::Label(ctx, { viewport.X + 4.0f, viewport.Y + 2.0f }, emptyText, theme.TextMuted, 12.0f);
				return -1;
			}
			const float contentHeight = kRowHeight * static_cast<float>(rows.size());
			Wui::BeginScrollArea(ctx, viewport, contentHeight, scroll, theme);
			int clicked = -1;
			for (std::size_t i = 0; i < rows.size(); ++i)
			{
				const Wui::WuiRect row { viewport.X, viewport.Y - scroll + kRowHeight * static_cast<float>(i),
					viewport.W, kRowHeight - 2.0f };
				const bool hovered = ctx.IsHovered(row);
				const bool isSelected = static_cast<int>(i) == selected;
				if (hovered && rows[i].Interactive)
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, theme.ButtonHover, 2.0f });
				else if (isSelected)
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, theme.Selection, 2.0f });
				const std::string& shown = rows[i].DisplayText.empty() ? rows[i].Label : rows[i].DisplayText;
				Wui::Label(ctx, { row.X + 6.0f, row.Y + 3.0f }, TruncateUtf8(shown, 110),
					isSelected ? theme.Text : theme.TextMuted, 12.0f);
				RegisterNode(Wui::HashId(rows[i].Id.c_str()), "list-row", row, rows[i].Label, rows[i].Value,
					true, rows[i].Tooltip, rows[i].Interactive);
				if (rows[i].Interactive && hovered)
				{
					ctx.SetCursor(Wui::WuiCursor::Hand);
					if (!rows[i].Tooltip.empty())
						ctx.SetTooltip(rows[i].Tooltip);
				}
				if (rows[i].Interactive && ctx.IsClicked(row))
					clicked = static_cast<int>(i);
			}
			Wui::EndScrollArea(ctx);
			return clicked;
		}
	}

	PrefabPanel::PrefabPanel(std::string logicalPath)
	{
		m_LogicalPath = NormalizePath(std::move(logicalPath));
		m_PanelId = "prefab:" + m_LogicalPath;
		const std::string name = std::filesystem::path(m_LogicalPath).filename().string();
		m_PanelTitle = "Prefab - " + (name.empty() ? m_LogicalPath : name);
		m_Status = Wui::Tr("panel.prefab.status.loading", "Loading prefab...");
	}

	PrefabPanel::~PrefabPanel() = default;

	bool PrefabPanel::ReloadNow(Scene* activeScene, std::string* message)
	{
		RefreshFromDisk(activeScene, true);
		if (message)
			*message = m_Status;
		return m_DocumentValid;
	}

	void PrefabPanel::RefreshFromDisk(Scene* activeScene, bool force)
	{
		const double now = NowSeconds();
		if (!force && now < m_NextDiskScan)
			return;
		m_NextDiskScan = now + kDiskScanInterval;
		LoadStaging(activeScene);
	}

	void PrefabPanel::LoadStaging(Scene* activeScene)
	{
		m_Staging = nullptr;
		m_Rows.clear();
		m_Assets.clear();
		m_DocumentValid = false;

		// staging 场景的上下文:优先当前文档场景(与它同一份 schemas/VFS),没有就用应用上下文。
		WorldContext* context = activeScene ? &activeScene->GetContext()
			: (Application::HasInstance() ? &Application::Get().GetContext() : nullptr);
		if (!context)
		{
			m_Status = Wui::Tr("panel.prefab.status.no_context", "Editor context is not ready yet");
			m_StatusIsError = true;
			return;
		}

		// 每次重扫都换一个干净场景:Deserialize 不负责清空旧实体,复用会让实体越读越多。
		m_Staging = CreateRef<Scene>(*context);
		const std::filesystem::path absolute =
			std::filesystem::path(std::string(WLD_ASSETPATH)) / m_LogicalPath;
		std::error_code ec;
		const bool onDisk = std::filesystem::is_regular_file(absolute, ec);

		// 与 Gameplay::InstantiateFromFile 同一条读法:不做 exists 预检,以反序列化结果为准
		// (内容根下的相对路径由 VFS/磁盘链路解析)。
		SceneSerializer serializer(m_Staging);
		if (!serializer.Deserialize(absolute.string()))
		{
			const std::string reason = serializer.GetLastError();
			m_Status = onDisk
				? std::string(Wui::Tr("panel.prefab.status.parse_failed",
					"Not a parseable prefab document: ")) + reason
				: std::string(Wui::Tr("panel.prefab.status.missing", "Prefab file not found: ")) + m_LogicalPath;
			m_StatusIsError = true;
			m_Staging = nullptr;
			return;
		}

		m_DocumentValid = true;
		RebuildRows();
		m_Assets = CollectReferencedAssets();
		m_Status = std::to_string(m_Rows.size()) + " entities · "
			+ std::to_string(m_Assets.size()) + " referenced assets";
		m_StatusIsError = false;
	}

	void PrefabPanel::RebuildRows()
	{
		m_Rows.clear();
		if (!m_Staging)
			return;
		const entt::registry& registry = m_Staging->GetRegistry();

		std::vector<entt::entity> handles;
		// 枚举全部实体:实体存储本身可迭代,且不要求实体带任何组件。
		if (const auto* entities = registry.storage<entt::entity>())
			for (const entt::entity handle : *entities)
				handles.push_back(handle);

		// 层级顺序:按 Parent 建反向索引(Children 是加载后重建的缓存,这里自己算一份更稳)。
		std::unordered_map<entt::entity, std::vector<entt::entity>> children;
		std::vector<entt::entity> roots;
		for (const entt::entity handle : handles)
		{
			const auto* hierarchy = registry.try_get<HierarchyComponent>(handle);
			const bool hasParent = hierarchy && hierarchy->Parent != entt::null
				&& registry.valid(hierarchy->Parent);
			if (hasParent)
				children[hierarchy->Parent].push_back(handle);
			else
				roots.push_back(handle);
		}

		const Schema::SchemaRegistry& schemas = m_Staging->GetContext().Schemas();
		const auto componentCount = [&registry, &schemas](entt::entity handle)
		{
			std::size_t count = 0;
			for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
			{
				if (!schema || !schema->Storage)
					continue;
				// 结构只读枚举:走 const registry 的 storage,不触发"活动场景禁止结构写"。
				const auto* storage = registry.storage(schema->Storage->ComponentId);
				if (storage && storage->contains(handle))
					++count;
			}
			return count;
		};

		// 深度上限:非法层级(自环/互指)不能让重扫挂死。
		constexpr int kMaxDepth = 64;
		std::function<void(entt::entity, int)> visit = [&](entt::entity handle, int depth)
		{
			if (depth > kMaxDepth)
				return;
			const auto* tag = registry.try_get<TagComponent>(handle);
			EntityRow row;
			row.Handle = handle;
			row.Name = (tag && !tag->Tag.empty())
				? tag->Tag : ("Entity " + std::to_string(static_cast<uint32_t>(handle)));
			row.ComponentCount = componentCount(handle);
			row.Depth = depth;
			m_Rows.push_back(std::move(row));
			if (const auto found = children.find(handle); found != children.end())
				for (const entt::entity child : found->second)
					visit(child, depth + 1);
		};
		for (const entt::entity root : roots)
			visit(root, 0);
	}

	std::vector<std::string> PrefabPanel::BuildComponentSummary(std::size_t rowIndex) const
	{
		std::vector<std::string> lines;
		if (!m_Staging || rowIndex >= m_Rows.size())
			return lines;
		const entt::entity handle = m_Rows[rowIndex].Handle;
		const entt::registry& registry = m_Staging->GetRegistry();
		if (!registry.valid(handle))
			return lines;

		// 只读摘要:组件名 + 关键字段。没列进来的组件仍按 schema 名字列一行(树行数/组件数
		// 与摘要能对上),但这一层不暴露可写控件。
		if (const auto* tag = registry.try_get<TagComponent>(handle))
			lines.push_back("Tag: " + tag->Tag);
		if (const auto* transform = registry.try_get<TransformComponent>(handle))
			lines.push_back("Transform: T(" + FormatVec3(transform->Location) + ")  R("
				+ FormatVec3(transform->Rotation) + ")  S(" + FormatVec3(transform->Scale) + ")");
		if (const auto* mesh = registry.try_get<MeshRendererComponent>(handle))
			lines.push_back("MeshRenderer: Primitive=" + (mesh->Primitive.empty() ? std::string("(none)") : mesh->Primitive)
				+ "  MeshPath=" + (mesh->MeshPath.empty() ? std::string("(none)") : mesh->MeshPath)
				+ "  MaterialPath=" + (mesh->MaterialPath.empty() ? std::string("(none)") : mesh->MaterialPath));
		if (const auto* skinned = registry.try_get<SkinnedMeshRendererComponent>(handle))
			lines.push_back("SkinnedMeshRenderer: MeshPath="
				+ (skinned->MeshPath.empty() ? std::string("(none)") : skinned->MeshPath)
				+ "  MaterialPath=" + (skinned->MaterialPath.empty() ? std::string("(none)") : skinned->MaterialPath));
		if (const auto* camera = registry.try_get<CameraComponent>(handle))
		{
			const bool perspective = camera->Camera.GetProjectionType() == SceneCamera::ProjectionType::Perspective;
			lines.push_back(std::string("Camera: Projection=") + (perspective ? "Perspective" : "Orthographic")
				+ "  Fov=" + FormatFloat(camera->Camera.GetPerspectiveFOV(), 1)
				+ "  Primary=" + (camera->Primary ? "yes" : "no"));
		}
		if (const auto* sprite = registry.try_get<SpriteComponent>(handle))
			lines.push_back("Sprite: TilingFactor=" + FormatFloat(sprite->TilingFactor, 2));
		return lines;
	}

	std::vector<std::string> PrefabPanel::CollectReferencedAssets() const
	{
		std::vector<std::string> assets;
		if (!m_Staging)
			return assets;
		const entt::registry& registry = m_Staging->GetRegistry();
		const auto add = [&assets](const std::string& path)
		{
			if (path.empty())
				return;
			if (std::find(assets.begin(), assets.end(), path) == assets.end())
				assets.push_back(path);
		};
		const auto* entities = registry.storage<entt::entity>();
		if (!entities)
			return assets;
		for (const entt::entity handle : *entities)
		{
			if (const auto* mesh = registry.try_get<MeshRendererComponent>(handle))
			{
				add(mesh->MeshPath);
				add(mesh->MaterialPath);
			}
			if (const auto* skinned = registry.try_get<SkinnedMeshRendererComponent>(handle))
			{
				add(skinned->MeshPath);
				add(skinned->MaterialPath);
			}
		}
		std::sort(assets.begin(), assets.end());
		return assets;
	}

	void PrefabPanel::RefreshInstances(Scene* scene)
	{
		m_Instances.clear();
		if (!scene)
			return;
		const Scene& sceneRef = *scene;
		const entt::registry& registry = sceneRef.GetRegistry();
		const std::string target = NormalizePath(m_LogicalPath);
		for (const Gameplay::PrefabInstanceRecord& record : sceneRef.PrefabInstances())
		{
			if (NormalizePath(record.PrefabPath) != target)
				continue;
			if (record.Root == entt::null || !registry.valid(record.Root))
				continue;
			InstanceRow row;
			row.Handle = record.Root;
			const auto* tag = registry.try_get<TagComponent>(record.Root);
			row.Name = (tag && !tag->Tag.empty())
				? tag->Tag : ("Entity " + std::to_string(static_cast<uint32_t>(record.Root)));
			m_Instances.push_back(std::move(row));
		}
	}

	void PrefabPanel::SelectInstance(PanelHost& host, Scene* scene, entt::entity root)
	{
		if (!scene || root == entt::null)
			return;
		const entt::registry& registry = scene->GetRegistry();
		if (!registry.valid(root))
			return;
		host.SetSelectedEntity(Entity(scene, root));
		m_SelectedInstance = root;
	}

	void PrefabPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, { 0.10f, 0.105f, 0.115f, 1.0f });

		const Ref<Scene> activeScene = host.GetActiveScene();
		Scene* const scene = activeScene.get();
		// 读盘带 2s TTL(每帧最多一次);实例表来自场景内存,逐帧刷新。
		RefreshFromDisk(scene, false);
		RefreshInstances(scene);
		if (!m_Rows.empty())
			m_SelectedRow = std::clamp(m_SelectedRow, 0, static_cast<int>(m_Rows.size()) - 1);
		else
			m_SelectedRow = 0;
		if (m_SelectedAsset >= static_cast<int>(m_Assets.size()))
			m_SelectedAsset = -1;

		const float pad = 8.0f;
		float y = rect.Y + 6.0f;

		// ---- 头部:标题 + 三个动作(全部带悬停说明 + 无障碍 id)----
		const std::string fileName = std::filesystem::path(m_LogicalPath).filename().string();
		const std::string title = std::string(Wui::Tr("panel.prefab.title", "Prefab")) + ": "
			+ (fileName.empty() ? m_LogicalPath : fileName);
		if (rect.W >= 380.0f)
			Wui::Label(ctx, { rect.X + pad, y + 4.0f }, TruncateUtf8(title, 72), theme.Text, 14.0f);

		const bool hasDocument = m_DocumentValid;
		const bool hasInstance = !m_Instances.empty();
		// Play/Simulate = 只读查看:实例化会写活动场景结构(编辑器其它入口同样是"仅编辑态")。
		const bool readOnly = host.IsReadOnlyMode();
		const bool canPlace = hasDocument && !readOnly;
		const std::string unreadableHint = Wui::Tr("panel.prefab.blocked.unreadable",
			"Unavailable: this file is not a parseable prefab document (see the status line).");
		const std::string placeText = Wui::Tr("panel.prefab.place", "Place in Scene");
		const std::string editText = Wui::Tr("panel.prefab.edit", "Edit Prefab");
		const std::string locateText = Wui::Tr("panel.prefab.locate", "Locate Instance");
		const std::string placeHint = !hasDocument ? unreadableHint
			: (readOnly
				? Wui::Tr("panel.prefab.place.readonly",
					"Read-only while Play/Simulate is running; exit Play to place the prefab.")
				: Wui::Tr("panel.prefab.place.tooltip",
					"Instantiate this prefab into the current scene (world origin)."));
		const std::string editHint = hasDocument
			? Wui::Tr("panel.prefab.edit.tooltip",
				"Open the prefab document edit session (3D viewport + gizmo); saving writes the asset back.")
			: unreadableHint;
		const std::string locateHint = hasInstance
			? Wui::Tr("panel.prefab.locate.tooltip",
				"Select this prefab's first instance root in the Hierarchy panel.")
			: Wui::Tr("panel.prefab.locate.disabled",
				"Disabled: the current scene has no instance of this prefab (use Place in Scene first).");

		const float buttonH = 24.0f;
		float buttonRight = rect.X + rect.W - pad;
		const auto placeButton = [&](const char* id, const std::string& text, float& right, bool enabled,
			const std::string& hint)
		{
			const float width = std::max(96.0f, ctx.MeasureTextWidth(text, 13.0f) + 20.0f);
			const Wui::WuiRect button { right - width, y, width, buttonH };
			right = button.X - 6.0f;
			return PanelActionButton(ctx, id, button, text, hint, enabled, theme);
		};
		const bool locateClicked = placeButton("prefab.locate", locateText, buttonRight, hasInstance, locateHint);
		const bool editClicked = placeButton("prefab.edit", editText, buttonRight, hasDocument, editHint);
		const bool placeClicked = placeButton("prefab.place", placeText, buttonRight, canPlace, placeHint);
		y += buttonH + 6.0f;

		if (placeClicked)
		{
			std::string message;
			if (host.InstantiatePrefabAsset(m_LogicalPath, &message))
			{
				m_Status = message.empty() ? ("placed " + m_LogicalPath) : message;
				m_StatusIsError = false;
			}
			else
			{
				m_Status = message.empty() ? "place failed" : message;
				m_StatusIsError = true;
				host.Notify(m_Status);
			}
		}
		if (editClicked)
		{
			// 编辑仍然走文档会话(窗口只负责"看/管理");横幅出现后保存 = 写回这个资产。
			host.OpenPrefabEditor(m_LogicalPath);
			m_Status = std::string(Wui::Tr("panel.prefab.status.editing", "Opened the edit session: "))
				+ m_LogicalPath;
			m_StatusIsError = false;
		}
		if (locateClicked && hasInstance)
			SelectInstance(host, scene, m_Instances.front().Handle);

		// ---- 状态行(解析失败时写可读原因)----
		const Wui::WuiRect statusRect { rect.X + pad, y, std::max(0.0f, rect.W - pad * 2.0f), 18.0f };
		RegisterNode(Wui::HashId("prefab.status"), "status", statusRect,
			Wui::Tr("panel.prefab.status", "Prefab status"), m_Status, true,
			m_StatusIsError ? m_Status : std::string(), false);
		Wui::Label(ctx, { statusRect.X + 1.0f, statusRect.Y + 2.0f }, TruncateUtf8(m_Status, 180),
			m_StatusIsError ? theme.Danger : theme.TextMuted, 12.0f);
		y += statusRect.H + 6.0f;

		// ---- 内容:左树右详情;窄窗口退化成单列 ----
		const Wui::WuiRect content { rect.X + pad, y, std::max(0.0f, rect.W - pad * 2.0f),
			std::max(0.0f, rect.Y + rect.H - pad - y) };
		Wui::WuiRect treeRect = content;
		Wui::WuiRect detailRect = content;
		if (content.W >= kTwoColumnMinWidth)
		{
			const float leftWidth = std::clamp(content.W * 0.42f, 200.0f, content.W - 220.0f);
			treeRect = { content.X, content.Y, leftWidth, content.H };
			detailRect = { content.X + leftWidth + 10.0f, content.Y,
				std::max(0.0f, content.W - leftWidth - 10.0f), content.H };
		}
		else
		{
			const float treeHeight = std::max(60.0f, content.H * 0.42f);
			treeRect = { content.X, content.Y, content.W, treeHeight };
			detailRect = { content.X, content.Y + treeHeight + 8.0f, content.W,
				std::max(0.0f, content.H - treeHeight - 8.0f) };
		}

		DrawEntityTree(ctx, treeRect, theme);
		const float detailsHeight = std::floor(detailRect.H * 0.42f);
		const float assetsHeight = std::floor(detailRect.H * 0.27f);
		const float instancesHeight = std::max(0.0f, detailRect.H - detailsHeight - assetsHeight);
		DrawDetails(ctx, { detailRect.X, detailRect.Y, detailRect.W, detailsHeight }, theme);
		DrawAssets(ctx, { detailRect.X, detailRect.Y + detailsHeight, detailRect.W, assetsHeight }, theme);
		DrawInstances(ctx, { detailRect.X, detailRect.Y + detailsHeight + assetsHeight, detailRect.W,
			instancesHeight }, host, scene, theme);
	}

	void PrefabPanel::DrawEntityTree(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		const std::string header = std::string(Wui::Tr("panel.prefab.entities", "Entities")) + " ("
			+ std::to_string(m_Rows.size()) + ")";
		Wui::SectionHeader(ctx, { rect.X, rect.Y, rect.W, kSectionHeaderHeight }, header, theme.Accent, theme);
		RegisterNode(Wui::HashId("prefab.entities"), "list", { rect.X, rect.Y, rect.W, kSectionHeaderHeight },
			Wui::Tr("panel.prefab.entities", "Entities"), std::to_string(m_Rows.size()), true, std::string(), false);

		const Wui::WuiRect viewport { rect.X, rect.Y + kSectionHeaderHeight + 1.0f, rect.W,
			std::max(0.0f, rect.H - kSectionHeaderHeight - 1.0f) };
		std::vector<RowSpec> rows;
		rows.reserve(m_Rows.size());
		for (std::size_t i = 0; i < m_Rows.size(); ++i)
		{
			RowSpec row;
			row.Id = "prefab.entity." + std::to_string(i);
			// 行文本口径 = "名字 (N components)";树形层级只在画面上缩进,不进无障碍名字
			// (脚本/读屏拿到的就是这一行说了什么,不掺布局空白)。
			row.Label = m_Rows[i].Name + " (" + std::to_string(m_Rows[i].ComponentCount) + " components)";
			row.DisplayText = std::string(static_cast<std::size_t>(std::max(0, m_Rows[i].Depth)) * 2, ' ')
				+ row.Label;
			row.Value = "#" + std::to_string(static_cast<uint32_t>(m_Rows[i].Handle));
			row.Tooltip = Wui::Tr("panel.prefab.entity_row.tooltip",
				"Click to inspect this entity's components (read-only).");
			row.Interactive = true;
			rows.push_back(std::move(row));
		}
		const std::string emptyText = m_DocumentValid
			? Wui::Tr("panel.prefab.entities.none", "This prefab has no entities")
			: Wui::Tr("panel.prefab.entities.unavailable", "No entities: the prefab could not be read");
		const int clicked = DrawRows(ctx, viewport, m_TreeScroll, rows, m_SelectedRow, theme, emptyText);
		if (clicked >= 0)
			m_SelectedRow = clicked;
	}

	void PrefabPanel::DrawDetails(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		std::vector<std::string> lines;
		const bool rowSelected = m_DocumentValid && m_SelectedRow >= 0
			&& m_SelectedRow < static_cast<int>(m_Rows.size());
		if (rowSelected)
			lines = BuildComponentSummary(static_cast<std::size_t>(m_SelectedRow));

		const std::string header = std::string(Wui::Tr("panel.prefab.components", "Components")) + " ("
			+ std::to_string(lines.size()) + ")";
		Wui::SectionHeader(ctx, { rect.X, rect.Y, rect.W, kSectionHeaderHeight }, header, theme.Accent, theme);

		std::string joined;
		for (std::size_t i = 0; i < lines.size(); ++i)
		{
			if (i)
				joined += " | ";
			joined += lines[i];
		}
		if (lines.empty())
			joined = Wui::Tr("panel.prefab.no_components", "No components");
		RegisterNode(Wui::HashId("prefab.details"), "text",
			{ rect.X, rect.Y + kSectionHeaderHeight, rect.W,
				std::max(0.0f, rect.H - kSectionHeaderHeight) },
			Wui::Tr("panel.prefab.details", "Component summary"), joined, true, joined, false);

		float y = rect.Y + kSectionHeaderHeight + 2.0f;
		if (lines.empty())
		{
			Wui::Label(ctx, { rect.X + 4.0f, y }, Wui::Tr("panel.prefab.no_components", "No components"),
				theme.TextMuted, 12.0f);
			return;
		}
		const std::size_t visibleLines = static_cast<std::size_t>(
			std::max(0.0f, (rect.H - kSectionHeaderHeight - 4.0f) / 16.0f));
		for (std::size_t i = 0; i < lines.size() && i < visibleLines; ++i)
		{
			Wui::Label(ctx, { rect.X + 4.0f, y }, TruncateUtf8(lines[i], 120), theme.Text, 12.0f);
			y += 16.0f;
		}
	}

	void PrefabPanel::DrawAssets(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		const std::string header = std::string(Wui::Tr("panel.prefab.assets", "Referenced assets")) + " ("
			+ std::to_string(m_Assets.size()) + ")";
		Wui::SectionHeader(ctx, { rect.X, rect.Y, rect.W, kSectionHeaderHeight }, header, theme.Accent, theme);
		RegisterNode(Wui::HashId("prefab.assets"), "list", { rect.X, rect.Y, rect.W, kSectionHeaderHeight },
			Wui::Tr("panel.prefab.assets", "Referenced assets"), std::to_string(m_Assets.size()), true,
			Wui::Tr("panel.prefab.assets.tooltip",
				"Material/mesh logical paths referenced by this prefab (deduplicated, read-only)."), false);
		// 选中一行 = 选中这段逻辑路径文本:单独一个节点,脚本/读屏读得到选中内容。
		RegisterNode(Wui::HashId("prefab.assets.selected"), "text",
			{ rect.X + 4.0f, rect.Y + kSectionHeaderHeight, std::max(0.0f, rect.W - 4.0f), 16.0f },
			Wui::Tr("panel.prefab.assets.selected", "Selected asset path"),
			m_SelectedAsset >= 0 && m_SelectedAsset < static_cast<int>(m_Assets.size())
				? m_Assets[static_cast<std::size_t>(m_SelectedAsset)] : std::string(),
			true, std::string(), false);

		const Wui::WuiRect viewport { rect.X, rect.Y + kSectionHeaderHeight + 18.0f, rect.W,
			std::max(0.0f, rect.H - kSectionHeaderHeight - 18.0f) };
		std::vector<RowSpec> rows;
		rows.reserve(m_Assets.size());
		for (std::size_t i = 0; i < m_Assets.size(); ++i)
		{
			RowSpec row;
			row.Id = "prefab.assets." + std::to_string(i);
			row.Label = m_Assets[i];
			row.Value = "logical asset path";
			row.Tooltip = Wui::Tr("panel.prefab.asset_row.tooltip",
				"Click to select this logical path text (read-only list).");
			row.Interactive = true;
			rows.push_back(std::move(row));
		}
		const int clicked = DrawRows(ctx, viewport, m_AssetsScroll, rows, m_SelectedAsset, theme,
			Wui::Tr("panel.prefab.assets.none", "No material/mesh references"));
		if (clicked >= 0)
			m_SelectedAsset = clicked;
	}

	void PrefabPanel::DrawInstances(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
		Scene* scene, const Wui::WuiTheme& theme)
	{
		const std::string header = std::string(Wui::Tr("panel.prefab.instances", "Scene instances")) + " ("
			+ std::to_string(m_Instances.size()) + ")";
		Wui::SectionHeader(ctx, { rect.X, rect.Y, rect.W, kSectionHeaderHeight }, header, theme.Accent, theme);
		RegisterNode(Wui::HashId("prefab.instances"), "list", { rect.X, rect.Y, rect.W, kSectionHeaderHeight },
			Wui::Tr("panel.prefab.instances", "Scene instances"), std::to_string(m_Instances.size()), true,
			Wui::Tr("panel.prefab.instances.tooltip",
				"Instances of this prefab in the current scene; click a row to select its root."), false);

		const Wui::WuiRect viewport { rect.X, rect.Y + kSectionHeaderHeight + 1.0f, rect.W,
			std::max(0.0f, rect.H - kSectionHeaderHeight - 1.0f) };
		std::vector<RowSpec> rows;
		int selected = -1;
		rows.reserve(m_Instances.size());
		for (std::size_t i = 0; i < m_Instances.size(); ++i)
		{
			RowSpec row;
			row.Id = "prefab.instances.row." + std::to_string(i);
			row.Label = "#" + std::to_string(static_cast<uint32_t>(m_Instances[i].Handle)) + " "
				+ m_Instances[i].Name;
			row.Value = "instance root";
			row.Tooltip = Wui::Tr("panel.prefab.instance_row.tooltip",
				"Click to select this instance root in the Hierarchy panel.");
			row.Interactive = true;
			if (m_Instances[i].Handle == m_SelectedInstance)
				selected = static_cast<int>(i);
			rows.push_back(std::move(row));
		}
		const int clicked = DrawRows(ctx, viewport, m_InstancesScroll, rows, selected, theme,
			Wui::Tr("panel.prefab.instances.none", "No instance of this prefab in the current scene"));
		if (clicked >= 0)
			SelectInstance(host, scene, m_Instances[static_cast<std::size_t>(clicked)].Handle);
	}
}
