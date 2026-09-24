#include "wldpch.h"
#include "PrefabPanel.h"
#include "ViewportPanel.h"

#include "EditorAssetCatalog.h"

#include "World/Core/Application.h"
#include "World/Core/KeyCodes.h"
#include "World/Gameplay/Prefab.h"
#include "World/Gameplay/PrefabTypes.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/Renderer.h"
#include "World/Scene/Components.h"
#include "World/Scene/SceneSerializer.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>
#include <sstream>
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
		// 保存/回滚后的静默期:状态行写的是 "Saved …"/"Reverted …",不要被 TTL 重扫刷掉。
		constexpr double kStatusHoldInterval = 30.0;
		// 就地编辑的行距/控件尺寸(与属性面板同一密度)。
		constexpr float kFieldRowHeight = 22.0f;
		constexpr float kReadOnlyLineHeight = 16.0f;
		// P4-U13f:预览离屏目标的长边范围(等比缩放;4K 窗口不把预览拖成大目标)。
		constexpr float kPreviewTargetMaxSide = 2048.0f;
		constexpr float kPreviewTargetMinSide = 128.0f;
		// 轨道旋转的俯仰限位:±89°(用户口径;留 1° 余量避免视线与上方向共线时 lookAt 退化)。
		constexpr float kPreviewPitchLimit = 1.55334f;

		// P4-U13f:SceneRenderer 的离屏目标尺寸 = OnResize 请求尺寸 × rendering.render_scale,
		// 所以"要 target 像素"必须反算请求值。lround 取整会让个别 (target, scale) 组合差 1 像素,
		// 在 ±3 里挑误差最小的请求值 —— 预览不跟着场景分辨率倍率降采样。
		uint32_t RequestedExtentForTarget(uint32_t target, float renderScale)
		{
			const float scale = renderScale > 0.0f ? renderScale : 1.0f;
			const long base = std::max(1L, std::lround(static_cast<double>(target) / scale));
			long best = base;
			long bestError = std::numeric_limits<long>::max();
			for (long candidate = std::max(1L, base - 3); candidate <= base + 3; ++candidate)
			{
				const long error = std::labs(std::lround(static_cast<double>(candidate) * scale)
					- static_cast<long>(target));
				if (error < bestError || (error == bestError && candidate < best))
				{
					bestError = error;
					best = candidate;
				}
			}
			return static_cast<uint32_t>(best);
		}

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

		// 组件短名(小节标题 / 字段 id / AI 通道的 component 参数共用一份拼写)。
		constexpr const char* kTransformComponent = "Transform";
		constexpr const char* kMeshRendererComponent = "MeshRenderer";
		constexpr const char* kCameraComponent = "Camera";
		constexpr const char* kDirectionalLightComponent = "DirectionalLight";
		constexpr const char* kPointLightComponent = "PointLight";
		constexpr const char* kAmbientLightComponent = "AmbientLight";

		std::string LocalTimeStamp()
		{
			const std::time_t now = std::time(nullptr);
			std::tm local {};
			localtime_s(&local, &now);
			char buffer[32] = {};
			std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
			return buffer;
		}

		// 逻辑路径 → 内容根下的绝对路径。
		std::filesystem::path AbsoluteAssetPath(const std::string& logical)
		{
			return std::filesystem::path(std::string(WLD_ASSETPATH)) / logical;
		}

		// 内容根里这个逻辑路径是不是真的存在(编辑时的只读校验,不阻断保存)。
		bool AssetPathExists(const std::string& logical)
		{
			if (logical.empty())
				return true;
			std::error_code ec;
			return std::filesystem::is_regular_file(AbsoluteAssetPath(logical), ec);
		}

		bool ParseFloat3Text(const std::string& text, glm::vec3* out)
		{
			std::stringstream stream(text);
			std::string part;
			float values[3] = { 0.0f, 0.0f, 0.0f };
			int index = 0;
			while (std::getline(stream, part, ',') && index < 3)
				values[index++] = std::strtof(part.c_str(), nullptr);
			if (index == 0)
				return false;
			for (; index < 3; ++index)
				values[index] = 0.0f;
			*out = { values[0], values[1], values[2] };
			return true;
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
			// WUI-P1c-W3.7:底色/描边/文字三条裸绘制改走库件(命令逐字段等价;禁用态配色、悬停口径、
			// 命中与 a11y 节点全部原样保留在面板侧)。
			Wui::PanelBackground(ctx, rect,
				enabled ? (hovered ? theme.ButtonHover : theme.ButtonBg) : theme.PanelBg, 3.0f);
			Wui::HighlightOutline(ctx, rect, hovered && enabled ? theme.Accent : theme.Border, 3.0f, 1.0f);
			Wui::Label(ctx, { rect.X + 9.0f, rect.Y + (rect.H - 13.0f) * 0.5f }, label,
				enabled ? theme.Text : theme.TextDisabled, 13.0f);
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
					Wui::PanelBackground(ctx, row, theme.ButtonHover, 2.0f);
				else if (isSelected)
					Wui::PanelBackground(ctx, row, theme.Selection, 2.0f);
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
		// 预览资源属于当前设备:设备释放(后端切换/关闭)前必须把句柄放掉,否则会在设备
		// 之后析构(与 ModelPreviewPanel 同因同修;实测漏掉会 0xC0000005)。
		Renderer::RegisterDeviceReleaseHook(this, [this] { ReleasePreviewResources(); });
		m_LogicalPath = NormalizePath(std::move(logicalPath));
		m_PanelId = "prefab:" + m_LogicalPath;
		const std::string name = std::filesystem::path(m_LogicalPath).filename().string();
		m_PanelTitle = "Prefab - " + (name.empty() ? m_LogicalPath : name);
		m_Status = Wui::Tr("panel.prefab.status.loading", "Loading prefab...");
	}

	PrefabPanel::~PrefabPanel()
	{
		Renderer::UnregisterDeviceReleaseHook(this);
		ReleasePreviewResources();
	}

	bool PrefabPanel::ReloadNow(Scene* activeScene, std::string* message)
	{
		// 有未保存改动时**不重读**:重复双击资产/`asset.open_prefab` 是"刷新"入口,
		// 但刷新不能把窗口里刚编辑的值静默盖掉(要丢改动得显式点 Revert)。
		if (m_Dirty)
		{
			if (message)
				*message = Wui::Tr("panel.prefab.reload.dirty",
					"Not reloaded: this window has unsaved changes (Save or Revert first).");
			return true;
		}
		RefreshFromDisk(activeScene, true);
		// 显式重开 = 重新取景(用户双击资产/`asset.open_prefab` 都走这条)。
		m_PreviewFramed = false;
		if (message)
			*message = m_Status;
		return m_DocumentValid;
	}

	void PrefabPanel::RefreshFromDisk(Scene* activeScene, bool force)
	{
		// 有未保存改动时不重扫:磁盘内容不能静默盖掉用户刚编辑的值(Revert/Save 才动它)。
		if (m_Dirty && !force)
			return;
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
		m_AssetWarning.clear();
		m_LastSavedText.clear();

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
		RefreshStatusText();
	}

	// ---- P4-U13e:状态行 / 脏标记 / 资产路径警告 ----

	void PrefabPanel::RefreshStatusText()
	{
		if (!m_DocumentValid)
			return;   // 失败原因由 LoadStaging 直接写(可读原因不能被覆盖)
		if (!m_AssetWarning.empty())
		{
			m_Status = m_AssetWarning;
			m_StatusIsError = true;
			return;
		}
		if (m_Dirty)
		{
			m_Status = Wui::Tr("panel.prefab.status.edited",
				"Unsaved changes — Save writes them back into this .wprefab.");
			m_StatusIsError = false;
			return;
		}
		if (!m_LastSavedText.empty())
		{
			m_Status = m_LastSavedText;
			m_StatusIsError = false;
			return;
		}
		m_Status = std::to_string(m_Rows.size()) + " entities · "
			+ std::to_string(m_Assets.size()) + " referenced assets";
		m_StatusIsError = false;
	}

	void PrefabPanel::MarkDirty()
	{
		m_Dirty = true;
		m_LastSavedText.clear();
		// 引用资产清单可能因路径编辑而变化(去重排序)。
		m_Assets = CollectReferencedAssets();
		RefreshStatusText();
	}

	void PrefabPanel::RefreshAssetWarning()
	{
		m_AssetWarning.clear();
		if (!m_Staging)
			return;
		const entt::registry& registry = m_Staging->GetRegistry();
		const auto* entities = registry.storage<entt::entity>();
		if (!entities)
			return;
		const auto check = [this](const std::string& path) -> bool
		{
			if (path.empty() || AssetPathExists(path))
				return false;
			m_AssetWarning = std::string(Wui::Tr("panel.prefab.status.asset_missing",
				"Warning: asset path not found in the content root: ")) + path;
			return true;
		};
		for (const entt::entity handle : *entities)
		{
			if (const auto* mesh = registry.try_get<MeshRendererComponent>(handle))
			{
				if (check(mesh->MeshPath) || check(mesh->MaterialPath))
					return;
			}
			if (const auto* skinned = registry.try_get<SkinnedMeshRendererComponent>(handle))
			{
				if (check(skinned->MeshPath) || check(skinned->MaterialPath))
					return;
			}
		}
	}

	entt::entity PrefabPanel::StagingRoot() const
	{
		if (!m_Staging)
			return entt::null;
		const entt::registry& registry = m_Staging->GetRegistry();
		// 与 Gameplay::InstantiateFromFile 同一口径:没有有效父节点的实体就是实例根
		// (prefab 文件按约定只有一个根)。
		for (const entt::entity handle : registry.view<UUIDComponent>())
		{
			const auto* hierarchy = registry.try_get<HierarchyComponent>(handle);
			if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
				return handle;
		}
		return entt::null;
	}

	void PrefabPanel::DiscardUnsavedChanges()
	{
		m_Dirty = false;
		m_AssetWarning.clear();
		m_LastSavedText.clear();
		// 下一帧强制重读:内存里的编辑丢弃,面板回到磁盘版本。
		m_NextDiskScan = 0.0;
	}

	bool PrefabPanel::SaveToDisk(std::string* message)
	{
		if (!m_Staging)
		{
			if (message) *message = Wui::Tr("panel.prefab.save.no_document", "Nothing to save: the prefab could not be read");
			return false;
		}
		const entt::entity root = StagingRoot();
		if (root == entt::null)
		{
			if (message) *message = Wui::Tr("panel.prefab.save.no_root",
				"Cannot save: this prefab has no root entity to export");
			m_Status = *message;
			m_StatusIsError = true;
			return false;
		}
		const std::filesystem::path absolute = AbsoluteAssetPath(m_LogicalPath);
		std::string error;
		if (!Gameplay::SaveFromScene(*m_Staging, Entity(m_Staging.get(), root), absolute, &error))
		{
			m_Status = error.empty()
				? (std::string(Wui::Tr("panel.prefab.save.failed", "Save failed: ")) + m_LogicalPath)
				: error;
			m_StatusIsError = true;
			if (message) *message = m_Status;
			return false;
		}
		m_Dirty = false;
		m_LastSavedText = std::string(Wui::Tr("panel.prefab.status.saved", "Saved ")) + LocalTimeStamp();
		RefreshStatusText();
		// 保存后的状态行(以及"Saved"语义)要留得住:TTL 重扫不能马上把它刷回实体计数。
		m_NextDiskScan = NowSeconds() + kStatusHoldInterval;
		WLD_CORE_INFO("[prefab] window saved '{0}' (entities={1})", m_LogicalPath, m_Rows.size());
		if (message) *message = m_Status;
		return true;
	}

	bool PrefabPanel::RevertFromDisk(Scene* activeScene, std::string* message)
	{
		m_Dirty = false;
		m_AssetWarning.clear();
		m_LastSavedText.clear();
		LoadStaging(activeScene);
		if (!m_DocumentValid)
		{
			if (message) *message = m_Status;
			return false;
		}
		m_LastSavedText = Wui::Tr("panel.prefab.status.reverted",
			"Reverted: reloaded this asset from disk (unsaved edits discarded).");
		RefreshStatusText();
		m_NextDiskScan = NowSeconds() + kStatusHoldInterval;
		WLD_CORE_INFO("[prefab] window reverted '{0}'", m_LogicalPath);
		if (message) *message = m_Status;
		return true;
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

	// P4-U13e:只读小节 —— 可编辑组件之外的部分仍要给摘要(用户 2026-09-21:
	// 「除上述字段外的组件仍显示只读摘要」)。短名走 schema 的 DisplayName(去命名空间)。
	std::vector<std::string> PrefabPanel::BuildReadOnlySummary(entt::entity handle) const
	{
		std::vector<std::string> lines;
		if (!m_Staging)
			return lines;
		const entt::registry& registry = m_Staging->GetRegistry();
		if (!registry.valid(handle))
			return lines;

		if (const auto* tag = registry.try_get<TagComponent>(handle))
			lines.push_back("Tag: " + tag->Tag);
		if (const auto* sprite = registry.try_get<SpriteComponent>(handle))
			lines.push_back("Sprite: TilingFactor=" + FormatFloat(sprite->TilingFactor, 2));
		if (const auto* skinned = registry.try_get<SkinnedMeshRendererComponent>(handle))
			lines.push_back("SkinnedMeshRenderer: MeshPath="
				+ (skinned->MeshPath.empty() ? std::string("(none)") : skinned->MeshPath)
				+ "  MaterialPath=" + (skinned->MaterialPath.empty() ? std::string("(none)") : skinned->MaterialPath));

		// 其余带 schema 的组件(物理/脚本/碰撞体…)按短名列一行,至少让用户知道它在这个实体上
		// (这一层不暴露可写控件,所以不做字段级摘要)。
		const Schema::SchemaRegistry& schemas = m_Staging->GetContext().Schemas();
		for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
		{
			if (!schema || !schema->Storage)
				continue;
			const std::string& display = schema->DisplayName;
			const std::string shortName = display.rfind("::", 0) == 0 ? display
				: (display.find("::") != std::string::npos ? display.substr(display.rfind("::") + 2) : display);
			if (shortName == "TagComponent" || shortName == "SpriteComponent"
				|| shortName == "SkinnedMeshRendererComponent")
				continue;   // 上面已经给过更具体的摘要
			if (shortName == "TransformComponent" || shortName == "MeshRendererComponent"
				|| shortName == "CameraComponent" || shortName == "DirectionalLightComponent"
				|| shortName == "PointLightComponent" || shortName == "AmbientLightComponent")
				continue;   // 可编辑小节已经覆盖
			const auto* storage = registry.storage(schema->Storage->ComponentId);
			if (storage && storage->contains(handle))
				lines.push_back(shortName + ": (read-only)");
		}
		return lines;
	}

	// P4-U13e:AI 通道的脚本化写字段 —— 与面板控件同一条写入口(同一个脏标记/状态行通道)。
	// 只接受 v1 的可编辑字段;不认识的 component/field 直接拒绝并给出可读原因。
	bool PrefabPanel::SetEditableField(const std::string& component, const std::string& field,
		const std::string& value, const std::string& axis, std::string* message)
	{
		if (!m_Staging || m_SelectedRow < 0 || m_SelectedRow >= static_cast<int>(m_Rows.size()))
		{
			if (message) *message = "no entity selected in the prefab window";
			return false;
		}
		const entt::entity handle = m_Rows[static_cast<std::size_t>(m_SelectedRow)].Handle;
		entt::registry& registry = m_Staging->GetRegistry();
		if (!registry.valid(handle))
		{
			if (message) *message = "selected entity is no longer valid";
			return false;
		}
		const auto reject = [&](const char* reason)
		{
			if (message)
				*message = std::string(reason) + ": " + component + "." + field;
			return false;
		};
		const auto parseScalar = [&](float* out)
		{
			char* end = nullptr;
			const float parsed = std::strtof(value.c_str(), &end);
			if (!end || end == value.c_str() || *end != 0)
				return false;
			*out = parsed;
			return true;
		};

		if (component == kTransformComponent)
		{
			auto* transform = registry.try_get<TransformComponent>(handle);
			if (!transform)
				return reject("entity has no such component");
			glm::vec3 target;
			if (field == "Location") target = transform->Location;
			else if (field == "Rotation") target = transform->Rotation;
			else if (field == "Scale") target = transform->Scale;
			else return reject("field is not editable");
			if (axis == "x" || axis == "y" || axis == "z")
			{
				float scalar = 0.0f;
				if (!parseScalar(&scalar))
					return reject("value is not a number");
				target[axis == "x" ? 0 : (axis == "y" ? 1 : 2)] = scalar;
			}
			else if (!ParseFloat3Text(value, &target))
			{
				return reject("value is not a number");
			}
			if (field == "Location") transform->SetLocation(target);
			else if (field == "Rotation") transform->SetRotation(target);
			else transform->SetScale(target);
		}
		else if (component == kMeshRendererComponent)
		{
			auto* mesh = registry.try_get<MeshRendererComponent>(handle);
			if (!mesh)
				return reject("entity has no such component");
			if (field == "Primitive") mesh->Primitive = value;
			else if (field == "MeshPath") mesh->MeshPath = value;
			else if (field == "MaterialPath") mesh->MaterialPath = value;
			else if (field == "MeshIndex")
			{
				float scalar = 0.0f;
				if (!parseScalar(&scalar))
					return reject("value is not a number");
				mesh->MeshIndex = static_cast<int32_t>(scalar);
			}
			else return reject("field is not editable");
			RefreshAssetWarning();
		}
		else if (component == kCameraComponent)
		{
			auto* camera = registry.try_get<CameraComponent>(handle);
			if (!camera)
				return reject("entity has no such component");
			float scalar = 0.0f;
			if (!parseScalar(&scalar))
				return reject("value is not a number");
			if (field == "Fov") camera->Camera.SetPerspectiveFOV(scalar);
			else if (field == "NearClip") camera->Camera.SetPerspectiveNearClip(scalar);
			else if (field == "FarClip") camera->Camera.SetPerspectiveFarClip(scalar);
			else return reject("field is not editable");
		}
		else if (component == kDirectionalLightComponent || component == kPointLightComponent
			|| component == kAmbientLightComponent)
		{
			glm::vec3* color = nullptr;
			float* intensity = nullptr;
			float* range = nullptr;
			if (auto* light = registry.try_get<DirectionalLightComponent>(handle);
				component == kDirectionalLightComponent && light)
			{
				color = &light->Color;
				intensity = &light->Intensity;
			}
			else if (auto* light = registry.try_get<PointLightComponent>(handle);
				component == kPointLightComponent && light)
			{
				color = &light->Color;
				intensity = &light->Intensity;
				range = &light->Range;
			}
			else if (auto* light = registry.try_get<AmbientLightComponent>(handle);
				component == kAmbientLightComponent && light)
			{
				color = &light->Color;
				intensity = &light->Intensity;
			}
			if (!color)
				return reject("entity has no such component");
			if (field == "Color")
			{
				glm::vec3 rgb;
				if (!ParseFloat3Text(value, &rgb))
					return reject("value is not a number");
				*color = rgb;
			}
			else if (field == "Intensity")
			{
				float scalar = 0.0f;
				if (!parseScalar(&scalar))
					return reject("value is not a number");
				*intensity = scalar;
			}
			else if (field == "Range" && range)
			{
				float scalar = 0.0f;
				if (!parseScalar(&scalar))
					return reject("value is not a number");
				*range = scalar;
			}
			else return reject("field is not editable");
		}
		else
		{
			return reject("component is not editable in the prefab window");
		}

		MarkDirty();
		RefreshStatusText();
		if (message)
			*message = "set " + component + "." + field + (axis.empty() ? "" : ("." + axis))
				+ " = " + value;
		return true;
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

		// ---- 头部:标题(脏时带 `*`)+ 动作(全部带悬停说明 + 无障碍 id)----
		const std::string fileName = std::filesystem::path(m_LogicalPath).filename().string();
		const std::string title = std::string(Wui::Tr("panel.prefab.title", "Prefab")) + ": "
			+ (fileName.empty() ? m_LogicalPath : fileName) + (m_Dirty ? " *" : "");
		if (rect.W >= 380.0f)
			Wui::Label(ctx, { rect.X + pad, y + 4.0f }, TruncateUtf8(title, 72), theme.Text, 14.0f);
		// 标题与脏标记都登记成节点:标题是判定"未保存"的稳定来源(脚本/读屏都能读)。
		RegisterNode(Wui::HashId("prefab.title"), "title",
			{ rect.X + pad, y, std::max(0.0f, rect.W * 0.5f), 22.0f },
			Wui::Tr("panel.prefab.title", "Prefab"), title, true,
			Wui::Tr("panel.prefab.title.tooltip",
				"Editing this window writes back into the asset file (Save). A trailing * marks unsaved edits."),
			false);
		if (m_Dirty)
			RegisterNode(Wui::HashId("prefab.dirty"), "status",
				{ rect.X + pad, y + 22.0f, 120.0f, 14.0f },
				Wui::Tr("panel.prefab.dirty", "Unsaved changes"), "true", true,
				Wui::Tr("panel.prefab.dirty.tooltip",
					"This window has edits that are not written into the .wprefab yet; Save writes them, Revert reloads from disk."),
				false);

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
		const std::string saveText = Wui::Tr("panel.prefab.save", "Save");
		const std::string revertText = Wui::Tr("panel.prefab.revert", "Revert");
		const std::string placeHint = !hasDocument ? unreadableHint
			: (readOnly
				? Wui::Tr("panel.prefab.place.readonly",
					"Read-only while Play/Simulate is running; exit Play to place the prefab.")
				: Wui::Tr("panel.prefab.place.tooltip",
					"Instantiate this prefab into the current scene (world origin)."));
		const std::string editHint = hasDocument
			? Wui::Tr("panel.prefab.edit.tooltip",
				"Open this prefab in the full editor (better for large hierarchy reworks); saving there writes the asset back.")
			: unreadableHint;
		const std::string locateHint = hasInstance
			? Wui::Tr("panel.prefab.locate.tooltip",
				"Select this prefab's first instance root in the Hierarchy panel.")
			: Wui::Tr("panel.prefab.locate.disabled",
				"Disabled: the current scene has no instance of this prefab (use Place in Scene first).");
		const std::string saveHint = Wui::Tr("panel.prefab.save.tooltip",
			"Write the current values back into this .wprefab; the window goes clean afterwards.");
		const std::string revertHint = Wui::Tr("panel.prefab.revert.tooltip",
			"Discard the edits in this window and reload the asset from disk.");

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
		// 未保存改动时头部才出现 Save / Revert(脏标记 + 这两个按钮一起出现,语义成对)。
		const bool saveClicked = m_Dirty
			? placeButton("prefab.save", saveText, buttonRight, hasDocument, saveHint) : false;
		const bool revertClicked = m_Dirty
			? placeButton("prefab.revert", revertText, buttonRight, true, revertHint) : false;
		const bool locateClicked = placeButton("prefab.locate", locateText, buttonRight, hasInstance, locateHint);
		const bool editClicked = placeButton("prefab.edit", editText, buttonRight, hasDocument, editHint);
		const bool placeClicked = placeButton("prefab.place", placeText, buttonRight, canPlace, placeHint);
		y += buttonH + 6.0f;

		if (saveClicked)
		{
			std::string message;
			if (!SaveToDisk(&message))
				host.Notify(message);
		}
		if (revertClicked)
		{
			std::string message;
			RevertFromDisk(scene, &message);
		}
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
			// 完整编辑器仍然走文档会话;有未保存改动时由宿主先问一次(丢弃 / 取消)。
			host.OpenPrefabEditor(m_LogicalPath);
			// 宿主拦下(弹确认)时这次并没有真的切过去 —— 状态行不能先写"已进入会话"。
			if (!m_Dirty)
			{
				m_Status = std::string(Wui::Tr("panel.prefab.status.editing", "Opened the edit session: "))
					+ m_LogicalPath;
				m_StatusIsError = false;
			}
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

		// ---- 内容:左列 = 3D 预览 + 实体树;右列 = 就地编辑 + 引用资产 + 场景实例 ----
		const Wui::WuiRect content { rect.X + pad, y, std::max(0.0f, rect.W - pad * 2.0f),
			std::max(0.0f, rect.Y + rect.H - pad - y) };
		Wui::WuiRect previewRect = content;
		Wui::WuiRect treeRect = content;
		Wui::WuiRect editorRect = content;
		Wui::WuiRect assetsRect = content;
		Wui::WuiRect instancesRect = content;
		if (content.W >= kTwoColumnMinWidth)
		{
			const float leftWidth = std::clamp(content.W * 0.44f, 200.0f,
				std::max(200.0f, content.W - 240.0f));
			const float previewSide = std::clamp(leftWidth * 0.86f, 120.0f,
				std::max(120.0f, content.H * 0.58f));
			previewRect = { content.X, content.Y, leftWidth, previewSide };
			treeRect = { content.X, content.Y + previewSide + 6.0f, leftWidth,
				std::max(0.0f, content.H - previewSide - 6.0f) };
			const Wui::WuiRect right { content.X + leftWidth + 10.0f, content.Y,
				std::max(0.0f, content.W - leftWidth - 10.0f), content.H };
			const float editorHeight = std::floor(right.H * 0.56f);
			const float assetsHeight = std::floor(right.H * 0.18f);
			editorRect = { right.X, right.Y, right.W, editorHeight };
			assetsRect = { right.X, right.Y + editorHeight, right.W, assetsHeight };
			instancesRect = { right.X, right.Y + editorHeight + assetsHeight, right.W,
				std::max(0.0f, right.H - editorHeight - assetsHeight) };
		}
		else
		{
			const float previewSide = std::min(content.W, std::max(120.0f, content.H * 0.34f));
			previewRect = { content.X, content.Y, content.W, previewSide };
			const float restY = content.Y + previewSide + 6.0f;
			const float restH = std::max(0.0f, content.Y + content.H - restY);
			const float treeHeight = std::floor(restH * 0.22f);
			const float editorHeight = std::floor(restH * 0.44f);
			const float assetsHeight = std::floor(restH * 0.14f);
			treeRect = { content.X, restY, content.W, treeHeight };
			editorRect = { content.X, restY + treeHeight, content.W, editorHeight };
			assetsRect = { content.X, restY + treeHeight + editorHeight, content.W, assetsHeight };
			instancesRect = { content.X, restY + treeHeight + editorHeight + assetsHeight, content.W,
				std::max(0.0f, restH - treeHeight - editorHeight - assetsHeight) };
		}

		DrawPreview(ctx, previewRect, theme);
		DrawEntityTree(ctx, treeRect, theme);
		DrawEditableComponents(ctx, editorRect, theme);
		DrawAssets(ctx, assetsRect, theme);
		DrawInstances(ctx, instancesRect, host, scene, theme);
	}

	// ---- P4-U13e:3D 预览(与 ModelPreviewPanel 同一套离屏写法) ----

	void PrefabPanel::EnsurePreviewResources()
	{
		const Rhi::Handle<Rhi::Device>& device = Renderer::GetDevice();
		if (!device)
			return;
		if (m_PreviewGpuDevice == device.get() && m_PreviewRenderer)
			return;
		// 设备变了(后端切换/重建):旧句柄属于旧设备,必须先放掉再重建。
		ReleasePreviewResources();
		m_PreviewGpuDevice = device.get();
		m_PreviewRenderer = CreateRef<SceneRenderer>();
		m_PreviewRenderer->Init();
		// 新设备的目标尺寸由 UpdatePreviewTargetSize 按当前预览区重算(Init 的默认
		// 1280×720 只是占位;这一帧就会换成预览区的物理像素尺寸)。
		m_PreviewSizeDirty = true;
	}

	void PrefabPanel::ReleasePreviewResources()
	{
		if (m_PreviewRenderer)
		{
			m_PreviewRenderer->Shutdown();
			m_PreviewRenderer.reset();
		}
		m_PreviewGpuDevice = nullptr;
		m_PreviewSizeDirty = true;
		// 注册表里那条纹理还指向已释放的设备资源:下一帧按新句柄 Update(或重登记)。
		m_PreviewTextureHandle = nullptr;
		m_UiTextureGeneration = 0;
	}

	// P4-U13f:目标尺寸 = 预览区物理像素(设计单位 × UiScale),长边 [128, 2048] 等比 clamp;
	// 只在矩形/倍率真的换了之后才 OnResize(逐帧路径不产生任何资源操作)。
	void PrefabPanel::UpdatePreviewTargetSize(const Wui::WuiRect& view)
	{
		if (!m_PreviewRenderer)
			return;
		const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		const float pixelW = std::max(1.0f, view.W * uiScale);
		const float pixelH = std::max(1.0f, view.H * uiScale);
		const float longSide = std::max(pixelW, pixelH);
		float clampScale = 1.0f;
		if (longSide > kPreviewTargetMaxSide)
			clampScale = kPreviewTargetMaxSide / longSide;
		else if (longSide < kPreviewTargetMinSide)
			clampScale = kPreviewTargetMinSide / longSide;
		const uint32_t desiredW = static_cast<uint32_t>(
			std::max(1.0f, std::round(pixelW * clampScale)));
		const uint32_t desiredH = static_cast<uint32_t>(
			std::max(1.0f, std::round(pixelH * clampScale)));

		// rendering.render_scale 只该缩放场景目标(视口/运行时窗口),预览按物理像素 1:1;
		// 这里把倍率从请求尺寸里除回去 —— 设置里改倍率不会把预览也降分辨率。
		const float renderScale = std::clamp(RenderSettings::RenderScale(),
			Asset::RenderingSettings::MinRenderScale, Asset::RenderingSettings::MaxRenderScale);
		const uint32_t requestedW = RequestedExtentForTarget(desiredW, renderScale);
		const uint32_t requestedH = RequestedExtentForTarget(desiredH, renderScale);
		m_PreviewUiScale = uiScale;
		m_PreviewRenderScale = renderScale;
		if (!m_PreviewSizeDirty && requestedW == m_PreviewRequestedW
			&& requestedH == m_PreviewRequestedH)
			return;

		m_PreviewSizeDirty = false;
		m_PreviewRequestedW = requestedW;
		m_PreviewRequestedH = requestedH;
		m_PreviewRenderer->OnResize(requestedW, requestedH);
		m_PreviewTargetW = std::max(1u, m_PreviewRenderer->GetWidth());
		m_PreviewTargetH = std::max(1u, m_PreviewRenderer->GetHeight());
		WLD_CORE_INFO("[prefab] preview target {0}x{1} (view {2:.0f}x{3:.0f} design, uiScale={4:.2f}, "
			"renderScale={5:.2f}, requested={6}x{7})",
			m_PreviewTargetW, m_PreviewTargetH, view.W, view.H, uiScale, renderScale,
			requestedW, requestedH);
	}

	void PrefabPanel::PreviewFocusBounds(glm::vec3* center, float* radius) const
	{
		glm::vec3 minimum { 0.0f };
		glm::vec3 maximum { 0.0f };
		bool any = false;
		if (m_Staging)
		{
			const entt::registry& registry = m_Staging->GetRegistry();
			const auto* entities = registry.storage<entt::entity>();
			if (entities)
			{
				for (const entt::entity handle : *entities)
				{
					const auto* transform = registry.try_get<TransformComponent>(handle);
					if (!transform)
						continue;
					// 世界矩阵优先(层级下的子物体);没有缓存时退回局部矩阵(近似取景足够)。
					glm::mat4 matrix = transform->Transform;
					if (const auto* world = registry.try_get<WorldTransformComponent>(handle))
						matrix = world->Matrix;
					const glm::vec3 position { matrix[3] };
					// 单位网格近似半尺寸(不加载 .wmodel 资源,取景只要求"框得住")。
					const glm::vec3 half {
						std::max(0.5f, 0.5f * glm::length(glm::vec3(matrix[0]))),
						std::max(0.5f, 0.5f * glm::length(glm::vec3(matrix[1]))),
						std::max(0.5f, 0.5f * glm::length(glm::vec3(matrix[2]))) };
					if (!any)
					{
						minimum = position - half;
						maximum = position + half;
						any = true;
					}
					else
					{
						minimum = glm::min(minimum, position - half);
						maximum = glm::max(maximum, position + half);
					}
				}
			}
		}
		const glm::vec3 extent = any ? (maximum - minimum) * 0.5f : glm::vec3 { 1.0f };
		if (center)
			*center = any ? (minimum + maximum) * 0.5f : glm::vec3 { 0.0f };
		if (radius)
			*radius = std::max(0.5f, glm::length(extent));
	}

	void PrefabPanel::FramePreview()
	{
		glm::vec3 center { 0.0f };
		float radius = 2.0f;
		PreviewFocusBounds(&center, &radius);
		m_Focus = center;
		m_MinDistance = std::max(0.1f, radius * 0.5f);
		m_MaxDistance = std::max(4.0f, radius * 40.0f);
		m_CameraDistance = std::clamp(radius * 3.0f, m_MinDistance, m_MaxDistance);
		m_OrbitYaw = 0.6f;
		m_OrbitPitch = 0.25f;
		m_PreviewFramed = true;
	}

	std::string PrefabPanel::PreviewUnavailableReason() const
	{
		if (!m_DocumentValid || !m_Staging)
			return Wui::Tr("panel.prefab.preview.no_document",
				"Preview unavailable: this prefab could not be read (see the status line).");
		if (m_Rows.empty())
			return Wui::Tr("panel.prefab.preview.empty", "Preview unavailable: this prefab has no entities.");
		if (!Renderer::GetDevice())
			return Wui::Tr("panel.prefab.preview.no_device",
				"Preview unavailable: the graphics device is not ready yet.");
		if (!m_PreviewRenderer)
			return Wui::Tr("panel.prefab.preview.no_target",
				"Preview unavailable: the offscreen render target could not be created.");
		return {};
	}

	uint64_t PrefabPanel::RenderPreview()
	{
		if (!m_Staging || m_Rows.empty())
			return 0;
		EnsurePreviewResources();
		if (!m_PreviewRenderer)
			return 0;
		if (!m_PreviewFramed)
			FramePreview();
		// 轨道相机:拖拽(yaw/pitch)+ 滚轮(distance)都只改这里的三个量,不动 staging 数据。
		const float distance = m_CameraDistance;
		const glm::vec3 eye {
			m_Focus.x + distance * std::cos(m_OrbitPitch) * std::sin(m_OrbitYaw),
			m_Focus.y + distance * std::sin(m_OrbitPitch),
			m_Focus.z + distance * std::cos(m_OrbitPitch) * std::cos(m_OrbitYaw) };
		const glm::mat4 view = glm::lookAt(eye, m_Focus, glm::vec3(0.0f, 1.0f, 0.0f));
		SceneCamera camera;
		camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
		// 相机宽高比 = 离屏目标宽高比 = 预览区宽高比 → 物体在预览区里不会被拉扁。
		camera.SetViewportSize(m_PreviewTargetW, m_PreviewTargetH);
		camera.SetPerspectiveFOV(40.0f);
		camera.SetPerspectiveNearClip(std::max(0.01f, distance * 0.01f));
		camera.SetPerspectiveFarClip(distance * 4.0f + 100.0f);
		// 与视口/相机预览同一条提交路径(SceneRenderer):prefab 里的网格/材质走引擎自己的加载。
		SceneRendererOptions options;
		options.ShowGrid = false;
		m_PreviewRenderer->BeginScene(m_Staging.get(), options);
		m_PreviewRenderer->SubmitScene(camera, glm::inverse(view));
		m_PreviewRenderer->EndScene();

		const Rhi::Handle<Rhi::Texture>& color = m_PreviewRenderer->GetColorTexture();
		if (!color)
			return 0;
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		const bool generationChanged = registry.Generation() != m_UiTextureGeneration;
		if (m_PreviewTextureId == 0)
		{
			m_PreviewTextureId = registry.Register(color);
			m_UiTextureGeneration = registry.Generation();
			m_PreviewTextureHandle = color.get();
		}
		else if (generationChanged || m_PreviewTextureHandle != color.get())
		{
			// 目标被 resize/重建后句柄会变(见"按对象地址做键的缓存必须让内容代参与"那条教训)。
			registry.Update(m_PreviewTextureId, color);
			m_UiTextureGeneration = registry.Generation();
			m_PreviewTextureHandle = color.get();
		}
		return m_PreviewTextureId;
	}

	void PrefabPanel::DrawPreview(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		const std::string header = Wui::Tr("panel.prefab.preview", "Preview");
		Wui::SectionHeader(ctx, { rect.X, rect.Y, rect.W, kSectionHeaderHeight }, header, theme.Accent, theme);
		const Wui::WuiRect view { rect.X, rect.Y + kSectionHeaderHeight + 1.0f, rect.W,
			std::max(0.0f, rect.H - kSectionHeaderHeight - 1.0f) };
		if (view.W <= 8.0f || view.H <= 8.0f)
			return;

		// P4-U13f:目标尺寸跟着矩形走(窗口缩放/分离/挂靠/分栏都换矩形,这里每帧核对重建)。
		EnsurePreviewResources();
		UpdatePreviewTargetSize(view);
		const uint64_t textureId = RenderPreview();
		const std::string reason = textureId != 0 ? std::string() : PreviewUnavailableReason();
		if (textureId != 0)
		{
			// 贴图按**物理像素网格**对齐:目标尺寸 = 这段物理尺寸时,每个屏幕像素正好采样
			// 一个纹素(1:1),线性过滤也不会糊(旧实现是 320×320 放大到整块预览区)。
			const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
			const Wui::WuiRect pixelView {
				std::round(view.X * uiScale) / uiScale,
				std::round(view.Y * uiScale) / uiScale,
				static_cast<float>(std::lround(view.W * uiScale)) / uiScale,
				static_cast<float>(std::lround(view.H * uiScale)) / uiScale };
			// U22:离屏预览按引擎统一口径贴({0,1,1,-1});三个预览面板共用同一份常量。
			Wui::Image(ctx, pixelView, textureId, ViewChrome::kPreviewImageUv, theme);
			// 轨道旋转(拖拽)/ 滚轮缩放 / 双击或 F 取景(与模型预览同一套手感)。
			if (ctx.Input().Wheel != 0.0f && ctx.IsHovered(view))
			{
				const float step = std::max(0.05f, m_CameraDistance * 0.1f);
				m_CameraDistance = std::clamp(m_CameraDistance - ctx.Input().Wheel * step,
					m_MinDistance, m_MaxDistance);
			}
			if (ctx.IsHovered(view) && ctx.Input().MouseClicked[0])
			{
				m_Orbiting = true;
				m_LastMouse = ctx.Input().MousePos;
			}
			if (m_Orbiting && ctx.Input().MouseDown[0] && !ctx.IsDoubleClicked(view))
			{
				const glm::vec2 delta = ctx.Input().MousePos - m_LastMouse;
				m_LastMouse = ctx.Input().MousePos;
				// U22:符号走 ViewChrome::ApplyOrbitDrag(与材质/模型预览 + 主视口唯一事实源)。
				ViewChrome::ApplyOrbitDrag(m_OrbitYaw, m_OrbitPitch, delta, kPreviewPitchLimit);
			}
			if (m_Orbiting && !ctx.Input().MouseDown[0])
				m_Orbiting = false;
			if (ctx.IsDoubleClicked(view) || (ctx.IsHovered(view) && ctx.WasKeyPressed(KeyCodes::F)))
				FramePreview();
			if (ctx.IsHovered(view))
				ctx.SetCursor(m_Orbiting ? Wui::WuiCursor::Hand : Wui::WuiCursor::Arrow);
			// U22:右下角坐标系指示器(方案 §5.5),朝向与上面 SceneRenderer 用的相机基一致。
			glm::vec3 axisRight { 1.0f, 0.0f, 0.0f };
			glm::vec3 axisUp { 0.0f, 1.0f, 0.0f };
			ViewChrome::OrbitBasis(m_OrbitYaw, m_OrbitPitch, &axisRight, &axisUp, nullptr);
			const Wui::WuiRect axisRect = ViewChrome::DrawAxisIndicator(ctx, view, axisRight, axisUp);
			ViewChrome::RegisterAxisNode(axisRect, "prefab.axis",
				Wui::Tr("panel.prefab.axis", "Preview axes"),
				ViewChrome::AxisReadout(glm::degrees(m_OrbitYaw), glm::degrees(m_OrbitPitch), false),
				Wui::Tr("panel.prefab.axis.tooltip",
					"World axes in the preview corner: X red, Y green, Z blue. They follow the "
					"preview camera; yaw/pitch of that camera are in the value."));
		}
		else
		{
			// 不留黑块:区里写可读原因(与模型预览同一条约定)。
			Wui::Label(ctx, { view.X + 8.0f, view.Y + 8.0f }, TruncateUtf8(reason, 160), theme.TextMuted, 12.0f);
		}
		// 角标:实际离屏目标分辨率(与模型预览面板的 "384px" 同一口径,便于核对清晰度)。
		// U22:移到右上角,给右下角的坐标系指示器让位。
		const std::string targetLabel = std::to_string(m_PreviewTargetW) + "×"
			+ std::to_string(m_PreviewTargetH) + "px";
		const float targetLabelW = ctx.MeasureTextWidth(targetLabel, 10.0f);
		Wui::Label(ctx, { view.X + std::max(4.0f, view.W - targetLabelW - 6.0f),
			view.Y + 4.0f }, targetLabel, theme.TextDisabled, 10.0f);
		const std::string hint = Wui::Tr("panel.prefab.preview.hint",
			"Drag = orbit, wheel = zoom, double-click or F = frame");
		Wui::Label(ctx, { view.X + 6.0f, view.Y + view.H - 15.0f }, TruncateUtf8(hint, 60),
			theme.TextDisabled, 11.0f);
		Wui::Tooltip(ctx, view, hint);

		// 无障碍:预览区(prefab.preview)+ 操作提示 + 相机状态 + 目标尺寸读数。
		RegisterNode(Wui::HashId("prefab.preview"), "image", view,
			Wui::Tr("panel.prefab.preview", "Preview"),
			textureId != 0 ? ("3D preview of " + m_LogicalPath) : reason, textureId != 0, hint, false);
		RegisterNode(Wui::HashId("prefab.preview.hint"), "text",
			{ view.X + 4.0f, view.Y + view.H - 16.0f, std::max(0.0f, view.W - 8.0f), 14.0f },
			Wui::Tr("panel.prefab.preview.hint", "Drag = orbit, wheel = zoom, double-click or F = frame"),
			hint, true, hint, false);
		char cameraText[192] = {};
		std::snprintf(cameraText, sizeof(cameraText),
			"yaw=%.2f pitch=%.2f pitchLimitDeg=89 dist=%.3f minDist=%.3f maxDist=%.3f focus=%.2f,%.2f,%.2f",
			m_OrbitYaw, m_OrbitPitch, m_CameraDistance, m_MinDistance, m_MaxDistance,
			m_Focus.x, m_Focus.y, m_Focus.z);
		RegisterNode(Wui::HashId("prefab.preview.camera"), "text",
			{ view.X + 4.0f, view.Y + 4.0f, std::max(0.0f, view.W - 8.0f), 14.0f },
			Wui::Tr("panel.prefab.preview.camera", "Preview camera"), cameraText, true, cameraText, false);
		// P4-U13f:目标尺寸读数(脚本断言 + 排查"预览糊不糊"的第一手数字)。
		char targetText[192] = {};
		std::snprintf(targetText, sizeof(targetText),
			"target=%ux%u view=%.0fx%.0f renderScale=%.2f uiScale=%.2f requested=%ux%u",
			m_PreviewTargetW, m_PreviewTargetH, view.W, view.H, m_PreviewRenderScale,
			m_PreviewUiScale, m_PreviewRequestedW, m_PreviewRequestedH);
		RegisterNode(Wui::HashId("prefab.preview.target"), "text",
			{ view.X + 4.0f, view.Y + 18.0f, std::max(0.0f, view.W - 8.0f), 14.0f },
			Wui::Tr("panel.prefab.preview.target", "Preview render target"), targetText, true,
			targetText, false);
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
				"Click to select this entity and edit its components on the right.");
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

	// ---- P4-U13e:就地编辑(右侧第一段) ----

	// 一行 vec3:标签 + X/Y/Z 三个 drag-float(与属性面板同一控件/同一"1,-1 = 无界"哨兵)。
	float PrefabPanel::DrawVec3Row(Wui::WuiContext& ctx, float x, float y, float width, const char* idPrefix,
		const std::string& label, const std::string& tooltip, glm::vec3& value, float speed,
		const Wui::WuiTheme& theme, bool& changed)
	{
		const float labelWidth = std::clamp(width * 0.32f, 56.0f, 120.0f);
		Wui::Label(ctx, { x, y + 3.0f }, TruncateUtf8(label, 48), theme.TextMuted, 12.0f);
		const float fieldsWidth = std::max(60.0f, width - labelWidth);
		const float slotWidth = fieldsWidth / 3.0f;
		static const char* const kAxisNames[3] = { "x", "y", "z" };
		for (int axis = 0; axis < 3; ++axis)
		{
			const Wui::WuiRect slot { x + labelWidth + slotWidth * static_cast<float>(axis), y,
				std::max(18.0f, slotWidth - 2.0f), 18.0f };
			const std::string idText = std::string(idPrefix) + "." + kAxisNames[axis];
			float component = value[axis];
			if (Wui::DragFloat(ctx, Wui::HashId(idText.c_str()), slot, component, speed, 1.0f, -1.0f, theme)
				&& component != value[axis])
			{
				value[axis] = component;
				changed = true;
			}
			// 覆盖控件自己的登记(空 label):脚本/读屏要读到"哪个字段的哪个轴"。
			RegisterNode(Wui::HashId(idText.c_str()), "drag-float", slot,
				label + " " + kAxisNames[axis], FormatFloat(value[axis]), true, tooltip, true);
			Wui::Tooltip(ctx, slot, tooltip);
		}
		return kFieldRowHeight;
	}

	float PrefabPanel::DrawScalarRow(Wui::WuiContext& ctx, float x, float y, float width, const char* idText,
		const std::string& label, const std::string& tooltip, float& value, float speed,
		float min, float max, const Wui::WuiTheme& theme, bool& changed)
	{
		const float labelWidth = std::clamp(width * 0.32f, 56.0f, 120.0f);
		Wui::Label(ctx, { x, y + 3.0f }, TruncateUtf8(label, 48), theme.TextMuted, 12.0f);
		const Wui::WuiRect ctrl { x + labelWidth, y, std::max(60.0f, width - labelWidth), 18.0f };
		if (Wui::DragFloat(ctx, Wui::HashId(idText), ctrl, value, speed, min, max, theme))
			changed = true;
		RegisterNode(Wui::HashId(idText), "drag-float", ctrl, label, FormatFloat(value), true, tooltip, true);
		Wui::Tooltip(ctx, ctrl, tooltip);
		return kFieldRowHeight;
	}

	float PrefabPanel::DrawColorRow(Wui::WuiContext& ctx, float x, float y, float width, const char* idText,
		const std::string& label, const std::string& tooltip, glm::vec4& value,
		const Wui::WuiTheme& theme, bool& changed)
	{
		const float labelWidth = std::clamp(width * 0.32f, 56.0f, 120.0f);
		Wui::Label(ctx, { x, y + 3.0f }, TruncateUtf8(label, 48), theme.TextMuted, 12.0f);
		const Wui::WuiRect ctrl { x + labelWidth, y, std::max(80.0f, width - labelWidth), 18.0f };
		if (Wui::ColorField(ctx, Wui::HashId(idText), ctrl, value, theme))
			changed = true;
		char buffer[16] = {};
		std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X",
			static_cast<int>(std::lround(std::clamp(value.x, 0.0f, 1.0f) * 255.0f)),
			static_cast<int>(std::lround(std::clamp(value.y, 0.0f, 1.0f) * 255.0f)),
			static_cast<int>(std::lround(std::clamp(value.z, 0.0f, 1.0f) * 255.0f)),
			static_cast<int>(std::lround(std::clamp(value.w, 0.0f, 1.0f) * 255.0f)));
		RegisterNode(Wui::HashId(idText), "color", ctrl, label, buffer, true, tooltip, true);
		Wui::Tooltip(ctx, ctrl, tooltip);
		return kFieldRowHeight;
	}

	float PrefabPanel::DrawAssetRow(Wui::WuiContext& ctx, float x, float y, float width, const char* idText,
		const std::string& label, const std::string& tooltip, const std::string& assetType,
		std::string& value, const Wui::WuiTheme& theme, bool& changed)
	{
		const float labelWidth = std::clamp(width * 0.32f, 56.0f, 120.0f);
		Wui::Label(ctx, { x, y + 3.0f }, TruncateUtf8(label, 48), theme.TextMuted, 12.0f);
		const Wui::WuiRect ctrl { x + labelWidth, y, std::max(80.0f, width - labelWidth), 18.0f };
		const std::vector<std::string>& paths = Editor::AssetCatalog::PathsForName(assetType);
		std::vector<std::string> options;
		options.reserve(paths.size() + 2);
		options.push_back(Wui::Tr("panel.prefab.asset_none", "(none)"));
		options.insert(options.end(), paths.begin(), paths.end());
		int selected = 0;
		const auto found = std::find(paths.begin(), paths.end(), value);
		if (found != paths.end())
			selected = static_cast<int>(found - paths.begin()) + 1;
		else if (!value.empty())
		{
			// 当前值不在扫描结果里(路径写错 / 资产还没建):照样显示,不假装它是"(none)"。
			options.push_back(value);
			selected = static_cast<int>(options.size()) - 1;
		}
		const int beforePick = selected;
		if (Wui::SearchableCombo(ctx, Wui::HashId(idText), ctrl, "", options, selected, theme)
			&& selected != beforePick)
		{
			value = selected <= 0 ? std::string() : options[static_cast<std::size_t>(selected)];
			changed = true;
		}
		RegisterNode(Wui::HashId(idText), "searchable-combo", ctrl, label, value, true, tooltip, true);
		Wui::Tooltip(ctx, ctrl, tooltip);
		return kFieldRowHeight;
	}

	float PrefabPanel::EstimateEditorHeight() const
	{
		if (!m_DocumentValid || !m_Staging || m_SelectedRow < 0
			|| m_SelectedRow >= static_cast<int>(m_Rows.size()))
			return 40.0f;
		const entt::registry& registry = m_Staging->GetRegistry();
		const entt::entity handle = m_Rows[static_cast<std::size_t>(m_SelectedRow)].Handle;
		if (!registry.valid(handle))
			return 40.0f;
		constexpr float kSection = 20.0f;
		float height = 8.0f;
		if (registry.try_get<TransformComponent>(handle))
			height += kSection + kFieldRowHeight * 3.0f + 6.0f;
		if (registry.try_get<MeshRendererComponent>(handle))
			height += kSection + kFieldRowHeight * 4.0f + 6.0f;
		if (registry.try_get<CameraComponent>(handle))
			height += kSection + kFieldRowHeight * 3.0f + 6.0f;
		if (registry.try_get<DirectionalLightComponent>(handle))
			height += kSection + kFieldRowHeight * 2.0f + 6.0f;
		if (registry.try_get<PointLightComponent>(handle))
			height += kSection + kFieldRowHeight * 3.0f + 6.0f;
		if (registry.try_get<AmbientLightComponent>(handle))
			height += kSection + kFieldRowHeight * 2.0f + 6.0f;
		const std::vector<std::string> readOnly = BuildReadOnlySummary(handle);
		if (!readOnly.empty())
			height += kSection + kReadOnlyLineHeight * static_cast<float>(readOnly.size()) + 8.0f;
		return std::max(60.0f, height);
	}

	void PrefabPanel::DrawEditableComponents(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
		const Wui::WuiTheme& theme)
	{
		const bool rowSelected = m_DocumentValid && m_SelectedRow >= 0
			&& m_SelectedRow < static_cast<int>(m_Rows.size());
		std::vector<std::string> summary;
		if (rowSelected)
			summary = BuildComponentSummary(static_cast<std::size_t>(m_SelectedRow));
		std::string joined;
		for (std::size_t i = 0; i < summary.size(); ++i)
		{
			if (i)
				joined += " | ";
			joined += summary[i];
		}
		if (joined.empty())
			joined = Wui::Tr("panel.prefab.no_components", "No components");
		RegisterNode(Wui::HashId("prefab.details"), "text",
			{ rect.X, rect.Y + kSectionHeaderHeight, rect.W,
				std::max(0.0f, rect.H - kSectionHeaderHeight) },
			Wui::Tr("panel.prefab.details", "Component summary"), joined, true, joined, false);

		const std::string header = std::string(Wui::Tr("panel.prefab.components", "Components")) + " ("
			+ std::to_string(summary.size()) + ")";
		Wui::SectionHeader(ctx, { rect.X, rect.Y, rect.W, kSectionHeaderHeight }, header, theme.Accent, theme);

		const Wui::WuiRect viewport { rect.X, rect.Y + kSectionHeaderHeight + 1.0f, rect.W,
			std::max(0.0f, rect.H - kSectionHeaderHeight - 1.0f) };
		if (viewport.H <= 8.0f || viewport.W <= 40.0f)
			return;
		Wui::BeginScrollArea(ctx, viewport, std::max(viewport.H, EstimateEditorHeight()), m_DetailsScroll,
			theme);
		const float x = viewport.X + 2.0f;
		const float width = std::max(40.0f, viewport.W - 4.0f);
		float y = viewport.Y + 2.0f;
		if (!rowSelected || !m_Staging)
		{
			Wui::Label(ctx, { x, y }, Wui::Tr("panel.prefab.select_entity",
				"Select an entity to view and edit its components."), theme.TextMuted, 12.0f);
			Wui::EndScrollArea(ctx);
			return;
		}

		entt::registry& registry = m_Staging->GetRegistry();
		const entt::entity handle = m_Rows[static_cast<std::size_t>(m_SelectedRow)].Handle;
		if (!registry.valid(handle))
		{
			Wui::Label(ctx, { x, y }, Wui::Tr("panel.prefab.select_entity",
				"Select an entity to view and edit its components."), theme.TextMuted, 12.0f);
			Wui::EndScrollArea(ctx);
			return;
		}

		// ---- Transform:T/R/S 三行 drag-float(与属性面板同控件)----
		if (auto* transform = registry.try_get<TransformComponent>(handle))
		{
			Wui::SectionHeader(ctx, { x, y, width, 18.0f },
				Wui::Tr("panel.prefab.section.transform", "Transform"), theme.Accent, theme, 13.0f);
			y += 20.0f;
			bool changed = false;
			glm::vec3 location = transform->Location;
			y += DrawVec3Row(ctx, x, y, width, "prefab.field.TransformComponent.Location",
				Wui::Tr("panel.prefab.field.location", "Location"),
				Wui::Tr("panel.prefab.field.location.tooltip",
					"Local position in the parent's space (world units); drag a number or click it to type."),
				location, 0.1f, theme, changed);
			if (changed)
			{
				transform->SetLocation(location);
				MarkDirty();
				changed = false;
			}
			glm::vec3 rotation = transform->Rotation;
			y += DrawVec3Row(ctx, x, y, width, "prefab.field.TransformComponent.Rotation",
				Wui::Tr("panel.prefab.field.rotation", "Rotation"),
				Wui::Tr("panel.prefab.field.rotation.tooltip",
					"Local Euler rotation in degrees (X/Y/Z); the engine stores the equivalent quaternion."),
				rotation, 0.5f, theme, changed);
			if (changed)
			{
				transform->SetRotation(rotation);
				MarkDirty();
				changed = false;
			}
			glm::vec3 scale = transform->Scale;
			y += DrawVec3Row(ctx, x, y, width, "prefab.field.TransformComponent.Scale",
				Wui::Tr("panel.prefab.field.scale", "Scale"),
				Wui::Tr("panel.prefab.field.scale.tooltip",
					"Local scale per axis; 1,1,1 = unscaled, negative values mirror."),
				scale, 0.1f, theme, changed);
			if (changed)
			{
				transform->SetScale(scale);
				MarkDirty();
				changed = false;
			}
			y += 6.0f;
		}

		// ---- MeshRenderer:Primitive / MeshPath / MaterialPath / MeshIndex ----
		if (auto* mesh = registry.try_get<MeshRendererComponent>(handle))
		{
			Wui::SectionHeader(ctx, { x, y, width, 18.0f },
				Wui::Tr("panel.prefab.section.mesh", "Mesh Renderer"), theme.Accent, theme, 13.0f);
			y += 20.0f;
			bool changed = false;
			// Primitive:固定集合用下拉(schema 的 Choices 同一份取值);当前值不在集合里也照样显示。
			{
				std::vector<std::string> options { "cube", "sphere", "plane" };
				if (std::find(options.begin(), options.end(), mesh->Primitive) == options.end())
					options.push_back(mesh->Primitive);
				int selected = static_cast<int>(std::find(options.begin(), options.end(), mesh->Primitive)
					- options.begin());
				const int beforePick = selected;
				const float labelWidth = std::clamp(width * 0.32f, 56.0f, 120.0f);
				Wui::Label(ctx, { x, y + 3.0f },
					Wui::Tr("panel.prefab.field.primitive", "Primitive"), theme.TextMuted, 12.0f);
				const Wui::WuiRect ctrl { x + labelWidth, y, std::max(80.0f, width - labelWidth), 18.0f };
				if (Wui::Combo(ctx, Wui::HashId("prefab.field.MeshRendererComponent.Primitive"), ctrl, "",
					options, selected, theme) && selected != beforePick)
				{
					mesh->Primitive = options[static_cast<std::size_t>(selected)];
					changed = true;
				}
				RegisterNode(Wui::HashId("prefab.field.MeshRendererComponent.Primitive"), "combo", ctrl,
					Wui::Tr("panel.prefab.field.primitive", "Primitive"), mesh->Primitive, true,
					Wui::Tr("panel.prefab.field.primitive.tooltip",
						"Built-in primitive used when Mesh Path is empty (cube / sphere / plane)."),
					true);
				Wui::Tooltip(ctx, ctrl, Wui::Tr("panel.prefab.field.primitive.tooltip",
					"Built-in primitive used when Mesh Path is empty (cube / sphere / plane)."));
			}
			y += kFieldRowHeight;
			y += DrawAssetRow(ctx, x, y, width, "prefab.field.MeshRendererComponent.MeshPath",
				Wui::Tr("panel.prefab.field.mesh_path", "Mesh Path"),
				Wui::Tr("panel.prefab.field.mesh_path.tooltip",
					"Imported model asset (.wmodel) relative to the project content root; empty = use Primitive. glTF/GLB must be imported first."),
				"Model", mesh->MeshPath, theme, changed);
			y += DrawAssetRow(ctx, x, y, width, "prefab.field.MeshRendererComponent.MaterialPath",
				Wui::Tr("panel.prefab.field.material_path", "Material Path"),
				Wui::Tr("panel.prefab.field.material_path.tooltip",
					"Material asset (.wmat); overrides the entity Color and the model's own material slots."),
				"Material", mesh->MaterialPath, theme, changed);
			{
				const float labelWidth = std::clamp(width * 0.32f, 56.0f, 120.0f);
				Wui::Label(ctx, { x, y + 3.0f },
					Wui::Tr("panel.prefab.field.mesh_index", "Mesh Index"), theme.TextMuted, 12.0f);
				const Wui::WuiRect ctrl { x + labelWidth, y, std::max(60.0f, width - labelWidth), 18.0f };
				int64_t meshIndex = mesh->MeshIndex;
				if (Wui::DragInt(ctx, Wui::HashId("prefab.field.MeshRendererComponent.MeshIndex"), ctrl,
					meshIndex, 0, 1024, theme) && meshIndex != mesh->MeshIndex)
				{
					mesh->MeshIndex = static_cast<int32_t>(meshIndex);
					changed = true;
				}
				RegisterNode(Wui::HashId("prefab.field.MeshRendererComponent.MeshIndex"), "drag-int", ctrl,
					Wui::Tr("panel.prefab.field.mesh_index", "Mesh Index"),
					std::to_string(mesh->MeshIndex), true,
					Wui::Tr("panel.prefab.field.mesh_index.tooltip",
						"Selects which mesh inside a .wmodel this entity draws (ignored by built-in primitives)."),
					true);
				Wui::Tooltip(ctx, ctrl, Wui::Tr("panel.prefab.field.mesh_index.tooltip",
					"Selects which mesh inside a .wmodel this entity draws (ignored by built-in primitives)."));
				y += kFieldRowHeight;
			}
			if (changed)
			{
				RefreshAssetWarning();
				MarkDirty();
				changed = false;
			}
			y += 6.0f;
		}

		// ---- Camera:Fov / NearClip / FarClip ----
		if (auto* camera = registry.try_get<CameraComponent>(handle))
		{
			Wui::SectionHeader(ctx, { x, y, width, 18.0f },
				Wui::Tr("panel.prefab.section.camera", "Camera"), theme.Accent, theme, 13.0f);
			y += 20.0f;
			bool changed = false;
			float fov = camera->Camera.GetPerspectiveFOV();
			y += DrawScalarRow(ctx, x, y, width, "prefab.field.CameraComponent.Fov",
				Wui::Tr("panel.prefab.field.fov", "Fov"),
				Wui::Tr("panel.prefab.field.fov.tooltip",
					"Vertical field of view in degrees for the perspective projection (1..179)."),
				fov, 0.5f, 1.0f, 179.0f, theme, changed);
			if (changed)
			{
				camera->Camera.SetPerspectiveFOV(fov);
				MarkDirty();
				changed = false;
			}
			float nearClip = camera->Camera.GetPerspectiveNearClip();
			y += DrawScalarRow(ctx, x, y, width, "prefab.field.CameraComponent.NearClip",
				Wui::Tr("panel.prefab.field.near_clip", "Near Clip"),
				Wui::Tr("panel.prefab.field.near_clip.tooltip",
					"Near plane distance of the perspective projection; keep it as large as the scene allows."),
				nearClip, 0.01f, 0.001f, 1000.0f, theme, changed);
			if (changed)
			{
				camera->Camera.SetPerspectiveNearClip(nearClip);
				MarkDirty();
				changed = false;
			}
			float farClip = camera->Camera.GetPerspectiveFarClip();
			y += DrawScalarRow(ctx, x, y, width, "prefab.field.CameraComponent.FarClip",
				Wui::Tr("panel.prefab.field.far_clip", "Far Clip"),
				Wui::Tr("panel.prefab.field.far_clip.tooltip",
					"Far plane distance of the perspective projection (visible range end)."),
				farClip, 1.0f, 0.01f, 100000.0f, theme, changed);
			if (changed)
			{
				camera->Camera.SetPerspectiveFarClip(farClip);
				MarkDirty();
			}
			y += 6.0f;
		}

		// ---- 灯光:Color(取色器)/ Intensity / Range ----
		const auto drawLightHeader = [&](const char* id, const char* fallback)
		{
			Wui::SectionHeader(ctx, { x, y, width, 18.0f }, Wui::Tr(id, fallback), theme.Accent, theme, 13.0f);
			y += 20.0f;
		};
		if (auto* light = registry.try_get<DirectionalLightComponent>(handle))
		{
			drawLightHeader("panel.prefab.section.directional_light", "Directional Light");
			bool changed = false;
			glm::vec4 color { light->Color, 1.0f };
			y += DrawColorRow(ctx, x, y, width, "prefab.field.DirectionalLightComponent.Color",
				Wui::Tr("panel.prefab.field.light_color", "Color"),
				Wui::Tr("panel.prefab.field.light_color.tooltip",
					"Linear color of the light (the shader decodes it with pow 2.2)."),
				color, theme, changed);
			if (changed)
			{
				light->Color = { color.x, color.y, color.z };
				MarkDirty();
				changed = false;
			}
			float intensity = light->Intensity;
			y += DrawScalarRow(ctx, x, y, width, "prefab.field.DirectionalLightComponent.Intensity",
				Wui::Tr("panel.prefab.field.intensity", "Intensity"),
				Wui::Tr("panel.prefab.field.intensity.tooltip", "Brightness multiplier applied to the light color."),
				intensity, 0.05f, 0.0f, 100.0f, theme, changed);
			if (changed)
			{
				light->Intensity = intensity;
				MarkDirty();
			}
			y += 6.0f;
		}
		if (auto* light = registry.try_get<PointLightComponent>(handle))
		{
			drawLightHeader("panel.prefab.section.point_light", "Point Light");
			bool changed = false;
			glm::vec4 color { light->Color, 1.0f };
			y += DrawColorRow(ctx, x, y, width, "prefab.field.PointLightComponent.Color",
				Wui::Tr("panel.prefab.field.light_color", "Color"),
				Wui::Tr("panel.prefab.field.light_color.tooltip",
					"Linear color of the light (the shader decodes it with pow 2.2)."),
				color, theme, changed);
			if (changed)
			{
				light->Color = { color.x, color.y, color.z };
				MarkDirty();
				changed = false;
			}
			float intensity = light->Intensity;
			y += DrawScalarRow(ctx, x, y, width, "prefab.field.PointLightComponent.Intensity",
				Wui::Tr("panel.prefab.field.intensity", "Intensity"),
				Wui::Tr("panel.prefab.field.intensity.tooltip", "Brightness multiplier applied to the light color."),
				intensity, 0.05f, 0.0f, 100.0f, theme, changed);
			if (changed)
			{
				light->Intensity = intensity;
				MarkDirty();
				changed = false;
			}
			float range = light->Range;
			y += DrawScalarRow(ctx, x, y, width, "prefab.field.PointLightComponent.Range",
				Wui::Tr("panel.prefab.field.range", "Range"),
				Wui::Tr("panel.prefab.field.range.tooltip",
					"Falloff radius in world units; attenuation is (1 - d/Range)^2."),
				range, 0.1f, 0.0f, 1000.0f, theme, changed);
			if (changed)
			{
				light->Range = range;
				MarkDirty();
			}
			y += 6.0f;
		}
		if (auto* light = registry.try_get<AmbientLightComponent>(handle))
		{
			drawLightHeader("panel.prefab.section.ambient_light", "Ambient Light");
			bool changed = false;
			glm::vec4 color { light->Color, 1.0f };
			y += DrawColorRow(ctx, x, y, width, "prefab.field.AmbientLightComponent.Color",
				Wui::Tr("panel.prefab.field.light_color", "Color"),
				Wui::Tr("panel.prefab.field.light_color.tooltip",
					"Linear ambient color added to every surface (the shader decodes it with pow 2.2)."),
				color, theme, changed);
			if (changed)
			{
				light->Color = { color.x, color.y, color.z };
				MarkDirty();
				changed = false;
			}
			float intensity = light->Intensity;
			y += DrawScalarRow(ctx, x, y, width, "prefab.field.AmbientLightComponent.Intensity",
				Wui::Tr("panel.prefab.field.intensity", "Intensity"),
				Wui::Tr("panel.prefab.field.intensity.tooltip", "Brightness multiplier applied to the light color."),
				intensity, 0.05f, 0.0f, 100.0f, theme, changed);
			if (changed)
			{
				light->Intensity = intensity;
				MarkDirty();
			}
			y += 6.0f;
		}

		// ---- 其余组件:只读摘要(小节标题写明"只读")----
		const std::vector<std::string> readOnly = BuildReadOnlySummary(handle);
		if (!readOnly.empty())
		{
			const std::string readOnlyTitle = Wui::Tr("panel.prefab.readonly_section",
				"Other Components (read-only)");
			Wui::SectionHeader(ctx, { x, y, width, 18.0f },
				readOnlyTitle, theme.TextMuted, theme, 13.0f);
			// 单独登记:脚本/读屏要能读到"这一节不能改"(与可编辑小节区分开)。
			RegisterNode(Wui::HashId("prefab.readonly_section"), "text", { x, y, width, 18.0f },
				readOnlyTitle, readOnlyTitle, true,
				Wui::Tr("panel.prefab.readonly_section.tooltip",
					"These components are shown as a read-only summary; edit them in the full editor."),
				false);
			y += 20.0f;
			for (const std::string& line : readOnly)
			{
				Wui::Label(ctx, { x + 2.0f, y + 1.0f }, TruncateUtf8(line, 140), theme.TextMuted, 12.0f);
				y += kReadOnlyLineHeight;
			}
		}
		Wui::EndScrollArea(ctx);
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
