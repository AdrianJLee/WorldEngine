#include "WUI/Panels/FoliagePanel.h"
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

		// 2. 动态植物生态卡片列表
		const float itemH = 28.0f;
		for (size_t i = 0; i < typeCount; ++i)
		{
			auto& type = m_Foliage->GetType(i);
			const bool isSelected = (m_ActiveTypeIdx == static_cast<int>(i));

			// 混刷复选框 [✓] (宽 20px)
			const Wui::WuiRect chkRect { startX + 2.0f, curY + 2.0f, 20.0f, 22.0f };
			bool active = type.PaintingActive;
			if (Wui::Checkbox(ctx, Wui::HashId(("foliage.type.chk." + std::to_string(i)).c_str()), chkRect, "", active, theme))
			{
				type.PaintingActive = active;
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, chkRect, "勾选后参与视口笔刷一笔多物种自然混刷");

			// 实例数徽标 Badge (宽 52px, 紧凑显示)
			const float badgeW = 52.0f;
			const float nameBtnX = startX + 26.0f;
			const float nameBtnW = std::max(50.0f, contentW - 26.0f - badgeW - 4.0f);
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
			}

			// 实例数徽标 Badge (右对齐)
			const size_t instCount = m_Foliage->GetInstanceCountByType(i);
			char countBuf[32] = {};
			std::snprintf(countBuf, sizeof(countBuf), "[%zu颗]", instCount);
			const Wui::WuiRect badgeRect { startX + contentW - badgeW, curY + 1.0f, badgeW, 24.0f };
			Wui::Label(ctx, { badgeRect.X + 4.0f, badgeRect.Y + 5.0f }, countBuf, isSelected ? theme.Accent : theme.TextMuted, theme.FontSizeCaption);

			curY += itemH + 3.0f;
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
		curY += 26.0f;

		// 4. 上下文自适应参数检视器 (Context Inspector)
		if (m_ActiveTypeIdx >= 0 && static_cast<size_t>(m_ActiveTypeIdx) < typeCount)
		{
			auto& t = m_Foliage->GetType(static_cast<size_t>(m_ActiveTypeIdx));

			// 同步编辑缓存
			if (m_CachedTypeIdx != m_ActiveTypeIdx)
			{
				m_EditName = t.Name;
				m_EditMeshPath = t.MeshAsset.Path.IsValid() ? std::string(StringPool::Get().PathOf(t.MeshAsset.Path)) : "";
				m_EditMatPath = t.MaterialAsset.Path.IsValid() ? std::string(StringPool::Get().PathOf(t.MaterialAsset.Path)) : "";
				m_CachedTypeIdx = m_ActiveTypeIdx;
			}

			Wui::Label(ctx, { startX, curY + 4.0f }, "植物参数检视器 (Type Inspector)", theme.Accent, theme.FontSizeBody);
			curY += 26.0f;

			// 名称
			Wui::Label(ctx, { startX, curY + 4.0f }, "植物名称:", theme.Text, theme.FontSizeSmall);
			if (Wui::TextField(ctx, Wui::HashId("foliage.insp.name"), { startX + 85.0f, curY, contentW - 85.0f, 22.0f }, m_EditName, theme))
			{
				t.Name = m_EditName;
				host.MarkDocumentDirty();
			}
			curY += 28.0f;

			// 资产绑定
			Wui::Label(ctx, { startX, curY + 4.0f }, "Mesh 资产:", theme.Text, theme.FontSizeSmall);
			if (Wui::TextField(ctx, Wui::HashId("foliage.insp.mesh"), { startX + 85.0f, curY, contentW - 85.0f, 22.0f }, m_EditMeshPath, theme))
			{
				t.MeshAsset.Path = m_EditMeshPath.empty() ? PathId() : StringPool::Get().InternPath(m_EditMeshPath);
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, { startX + 85.0f, curY, contentW - 85.0f, 22.0f }, "植被三维几何网格模型资产路径 (.wmodel / .obj)");
			curY += 28.0f;

			Wui::Label(ctx, { startX, curY + 4.0f }, "材质资产:", theme.Text, theme.FontSizeSmall);
			if (Wui::TextField(ctx, Wui::HashId("foliage.insp.mat"), { startX + 85.0f, curY, contentW - 85.0f, 22.0f }, m_EditMatPath, theme))
			{
				t.MaterialAsset.Path = m_EditMatPath.empty() ? PathId() : StringPool::Get().InternPath(m_EditMatPath);
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, { startX + 85.0f, curY, contentW - 85.0f, 22.0f }, "植被渲染材质定义文件路径 (.wmaterial)");
			curY += 28.0f;

			// 散布与形态
			Wui::Label(ctx, { startX, curY + 4.0f }, "相对密度权重:", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.density"), { startX + 95.0f, curY, contentW - 95.0f, 22.0f }, t.Density, 0.1f, 0.1f, 10.0f, theme))
			{
				host.MarkDocumentDirty();
			}
			Wui::Tooltip(ctx, { startX + 95.0f, curY, contentW - 95.0f, 22.0f }, "多物种一笔混刷时分配给该物种的相对生成概率权重");
			curY += 28.0f;

			// 缩放区间 Min / Max
			const float halfFieldW = (contentW - 95.0f - theme.PadSmall) * 0.5f;
			Wui::Label(ctx, { startX, curY + 4.0f }, "随机缩放区间:", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.scale_min"), { startX + 95.0f, curY, halfFieldW, 22.0f }, t.ScaleMinMax.x, 0.05f, 0.1f, 5.0f, theme))
			{
				if (t.ScaleMinMax.x > t.ScaleMinMax.y) t.ScaleMinMax.y = t.ScaleMinMax.x;
				host.MarkDocumentDirty();
			}
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.scale_max"), { startX + 95.0f + halfFieldW + theme.PadSmall, curY, halfFieldW, 22.0f }, t.ScaleMinMax.y, 0.05f, 0.1f, 5.0f, theme))
			{
				if (t.ScaleMinMax.y < t.ScaleMinMax.x) t.ScaleMinMax.x = t.ScaleMinMax.y;
				host.MarkDocumentDirty();
			}
			curY += 28.0f;

			// 偏航与俯仰随机角
			Wui::Label(ctx, { startX, curY + 4.0f }, "随机偏航角 (°):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.yaw"), { startX + 95.0f, curY, contentW - 95.0f, 22.0f }, t.RandomYaw, 5.0f, 0.0f, 360.0f, theme))
			{
				host.MarkDocumentDirty();
			}
			curY += 28.0f;

			Wui::Label(ctx, { startX, curY + 4.0f }, "倾斜随机度 (°):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.pitch"), { startX + 95.0f, curY, contentW - 95.0f, 22.0f }, t.PitchVariation, 1.0f, 0.0f, 30.0f, theme))
			{
				host.MarkDocumentDirty();
			}
			curY += 28.0f;

			Wui::Label(ctx, { startX, curY + 4.0f }, "贴合法线旋转:", theme.Text, theme.FontSizeSmall);
			if (Wui::Checkbox(ctx, Wui::HashId("foliage.insp.align_norm"), { startX + 95.0f, curY, 80.0f, 22.0f }, "开启", t.AlignToNormal, theme))
			{
				host.MarkDocumentDirty();
			}
			curY += 28.0f;

			// 放置过滤规则 (Placement Rules)
			const float filterHalfW = (contentW - 110.0f - theme.PadSmall) * 0.5f;
			Wui::Label(ctx, { startX, curY + 4.0f }, "地表坡度限制 (°):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.slope_min"), { startX + 110.0f, curY, filterHalfW, 22.0f }, t.SlopeLimit.x, 1.0f, 0.0f, 90.0f, theme))
			{
				if (t.SlopeLimit.x > t.SlopeLimit.y) t.SlopeLimit.y = t.SlopeLimit.x;
				host.MarkDocumentDirty();
			}
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.slope_max"), { startX + 110.0f + filterHalfW + theme.PadSmall, curY, filterHalfW, 22.0f }, t.SlopeLimit.y, 1.0f, 0.0f, 90.0f, theme))
			{
				if (t.SlopeLimit.y < t.SlopeLimit.x) t.SlopeLimit.x = t.SlopeLimit.y;
				host.MarkDocumentDirty();
			}
			curY += 28.0f;

			Wui::Label(ctx, { startX, curY + 4.0f }, "世界高程区间 (m):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.height_min"), { startX + 110.0f, curY, filterHalfW, 22.0f }, t.HeightLimit.x, 5.0f, -1000.0f, 1000.0f, theme))
			{
				if (t.HeightLimit.x > t.HeightLimit.y) t.HeightLimit.y = t.HeightLimit.x;
				host.MarkDocumentDirty();
			}
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.height_max"), { startX + 110.0f + filterHalfW + theme.PadSmall, curY, filterHalfW, 22.0f }, t.HeightLimit.y, 5.0f, -1000.0f, 1000.0f, theme))
			{
				if (t.HeightLimit.y < t.HeightLimit.x) t.HeightLimit.x = t.HeightLimit.y;
				host.MarkDocumentDirty();
			}
			curY += 28.0f;

			// 地表材质层掩码 (LayerMask 4 个复选框)
			Wui::Label(ctx, { startX, curY + 4.0f }, "地表材质过滤:", theme.Text, theme.FontSizeSmall);
			curY += 22.0f;

			const float layerChkW = (contentW - theme.PadSmall * 3.0f) * 0.25f;
			bool grassOn = (t.LayerMask & 0x01) != 0;
			bool dirtOn  = (t.LayerMask & 0x02) != 0;
			bool rockOn  = (t.LayerMask & 0x04) != 0;
			bool sandOn  = (t.LayerMask & 0x08) != 0;

			if (Wui::Checkbox(ctx, Wui::HashId("foliage.layer.grass"), { startX, curY, layerChkW, 22.0f }, "草地", grassOn, theme))
			{
				t.LayerMask = grassOn ? (t.LayerMask | 0x01) : (t.LayerMask & ~0x01);
				host.MarkDocumentDirty();
			}
			if (Wui::Checkbox(ctx, Wui::HashId("foliage.layer.dirt"), { startX + layerChkW + theme.PadSmall, curY, layerChkW, 22.0f }, "泥土", dirtOn, theme))
			{
				t.LayerMask = dirtOn ? (t.LayerMask | 0x02) : (t.LayerMask & ~0x02);
				host.MarkDocumentDirty();
			}
			if (Wui::Checkbox(ctx, Wui::HashId("foliage.layer.rock"), { startX + (layerChkW + theme.PadSmall) * 2.0f, curY, layerChkW, 22.0f }, "岩石", rockOn, theme))
			{
				t.LayerMask = rockOn ? (t.LayerMask | 0x04) : (t.LayerMask & ~0x04);
				host.MarkDocumentDirty();
			}
			if (Wui::Checkbox(ctx, Wui::HashId("foliage.layer.sand"), { startX + (layerChkW + theme.PadSmall) * 3.0f, curY, layerChkW, 22.0f }, "沙地", sandOn, theme))
			{
				t.LayerMask = sandOn ? (t.LayerMask | 0x08) : (t.LayerMask & ~0x08);
				host.MarkDocumentDirty();
			}
			curY += 28.0f;

			// 独立视距与风力
			Wui::Label(ctx, { startX, curY + 4.0f }, "独立剔除视距 (m):", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.cull_dist"), { startX + 115.0f, curY, contentW - 115.0f, 22.0f }, t.CullDistance, 5.0f, 20.0f, 500.0f, theme))
			{
				host.MarkDocumentDirty();
			}
			curY += 28.0f;

			Wui::Label(ctx, { startX, curY + 4.0f }, "风力摆动乘数:", theme.Text, theme.FontSizeSmall);
			if (Wui::DragFloat(ctx, Wui::HashId("foliage.insp.wind_mult"), { startX + 115.0f, curY, contentW - 115.0f, 22.0f }, t.WindMultiplier, 0.05f, 0.0f, 3.0f, theme))
			{
				host.MarkDocumentDirty();
			}
			curY += 32.0f;
		}

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
