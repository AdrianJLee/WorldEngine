#include "WUI/Panels/FoliagePanel.h"
#include "WUI/Common/EditorAssetCatalog.h"
#include "WUI/Common/AssetThumbnailRenderer.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Scene/Components.h"
#include "World/Foliage/FoliageRenderer.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Core/Log.h"
#include "World/Core/KeyCodes.h"
#include "World/Core/StringPool.h"
#include "World/Utils/Paths.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	FoliagePanel::FoliagePanel()
	{
		ResetDefaultPalette();
	}

	void FoliagePanel::ResetDefaultPalette()
	{
		m_Foliage = std::make_shared<FoliageData>();
		if (m_Foliage)
		{
			m_Foliage->Clear();
			while (m_Foliage->GetTypeCount() > 0)
			{
				m_Foliage->RemoveType(0);
			}

			FoliageType grass;
			grass.Name = "翠绿草丛 (Wild Grass)";
			grass.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/grass_cluster.wmodel");
			grass.MaterialAsset.Path = StringPool::Get().InternPath("textures/foliage/grass_tuft.png");
			grass.Density = 3.0f;
			grass.ScaleMinMax = glm::vec2(0.8f, 1.3f);
			grass.RandomYaw = 360.0f;
			grass.PitchVariation = 5.0f;
			grass.SlopeLimit = glm::vec2(0.0f, 45.0f);
			grass.HeightLimit = glm::vec2(-1000.0f, 1000.0f);
			grass.CullDistance = 150.0f;
			grass.WindMultiplier = 1.0f;
			grass.AlignToNormal = true;
			grass.PaintingActive = true;
			grass.LayerMask = 0x01; // Bit 0: Grass
			m_Foliage->AddType(grass);

			FoliageType flower;
			flower.Name = "雏菊野花 (Wild Flowers)";
			flower.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/grass_cluster.wmodel");
			flower.MaterialAsset.Path = StringPool::Get().InternPath("textures/foliage/wild_flower.png");
			flower.Density = 1.0f;
			flower.ScaleMinMax = glm::vec2(0.6f, 1.0f);
			flower.RandomYaw = 360.0f;
			flower.PitchVariation = 6.0f;
			flower.SlopeLimit = glm::vec2(0.0f, 35.0f);
			flower.HeightLimit = glm::vec2(-1000.0f, 1000.0f);
			flower.CullDistance = 120.0f;
			flower.WindMultiplier = 1.2f;
			flower.AlignToNormal = true;
			flower.PaintingActive = true;
			flower.LayerMask = 0x01; // Bit 0: Grass
			m_Foliage->AddType(flower);

			FoliageType shrub;
			shrub.Name = "矮生灌木 (Forest Shrub)";
			shrub.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/bush_cluster.wmodel");
			shrub.MaterialAsset.Path = StringPool::Get().InternPath("textures/foliage/bush_leaves.png");
			shrub.Density = 0.6f;
			shrub.ScaleMinMax = glm::vec2(0.8f, 1.8f);
			shrub.RandomYaw = 360.0f;
			shrub.PitchVariation = 8.0f;
			shrub.SlopeLimit = glm::vec2(0.0f, 40.0f);
			shrub.HeightLimit = glm::vec2(-1000.0f, 1000.0f);
			shrub.CullDistance = 180.0f;
			shrub.WindMultiplier = 0.8f;
			shrub.AlignToNormal = true;
			shrub.PaintingActive = true;
			shrub.LayerMask = 0x03; // Bit 0 & 1: Grass & Dirt
			m_Foliage->AddType(shrub);

			FoliageType rock;
			rock.Name = "地表碎石 (Scatter Rocks)";
			rock.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/grass_cluster.wmodel");
			rock.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/scatter_pebble.wmodel");
			rock.MaterialAsset.Path = StringPool::Get().InternPath("textures/terrain/rock_albedo.png");
			rock.Density = 0.4f;
			rock.ScaleMinMax = glm::vec2(0.5f, 2.0f);
			rock.RandomYaw = 360.0f;
			rock.PitchVariation = 15.0f;
			rock.SlopeLimit = glm::vec2(0.0f, 60.0f);
			rock.HeightLimit = glm::vec2(-1000.0f, 1000.0f);
			rock.CullDistance = 200.0f;
			rock.WindMultiplier = 0.0f;
			rock.AlignToNormal = true;
			rock.PaintingActive = true;
			rock.LayerMask = 0x0E; // Bit 1, 2, 3: Dirt, Rock, Sand
			m_Foliage->AddType(rock);
		}
		m_ActiveTypeIdx = 0;
		m_CachedTypeIdx = -1;
		m_StatusFeedback = "植被生态调色板已就绪，预置 4 种自然植被与地貌规则。";
	}

	bool FoliagePanel::LoadFoliageAsset(const std::string& path)
	{
		if (!m_Foliage)
			m_Foliage = std::make_shared<FoliageData>();

		if (m_Foliage->LoadFromFile(path))
		{
			m_LoadedPath = path;
			for (size_t ti = 0; ti < m_Foliage->GetTypeCount(); ++ti)
			{
				auto& t = m_Foliage->GetType(ti);
				if (!t.MeshAsset.Path.IsValid() || StringPool::Get().PathOf(t.MeshAsset.Path).empty())
					t.MeshAsset.Path = StringPool::Get().InternPath(ti == 2 ? "models/foliage/bush_cluster.wmodel" : (ti == 3 ? "models/foliage/scatter_pebble.wmodel" : "models/foliage/grass_cluster.wmodel"));
				if (!t.MaterialAsset.Path.IsValid() || StringPool::Get().PathOf(t.MaterialAsset.Path).empty())
					t.MaterialAsset.Path = StringPool::Get().InternPath(ti == 1 ? "textures/foliage/wild_flower.png" : (ti == 3 ? "textures/terrain/rock_albedo.png" : "textures/foliage/grass_tuft.png"));
			}
			m_CachedTypeIdx = -1;
			m_StatusFeedback = "已载入植被资产: " + path + " (实例总数: " + std::to_string(m_Foliage->GetTotalInstanceCount()) + ")";
			return true;
		}
		m_StatusFeedback = "载入植被资产失败: " + path;
		return false;
	}

	bool FoliagePanel::SaveFoliageAsset(const std::string& path)
	{
		if (!m_Foliage)
			return false;

		if (m_Foliage->SaveToFile(path))
		{
			m_LoadedPath = path;
			m_StatusFeedback = "成功保存植被资产: " + path + " (实例总数: " + std::to_string(m_Foliage->GetTotalInstanceCount()) + ")";
			return true;
		}
		m_StatusFeedback = "保存植被资产失败: " + path;
		return false;
	}

	bool FoliagePanel::SaveCurrentFoliage()
	{
		if (!m_Foliage)
			return false;

		std::filesystem::path target = m_LoadedPath;
		if (target.empty())
		{
			target = World::Paths::AssetRoot() / "foliage" / "default.wfoliage";
		}
		else if (!std::filesystem::exists(target))
		{
			std::filesystem::path p1 = World::Paths::AssetRoot() / target;
			std::filesystem::path p2 = World::Paths::ProjectFile(target.string());
			if (std::filesystem::exists(p1.parent_path()))
				target = p1;
			else if (std::filesystem::exists(p2.parent_path()))
				target = p2;
			else
				target = World::Paths::AssetRoot() / "foliage" / "default.wfoliage";
		}
		std::error_code ec;
		std::filesystem::create_directories(target.parent_path(), ec);
		return SaveFoliageAsset(target.string());
	}

	bool FoliagePanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)ctrl;
		(void)alt;
		if (keyCode == KeyCodes::LeftBracket)
		{
			if (shift)
				SetBrushDensity(m_BrushDensity - 0.2f);
			else
				SetBrushRadius(m_BrushRadius - 1.0f);
			return true;
		}
		else if (keyCode == KeyCodes::RightBracket)
		{
			if (shift)
				SetBrushDensity(m_BrushDensity + 0.2f);
			else
				SetBrushRadius(m_BrushRadius + 1.0f);
			return true;
		}
		return false;
	}

	size_t FoliagePanel::GetInstanceCountByType(size_t typeIndex) const
	{
		if (!m_Foliage)
			return 0;
		return m_Foliage->GetInstanceCountByType(typeIndex);
	}

	void FoliagePanel::RealignToTerrain(const TerrainData* terrain, const glm::mat4& terrainWorld)
	{
		if (!m_Foliage || !terrain)
			return;
		m_Foliage->RealignToTerrain(terrain, terrainWorld);
	}

	std::vector<size_t> FoliagePanel::GetActivePaintingTypes() const
	{
		std::vector<size_t> types;
		if (!m_Foliage)
			return types;

		const size_t count = m_Foliage->GetTypeCount();
		for (size_t i = 0; i < count; ++i)
		{
			if (m_Foliage->GetType(i).PaintingActive)
				types.push_back(i);
		}

		if (types.empty() && m_ActiveTypeIdx >= 0 && static_cast<size_t>(m_ActiveTypeIdx) < count)
		{
			types.push_back(static_cast<size_t>(m_ActiveTypeIdx));
		}
		return types;
	}

	void FoliagePanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::Panel(ctx, rect, Title(), theme);

		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 32.0f + pad;

		// 1. 双向绑定场景 FoliageComponent
		Ref<Scene> activeScene = host.GetActiveScene();
		m_BoundEntity = {};
		if (activeScene)
		{
			Entity selected = host.GetSelectedEntity();
			if (selected.IsValid() && selected.GetScene() == activeScene.get() && selected.HasComponent<FoliageComponent>())
			{
				m_BoundEntity = selected;
			}
			else
			{
				for (auto [handle, comp] : activeScene->GetRegistry().view<FoliageComponent>().each())
				{
					m_BoundEntity = Entity(activeScene.get(), handle);
					break;
				}
			}

			if (!m_BoundEntity.IsValid())
			{
				activeScene->DeferStructuralChange([](Scene& s) {
					Entity fol = Entity::CreateEntity(&s, "Foliage (3D 植被)");
					fol.AddComponent<TransformComponent>();
					auto& comp = fol.AddComponent<FoliageComponent>();
					comp.FoliageAsset.Path = StringPool::Get().InternPath("assets/foliage/default.wfoliage");
				});
			}
			else
			{
				auto& comp = m_BoundEntity.GetComponent<FoliageComponent>();
				m_Foliage = FoliageRenderer::GetOrCreateFoliageData(comp.FoliageAsset);
				m_LoadedPath = std::string(StringPool::Get().PathOf(comp.FoliageAsset.Path));
				if (m_Foliage)
				{
					for (size_t ti = 0; ti < m_Foliage->GetTypeCount(); ++ti)
					{
						auto& t = m_Foliage->GetType(ti);
						if (!t.MeshAsset.Path.IsValid())
							t.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/grass_cluster.wmodel");
						if (!t.MaterialAsset.Path.IsValid())
							t.MaterialAsset.Path = StringPool::Get().InternPath(ti == 1 ? "textures/foliage/wild_flower.png" : "textures/foliage/grass_tuft.png");
					}
				}
				comp.InstanceCount = m_Foliage ? static_cast<uint32_t>(m_Foliage->GetTotalInstanceCount()) : 0;
			}
		}

		if (!m_Foliage)
		{
			m_Foliage = std::make_shared<FoliageData>();
		}

		// 2. 顶部概览信息
		const size_t totalInstances = m_Foliage->GetTotalInstanceCount();
		const size_t typeCount = m_Foliage->GetTypeCount();
		char headerBuf[128] = {};
		std::snprintf(headerBuf, sizeof(headerBuf), "资产: %s | 种类: %zu | 实例总数: %zu 颗",
			m_LoadedPath.empty() ? "default.wfoliage" : std::filesystem::path(m_LoadedPath).filename().string().c_str(),
			typeCount, totalInstances);
		Wui::Label(ctx, { startX, curY }, headerBuf, theme.TextMuted, theme.FontSizeSmall);
		curY += 22.0f;

		// 3. 工具模式选项卡: [散布 (Scatter)] [擦除 (Erase)] [环境与优化 (Settings)]
		const std::vector<std::string> tools = { "生态散布", "擦除管理", "全局风场" };
		if (Wui::TabBar(ctx, Wui::HashId("foliage.tool.tabs"), { startX, curY, contentW, 28.0f }, tools, m_ToolIdx, theme))
		{
			m_Tool = static_cast<FoliageBrushTool>(m_ToolIdx);
		}
		curY += 34.0f;

		// 分流子页面
		switch (m_Tool)
		{
		case FoliageBrushTool::Paint:
			RenderPaintTab(ctx, startX, curY, contentW, theme, host);
			break;
		case FoliageBrushTool::Erase:
			RenderEraseTab(ctx, startX, curY, contentW, theme, host);
			break;
		case FoliageBrushTool::Settings:
			RenderSettingsTab(ctx, startX, curY, contentW, theme, host);
			break;
		}

		// 底部状态反馈栏
		if (!m_StatusFeedback.empty())
		{
			const float statusY = rect.Y + rect.H - 24.0f;
			Wui::Label(ctx, { startX, statusY }, m_StatusFeedback, theme.TextMuted, theme.FontSizeSmall);
		}
	}

	void FoliagePanel::RenderPaintTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		const size_t typeCount = m_Foliage ? m_Foliage->GetTypeCount() : 0;
		if (m_ActiveTypeIdx >= static_cast<int>(typeCount))
			m_ActiveTypeIdx = typeCount > 0 ? static_cast<int>(typeCount - 1) : -1;

		// 1. 生态调色板标题与操作工具栏
		Wui::Label(ctx, { startX, curY + 4.0f }, "自然生态调色板 (Foliage Palette)", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		// 按钮组: 2 行 x 2 列宽按钮，严谨对齐窄侧栏
		const float halfBtnW = (contentW - theme.PadSmall) * 0.5f;
		const float btnH = 24.0f;

		// Row 1: [+ 添加植物] [- 移除选中]
		if (Wui::Button(ctx, Wui::HashId("foliage.btn.add"), { startX, curY, halfBtnW, btnH }, "+ 添加植物", theme))
		{
			FoliageType newType;
			newType.Name = "新植物 " + std::to_string(typeCount + 1);
			newType.Density = 1.0f;
			newType.ScaleMinMax = glm::vec2(0.8f, 1.2f);
			newType.RandomYaw = 360.0f;
			newType.PitchVariation = 5.0f;
			newType.SlopeLimit = glm::vec2(0.0f, 45.0f);
			newType.HeightLimit = glm::vec2(-1000.0f, 1000.0f);
			newType.CullDistance = 150.0f;
			newType.WindMultiplier = 1.0f;
			newType.AlignToNormal = true;
			newType.PaintingActive = true;
			newType.LayerMask = 0xFFFFFFFF;

			m_ActiveTypeIdx = static_cast<int>(m_Foliage->AddType(newType));
			m_CachedTypeIdx = -1;
			m_StatusFeedback = "已新增植物生态项: " + newType.Name;
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, halfBtnW, btnH }, "在生态列表中新增一种植物类型，并立即在检视器中配置参数与网格资产");

		const bool canRemove = (typeCount > 0 && m_ActiveTypeIdx >= 0 && m_ActiveTypeIdx < static_cast<int>(typeCount));
		auto removeTheme = theme;
		if (!canRemove)
			removeTheme.Text = theme.TextDisabled;
		if (Wui::Button(ctx, Wui::HashId("foliage.btn.remove"), { startX + halfBtnW + theme.PadSmall, curY, halfBtnW, btnH }, "- 移除选中", removeTheme) && canRemove)
		{
			const std::string removedName = m_Foliage->GetType(m_ActiveTypeIdx).Name;
			m_Foliage->RemoveType(static_cast<size_t>(m_ActiveTypeIdx));
			if (m_ActiveTypeIdx >= static_cast<int>(m_Foliage->GetTypeCount()))
				m_ActiveTypeIdx = static_cast<int>(m_Foliage->GetTypeCount()) - 1;
			m_CachedTypeIdx = -1;
			m_StatusFeedback = "已移除植物生态项: " + removedName;
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + halfBtnW + theme.PadSmall, curY, halfBtnW, btnH }, "移除当前选中的植物类型，并同步清理该物种在场景中的全部散布实例");

		curY += btnH + 4.0f;

		// Row 2: [全选混刷] [单选当前]
		if (Wui::Button(ctx, Wui::HashId("foliage.btn.select_all"), { startX, curY, halfBtnW, btnH }, "全选混刷", theme))
		{
			for (size_t i = 0; i < typeCount; ++i)
				m_Foliage->GetType(i).PaintingActive = true;
			m_StatusFeedback = "已激活全物种混刷散布模式。";
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX, curY, halfBtnW, btnH }, "一键勾选所有植物参与视口笔刷多物种混刷");

		if (Wui::Button(ctx, Wui::HashId("foliage.btn.select_single"), { startX + halfBtnW + theme.PadSmall, curY, halfBtnW, btnH }, "单选当前", theme))
		{
			for (size_t i = 0; i < typeCount; ++i)
				m_Foliage->GetType(i).PaintingActive = (static_cast<int>(i) == m_ActiveTypeIdx);
			m_StatusFeedback = "已切换为单物种独立散布模式。";
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + halfBtnW + theme.PadSmall, curY, halfBtnW, btnH }, "仅保留当前选中的植物参与笔刷散布，取消其他所有植物的混刷勾选");

		curY += btnH + 8.0f;

		// 2. 动态植物生态列表 (直接内联折叠展开参数检视器)
		const float itemH = 26.0f;
		for (size_t i = 0; i < typeCount; ++i)
		{
			auto& type = m_Foliage->GetType(i);
			const bool isSelected = (m_ActiveTypeIdx == static_cast<int>(i));
			const bool isExpanded = (m_ExpandedTypeIdx == static_cast<int>(i));

			// 折叠/展开箭头 [v] / [>] (宽 18px)
			const Wui::WuiRect foldRect { startX, curY + 2.0f, 18.0f, 22.0f };
			auto foldTheme = theme;
			foldTheme.ButtonBg = { 0, 0, 0, 0 };
			foldTheme.Border = { 0, 0, 0, 0 };
			foldTheme.Text = isExpanded ? theme.Accent : theme.TextMuted;
			if (Wui::Button(ctx, Wui::HashId(("foliage.type.fold." + std::to_string(i)).c_str()), foldRect, isExpanded ? "v" : ">", foldTheme))
			{
				m_ExpandedTypeIdx = isExpanded ? -1 : static_cast<int>(i);
				m_ActiveTypeIdx = static_cast<int>(i);
			}

			// 混刷复选框 [✓] (宽 20px)
			const Wui::WuiRect chkRect { startX + 20.0f, curY + 2.0f, 20.0f, 22.0f };
			bool active = type.PaintingActive;
			if (Wui::Checkbox(ctx, Wui::HashId(("foliage.type.chk." + std::to_string(i)).c_str()), chkRect, "", active, theme))
			{
				type.PaintingActive = active;
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, chkRect, "勾选后参与视口笔刷一笔多物种自然混刷");

			// 名称选择按钮
			const float badgeW = 52.0f;
			const float nameBtnX = startX + 44.0f;
			const float nameBtnW = std::max(50.0f, contentW - 44.0f - badgeW - 4.0f);
			const Wui::WuiRect nameRect { nameBtnX, curY + 1.0f, nameBtnW, 24.0f };

			auto nameTheme = theme;
			if (isSelected)
			{
				nameTheme.ButtonBg = theme.ActiveBg;
				nameTheme.Border = theme.Accent;
				nameTheme.Text = theme.Accent;
			}
			if (Wui::Button(ctx, Wui::HashId(("foliage.type.sel." + std::to_string(i)).c_str()), nameRect, type.Name, nameTheme))
			{
				m_ActiveTypeIdx = static_cast<int>(i);
				m_ExpandedTypeIdx = static_cast<int>(i);
			}

			// 实例数徽标 Badge (右对齐)
			const size_t instCount = m_Foliage->GetInstanceCountByType(i);
			char countBuf[32] = {};
			std::snprintf(countBuf, sizeof(countBuf), "[%zu颗]", instCount);
			const Wui::WuiRect badgeRect { startX + contentW - badgeW, curY + 1.0f, badgeW, 24.0f };
			Wui::Label(ctx, { badgeRect.X + 4.0f, badgeRect.Y + 5.0f }, countBuf, isSelected ? theme.Accent : theme.TextMuted, theme.FontSizeCaption);

			curY += itemH + 2.0f;

			// 若该物种处于展开状态，直接在其正下方内联渲染完整参数检视器
			if (isExpanded)
			{
				const float inspIndent = 12.0f;
				const float inspX = startX + inspIndent;
				const float inspW = contentW - inspIndent;

				if (m_CachedTypeIdx != static_cast<int>(i))
				{
					m_EditName = type.Name;
					m_EditMeshPath = type.MeshAsset.Path.IsValid() ? std::string(StringPool::Get().PathOf(type.MeshAsset.Path)) : "models/foliage/grass_cluster.wmodel";
					m_EditMatPath = type.MaterialAsset.Path.IsValid() ? std::string(StringPool::Get().PathOf(type.MaterialAsset.Path)) : (i == 1 ? "textures/foliage/wild_flower.png" : "textures/foliage/grass_tuft.png");
					m_CachedTypeIdx = static_cast<int>(i);
				}

				// 1. 植物名称
				Wui::Label(ctx, { inspX, curY + 4.0f }, "名称:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::TextField(ctx, Wui::HashId(("foliage.insp.name." + std::to_string(i)).c_str()), { inspX + 45.0f, curY, inspW - 45.0f, 22.0f }, m_EditName, theme))
				{
					type.Name = m_EditName;
					host.MarkDocumentDirty();
				}
				curY += 26.0f;

				// 2. Mesh 资产绑定与缩略图卡槽
				Wui::Label(ctx, { inspX, curY + 3.0f }, "Mesh:", theme.TextMuted, theme.FontSizeSmall);
				const float thumbSize = 22.0f;
				const float thumbPad = 4.0f;
				const Wui::WuiRect meshThumbRect { inspX + 45.0f, curY, thumbSize, thumbSize };
				const Wui::WuiRect meshComboRect { inspX + 45.0f + thumbSize + thumbPad, curY, inspW - 45.0f - thumbSize - thumbPad, 22.0f };

				if (!m_EditMeshPath.empty() && m_EditMeshPath != "(none)")
				{
					Editor::AssetThumbnailRenderer::DrawThumbnail(ctx, meshThumbRect, m_EditMeshPath, AssetCategory::Mesh, theme);
					Wui::HighlightOutline(ctx, meshThumbRect, theme.BorderStrong, 3.0f, 1.0f);
				}
				else
				{
					Wui::PanelBackground(ctx, meshThumbRect, theme.ContentBg, 3.0f);
					Wui::HighlightOutline(ctx, meshThumbRect, theme.Border, 3.0f, 1.0f);
					Wui::Label(ctx, { meshThumbRect.X + thumbSize * 0.5f - 4.0f, meshThumbRect.Y + (thumbSize - 14.0f) * 0.5f }, "+", theme.TextDisabled, 12.0f);
				}
				Wui::Tooltip(ctx, meshThumbRect, m_EditMeshPath.empty() || m_EditMeshPath == "(none)" ? "选择植被 3D Mesh" : (m_EditMeshPath + " (点击浏览)"));
				if (ctx.IsClicked(meshThumbRect))
				{
					host.OpenAssetPicker("选择植被 Mesh 资产", AssetCategory::Mesh, [this, &type, &host](const std::string& picked) {
						type.MeshAsset.Path = picked.empty() ? PathId() : StringPool::Get().InternPath(picked);
						m_EditMeshPath = picked;
						m_StatusFeedback = "已选择植被 Mesh: " + picked;
						host.MarkDocumentDirty();
					});
				}

				const auto& modelAssets = Editor::AssetCatalog::PathsFor(EditorAssetKind::Model);
				const auto& modelSrcs = Editor::AssetCatalog::PathsFor(EditorAssetKind::ModelSource);
				std::vector<std::string> meshOptions;
				meshOptions.push_back("(none)");
				auto addUniqueMesh = [&](const std::string& opt) {
					std::string optNorm = (opt.rfind("engine/", 0) == 0) ? opt.substr(7) : opt;
					for (const auto& existing : meshOptions) {
						std::string exNorm = (existing.rfind("engine/", 0) == 0) ? existing.substr(7) : existing;
						if (exNorm == optNorm) return;
					}
					meshOptions.push_back(opt);
				};
				addUniqueMesh("models/foliage/grass_cluster.wmodel");
				addUniqueMesh("models/foliage/bush_cluster.wmodel");
				addUniqueMesh("models/foliage/scatter_pebble.wmodel");
				for (const auto& mp : modelAssets) addUniqueMesh(mp);
				for (const auto& mp : modelSrcs) addUniqueMesh(mp);

				int curMeshIdx = 0;
				if (!m_EditMeshPath.empty())
				{
					std::string normCur = (m_EditMeshPath.rfind("engine/", 0) == 0) ? m_EditMeshPath.substr(7) : m_EditMeshPath;
					for (size_t mi = 0; mi < meshOptions.size(); ++mi) {
						std::string optNorm = (meshOptions[mi].rfind("engine/", 0) == 0) ? meshOptions[mi].substr(7) : meshOptions[mi];
						if (optNorm == normCur) {
							curMeshIdx = static_cast<int>(mi);
							break;
						}
					}
					if (curMeshIdx == 0 && m_EditMeshPath != "(none)")
					{
						meshOptions.push_back(m_EditMeshPath);
						curMeshIdx = static_cast<int>(meshOptions.size() - 1);
					}
				}

				if (Wui::SearchableCombo(ctx, Wui::HashId(("foliage.insp.mesh_combo." + std::to_string(i)).c_str()), meshComboRect, "", meshOptions, curMeshIdx, theme))
				{
					const std::string picked = (curMeshIdx <= 0) ? "" : meshOptions[curMeshIdx];
					type.MeshAsset.Path = picked.empty() ? PathId() : StringPool::Get().InternPath(picked);
					m_EditMeshPath = picked;
					m_StatusFeedback = "已选择植被 Mesh: " + picked;
					host.MarkDocumentDirty();
				}
				curY += 26.0f;

				// 3. 材质资产绑定与缩略图卡槽
				Wui::Label(ctx, { inspX, curY + 3.0f }, "材质:", theme.TextMuted, theme.FontSizeSmall);
				const Wui::WuiRect matThumbRect { inspX + 45.0f, curY, thumbSize, thumbSize };
				const Wui::WuiRect matComboRect { inspX + 45.0f + thumbSize + thumbPad, curY, inspW - 45.0f - thumbSize - thumbPad, 22.0f };

				if (!m_EditMatPath.empty() && m_EditMatPath != "(none)")
				{
					Editor::AssetThumbnailRenderer::DrawThumbnail(ctx, matThumbRect, m_EditMatPath, AssetCategory::Unknown, theme);
					Wui::HighlightOutline(ctx, matThumbRect, theme.BorderStrong, 3.0f, 1.0f);
				}
				else
				{
					Wui::PanelBackground(ctx, matThumbRect, theme.ContentBg, 3.0f);
					Wui::HighlightOutline(ctx, matThumbRect, theme.Border, 3.0f, 1.0f);
					Wui::Label(ctx, { matThumbRect.X + thumbSize * 0.5f - 4.0f, matThumbRect.Y + (thumbSize - 14.0f) * 0.5f }, "+", theme.TextDisabled, 12.0f);
				}
				Wui::Tooltip(ctx, matThumbRect, m_EditMatPath.empty() || m_EditMatPath == "(none)" ? "选择植被材质" : (m_EditMatPath + " (点击浏览)"));
				if (ctx.IsClicked(matThumbRect))
				{
					host.OpenAssetPicker("选择植被材质或纹理", AssetCategory::Material, [this, &type, &host](const std::string& picked) {
						type.MaterialAsset.Path = picked.empty() ? PathId() : StringPool::Get().InternPath(picked);
						m_EditMatPath = picked;
						m_StatusFeedback = "已选择植被材质: " + picked;
						host.MarkDocumentDirty();
					});
				}

				const auto& texAssets = Editor::AssetCatalog::PathsFor(EditorAssetKind::TextureSource);
				const auto& matAssets = Editor::AssetCatalog::PathsFor(EditorAssetKind::Material);
				std::vector<std::string> matOptions;
				matOptions.push_back("(none)");
				auto addUniqueMat = [&](const std::string& opt) {
					std::string optNorm = (opt.rfind("engine/", 0) == 0) ? opt.substr(7) : opt;
					for (const auto& existing : matOptions) {
						std::string exNorm = (existing.rfind("engine/", 0) == 0) ? existing.substr(7) : existing;
						if (exNorm == optNorm) return;
					}
					matOptions.push_back(opt);
				};
				addUniqueMat("textures/foliage/grass_tuft.png");
				addUniqueMat("textures/foliage/wild_flower.png");
				addUniqueMat("textures/foliage/bush_leaves.png");
				addUniqueMat("textures/terrain/grass_albedo.png");
				addUniqueMat("textures/terrain/rock_albedo.png");
				for (const auto& tp : texAssets) addUniqueMat(tp);
				for (const auto& mp : matAssets) addUniqueMat(mp);

				int curMatIdx = 0;
				if (!m_EditMatPath.empty())
				{
					std::string normCur = (m_EditMatPath.rfind("engine/", 0) == 0) ? m_EditMatPath.substr(7) : m_EditMatPath;
					for (size_t mi = 0; mi < matOptions.size(); ++mi) {
						std::string optNorm = (matOptions[mi].rfind("engine/", 0) == 0) ? matOptions[mi].substr(7) : matOptions[mi];
						if (optNorm == normCur) {
							curMatIdx = static_cast<int>(mi);
							break;
						}
					}
					if (curMatIdx == 0 && m_EditMatPath != "(none)")
					{
						matOptions.push_back(m_EditMatPath);
						curMatIdx = static_cast<int>(matOptions.size() - 1);
					}
				}

				if (Wui::SearchableCombo(ctx, Wui::HashId(("foliage.insp.mat_combo." + std::to_string(i)).c_str()), matComboRect, "", matOptions, curMatIdx, theme))
				{
					const std::string picked = (curMatIdx <= 0) ? "" : matOptions[curMatIdx];
					type.MaterialAsset.Path = picked.empty() ? PathId() : StringPool::Get().InternPath(picked);
					m_EditMatPath = picked;
					m_StatusFeedback = "已选择植被材质: " + picked;
					host.MarkDocumentDirty();
				}
				curY += 26.0f;

				// 4. 密度权重与缩放区间
				Wui::Label(ctx, { inspX, curY + 4.0f }, "密度权重:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId(("foliage.insp.dens." + std::to_string(i)).c_str()), { inspX + 65.0f, curY, inspW - 65.0f, 22.0f }, type.Density, 0.1f, 0.1f, 10.0f, theme))
				{
					host.MarkDocumentDirty();
				}
				curY += 26.0f;

				const float halfFieldW = (inspW - 65.0f - theme.PadSmall) * 0.5f;
				Wui::Label(ctx, { inspX, curY + 4.0f }, "缩放区间:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId(("foliage.insp.smin." + std::to_string(i)).c_str()), { inspX + 65.0f, curY, halfFieldW, 22.0f }, type.ScaleMinMax.x, 0.05f, 0.1f, 5.0f, theme))
				{
					if (type.ScaleMinMax.x > type.ScaleMinMax.y) type.ScaleMinMax.y = type.ScaleMinMax.x;
					host.MarkDocumentDirty();
				}
				if (Wui::DragFloat(ctx, Wui::HashId(("foliage.insp.smax." + std::to_string(i)).c_str()), { inspX + 65.0f + halfFieldW + theme.PadSmall, curY, halfFieldW, 22.0f }, type.ScaleMinMax.y, 0.05f, 0.1f, 5.0f, theme))
				{
					if (type.ScaleMinMax.y < type.ScaleMinMax.x) type.ScaleMinMax.x = type.ScaleMinMax.y;
					host.MarkDocumentDirty();
				}
				curY += 26.0f;

				// 5. 偏航与法线对齐
				Wui::Label(ctx, { inspX, curY + 4.0f }, "偏航/法线:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId(("foliage.insp.yaw." + std::to_string(i)).c_str()), { inspX + 65.0f, curY, halfFieldW, 22.0f }, type.RandomYaw, 5.0f, 0.0f, 360.0f, theme))
				{
					host.MarkDocumentDirty();
				}
				bool alignNorm = type.AlignToNormal;
				if (Wui::Checkbox(ctx, Wui::HashId(("foliage.insp.anorm." + std::to_string(i)).c_str()), { inspX + 65.0f + halfFieldW + theme.PadSmall, curY, halfFieldW, 22.0f }, "对齐法线", alignNorm, theme))
				{
					type.AlignToNormal = alignNorm;
					host.MarkDocumentDirty();
				}
				curY += 26.0f;

				// 6. 坡度与高度限制
				Wui::Label(ctx, { inspX, curY + 4.0f }, "坡度上限:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId(("foliage.insp.slope." + std::to_string(i)).c_str()), { inspX + 65.0f, curY, inspW - 65.0f, 22.0f }, type.SlopeLimit.y, 1.0f, 0.0f, 90.0f, theme))
				{
					host.MarkDocumentDirty();
				}
				curY += 26.0f;

				// 7. 地貌材质层掩码 (LayerMask 动态图层显示)
				Wui::Label(ctx, { inspX, curY + 4.0f }, "地貌图层过滤 (Splatmap 权重过滤):", theme.Accent, theme.FontSizeSmall);
				Wui::Tooltip(ctx, { inspX, curY, inspW, 22.0f }, "根据笔刷处地貌 Splatmap 权重过滤：仅在勾选图层权重 ≥ 25% 的地表区域散布生长（如草丛仅在草地生长，不会刷到峭壁岩石上）。");
				curY += 22.0f;

				// 获取当前地形图层名称
				Ref<Scene> activeScene = host.GetActiveScene();
				std::shared_ptr<TerrainData> curTerrain = nullptr;
				if (activeScene)
				{
					for (auto [handle, comp] : activeScene->GetRegistry().view<TerrainComponent>().each())
					{
						curTerrain = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);
						break;
					}
				}

				const uint32_t totalLayers = curTerrain ? curTerrain->GetLayerCount() : 4;
				const float layerChkHalfW = (inspW - theme.PadSmall) * 0.5f;
				for (uint32_t l = 0; l < totalLayers; ++l)
				{
					const int row = static_cast<int>(l / 2);
					const int col = static_cast<int>(l % 2);
					const Wui::WuiRect chkR {
						inspX + static_cast<float>(col) * (layerChkHalfW + theme.PadSmall),
						curY + static_cast<float>(row) * 24.0f,
						layerChkHalfW, 22.0f
					};
					bool layerAllowed = (type.LayerMask & (1u << l)) != 0;
					std::string lName;
					if (curTerrain && l < curTerrain->GetLayerCount())
						lName = curTerrain->GetLayer(l).Name;
					if (lName.empty())
					{
						const char* defNames[4] = { "Alpine Grass (高山草甸)", "Forest Dirt (林地黑土)", "Mountain Rock (花岗岩壁)", "Riverbed Sand (河滩细沙)" };
						lName = (l < 4) ? defNames[l] : ("图层 " + std::to_string(l));
					}
					else if (lName.find(':') != std::string::npos)
					{
						lName = lName.substr(lName.find(':') + 1);
					}
					if (Wui::Checkbox(ctx, Wui::HashId(("foliage.insp.lmask." + std::to_string(i) + "." + std::to_string(l)).c_str()),
						chkR, lName, layerAllowed, theme))
					{
						if (layerAllowed)
							type.LayerMask |= (1u << l);
						else
							type.LayerMask &= ~(1u << l);
						host.MarkDocumentDirty();
					}
				}
				const int totalLMaskRows = static_cast<int>((totalLayers + 1) / 2);
				curY += static_cast<float>(totalLMaskRows) * 24.0f + 4.0f;

				// 8. 独立视距与风力
				Wui::Label(ctx, { inspX, curY + 4.0f }, "视距/风力:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::DragFloat(ctx, Wui::HashId(("foliage.insp.cull." + std::to_string(i)).c_str()), { inspX + 65.0f, curY, halfFieldW, 22.0f }, type.CullDistance, 5.0f, 20.0f, 500.0f, theme))
				{
					host.MarkDocumentDirty();
				}
				if (Wui::DragFloat(ctx, Wui::HashId(("foliage.insp.wind." + std::to_string(i)).c_str()), { inspX + 65.0f + halfFieldW + theme.PadSmall, curY, halfFieldW, 22.0f }, type.WindMultiplier, 0.1f, 0.0f, 3.0f, theme))
				{
					host.MarkDocumentDirty();
				}
				curY += 28.0f;

				// 9. 单项清空与删除
				if (Wui::Button(ctx, Wui::HashId(("foliage.insp.clear_inst." + std::to_string(i)).c_str()), { inspX, curY, halfFieldW, 24.0f }, "清空此物种", theme))
				{
					m_Foliage->EraseFoliage(glm::vec3(0.0f), 100000.0f, static_cast<int32_t>(i));
					m_StatusFeedback = "已清空物种 [" + type.Name + "] 的全部场景实例。";
					host.MarkDocumentDirty();
				}
				if (Wui::Button(ctx, Wui::HashId(("foliage.insp.del_type." + std::to_string(i)).c_str()), { inspX + halfFieldW + theme.PadSmall, curY, halfFieldW, 24.0f }, "- 删除此植物", theme))
				{
					const std::string delName = type.Name;
					m_Foliage->RemoveType(i);
					m_ExpandedTypeIdx = -1;
					if (m_ActiveTypeIdx >= static_cast<int>(m_Foliage->GetTypeCount()))
						m_ActiveTypeIdx = static_cast<int>(m_Foliage->GetTypeCount()) - 1;
					m_CachedTypeIdx = -1;
					m_StatusFeedback = "已删除植物类型: " + delName;
					host.MarkDocumentDirty();
					break;
				}
				curY += 32.0f;
			}
		}

		if (typeCount == 0)
		{
			Wui::Label(ctx, { startX + 8.0f, curY + 4.0f }, "当前生态列表为空，请点击 [+ 添加植物] 创建物种。", theme.TextMuted, theme.FontSizeSmall);
			curY += 28.0f;
		}

		curY += 4.0f;

		// 3. 笔刷全局控制卡片
		Wui::Label(ctx, { startX, curY + 4.0f }, "笔刷参数 (Brush Controls)", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "笔刷半径 (m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.brush.radius"), { startX + 85.0f, curY, contentW - 85.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 100.0f, theme);
		Wui::Tooltip(ctx, { startX + 85.0f, curY, contentW - 85.0f, 22.0f }, "视口地表植被散布/擦除圆环的作用半径 (快捷键: [ 减小 / ] 增大)");
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "总散布密度:", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.brush.density"), { startX + 85.0f, curY, contentW - 85.0f, 22.0f }, m_BrushDensity, 0.1f, 0.1f, 10.0f, theme);
		Wui::Tooltip(ctx, { startX + 85.0f, curY, contentW - 85.0f, 22.0f }, "单位面积内的期望实例生成密度 (快捷键: Shift+[ 减小 / Shift+] 增大)");
		curY += 28.0f;

		// 混刷状态指示
		const std::vector<size_t> activePaintTypes = GetActivePaintingTypes();
		char modeDesc[128] = {};
		if (activePaintTypes.size() > 1)
		{
			std::snprintf(modeDesc, sizeof(modeDesc), "混刷状态: 已启用 %zu 种物种按密度混合共生散布", activePaintTypes.size());
			Wui::Label(ctx, { startX, curY + 2.0f }, modeDesc, theme.Success, theme.FontSizeSmall);
		}
		else if (activePaintTypes.size() == 1)
		{
			std::snprintf(modeDesc, sizeof(modeDesc), "散布状态: 单物种散布模式 [%s]",
				activePaintTypes[0] < typeCount ? m_Foliage->GetType(activePaintTypes[0]).Name.c_str() : "未命名");
			Wui::Label(ctx, { startX, curY + 2.0f }, modeDesc, theme.Text, theme.FontSizeSmall);
		}
		else
		{
			Wui::Label(ctx, { startX, curY + 2.0f }, "未激活任何散布物种", theme.Warning, theme.FontSizeSmall);
		}
		curY += 28.0f;
		// 5. 批处理工具栏 (Batch Operations)
		Wui::Label(ctx, { startX, curY + 4.0f }, "批处理与操作 (Batch Tools)", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		// 重投影贴地
		if (Wui::Button(ctx, Wui::HashId("foliage.btn.realign"), { startX, curY, halfW, 28.0f }, "重投影对齐地表", theme))
		{
			Ref<Scene> activeScene = host.GetActiveScene();
			std::shared_ptr<TerrainData> terrain = nullptr;
			glm::mat4 terrainWorld { 1.0f };
			if (activeScene)
			{
				for (auto [handle, comp] : activeScene->GetRegistry().view<TerrainComponent>().each())
				{
					Entity e(activeScene.get(), handle);
					if (e.HasComponent<TransformComponent>())
					{
						terrainWorld = e.GetComponent<TransformComponent>().GetLocalMatrix();
						if (e.HasComponent<WorldTransformComponent>())
							terrainWorld = e.GetComponent<WorldTransformComponent>().Matrix;
					}
					terrain = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);
					break;
				}
			}

			if (terrain && m_Foliage)
			{
				m_Foliage->RealignToTerrain(terrain.get(), terrainWorld);
				m_StatusFeedback = "已完成地表重投影，" + std::to_string(m_Foliage->GetTotalInstanceCount()) + " 颗植被已重新贴合并对齐地表法线。";
				host.MarkDocumentDirty();
			}
			else
			{
				m_StatusFeedback = "未在场景中检测到地形组件 (TerrainComponent)，重投影取消。";
			}
		}
		Wui::Tooltip(ctx, { startX, curY, halfW, 28.0f }, "地貌雕刻微调后，一键将场景内所有植被实例重新投影贴合并对齐新地形高度与法线");

		// 清空当前选中类型
		if (Wui::Button(ctx, Wui::HashId("foliage.btn.clear_type"), { startX + halfW + theme.PadSmall, curY, halfW, 28.0f }, "清空选中物种", theme))
		{
			if (m_Foliage && m_ActiveTypeIdx >= 0 && static_cast<size_t>(m_ActiveTypeIdx) < typeCount)
			{
				const std::string typeName = m_Foliage->GetType(m_ActiveTypeIdx).Name;
				m_Foliage->EraseFoliage(glm::vec3(0.0f), 100000.0f, static_cast<int32_t>(m_ActiveTypeIdx));
				m_StatusFeedback = "已清空物种 [" + typeName + "] 的全部场景实例。";
				host.MarkDocumentDirty();
			}
		}
		curY += 32.0f;

		// 清空全图
		if (Wui::Button(ctx, Wui::HashId("foliage.btn.clear_all"), { startX, curY, halfW, 28.0f }, "清空全图植被", theme))
		{
			if (m_Foliage)
			{
				m_Foliage->Clear();
				m_StatusFeedback = "已彻底清空场景内所有植被实例。";
				host.MarkDocumentDirty();
			}
		}

		// 保存资产
		if (Wui::Button(ctx, Wui::HashId("foliage.btn.save_asset"), { startX + halfW + theme.PadSmall, curY, halfW, 28.0f }, "保存植被资产 (.wfoliage)", theme))
		{
			SaveCurrentFoliage();
			host.MarkDocumentDirty();
		}
		curY += 34.0f;
	}

	void FoliagePanel::RenderEraseTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 4.0f }, "植被区域擦除 (Foliage Erase)", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "擦除半径 (m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("foliage.erase.radius"), { startX + 90.0f, curY, contentW - 90.0f, 22.0f }, m_BrushRadius, 0.5f, 0.5f, 100.0f, theme);
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "擦除目标物种:", theme.Text, theme.FontSizeSmall);
		curY += 22.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;
		const bool isAll = (m_EraseTarget == FoliageEraseTarget::AllSpecies);

		auto allTheme = theme;
		if (isAll) { allTheme.ButtonBg = theme.ActiveBg; allTheme.Border = theme.Accent; allTheme.Text = theme.Accent; }
		if (Wui::Button(ctx, Wui::HashId("foliage.erase.target_all"), { startX, curY, halfW, 26.0f }, "擦除全部物种", allTheme))
		{
			m_EraseTarget = FoliageEraseTarget::AllSpecies;
		}

		auto singleTheme = theme;
		if (!isAll) { singleTheme.ButtonBg = theme.ActiveBg; singleTheme.Border = theme.Accent; singleTheme.Text = theme.Accent; }
		if (Wui::Button(ctx, Wui::HashId("foliage.erase.target_single"), { startX + halfW + theme.PadSmall, curY, halfW, 26.0f }, "仅擦除选中物种", singleTheme))
		{
			m_EraseTarget = FoliageEraseTarget::ActiveTypeOnly;
		}
		curY += 32.0f;

		// 快捷擦除说明
		Wui::Label(ctx, { startX, curY }, "视口操作提示:", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		Wui::Label(ctx, { startX + 8.0f, curY }, "• 视口散布模式下随时按住 Shift 即可临时拖拽擦除", theme.TextMuted, theme.FontSizeSmall);
		curY += 18.0f;
		Wui::Label(ctx, { startX + 8.0f, curY }, "• 按住 Shift + Alt 可在视口中临时擦除全图物种", theme.TextMuted, theme.FontSizeSmall);
		curY += 28.0f;

		// 快速全量清理
		if (Wui::Button(ctx, Wui::HashId("foliage.erase.btn_clear_active"), { startX, curY, halfW, 28.0f }, "清空当前选中物种", theme))
		{
			if (m_Foliage && m_ActiveTypeIdx >= 0 && static_cast<size_t>(m_ActiveTypeIdx) < m_Foliage->GetTypeCount())
			{
				m_Foliage->EraseFoliage(glm::vec3(0.0f), 100000.0f, static_cast<int32_t>(m_ActiveTypeIdx));
				m_StatusFeedback = "已清空选中物种的所有实例。";
				host.MarkDocumentDirty();
			}
		}

		if (Wui::Button(ctx, Wui::HashId("foliage.erase.btn_clear_all"), { startX + halfW + theme.PadSmall, curY, halfW, 28.0f }, "彻底清空全图", theme))
		{
			if (m_Foliage)
			{
				m_Foliage->Clear();
				m_StatusFeedback = "已彻底清空场景内所有植被实例。";
				host.MarkDocumentDirty();
			}
		}
		curY += 34.0f;
	}

	void FoliagePanel::RenderSettingsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 4.0f }, "真实场景风场与视距同步 (FoliageComponent)", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		if (!m_BoundEntity.IsValid() || !m_BoundEntity.HasComponent<FoliageComponent>())
		{
			Wui::Label(ctx, { startX, curY + 4.0f }, "未在当前活动场景中找到 FoliageComponent 实体。", theme.Warning, theme.FontSizeSmall);
			curY += 26.0f;
			return;
		}

		auto& comp = m_BoundEntity.GetComponent<FoliageComponent>();

		// 风场扰动控制
		Wui::Label(ctx, { startX, curY + 4.0f }, "风场扰动强度:", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("foliage.comp.wind_strength"), { startX + 110.0f, curY, contentW - 110.0f, 22.0f }, comp.WindStrength, 0.05f, 0.0f, 10.0f, theme))
		{
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 110.0f, curY, contentW - 110.0f, 22.0f }, "植被顶点沿水平风向受力摇摆的最大振幅倍率");
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "风场波浪频率:", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("foliage.comp.wind_freq"), { startX + 110.0f, curY, contentW - 110.0f, 22.0f }, comp.WindFrequency, 0.05f, 0.1f, 10.0f, theme))
		{
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 110.0f, curY, contentW - 110.0f, 22.0f }, "大面积草浪起伏扩散的时间周期振荡频率");
		curY += 28.0f;

		bool enableWind = (comp.EnableWind != 0);
		Wui::Label(ctx, { startX, curY + 4.0f }, "开启风场动画:", theme.Text, theme.FontSizeSmall);
		if (Wui::Checkbox(ctx, Wui::HashId("foliage.comp.enable_wind"), { startX + 110.0f, curY, 80.0f, 22.0f }, "启用", enableWind, theme))
		{
			comp.EnableWind = enableWind ? 1 : 0;
			host.MarkDocumentDirty();
		}
		curY += 32.0f;

		// 视距与消隐优化
		Wui::Label(ctx, { startX, curY + 4.0f }, "视距与渲染优化 (Culling & Fade)", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "最大视距剔除 (m):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("foliage.comp.max_cull"), { startX + 120.0f, curY, contentW - 120.0f, 22.0f }, comp.MaxCullDistance, 5.0f, 10.0f, 1000.0f, theme))
		{
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 120.0f, curY, contentW - 120.0f, 22.0f }, "植被相机视距完全剔除距离 (超过该距离不提交 GPU 绘制)");
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 4.0f }, "晶格消隐过渡 (m):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("foliage.comp.fade_range"), { startX + 120.0f, curY, contentW - 120.0f, 22.0f }, comp.FadeRange, 1.0f, 1.0f, 100.0f, theme))
		{
			host.MarkDocumentDirty();
		}
		Wui::Tooltip(ctx, { startX + 120.0f, curY, contentW - 120.0f, 22.0f }, "Screen-Door 晶格抖动平滑过渡区间，彻底杜绝 LOD 突变撕裂跳出");
		curY += 28.0f;

		bool castShadow = (comp.CastShadow != 0);
		Wui::Label(ctx, { startX, curY + 4.0f }, "投射阴影 (Shadow):", theme.Text, theme.FontSizeSmall);
		if (Wui::Checkbox(ctx, Wui::HashId("foliage.comp.cast_shadow"), { startX + 120.0f, curY, 80.0f, 22.0f }, "开启", castShadow, theme))
		{
			comp.CastShadow = castShadow ? 1 : 0;
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		bool receiveShadow = (comp.ReceiveShadow != 0);
		Wui::Label(ctx, { startX, curY + 4.0f }, "接收阴影 (Receive):", theme.Text, theme.FontSizeSmall);
		if (Wui::Checkbox(ctx, Wui::HashId("foliage.comp.recv_shadow"), { startX + 120.0f, curY, 80.0f, 22.0f }, "开启", receiveShadow, theme))
		{
			comp.ReceiveShadow = receiveShadow ? 1 : 0;
			host.MarkDocumentDirty();
		}
		curY += 32.0f;

		// 实例与 Chunk 拓扑统计
		const size_t totalInst = m_Foliage ? m_Foliage->GetTotalInstanceCount() : 0;
		const size_t chunkCount = m_Foliage ? m_Foliage->GetChunks().size() : 0;
		char statBuf[128] = {};
		std::snprintf(statBuf, sizeof(statBuf), "场景当前活跃实例: %zu 颗 | 空间分块: %zu Chunks", totalInst, chunkCount);
		Wui::Label(ctx, { startX, curY }, statBuf, theme.TextMuted, theme.FontSizeSmall);
		curY += 24.0f;
	}
}
