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

#include <algorithm>
#include <cstdio>

namespace World
{
	PCGEditorPanel::PCGEditorPanel()
	{
		m_StatusFeedback = "PCG 生态与世界生成器已就绪。";
	}

	void PCGEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const float pad = theme.Pad;
		const float startX = rect.X + pad;
		const float contentW = rect.W - pad * 2.0f;
		float curY = rect.Y + 4.0f;

		// 1. 头部徽章与状态
		const Wui::WuiRect badgeRect { startX, curY, 240.0f, 18.0f };
		const Wui::WuiColor badgeBg { 0.35f, 0.28f, 0.08f, 0.35f };
		const Wui::WuiColor badgeBorder { 0.95f, 0.78f, 0.20f, 0.90f };
		Wui::Badge(ctx, badgeRect, "PCG BIOME & WORLD GENERATOR", badgeBg, badgeBorder, theme, 10.5f, true, 3.0f);
		curY += 26.0f;

		// 状态信息
		Wui::Label(ctx, { startX, curY }, m_StatusFeedback.c_str(), theme.Accent, theme.FontSizeSmall);
		curY += 22.0f;

		// 2. 选项卡: [生态规则] [程序化散布] [道路放坡] [生态遥测]
		const std::vector<std::string> tabs = { "生态规则", "程序化散布", "道路放坡", "生态遥测" };
		if (Wui::TabBar(ctx, Wui::HashId("pcg.tabs"), { startX, curY, contentW, 28.0f }, tabs, m_ActiveTabIdx, theme))
		{
			m_ActiveTab = static_cast<PCGEditorTab>(m_ActiveTabIdx);
		}
		curY += 34.0f;

		switch (m_ActiveTab)
		{
		case PCGEditorTab::BiomeRules:
			RenderBiomeRulesTab(ctx, startX, curY, contentW, theme, host);
			break;
		case PCGEditorTab::Scatter:
			RenderScatterTab(ctx, startX, curY, contentW, theme, host);
			break;
		case PCGEditorTab::SplineRoads:
			RenderSplineRoadsTab(ctx, startX, curY, contentW, theme, host);
			break;
		case PCGEditorTab::Telemetry:
			RenderTelemetryTab(ctx, startX, curY, contentW, theme, host);
			break;
		}
	}

	void PCGEditorPanel::RenderBiomeRulesTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 4.0f }, "多因子生态群落决策矩阵 (Multi-factor Biome Rules)", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		// 标高范围
		Wui::Label(ctx, { startX, curY + 2.0f }, "散布标高区间 (Elevation Bounds m):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.min_elev"), { startX, curY, halfW, 22.0f }, m_MinElevation, 1.0f, 0.0f, 5000.0f, theme))
		{
			m_MinElevation = std::clamp(m_MinElevation, 0.0f, 5000.0f);
			host.MarkDocumentDirty();
		}
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.max_elev"), { startX + halfW + theme.PadSmall, curY, halfW, 22.0f }, m_MaxElevation, 1.0f, 0.0f, 5000.0f, theme))
		{
			m_MaxElevation = std::clamp(m_MaxElevation, m_MinElevation, 5000.0f);
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 最大坡度与湿度阈值
		Wui::Label(ctx, { startX, curY + 2.0f }, "地表极限坡度 (Slope deg) 与 湿度生成阈值 (Moisture):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.max_slope"), { startX, curY, halfW, 22.0f }, m_MaxSlope, 0.5f, 0.0f, 89.0f, theme))
		{
			m_MaxSlope = std::clamp(m_MaxSlope, 0.0f, 89.0f);
			host.MarkDocumentDirty();
		}
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.moisture"), { startX + halfW + theme.PadSmall, curY, halfW, 22.0f }, m_MoistureThreshold, 0.02f, 0.0f, 1.0f, theme))
		{
			m_MoistureThreshold = std::clamp(m_MoistureThreshold, 0.0f, 1.0f);
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 伪随机种子
		Wui::Label(ctx, { startX, curY + 2.0f }, "生态程序化伪随机种子 (Seed):", theme.Text, theme.FontSizeSmall);
		int64_t seedVal = static_cast<int64_t>(m_Seed);
		if (Wui::DragInt(ctx, Wui::HashId("pcg.seed"), { startX + 160.0f, curY, contentW - 160.0f, 22.0f }, seedVal, 0, 9999999, theme))
		{
			m_Seed = static_cast<int32_t>(seedVal);
			host.MarkDocumentDirty();
		}
		curY += 32.0f;

		// 生态群落规则释义卡片
		const struct BiomeInfoCard { const char* name; const char* rule; Wui::WuiColor color; } biomes[5] = {
			{ "🌲 Forest (温带森林)", "海拔 < 100m 且 湿度 ≥ 0.50 | 优势植被: 翠绿草丛 (65%) + 矮生灌木 (35%)", Wui::WuiColor{ 0.15f, 0.65f, 0.30f, 0.85f } },
			{ "🌼 Meadow (花海草甸)", "海拔 < 150m 且 坡度平缓 | 优势植被: 雏菊野花 (55%) + 翠绿草丛 (45%)", Wui::WuiColor{ 0.45f, 0.75f, 0.20f, 0.85f } },
			{ "⛰️ RockyMountain (高山岩壁)", "地表坡度 > 40° 陡峭悬崖 | 优势植被: 地表碎石 (70%) + 矮生灌木 (30%)", Wui::WuiColor{ 0.55f, 0.55f, 0.55f, 0.85f } },
			{ "🏜️ Desert (干燥荒原)", "湿度 < 0.25 贫瘠干旱地貌 | 优势植被: 地表碎石 (80%) + 旱地耐旱灌木 (20%)", Wui::WuiColor{ 0.80f, 0.65f, 0.25f, 0.85f } },
			{ "❄️ SnowPeak (雪线极峰)", "海拔 > 150m 终年积雪区 | 优势植被: 冰碛碎石与冻原岩块", Wui::WuiColor{ 0.20f, 0.80f, 0.95f, 0.85f } }
		};

		for (int i = 0; i < 5; ++i)
		{
			const Wui::WuiRect itemRect { startX, curY, contentW, 42.0f };
			Wui::PanelBackground(ctx, itemRect, Wui::WuiColor { 0.10f, 0.14f, 0.18f, 0.85f }, 4.0f);
			Wui::HighlightOutline(ctx, itemRect, biomes[i].color, 4.0f, 1.0f);

			Wui::Label(ctx, { startX + 8.0f, curY + 4.0f }, biomes[i].name, biomes[i].color, theme.FontSizeSmall);
			Wui::Label(ctx, { startX + 8.0f, curY + 22.0f }, biomes[i].rule, theme.TextMuted, theme.FontSizeSmall);
			curY += 48.0f;
		}
	}

	void PCGEditorPanel::RenderScatterTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 4.0f }, "生态散布流水线 (PCG Scatter Pipeline)", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "散布密度倍数 (Density):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.density"), { startX + 130.0f, curY, halfW - 130.0f, 22.0f }, m_Density, 0.1f, 0.1f, 10.0f, theme);

		Wui::Label(ctx, { startX + halfW + theme.PadSmall, curY + 2.0f }, "尺度扰动系数:", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.scale_mult"), { startX + halfW + theme.PadSmall + 95.0f, curY, halfW - 95.0f, 22.0f }, m_ScaleMultiplier, 0.05f, 0.2f, 3.0f, theme);
		curY += 28.0f;

		// 按钮 1: 生成采样散布点云
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.generate_cloud"), { startX, curY, halfW, 26.0f }, "✦ 1. 解算生态点云 (Compute Points)", theme))
		{
			auto activeScene = host.GetActiveScene();
			if (activeScene)
			{
				std::shared_ptr<TerrainData> terrain;
				for (auto [h, comp] : activeScene->GetRegistry().view<TerrainComponent>().each())
				{
					terrain = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);
					if (terrain) break;
				}
				if (!terrain) terrain = std::make_shared<TerrainData>(128, 128, 2.0f, 30.0f);

				BiomePCGComponent pcgComp;
				pcgComp.MinElevation = m_MinElevation;
				pcgComp.MaxElevation = m_MaxElevation;
				pcgComp.MaxSlope = m_MaxSlope;
				pcgComp.MoistureThreshold = m_MoistureThreshold;
				pcgComp.Seed = static_cast<uint32_t>(m_Seed);

				PCGBiomeGenerator::GenerateBiomePoints(*terrain, pcgComp, m_CachedPoints);
				for (int b = 0; b < 5; ++b) m_DominantCounts[b] = 0;
				for (const auto& pt : m_CachedPoints)
				{
					int idx = static_cast<int>(pt.Biome);
					if (idx >= 0 && idx < 5) m_DominantCounts[idx]++;
				}
				m_StatusFeedback = "解算完成：成功生成 " + std::to_string(m_CachedPoints.size()) + " 个生态空间采样点！";
				host.MarkDocumentDirty();
			}
		}

		// 按钮 2: 一键烘焙到植被系统
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.bake_foliage"), { startX + halfW + theme.PadSmall, curY, halfW, 26.0f }, "✦ 2. 烘焙至植被层 (Bake to Foliage)", theme))
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

				if (!folData)
				{
					// 自动在场景中创建植被层
					for (auto [h, tc] : activeScene->GetRegistry().view<TerrainComponent>().each())
					{
						Entity e(activeScene.get(), h);
						auto& fc = e.AddComponent<FoliageComponent>();
						fc.FoliageAsset = AssetRef::FromPath("foliage/default.wfoliage");
						fc.EnableWind = 1;
						folData = FoliageRenderer::GetOrCreateFoliageData(fc.FoliageAsset);
						break;
					}
				}

				if (folData)
				{
					if (m_CachedPoints.empty())
					{
						std::shared_ptr<TerrainData> terrain;
						for (auto [h, comp] : activeScene->GetRegistry().view<TerrainComponent>().each())
						{
							terrain = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);
							if (terrain) break;
						}
						if (!terrain) terrain = std::make_shared<TerrainData>(128, 128, 2.0f, 30.0f);

						BiomePCGComponent pcgComp;
						pcgComp.MinElevation = m_MinElevation;
						pcgComp.MaxElevation = m_MaxElevation;
						pcgComp.MaxSlope = m_MaxSlope;
						pcgComp.MoistureThreshold = m_MoistureThreshold;
						pcgComp.Seed = static_cast<uint32_t>(m_Seed);
						PCGBiomeGenerator::GenerateBiomePoints(*terrain, pcgComp, m_CachedPoints);
					}

					const uint32_t spawned = PCGBiomeGenerator::PopulateFoliageFromBiomePoints(m_CachedPoints, *folData, m_ScaleMultiplier);
					m_StatusFeedback = "烘焙完成：已将 " + std::to_string(spawned) + " 处次世代植被实例写入场景植被层！";
					host.MarkDocumentDirty();
				}
				else
				{
					m_StatusFeedback = "未在场景中检测到地形或植被组件。";
				}
			}
		}
		curY += 34.0f;

		// 清理与复位
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.clear_all"), { startX, curY, contentW, 24.0f }, "✖ 清空场景植被实例 (Clear Scene Foliage)", theme))
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
				for (int b = 0; b < 5; ++b) m_DominantCounts[b] = 0;
				m_StatusFeedback = "已清空场景中所有植被实例与点云。";
				host.MarkDocumentDirty();
			}
		}
		curY += 32.0f;
	}

	void PCGEditorPanel::RenderSplineRoadsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 4.0f }, "样条道路地形自动填挖平整与走廊剔除", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "道路净宽 (Road Width m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.road.width"), { startX + 130.0f, curY, halfW - 130.0f, 22.0f }, m_RoadWidth, 0.5f, 1.0f, 50.0f, theme);

		Wui::Label(ctx, { startX + halfW + theme.PadSmall, curY + 2.0f }, "开槽切深 (Cut Depth m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.road.cut"), { startX + halfW + theme.PadSmall + 130.0f, curY, halfW - 130.0f, 22.0f }, m_RoadCutDepth, 0.1f, 0.0f, 10.0f, theme);
		curY += 28.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "边坡过渡放坡宽度 (m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.road.slope"), { startX + 130.0f, curY, halfW - 130.0f, 22.0f }, m_RoadSlopeTransition, 0.5f, 0.5f, 30.0f, theme);

		bool clearFoliage = m_ClearFoliageOnRoad;
		if (Wui::Checkbox(ctx, Wui::HashId("pcg.road.clear_foliage"), { startX + halfW + theme.PadSmall, curY, halfW, 22.0f }, "自动清除路面植被走廊", clearFoliage, theme))
		{
			m_ClearFoliageOnRoad = clearFoliage;
			host.MarkDocumentDirty();
		}
		curY += 32.0f;

		if (Wui::Button(ctx, Wui::HashId("pcg.btn.exec_road_grading"), { startX, curY, contentW, 26.0f }, "✦ 沿场景所有样条道路执行地形挖填与净空 (Execute Road Grading)", theme))
		{
			auto activeScene = host.GetActiveScene();
			if (activeScene)
			{
				std::shared_ptr<TerrainData> terrain;
				for (auto [h, comp] : activeScene->GetRegistry().view<TerrainComponent>().each())
				{
					terrain = TerrainRenderer::GetOrCreateTerrainData(comp.TerrainAsset, comp.CellSize, comp.HeightScale);
					if (terrain) break;
				}

				std::shared_ptr<FoliageData> folData;
				for (auto [h, folComp] : activeScene->GetRegistry().view<FoliageComponent>().each())
				{
					folData = FoliageRenderer::GetOrCreateFoliageData(folComp.FoliageAsset);
					if (folData) break;
				}

				size_t roadCount = 0;
				uint32_t foliageCleared = 0;
				for (auto [sh, splineComp] : activeScene->GetRegistry().view<SplineComponent>().each())
				{
					Spline3D spline;
					std::vector<glm::vec3> knots;
					const auto& children = Hierarchy::ChildrenOf(activeScene->GetRegistry(), sh);
					for (entt::entity ch : children)
					{
						if (const auto* xf = activeScene->GetRegistry().try_get<TransformComponent>(ch))
							knots.push_back(xf->Location);
					}
					if (knots.size() >= 2)
					{
						for (const auto& k : knots) spline.AddPoint(k);
						spline.SetClosedLoop(splineComp.ClosedLoop != 0);
						spline.UpdateTangents();

						if (terrain)
						{
							PCGBiomeGenerator::DeformTerrainAlongSpline(
								*terrain, spline, m_RoadWidth, m_RoadCutDepth, m_RoadSlopeTransition);
						}
						if (folData && m_ClearFoliageOnRoad)
						{
							foliageCleared += PCGBiomeGenerator::ClearFoliageAlongSpline(*folData, spline, m_RoadWidth + 1.0f);
						}
						roadCount++;
					}
				}

				if (terrain) TerrainRenderer::ClearCache();
				m_StatusFeedback = "道路平整完成：处理了 " + std::to_string(roadCount) + " 条样条道路，净空清除 " + std::to_string(foliageCleared) + " 处路面阻挡植被！";
				host.MarkDocumentDirty();
			}
		}
		curY += 32.0f;
	}

	void PCGEditorPanel::RenderTelemetryTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 4.0f }, "生态群落全局遥测与预算分析 (Biome Telemetry)", theme.Accent, theme.FontSizeBody);
		curY += 26.0f;

		const size_t totalPoints = m_CachedPoints.size();
		char summaryBuf[128];
		std::snprintf(summaryBuf, sizeof(summaryBuf), "当前解算采样点总数: %zu 处  |  随机种子: %d", totalPoints, m_Seed);
		Wui::Label(ctx, { startX, curY }, summaryBuf, theme.Text, theme.FontSizeSmall);
		curY += 24.0f;

		const char* biomeNames[5] = { "Forest (森林)", "Meadow (草甸)", "RockyMountain (岩山)", "Desert (荒漠)", "SnowPeak (雪峰)" };
		const Wui::WuiColor biomeColors[5] = {
			Wui::WuiColor{ 0.15f, 0.65f, 0.30f, 0.90f },
			Wui::WuiColor{ 0.45f, 0.75f, 0.20f, 0.90f },
			Wui::WuiColor{ 0.55f, 0.55f, 0.55f, 0.90f },
			Wui::WuiColor{ 0.80f, 0.65f, 0.25f, 0.90f },
			Wui::WuiColor{ 0.20f, 0.80f, 0.95f, 0.90f }
		};

		for (int i = 0; i < 5; ++i)
		{
			const size_t cnt = m_DominantCounts[i];
			const float pct = (totalPoints > 0) ? (static_cast<float>(cnt) / static_cast<float>(totalPoints) * 100.0f) : 0.0f;

			const Wui::WuiRect barCard { startX, curY, contentW, 36.0f };
			Wui::PanelBackground(ctx, barCard, Wui::WuiColor { 0.10f, 0.12f, 0.16f, 0.85f }, 4.0f);
			Wui::HighlightOutline(ctx, barCard, biomeColors[i], 4.0f, 1.0f);

			char barLabel[128];
			std::snprintf(barLabel, sizeof(barLabel), "%s: %zu 处 (%.1f%%)", biomeNames[i], cnt, pct);
			Wui::Label(ctx, { startX + 8.0f, curY + 4.0f }, barLabel, theme.Text, theme.FontSizeSmall);

			// 进度条槽
			const float barW = contentW - 16.0f;
			const float fillW = barW * (pct * 0.01f);
			const Wui::WuiRect trackRect { startX + 8.0f, curY + 22.0f, barW, 6.0f };
			Wui::PanelBackground(ctx, trackRect, Wui::WuiColor { 0.05f, 0.07f, 0.09f, 0.90f }, 2.0f);
			if (fillW > 1.0f)
			{
				const Wui::WuiRect fillRect { startX + 8.0f, curY + 22.0f, fillW, 6.0f };
				Wui::PanelBackground(ctx, fillRect, biomeColors[i], 2.0f);
			}

			curY += 42.0f;
		}
	}
}
