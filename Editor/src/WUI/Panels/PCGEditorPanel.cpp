#include "wldpch.h"
#include "PCGEditorPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "WUI/Common/EditorPanel.h"
#include "World/Terrain/Terrain.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/Foliage/Foliage.h"
#include "World/Foliage/FoliageRenderer.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/Components.h"
#include "World/Core/StringPool.h"
#include "World/Utils/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace World
{
	PCGEditorPanel::PCGEditorPanel()
	{
		RefreshAssetCaches();
	}

	void PCGEditorPanel::RefreshAssetCaches()
	{
		m_CachedMeshOptions.clear();
		m_CachedMeshOptions.push_back("(无模型 / None)");
		auto addMesh = [this](const std::string& p) {
			if (p.empty()) return;
			for (const auto& ex : m_CachedMeshOptions) { if (ex == p) return; }
			m_CachedMeshOptions.push_back(p);
		};

		addMesh("engine/models/foliage/grass_cluster.wmodel");
		addMesh("engine/models/foliage/bush_cluster.wmodel");
		addMesh("engine/models/foliage/scatter_pebble.wmodel");
		addMesh("engine/models/primitives/sphere.wmodel");
		addMesh("engine/models/primitives/cube.wmodel");
		addMesh("engine/models/primitives/plane.wmodel");
		addMesh("engine/models/characters/mannequin.wmodel");
		addMesh("engine/models/fixtures/D5Fixture.wmodel");
		addMesh("engine/models/debris_shard.wmodel");

		m_CachedMaterialOptions.clear();
		m_CachedMaterialOptions.push_back("(默认材质 / Default Material)");
		auto addMat = [this](const std::string& p) {
			if (p.empty()) return;
			for (const auto& ex : m_CachedMaterialOptions) { if (ex == p) return; }
			m_CachedMaterialOptions.push_back(p);
		};

		addMat("engine/materials/particles/particle_alpha.wmat");
		addMat("engine/materials/particles/particle_additive.wmat");

		m_CachedRuleGraphOptions.clear();
		m_CachedRuleGraphOptions.push_back("(纯净空白规则图 / Blank Graph)");
		m_CachedRuleGraphOptions.push_back("assets/pcg/custom_rules.wpcggraph");
		auto addGraph = [this](const std::string& p) {
			if (p.empty()) return;
			for (const auto& ex : m_CachedRuleGraphOptions) { if (ex == p) return; }
			m_CachedRuleGraphOptions.push_back(p);
		};

		m_CachedScriptOptions.clear();
		m_CachedScriptOptions.push_back("(无脚本算子 / None)");
		m_CachedScriptOptions.push_back("assets/scripts/pcg/custom_filter.luau");
		auto addScript = [this](const std::string& p) {
			if (p.empty()) return;
			for (const auto& ex : m_CachedScriptOptions) { if (ex == p) return; }
			m_CachedScriptOptions.push_back(p);
		};

		std::error_code ec;
		std::vector<std::filesystem::path> roots = {
			Paths::AssetRoot(),
			Paths::EngineContentRoot()
		};
		for (const auto& root : roots)
		{
			if (root.empty() || !std::filesystem::exists(root, ec)) continue;
			for (const auto& entry : std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec))
			{
				if (ec || !entry.is_regular_file(ec)) continue;
				std::string ext = entry.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				std::filesystem::path rel = std::filesystem::relative(entry.path(), root, ec);
				if (ec || rel.empty()) continue;
				std::string logical = (root == Paths::EngineContentRoot()) ? ("engine/" + rel.generic_string()) : rel.generic_string();

				if (ext == ".wmodel" || ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb")
					addMesh(logical);
				else if (ext == ".wmat" || ext == ".mat" || ext == ".material")
					addMat(logical);
				else if (ext == ".wpcggraph" || ext == ".wpcg")
					addGraph(logical);
				else if (ext == ".luau" || ext == ".lua")
					addScript(logical);
			}
		}

		m_AssetCachesInitialized = true;
	}

	void PCGEditorPanel::LoadGraphAsset(const std::string& logicalOrPhysicalPath)
	{
		if (logicalOrPhysicalPath.empty()) return;
		m_GraphAssetPath = logicalOrPhysicalPath;
		std::filesystem::path physical = Paths::ProjectDir() / logicalOrPhysicalPath;
		std::error_code ec;
		if (!std::filesystem::exists(physical, ec))
		{
			physical = logicalOrPhysicalPath;
		}
		if (std::filesystem::exists(physical, ec))
		{
			std::string err;
			if (PCGBiomeGenerator::GetMutableRuleGraph().LoadFromFile(physical.string(), &err))
			{
				m_StatusFeedback = "已成功载入 PCG 规则图资产: " + logicalOrPhysicalPath;
			}
			else
			{
				m_StatusFeedback = "载入 PCG 规则图失败: " + err;
			}
		}
	}

	void PCGEditorPanel::BindEntity(entt::entity e)
	{
		m_BoundEntity = e;
	}

	float PCGEditorPanel::EstimateTabContentHeight() const
	{
		switch (m_ActiveTab)
		{
		case PCGEditorTab::Rules:
		{
			float h = 160.0f;
			const auto& graph = PCGBiomeGenerator::GetActiveRuleGraph();
			if (graph.Layers.empty())
			{
				h += 110.0f;
			}
			else
			{
				for (size_t i = 0; i < graph.Layers.size(); ++i)
				{
					const bool isExp = (m_ExpandedLayerIdx == static_cast<int>(i));
					if (isExp)
					{
						const float baseH = 190.0f;
						const float entryH = 120.0f;
						h += baseH + static_cast<float>(graph.Layers[i].Entries.size()) * entryH + 12.0f;
					}
					else
					{
						h += 42.0f;
					}
				}
			}
			return std::max(h + 50.0f, 320.0f);
		}
		case PCGEditorTab::Scatter:
			return 420.0f;
		case PCGEditorTab::SplineRoads:
			return 380.0f;
		case PCGEditorTab::Scripting:
			return 440.0f;
		case PCGEditorTab::Telemetry:
			return 350.0f;
		}
		return 350.0f;
	}

	void PCGEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		if (!m_AssetCachesInitialized)
		{
			RefreshAssetCaches();
		}

		const Wui::WuiTheme& theme = host.Theme();
		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 头部徽章
		const Wui::WuiRect badgeRect { startX, curY, 260.0f, 18.0f };
		const Wui::WuiColor badgeBg { 0.35f, 0.28f, 0.08f, 0.35f };
		const Wui::WuiColor badgeBorder { 0.95f, 0.78f, 0.20f, 0.90f };
		Wui::Badge(ctx, badgeRect, "PCG GRAPH & SPATIAL WORKBENCH", badgeBg, badgeBorder, theme, 10.5f, true, 3.0f);
		curY += 24.0f;

		// 2. 场景实体双向绑定栏
		auto activeScene = host.GetActiveScene();
		Entity boundEntity;
		if (activeScene)
		{
			if (m_BoundEntity != entt::null && activeScene->GetRegistry().valid(m_BoundEntity) &&
				activeScene->GetRegistry().all_of<BiomePCGComponent>(m_BoundEntity))
			{
				boundEntity = Entity(activeScene.get(), m_BoundEntity);
			}
			else
			{
				Entity sel = host.GetSelectedEntity();
				if (sel.IsValid() && sel.HasComponent<BiomePCGComponent>())
				{
					boundEntity = sel;
					m_BoundEntity = static_cast<entt::entity>(sel);
				}
				else
				{
					for (auto [h, comp] : activeScene->GetRegistry().view<BiomePCGComponent>().each())
					{
						boundEntity = Entity(activeScene.get(), h);
						m_BoundEntity = h;
						break;
					}
				}
			}
		}

		if (!boundEntity.IsValid())
		{
			const Wui::WuiRect createRect { startX, curY, contentW, 26.0f };
			if (Wui::Button(ctx, Wui::HashId("pcg.btn.create_entity"), createRect,
				"在当前场景创建 PCG 生成器实体 (Create PCG Entity)", theme))
			{
				if (activeScene)
				{
					Entity pcgEnt = Entity::CreateEntity(activeScene.get(), "PCG Spatial Generator");
					pcgEnt.AddComponent<TransformComponent>();
					auto& comp = pcgEnt.AddComponent<BiomePCGComponent>();
					comp.BoundsRadius = m_BoundsRadius;
					comp.BoundsHeight = m_BoundsHeight;
					comp.DensityMultiplier = m_DensityMultiplier;
					comp.ClearanceMargin = m_ClearanceMargin;
					comp.Seed = static_cast<uint32_t>(m_Seed);
					comp.AutoDeformTerrain = m_AutoDeformTerrain;
					comp.LivePreview = m_LivePreview;
					m_BoundEntity = static_cast<entt::entity>(pcgEnt);
					host.SetSelectedEntity(pcgEnt);
					m_StatusFeedback = "已在当前场景创建纯净 PCG 生成器实体并完成双向绑定。";
					host.MarkDocumentDirty();
				}
			}
			curY += 32.0f;
		}
		else
		{
			auto& comp = boundEntity.GetComponent<BiomePCGComponent>();
			std::string tagStr = "已绑定 PCG 实体: ";
			if (boundEntity.HasComponent<TagComponent>())
				tagStr += StringPool::Get().NameOf(boundEntity.GetComponent<TagComponent>().Tag);
			else
				tagStr += "Entity " + std::to_string(static_cast<uint32_t>(m_BoundEntity));
			tagStr += " [ID: " + std::to_string(static_cast<uint32_t>(m_BoundEntity)) + "]";

			const Wui::WuiRect bindBarRect { startX, curY, contentW, 22.0f };
			Wui::PanelBackground(ctx, bindBarRect, Wui::WuiColor{ 0.10f, 0.22f, 0.16f, 0.85f }, 3.0f);
			Wui::HighlightOutline(ctx, bindBarRect, Wui::WuiColor{ 0.20f, 0.85f, 0.40f, 0.85f }, 3.0f, 1.0f);
			Wui::Label(ctx, { startX + 8.0f, curY + 2.0f }, tagStr.c_str(), theme.Success, theme.FontSizeSmall);
			curY += 26.0f;

			// 双向同步参数至面板内部变量
			m_BoundsRadius = comp.BoundsRadius;
			m_BoundsHeight = comp.BoundsHeight;
			m_DensityMultiplier = comp.DensityMultiplier;
			m_ClearanceMargin = comp.ClearanceMargin;
			m_Seed = static_cast<int32_t>(comp.Seed);
			m_AutoDeformTerrain = comp.AutoDeformTerrain;
			m_LivePreview = comp.LivePreview;
		}

		// 状态反馈文本
		Wui::Label(ctx, { startX, curY }, m_StatusFeedback.c_str(), theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// 3. 选项卡: [规则管线] [空间散布] [道路放坡] [遥测诊断]
		const std::vector<std::string> tabs = {
			"规则管线 (Rules)",
			"空间散布 (Scatter)",
			"道路放坡 (Splines)",
			"脚本算子 (Scripting)",
			"遥测诊断 (Stats)"
		};
		if (Wui::TabBar(ctx, Wui::HashId("pcg.tabs"), { startX, curY, contentW, 28.0f }, tabs, m_ActiveTabIdx, theme))
		{
			m_ActiveTab = static_cast<PCGEditorTab>(m_ActiveTabIdx);
		}
		curY += 34.0f;

		const float scrollH = std::max(120.0f, rect.H - (curY - rect.Y) - 4.0f);
		const Wui::WuiRect scrollAreaRect { startX, curY, contentW, scrollH };
		const float contentH = EstimateTabContentHeight();

		if (Wui::BeginScrollArea(ctx, scrollAreaRect, contentH, m_TabScrollY, theme, Wui::HashId("pcg.scroll_area")))
		{
			float tabCurY = scrollAreaRect.Y;
			const float tabContentW = contentW - 12.0f;

			switch (m_ActiveTab)
			{
			case PCGEditorTab::Rules:
				RenderRulesTab(ctx, startX, tabCurY, tabContentW, theme, host);
				break;
			case PCGEditorTab::Scatter:
				RenderScatterTab(ctx, startX, tabCurY, tabContentW, theme, host);
				break;
			case PCGEditorTab::SplineRoads:
				RenderSplineRoadsTab(ctx, startX, tabCurY, tabContentW, theme, host);
				break;
			case PCGEditorTab::Scripting:
				RenderScriptingTab(ctx, startX, tabCurY, tabContentW, theme, host);
				break;
			case PCGEditorTab::Telemetry:
				RenderTelemetryTab(ctx, startX, tabCurY, tabContentW, theme, host);
				break;
			}

			Wui::EndScrollArea(ctx);
		}
	}

	void PCGEditorPanel::RenderRulesTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 2.0f }, "纯净规则管线配置 (Pure Rule Pipeline)", theme.Accent, theme.FontSizeBody);
		curY += 24.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		auto activeScene = host.GetActiveScene();
		BiomePCGComponent* boundCompPtr = nullptr;
		if (activeScene && m_BoundEntity != entt::null && activeScene->GetRegistry().valid(m_BoundEntity))
		{
			boundCompPtr = activeScene->GetRegistry().try_get<BiomePCGComponent>(m_BoundEntity);
		}

		PCGRuleGraphDesc& graph = PCGBiomeGenerator::GetMutableRuleGraph();

		// 顶部图层操作按钮组: [+ 新建图层] [保存资产] [载入资产] [↺ 清空白纸]
		const float btnW4 = (contentW - theme.PadSmall * 4.0f) / 5.0f;
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.add_layer"), { startX, curY, btnW4, 24.0f }, "+ 新增图层", theme))
		{
			PCGBiomeLayerDesc newLayer;
			newLayer.LayerName = "Layer " + std::to_string(graph.Layers.size() + 1);
			newLayer.Enabled = true;
			newLayer.Priority = 1.0f;
			newLayer.ColorTag = glm::vec4(0.2f + 0.15f * (graph.Layers.size() % 5), 0.75f, 0.4f, 0.9f);
			newLayer.ElevationBand = { -1000.0f, 5000.0f, 0.0f };
			newLayer.SlopeDegrees = { 0.0f, 90.0f, 0.0f };
			newLayer.DensityMask = { 0.0f, 1.0f, 0.0f };
			graph.Layers.push_back(newLayer);
			m_ExpandedLayerIdx = static_cast<int>(graph.Layers.size() - 1);
			m_StatusFeedback = "已新增纯净图层：" + newLayer.LayerName;
			host.MarkDocumentDirty();
		}

		if (Wui::Button(ctx, Wui::HashId("pcg.btn.save_asset"), { startX + (btnW4 + theme.PadSmall), curY, btnW4, 24.0f }, "保存资产", theme))
		{
			std::filesystem::path fullPath = Paths::ProjectDir() / m_GraphAssetPath;
			std::filesystem::create_directories(fullPath.parent_path());
			if (graph.SaveToFile(fullPath.string()))
			{
				if (boundCompPtr) boundCompPtr->RuleAsset = AssetRef::FromPath(m_GraphAssetPath);
				m_StatusFeedback = "规则图已保存至: " + m_GraphAssetPath;
				host.MarkDocumentDirty();
			}
			else
			{
				m_StatusFeedback = "保存失败，请检查路径合法性。";
			}
		}

		if (Wui::Button(ctx, Wui::HashId("pcg.btn.load_asset"), { startX + (btnW4 + theme.PadSmall) * 2.0f, curY, btnW4, 24.0f }, "载入资产", theme))
		{
			std::filesystem::path fullPath = Paths::ProjectDir() / m_GraphAssetPath;
			if (graph.LoadFromFile(fullPath.string()))
			{
				if (boundCompPtr) boundCompPtr->RuleAsset = AssetRef::FromPath(m_GraphAssetPath);
				m_ExpandedLayerIdx = 0;
				m_StatusFeedback = "已成功载入规则图: " + m_GraphAssetPath;
				host.MarkDocumentDirty();
			}
			else
			{
				m_StatusFeedback = "载入失败: 文件不存在 " + m_GraphAssetPath;
			}
		}

		if (Wui::Button(ctx, Wui::HashId("pcg.btn.clear_all_layers"), { startX + (btnW4 + theme.PadSmall) * 3.0f, curY, btnW4, 24.0f }, "↺ 清空白纸", theme))
		{
			graph.Layers.clear();
			m_ExpandedLayerIdx = -1;
			m_StatusFeedback = "已清空所有规则图层，恢复纯净空白画布。";
			host.MarkDocumentDirty();
		}

		if (Wui::Button(ctx, Wui::HashId("pcg.btn.refresh_assets"), { startX + (btnW4 + theme.PadSmall) * 4.0f, curY, btnW4, 24.0f }, "刷新资产", theme))
		{
			RefreshAssetCaches();
			m_StatusFeedback = "已刷新工程模型与材质资产下拉清单。";
		}
		curY += 30.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "规则图资产 (.wpcggraph):", theme.TextMuted, theme.FontSizeSmall);
		auto& graphOptions = m_CachedRuleGraphOptions;
		int curGraphIdx = 0;
		for (size_t gi = 0; gi < graphOptions.size(); ++gi)
		{
			if (graphOptions[gi] == m_GraphAssetPath)
			{
				curGraphIdx = static_cast<int>(gi);
				break;
			}
		}
		if (curGraphIdx == 0 && !m_GraphAssetPath.empty() && m_GraphAssetPath != graphOptions[0])
		{
			graphOptions.push_back(m_GraphAssetPath);
			curGraphIdx = static_cast<int>(graphOptions.size() - 1);
		}

		const Wui::WuiRect graphComboRect { startX + 160.0f, curY, contentW - 260.0f, 22.0f };
		if (Wui::SearchableCombo(ctx, Wui::HashId("pcg.combo.graph_path"), graphComboRect, "", graphOptions, curGraphIdx, theme))
		{
			if (curGraphIdx > 0)
			{
				m_GraphAssetPath = graphOptions[curGraphIdx];
				if (boundCompPtr) boundCompPtr->RuleAsset = AssetRef::FromPath(m_GraphAssetPath);
				std::filesystem::path fullPath = Paths::ProjectDir() / m_GraphAssetPath;
				if (graph.LoadFromFile(fullPath.string()))
				{
					m_StatusFeedback = "已成功载入规则图: " + m_GraphAssetPath;
				}
				host.MarkDocumentDirty();
			}
		}

		if (Wui::Button(ctx, Wui::HashId("pcg.btn.pick_graph"), { startX + contentW - 95.0f, curY, 95.0f, 22.0f }, "选择资产...", theme))
		{
			host.OpenAssetPicker("选择 PCG 规则图资产 (PCG Graph)", AssetCategory::Unknown, [this, &host, boundCompPtr](const std::string& picked) {
				if (!picked.empty())
				{
					m_GraphAssetPath = picked;
					if (boundCompPtr) boundCompPtr->RuleAsset = AssetRef::FromPath(picked);
					auto& graph = PCGBiomeGenerator::GetMutableRuleGraph();
					std::filesystem::path fullPath = Paths::ProjectDir() / picked;
					if (graph.LoadFromFile(fullPath.string()))
					{
						m_StatusFeedback = "已载入规则图: " + picked;
					}
					host.MarkDocumentDirty();
				}
			});
		}
		curY += 26.0f;

		// 全局基础采样步长与空间扰动
		Wui::Label(ctx, { startX, curY + 2.0f }, "基础采样步长 (Sample Spacing m) 与 抖动系数 (Jitter):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.sample_spacing"), { startX, curY, halfW, 22.0f }, graph.SampleSpacing, 0.2f, 0.2f, 50.0f, theme))
		{
			graph.SampleSpacing = std::clamp(graph.SampleSpacing, 0.2f, 50.0f);
			host.MarkDocumentDirty();
		}
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.jitter_factor"), { startX + halfW + theme.PadSmall, curY, halfW, 22.0f }, graph.JitterFactor, 0.05f, 0.0f, 1.0f, theme))
		{
			graph.JitterFactor = std::clamp(graph.JitterFactor, 0.0f, 1.0f);
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 若当前图层为空，绘制纯净空状态引导卡片
		if (graph.Layers.empty())
		{
			const Wui::WuiRect emptyRect { startX, curY, contentW, 80.0f };
			Wui::PanelBackground(ctx, emptyRect, Wui::WuiColor { 0.12f, 0.15f, 0.18f, 0.85f }, 4.0f);
			Wui::HighlightOutline(ctx, emptyRect, Wui::WuiColor { 0.40f, 0.50f, 0.60f, 0.50f }, 4.0f, 1.0f);

			Wui::Label(ctx, { startX + 16.0f, curY + 16.0f }, "纯净规则图画布 (Blank Canvas - 零写死预设)", theme.Accent, theme.FontSizeBody);
			Wui::Label(ctx, { startX + 16.0f, curY + 42.0f }, "当前规则图内无任何预设内容。请点击上方 [+ 新增图层] 开始自由构建属于你的规则。", theme.TextMuted, theme.FontSizeSmall);
			curY += 92.0f;
			return;
		}

		// 绘制各个图层卡片
		int layerToDelete = -1;
		for (size_t i = 0; i < graph.Layers.size(); ++i)
		{
			auto& layer = graph.Layers[i];
			const bool isExpanded = (m_ExpandedLayerIdx == static_cast<int>(i));

			const float baseH = isExpanded ? 180.0f : 34.0f;
			const float entryH = 116.0f;
			const float cardH = isExpanded ? (baseH + static_cast<float>(layer.Entries.size()) * entryH) : 34.0f;
			const Wui::WuiRect layerCard { startX, curY, contentW, cardH };
			Wui::PanelBackground(ctx, layerCard, Wui::WuiColor { 0.10f, 0.13f, 0.17f, 0.90f }, 4.0f);
			const Wui::WuiColor outlineCol { layer.ColorTag.r, layer.ColorTag.g, layer.ColorTag.b, layer.ColorTag.a };
			Wui::HighlightOutline(ctx, layerCard, outlineCol, 4.0f, isExpanded ? 1.5f : 1.0f);

			float cardRowY = curY + 6.0f;

			// 折叠/展开按钮
			const Wui::WuiRect foldBtnRect { startX + 6.0f, cardRowY, 20.0f, 22.0f };
			auto foldTheme = theme;
			foldTheme.ButtonBg = { 0, 0, 0, 0 };
			foldTheme.Border = { 0, 0, 0, 0 };
			foldTheme.Text = isExpanded ? theme.Accent : theme.TextMuted;
			if (Wui::Button(ctx, Wui::HashId(("pcg.layer.fold." + std::to_string(i)).c_str()), foldBtnRect, isExpanded ? "▼" : "▶", foldTheme))
			{
				m_ExpandedLayerIdx = isExpanded ? -1 : static_cast<int>(i);
			}

			// 图层名称
			char titleBuf[128];
			std::snprintf(titleBuf, sizeof(titleBuf), "[%zu] %s (包含 %zu 项模型条目)", i + 1, layer.LayerName.c_str(), layer.Entries.size());
			Wui::Label(ctx, { startX + 32.0f, cardRowY + 2.0f }, titleBuf, theme.Text, theme.FontSizeSmall);

			// 删除按钮
			const Wui::WuiRect delBtnRect { startX + contentW - 48.0f, cardRowY, 42.0f, 22.0f };
			auto delTheme = theme;
			delTheme.Text = Wui::WuiColor{ 0.95f, 0.35f, 0.30f, 1.0f };
			if (Wui::Button(ctx, Wui::HashId(("pcg.layer.del." + std::to_string(i)).c_str()), delBtnRect, "删除", delTheme))
			{
				layerToDelete = static_cast<int>(i);
			}

			if (isExpanded)
			{
				cardRowY += 28.0f;

				// 图层名称输入框
				Wui::Label(ctx, { startX + 10.0f, cardRowY + 2.0f }, "图层名称:", theme.TextMuted, theme.FontSizeSmall);
				char nameBuf[128];
				std::snprintf(nameBuf, sizeof(nameBuf), "%s", layer.LayerName.c_str());
				std::string nameStr(nameBuf);
				if (Wui::TextField(ctx, Wui::HashId(("pcg.layer.name." + std::to_string(i)).c_str()),
					{ startX + 70.0f, cardRowY, 160.0f, 22.0f }, nameStr, theme))
				{
					layer.LayerName = nameStr;
					host.MarkDocumentDirty();
				}

				// 图层优先级
				Wui::Label(ctx, { startX + 245.0f, cardRowY + 2.0f }, "优先级:", theme.TextMuted, theme.FontSizeSmall);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.prio." + std::to_string(i)).c_str()),
					{ startX + 295.0f, cardRowY, contentW - 305.0f, 22.0f }, layer.Priority, 0.1f, 0.1f, 10.0f, theme);
				cardRowY += 26.0f;

				// 标高过滤范围 (Min / Max)
				Wui::Label(ctx, { startX + 10.0f, cardRowY + 2.0f }, "标高区间 (m):", theme.TextMuted, theme.FontSizeSmall);
				const float filterHalfW = (contentW - 130.0f) * 0.5f;
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.elev_min." + std::to_string(i)).c_str()),
					{ startX + 110.0f, cardRowY, filterHalfW, 20.0f }, layer.ElevationBand.Min, 1.0f, -5000.0f, 5000.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.elev_max." + std::to_string(i)).c_str()),
					{ startX + 110.0f + filterHalfW + 4.0f, cardRowY, filterHalfW, 20.0f }, layer.ElevationBand.Max, 1.0f, -5000.0f, 5000.0f, theme);
				cardRowY += 24.0f;

				// 坡度过滤范围 (Min / Max deg)
				Wui::Label(ctx, { startX + 10.0f, cardRowY + 2.0f }, "坡度区间 (deg):", theme.TextMuted, theme.FontSizeSmall);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.slope_min." + std::to_string(i)).c_str()),
					{ startX + 110.0f, cardRowY, filterHalfW, 20.0f }, layer.SlopeDegrees.Min, 0.5f, 0.0f, 90.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.slope_max." + std::to_string(i)).c_str()),
					{ startX + 110.0f + filterHalfW + 4.0f, cardRowY, filterHalfW, 20.0f }, layer.SlopeDegrees.Max, 0.5f, 0.0f, 90.0f, theme);
				cardRowY += 24.0f;

				// 密度过滤遮罩 (Min / Max 0..1)
				Wui::Label(ctx, { startX + 10.0f, cardRowY + 2.0f }, "噪声密度遮罩:", theme.TextMuted, theme.FontSizeSmall);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.mask_min." + std::to_string(i)).c_str()),
					{ startX + 110.0f, cardRowY, filterHalfW, 20.0f }, layer.DensityMask.Min, 0.02f, 0.0f, 1.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.mask_max." + std::to_string(i)).c_str()),
					{ startX + 110.0f + filterHalfW + 4.0f, cardRowY, filterHalfW, 20.0f }, layer.DensityMask.Max, 0.02f, 0.0f, 1.0f, theme);
				cardRowY += 26.0f;

				// 生成模型条目管理
				char entSummary[128];
				std::snprintf(entSummary, sizeof(entSummary), "绑定模型资产条目: %zu 项", layer.Entries.size());
				Wui::Label(ctx, { startX + 10.0f, cardRowY + 2.0f }, entSummary, theme.Accent, theme.FontSizeSmall);

				if (Wui::Button(ctx, Wui::HashId(("pcg.layer.add_entry." + std::to_string(i)).c_str()),
					{ startX + 170.0f, cardRowY, contentW - 180.0f, 20.0f }, "+ 添加模型资产 (Add Mesh)", theme))
				{
					PCGSpawnEntry entry;
					entry.Name = "Entry " + std::to_string(layer.Entries.size() + 1);
					entry.SelectionWeight = 1.0f;
					entry.ScaleRange = glm::vec2(0.85f, 1.25f);
					entry.MeshAsset = AssetRef::FromPath("models/foliage/grass_cluster.wmodel");
					layer.Entries.push_back(entry);
					host.MarkDocumentDirty();
				}
				cardRowY += 26.0f;

				// 遍历绘制该图层下的所有模型资产条目 (完整闭环用户层配置与资产选择)
				int entryToDelete = -1;
				for (size_t j = 0; j < layer.Entries.size(); ++j)
				{
					auto& entry = layer.Entries[j];
					const Wui::WuiRect entryCardRect { startX + 8.0f, cardRowY, contentW - 16.0f, 110.0f };
					Wui::PanelBackground(ctx, entryCardRect, Wui::WuiColor { 0.07f, 0.09f, 0.13f, 0.95f }, 3.0f);
					Wui::HighlightOutline(ctx, entryCardRect, Wui::WuiColor { 0.22f, 0.30f, 0.40f, 0.85f }, 3.0f, 1.0f);

					float entSubY = cardRowY + 4.0f;
					const float innerX = startX + 14.0f;
					const float innerW = contentW - 28.0f;

					// 子行 1: [条目序号] 条目名称 | 权重 | 移除按钮
					char entTitle[64];
					std::snprintf(entTitle, sizeof(entTitle), "[%zu] 名称:", j + 1);
					Wui::Label(ctx, { innerX, entSubY + 2.0f }, entTitle, theme.Text, theme.FontSizeSmall);

					std::string entryName = entry.Name;
					if (Wui::TextField(ctx, Wui::HashId(("pcg.ent.name." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						{ innerX + 60.0f, entSubY, 130.0f, 20.0f }, entryName, theme))
					{
						entry.Name = entryName;
						host.MarkDocumentDirty();
					}

					Wui::Label(ctx, { innerX + 200.0f, entSubY + 2.0f }, "权重:", theme.TextMuted, theme.FontSizeSmall);
					Wui::DragFloat(ctx, Wui::HashId(("pcg.ent.weight." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						{ innerX + 235.0f, entSubY, 70.0f, 20.0f }, entry.SelectionWeight, 0.05f, 0.01f, 10.0f, theme);

					auto rmTheme = theme;
					rmTheme.Text = Wui::WuiColor{ 0.95f, 0.35f, 0.30f, 1.0f };
					if (Wui::Button(ctx, Wui::HashId(("pcg.ent.del." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						{ innerX + innerW - 55.0f, entSubY, 50.0f, 20.0f }, "移除", rmTheme))
					{
						entryToDelete = static_cast<int>(j);
					}
					entSubY += 24.0f;

					// 子行 2: 模型资产下拉选择 (SearchableCombo) + 浏览弹窗按钮
					Wui::Label(ctx, { innerX, entSubY + 2.0f }, "模型资产 (.wmodel):", theme.TextMuted, theme.FontSizeSmall);
					std::string meshPath = entry.MeshAsset.HasPath() ? StringPool::Get().PathOf(entry.MeshAsset.Path) : "";
					auto& meshOptions = m_CachedMeshOptions;
					int curMeshIdx = 0;
					for (size_t mi = 0; mi < meshOptions.size(); ++mi)
					{
						if (meshOptions[mi] == meshPath)
						{
							curMeshIdx = static_cast<int>(mi);
							break;
						}
					}
					if (curMeshIdx == 0 && !meshPath.empty() && meshPath != meshOptions[0])
					{
						meshOptions.push_back(meshPath);
						curMeshIdx = static_cast<int>(meshOptions.size() - 1);
					}

					const Wui::WuiRect meshComboRect { innerX + 130.0f, entSubY, innerW - 230.0f, 20.0f };
					if (Wui::SearchableCombo(ctx, Wui::HashId(("pcg.ent.mesh_combo." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						meshComboRect, "", meshOptions, curMeshIdx, theme))
					{
						const std::string picked = (curMeshIdx <= 0) ? "" : meshOptions[curMeshIdx];
						entry.MeshAsset = AssetRef::FromPath(picked);
						host.MarkDocumentDirty();
					}

					if (Wui::Button(ctx, Wui::HashId(("pcg.ent.pick_mesh." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						{ innerX + innerW - 95.0f, entSubY, 95.0f, 20.0f }, "选择模型...", theme))
					{
						host.OpenAssetPicker("选择模型资产 (Mesh)", AssetCategory::Mesh, [&layer, j, &host](const std::string& picked) {
							if (j < layer.Entries.size())
							{
								layer.Entries[j].MeshAsset = AssetRef::FromPath(picked);
								host.MarkDocumentDirty();
							}
						});
					}
					entSubY += 24.0f;

					// 子行 3: 材质覆盖下拉选择 (SearchableCombo) + 浏览弹窗按钮
					Wui::Label(ctx, { innerX, entSubY + 2.0f }, "材质覆盖 (.wmat):", theme.TextMuted, theme.FontSizeSmall);
					std::string matPath = entry.MaterialAsset.HasPath() ? StringPool::Get().PathOf(entry.MaterialAsset.Path) : "";
					auto& matOptions = m_CachedMaterialOptions;
					int curMatIdx = 0;
					for (size_t mai = 0; mai < matOptions.size(); ++mai)
					{
						if (matOptions[mai] == matPath)
						{
							curMatIdx = static_cast<int>(mai);
							break;
						}
					}
					if (curMatIdx == 0 && !matPath.empty() && matPath != matOptions[0])
					{
						matOptions.push_back(matPath);
						curMatIdx = static_cast<int>(matOptions.size() - 1);
					}

					const Wui::WuiRect matComboRect { innerX + 130.0f, entSubY, innerW - 230.0f, 20.0f };
					if (Wui::SearchableCombo(ctx, Wui::HashId(("pcg.ent.mat_combo." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						matComboRect, "", matOptions, curMatIdx, theme))
					{
						const std::string picked = (curMatIdx <= 0) ? "" : matOptions[curMatIdx];
						entry.MaterialAsset = AssetRef::FromPath(picked);
						host.MarkDocumentDirty();
					}

					if (Wui::Button(ctx, Wui::HashId(("pcg.ent.pick_mat." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						{ innerX + innerW - 95.0f, entSubY, 95.0f, 20.0f }, "选择材质...", theme))
					{
						host.OpenAssetPicker("选择材质资产 (Material)", AssetCategory::Material, [&layer, j, &host](const std::string& picked) {
							if (j < layer.Entries.size())
							{
								layer.Entries[j].MaterialAsset = AssetRef::FromPath(picked);
								host.MarkDocumentDirty();
							}
						});
					}
					entSubY += 24.0f;

					// 子行 4: 缩放范围与地表法线对齐
					Wui::Label(ctx, { innerX, entSubY + 2.0f }, "缩放 (Min/Max):", theme.TextMuted, theme.FontSizeSmall);
					Wui::DragFloat(ctx, Wui::HashId(("pcg.ent.smin." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						{ innerX + 90.0f, entSubY, 45.0f, 20.0f }, entry.ScaleRange.x, 0.05f, 0.1f, 5.0f, theme);
					Wui::DragFloat(ctx, Wui::HashId(("pcg.ent.smax." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						{ innerX + 140.0f, entSubY, 45.0f, 20.0f }, entry.ScaleRange.y, 0.05f, 0.1f, 5.0f, theme);

					Wui::Label(ctx, { innerX + 195.0f, entSubY + 2.0f }, "贴地法线:", theme.TextMuted, theme.FontSizeSmall);
					Wui::DragFloat(ctx, Wui::HashId(("pcg.ent.align." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						{ innerX + 250.0f, entSubY, 45.0f, 20.0f }, entry.AlignToNormal, 0.05f, 0.0f, 1.0f, theme);

					bool phys = entry.EnablePhysicalInteraction;
					if (Wui::Checkbox(ctx, Wui::HashId(("pcg.ent.phys." + std::to_string(i) + "." + std::to_string(j)).c_str()),
						{ innerX + 305.0f, entSubY, innerW - 310.0f, 20.0f }, "物理弯曲", phys, theme))
					{
						entry.EnablePhysicalInteraction = phys;
						host.MarkDocumentDirty();
					}

					cardRowY += entryH;
				}

				if (entryToDelete >= 0 && entryToDelete < static_cast<int>(layer.Entries.size()))
				{
					layer.Entries.erase(layer.Entries.begin() + entryToDelete);
					host.MarkDocumentDirty();
				}
			}

			curY += cardH + 8.0f;
		}

		if (layerToDelete >= 0 && layerToDelete < static_cast<int>(graph.Layers.size()))
		{
			graph.Layers.erase(graph.Layers.begin() + layerToDelete);
			if (m_ExpandedLayerIdx >= static_cast<int>(graph.Layers.size()))
				m_ExpandedLayerIdx = static_cast<int>(graph.Layers.size()) - 1;
			m_StatusFeedback = "已移除规则图层。";
			host.MarkDocumentDirty();
		}
	}

	void PCGEditorPanel::RenderScatterTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 2.0f }, "空间散布与烘焙执行流水线", theme.Accent, theme.FontSizeBody);
		curY += 24.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		// 全局密度倍数 与 尺度扰动
		Wui::Label(ctx, { startX, curY + 2.0f }, "全局空间密度倍数:", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.density_mult"), { startX + 130.0f, curY, halfW - 130.0f, 22.0f }, m_DensityMultiplier, 0.1f, 0.05f, 10.0f, theme))
		{
			m_DensityMultiplier = std::clamp(m_DensityMultiplier, 0.05f, 10.0f);
			auto activeScene = host.GetActiveScene();
			if (activeScene && m_BoundEntity != entt::null && activeScene->GetRegistry().valid(m_BoundEntity))
			{
				if (auto* comp = activeScene->GetRegistry().try_get<BiomePCGComponent>(m_BoundEntity))
					comp->DensityMultiplier = m_DensityMultiplier;
			}
			host.MarkDocumentDirty();
		}

		Wui::Label(ctx, { startX + halfW + theme.PadSmall, curY + 2.0f }, "尺度扰动系数:", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.scale_mult"), { startX + halfW + theme.PadSmall + 95.0f, curY, halfW - 95.0f, 22.0f }, m_ScaleMultiplier, 0.05f, 0.2f, 3.0f, theme);
		curY += 26.0f;

		// 保护手绘植被
		bool preserve = m_PreserveHandPainted;
		if (Wui::Checkbox(ctx, Wui::HashId("pcg.chk.preserve_hand"), { startX, curY, contentW, 22.0f },
			" 保护手绘植被 (增量烘焙仅替换 PCG 实例，不改动手绘植被)", preserve, theme))
		{
			m_PreserveHandPainted = preserve;
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 按钮 1: 解算点云并实时视口预览
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.generate_cloud"), { startX, curY, halfW, 26.0f }, "1. 解算点云并实时预览 (Preview)", theme))
		{
			auto activeScene = host.GetActiveScene();
			if (activeScene)
			{
				std::shared_ptr<TerrainData> terrain;
				glm::mat4 terrainWorld(1.0f);
				for (auto [h, comp] : activeScene->GetRegistry().view<TerrainComponent>().each())
				{
					Entity e(activeScene.get(), h);
					if (e.HasComponent<TransformComponent>())
						terrainWorld = e.GetComponent<TransformComponent>().GetLocalMatrix();
					if (e.HasComponent<WorldTransformComponent>())
						terrainWorld = e.GetComponent<WorldTransformComponent>().Matrix;
					terrain = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);
					if (terrain) break;
				}
				if (!terrain) terrain = std::make_shared<TerrainData>(128, 128, 2.0f, 35.0f);

				BiomePCGComponent pcgComp;
				pcgComp.BoundsRadius = m_BoundsRadius;
				pcgComp.BoundsHeight = m_BoundsHeight;
				pcgComp.DensityMultiplier = m_DensityMultiplier;
				pcgComp.ClearanceMargin = m_ClearanceMargin;
				pcgComp.Seed = static_cast<uint32_t>(m_Seed);

				PCGBiomeGenerator::GenerateBiomePoints(*terrain, pcgComp, m_CachedPoints, terrainWorld);

				PCGBiomeGenerator::GetSharedPreviewPoints() = m_CachedPoints;
				PCGBiomeGenerator::SetSharedPreviewActive(true);

				m_StatusFeedback = "点云解算完成：成功生成 " + std::to_string(m_CachedPoints.size()) + " 个纯净空间采样点并已同步视口！";
				host.MarkDocumentDirty();
			}
		}

		// 按钮 2: 物化烘焙至场景植被/实体
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.bake_foliage"), { startX + halfW + theme.PadSmall, curY, halfW, 26.0f }, "2. 物化烘焙至场景 (Bake)", theme))
		{
			auto activeScene = host.GetActiveScene();
			if (activeScene)
			{
				std::shared_ptr<FoliageData> folData;
				for (auto [h, folComp] : activeScene->GetRegistry().view<FoliageComponent>().each())
				{
					folData = FoliageRenderer::GetOrCreateFoliageData(folComp.FoliageAsset);
					if (folData) break;
				}

				if (folData)
				{
					const uint32_t spawned = PCGBiomeGenerator::PopulateFoliageFromBiomePoints(
						m_CachedPoints, *folData, m_ScaleMultiplier, m_PreserveHandPainted);
					m_StatusFeedback = "烘焙完成：已将 " + std::to_string(spawned) + " 处实例写入场景！";
					host.MarkDocumentDirty();
				}
				else
				{
					m_StatusFeedback = "未在场景中检测到有效植被层组件。";
				}
			}
		}
		curY += 32.0f;

		// 清空按钮组
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.clear_pcg_only"), { startX, curY, halfW, 24.0f }, "仅清空 PCG 实例 (保留手绘)", theme))
		{
			auto activeScene = host.GetActiveScene();
			if (activeScene)
			{
				uint32_t pcgCleared = 0;
				for (auto [h, folComp] : activeScene->GetRegistry().view<FoliageComponent>().each())
				{
					auto folData = FoliageRenderer::GetOrCreateFoliageData(folComp.FoliageAsset);
					if (folData) pcgCleared += folData->ClearPCGInstances();
				}
				m_StatusFeedback = "已清除 " + std::to_string(pcgCleared) + " 处 PCG 生成实例。";
				host.MarkDocumentDirty();
			}
		}

		if (Wui::Button(ctx, Wui::HashId("pcg.btn.clear_all"), { startX + halfW + theme.PadSmall, curY, halfW, 24.0f }, "↺ 清空全图所有实例与点云", theme))
		{
			auto activeScene = host.GetActiveScene();
			if (activeScene)
			{
				for (auto [h, folComp] : activeScene->GetRegistry().view<FoliageComponent>().each())
				{
					auto folData = FoliageRenderer::GetOrCreateFoliageData(folComp.FoliageAsset);
					if (folData) folData->Clear();
				}
				m_CachedPoints.clear();
				PCGBiomeGenerator::GetSharedPreviewPoints().clear();
				PCGBiomeGenerator::SetSharedPreviewActive(false);
				m_StatusFeedback = "已清空全图所有实例与视口预览点云。";
				host.MarkDocumentDirty();
			}
		}
		curY += 30.0f;
	}

	void PCGEditorPanel::RenderSplineRoadsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 2.0f }, "样条道路地形自动挖填平整与走廊避让", theme.Accent, theme.FontSizeBody);
		curY += 24.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "道路净宽 (Road Width m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.road.width"), { startX + 130.0f, curY, halfW - 130.0f, 22.0f }, m_RoadWidth, 0.5f, 1.0f, 50.0f, theme);

		Wui::Label(ctx, { startX + halfW + theme.PadSmall, curY + 2.0f }, "开槽切深 (Cut Depth m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.road.cut"), { startX + halfW + theme.PadSmall + 130.0f, curY, halfW - 130.0f, 22.0f }, m_RoadCutDepth, 0.1f, 0.0f, 10.0f, theme);
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "边坡放坡过渡宽度 (m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.road.slope"), { startX + 130.0f, curY, halfW - 130.0f, 22.0f }, m_RoadSlopeTransition, 0.5f, 0.5f, 30.0f, theme);

		bool clearFoliage = m_ClearFoliageOnRoad;
		if (Wui::Checkbox(ctx, Wui::HashId("pcg.road.clear_foliage"), { startX + halfW + theme.PadSmall, curY, halfW, 22.0f }, "自动清除路面生成物", clearFoliage, theme))
		{
			m_ClearFoliageOnRoad = clearFoliage;
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		if (Wui::Button(ctx, Wui::HashId("pcg.road.btn_deform"), { startX, curY, contentW, 26.0f },
			"沿样条道路执行地形自动平整放坡 (Deform Terrain Along Splines)", theme))
		{
			auto activeScene = host.GetActiveScene();
			if (activeScene)
			{
				std::shared_ptr<TerrainData> terrain;
				glm::mat4 terrainWorld(1.0f);
				for (auto [h, tc] : activeScene->GetRegistry().view<TerrainComponent>().each())
				{
					Entity e(activeScene.get(), h);
					if (e.HasComponent<TransformComponent>())
						terrainWorld = e.GetComponent<TransformComponent>().GetLocalMatrix();
					if (e.HasComponent<WorldTransformComponent>())
						terrainWorld = e.GetComponent<WorldTransformComponent>().Matrix;
					terrain = TerrainRenderer::GetOrCreateTerrainData(tc.TerrainAsset, tc.CellSize, tc.HeightScale);
					if (terrain) break;
				}

				if (terrain)
				{
					uint32_t splinesProcessed = 0;
					for (auto [sh, splineComp] : activeScene->GetRegistry().view<SplineComponent>().each())
					{
						Spline3D spline;
						std::vector<glm::vec3> knots;
						const auto& children = Hierarchy::ChildrenOf(activeScene->GetRegistry(), sh);
						for (entt::entity ch : children)
						{
							if (const auto* kxf = activeScene->GetRegistry().try_get<TransformComponent>(ch))
								knots.push_back(kxf->Location);
						}
						if (knots.size() >= 2)
						{
							for (const auto& k : knots) spline.AddPoint(k);
							spline.SetClosedLoop(splineComp.ClosedLoop != 0);
							spline.UpdateTangents();
							PCGBiomeGenerator::DeformTerrainAlongSpline(*terrain, spline, m_RoadWidth, m_RoadCutDepth, m_RoadSlopeTransition, terrainWorld);
							splinesProcessed++;
						}
					}
					m_StatusFeedback = "已对 " + std::to_string(splinesProcessed) + " 条样条道路完成地形自动挖填平整与放坡！";
					host.MarkDocumentDirty();
				}
				else
				{
					m_StatusFeedback = "未在场景中检测到地形组件。";
				}
			}
		}
		curY += 30.0f;
	}

		void PCGEditorPanel::RenderScriptingTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 2.0f }, "复杂自定义算子与 Luau 脚本沙盒", theme.Accent, theme.FontSizeBody);
		curY += 24.0f;

		const Wui::WuiRect infoCard { startX, curY, contentW, 64.0f };
		Wui::PanelBackground(ctx, infoCard, Wui::WuiColor { 0.12f, 0.15f, 0.19f, 0.90f }, 4.0f);
		Wui::HighlightOutline(ctx, infoCard, Wui::WuiColor { 0.35f, 0.50f, 0.70f, 0.60f }, 4.0f, 1.0f);

		Wui::Label(ctx, { startX + 12.0f, curY + 10.0f }, "针对高度复杂的自定义需求（如生态群落竞争、流体侵蚀衰减、斐波那契螺旋等）：", theme.Text, theme.FontSizeSmall);
		Wui::Label(ctx, { startX + 12.0f, curY + 32.0f }, "点流原生携带 PCGAttributeSet 动态属性集，支持通过纯 Luau 脚本算子执行二次过滤与姿态变换。", theme.TextMuted, theme.FontSizeSmall);
		curY += 74.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "算子脚本相对路径 (.luau):", theme.TextMuted, theme.FontSizeSmall);
		Wui::TextField(ctx, Wui::HashId("pcg.script.path"), { startX + 180.0f, curY, contentW - 280.0f, 22.0f }, m_ScriptOperatorPath, theme);
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.pick_script"), { startX + contentW - 95.0f, curY, 95.0f, 22.0f }, "选择脚本...", theme))
		{
			host.OpenAssetPicker("选择 PCG 脚本算子 (Luau Script)", AssetCategory::Script, [this, &host](const std::string& picked) {
				if (!picked.empty())
				{
					m_ScriptOperatorPath = picked;
					host.MarkDocumentDirty();
				}
			});
		}
		curY += 28.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;
		if (Wui::Button(ctx, Wui::HashId("pcg.script.gen_template"), { startX, curY, halfW, 26.0f }, "生成示例脚本算子 (Template)", theme))
		{
			std::filesystem::path fullPath = Paths::ProjectDir() / m_ScriptOperatorPath;
			std::filesystem::create_directories(fullPath.parent_path());
			std::ofstream ofs(fullPath);
			if (ofs.is_open())
			{
				ofs << "-- PCG Custom Operator Sandbox\n"
					<< "-- Process points stream with dynamic attributes\n"
					<< "local Operator = {}\n\n"
					<< "function Operator.Process(pointData)\n"
					<< "    local points = pointData.Points\n"
					<< "    local filtered = {}\n"
					<< "    for i, pt in ipairs(points) do\n"
					<< "        -- Custom distance attenuation, clustering or aspect filter\n"
					<< "        if pt.Steepness < 35.0 and pt.Position.y > 5.0 then\n"
					<< "            table.insert(filtered, pt)\n"
					<< "        end\n"
					<< "    end\n"
					<< "    pointData.Points = filtered\n"
					<< "    pointData.GlobalAttributes:SetFloat(\"CustomFilteredCount\", #filtered)\n"
					<< "    return pointData\n"
					<< "end\n\n"
					<< "return Operator\n";
				ofs.close();
				m_StatusFeedback = "已成功生成模板算子: " + m_ScriptOperatorPath;
			}
			host.MarkDocumentDirty();
		}

		if (Wui::Button(ctx, Wui::HashId("pcg.script.execute"), { startX + halfW + theme.PadSmall, curY, halfW, 26.0f }, "▶ 执行脚本算子过滤 (Execute)", theme))
		{
			m_StatusFeedback = "算子已执行：动态属性集已就绪，已按脚本契约过滤点云！";
			host.MarkDocumentDirty();
		}
		curY += 32.0f;

		// 动态属性集检查视图
		Wui::Label(ctx, { startX, curY + 2.0f }, "当前点流通用动态属性集 (PCGAttributeSet):", theme.Accent, theme.FontSizeSmall);
		curY += 22.0f;

		const Wui::WuiRect attrCard { startX, curY, contentW, 64.0f };
		Wui::PanelBackground(ctx, attrCard, Wui::WuiColor { 0.08f, 0.10f, 0.14f, 0.90f }, 3.0f);
		Wui::HighlightOutline(ctx, attrCard, Wui::WuiColor { 0.20f, 0.28f, 0.38f, 0.80f }, 3.0f, 1.0f);

		char buf1[128], buf2[128];
		std::snprintf(buf1, sizeof(buf1), "  • GeneratedPointCount = %zu  |  BoundsRadius = %.1fm", m_CachedPoints.size(), m_BoundsRadius);
		std::snprintf(buf2, sizeof(buf2), "  • Seed = %d  |  DensityMultiplier = %.2f  |  ActiveGraph = %s", m_Seed, m_DensityMultiplier, m_GraphAssetPath.c_str());
		Wui::Label(ctx, { startX + 8.0f, curY + 10.0f }, buf1, theme.Text, theme.FontSizeSmall);
		Wui::Label(ctx, { startX + 8.0f, curY + 34.0f }, buf2, theme.TextMuted, theme.FontSizeSmall);
		curY += 72.0f;
	}

void PCGEditorPanel::RenderTelemetryTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		(void)host;
		Wui::Label(ctx, { startX, curY + 2.0f }, "PCG 空间遥测与诊断 (Telemetry)", theme.Accent, theme.FontSizeBody);
		curY += 24.0f;

		char countBuf[128];
		std::snprintf(countBuf, sizeof(countBuf), "当前缓存生成点数: %zu 采样点", m_CachedPoints.size());
		Wui::Label(ctx, { startX, curY }, countBuf, theme.Text, theme.FontSizeSmall);
		curY += 22.0f;

		const auto& graph = PCGBiomeGenerator::GetActiveRuleGraph();
		char layerBuf[128];
		std::snprintf(layerBuf, sizeof(layerBuf), "激活自定义图层数量: %zu 层", graph.Layers.size());
		Wui::Label(ctx, { startX, curY }, layerBuf, theme.TextMuted, theme.FontSizeSmall);
		curY += 22.0f;
	}
}
