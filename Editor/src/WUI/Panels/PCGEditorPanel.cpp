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

#include <algorithm>
#include <cmath>
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

		// 1. 头部徽章
		const Wui::WuiRect badgeRect { startX, curY, 240.0f, 18.0f };
		const Wui::WuiColor badgeBg { 0.35f, 0.28f, 0.08f, 0.35f };
		const Wui::WuiColor badgeBorder { 0.95f, 0.78f, 0.20f, 0.90f };
		Wui::Badge(ctx, badgeRect, "PCG BIOME & WORLD GENERATOR", badgeBg, badgeBorder, theme, 10.5f, true, 3.0f);
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
				"✦ 在当前关卡创建 PCG 生态管理器 (Create BiomePCG Entity)", theme))
			{
				if (activeScene)
				{
					Entity pcgEnt = Entity::CreateEntity(activeScene.get(), "Alpine Valley Terrain & PCG Hub");
					pcgEnt.AddComponent<TransformComponent>();
					auto& comp = pcgEnt.AddComponent<BiomePCGComponent>();
					comp.MinElevation = m_MinElevation;
					comp.MaxElevation = m_MaxElevation;
					comp.MaxSlope = m_MaxSlope;
					comp.MoistureThreshold = m_MoistureThreshold;
					comp.Seed = static_cast<uint32_t>(m_Seed);
					comp.AutoDeformTerrain = m_AutoDeformTerrain;
					m_BoundEntity = static_cast<entt::entity>(pcgEnt);
					host.SetSelectedEntity(pcgEnt);
					m_StatusFeedback = "已在当前场景创建 PCG 生态管理器实体并完成双向绑定。";
					host.MarkDocumentDirty();
				}
			}
			curY += 32.0f;
		}
		else
		{
			auto& comp = boundEntity.GetComponent<BiomePCGComponent>();
			std::string tagStr = "已绑定生态实体: ";
			if (boundEntity.HasComponent<TagComponent>()) tagStr += StringPool::Get().NameOf(boundEntity.GetComponent<TagComponent>().Tag);
			else tagStr += "Entity " + std::to_string(static_cast<uint32_t>(m_BoundEntity));
			tagStr += " [ID: " + std::to_string(static_cast<uint32_t>(m_BoundEntity)) + "]";

			const Wui::WuiRect bindBarRect { startX, curY, contentW, 22.0f };
			Wui::PanelBackground(ctx, bindBarRect, Wui::WuiColor{ 0.10f, 0.22f, 0.16f, 0.85f }, 3.0f);
			Wui::HighlightOutline(ctx, bindBarRect, Wui::WuiColor{ 0.20f, 0.85f, 0.40f, 0.85f }, 3.0f, 1.0f);
			Wui::Label(ctx, { startX + 8.0f, curY + 2.0f }, tagStr.c_str(), theme.Success, theme.FontSizeSmall);
			curY += 26.0f;

			// 双向同步参数至面板内部变量
			m_MinElevation = comp.MinElevation;
			m_MaxElevation = comp.MaxElevation;
			m_MaxSlope = comp.MaxSlope;
			m_MoistureThreshold = comp.MoistureThreshold;
			m_Seed = static_cast<int32_t>(comp.Seed);
			m_AutoDeformTerrain = comp.AutoDeformTerrain;
		}

		// 状态反馈文本
		Wui::Label(ctx, { startX, curY }, m_StatusFeedback.c_str(), theme.Accent, theme.FontSizeSmall);
		curY += 20.0f;

		// 3. 选项卡: [生态规则] [程序化散布] [道路放坡] [生态遥测]
		const std::vector<std::string> tabs = { "生态规则 (Rules)", "程序化散布 (Scatter)", "道路放坡 (Splines)", "生态遥测 (Stats)" };
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
		Wui::Label(ctx, { startX, curY + 2.0f }, "多因子生态群落决策矩阵 (Multi-factor Biome Rules)", theme.Accent, theme.FontSizeBody);
		curY += 24.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		auto activeScene = host.GetActiveScene();
		BiomePCGComponent* boundCompPtr = nullptr;
		if (activeScene && m_BoundEntity != entt::null && activeScene->GetRegistry().valid(m_BoundEntity))
		{
			boundCompPtr = activeScene->GetRegistry().try_get<BiomePCGComponent>(m_BoundEntity);
		}

		// 标高范围
		Wui::Label(ctx, { startX, curY + 2.0f }, "散布标高区间 (Elevation Bounds m):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.min_elev"), { startX, curY, halfW, 22.0f }, m_MinElevation, 1.0f, 0.0f, 5000.0f, theme))
		{
			m_MinElevation = std::clamp(m_MinElevation, 0.0f, 5000.0f);
			if (boundCompPtr) boundCompPtr->MinElevation = m_MinElevation;
			host.MarkDocumentDirty();
		}
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.max_elev"), { startX + halfW + theme.PadSmall, curY, halfW, 22.0f }, m_MaxElevation, 1.0f, 0.0f, 5000.0f, theme))
		{
			m_MaxElevation = std::clamp(m_MaxElevation, m_MinElevation, 5000.0f);
			if (boundCompPtr) boundCompPtr->MaxElevation = m_MaxElevation;
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 最大坡度与湿度阈值
		Wui::Label(ctx, { startX, curY + 2.0f }, "地表极限坡度 (Slope deg) 与 湿度生成阈值 (Moisture):", theme.Text, theme.FontSizeSmall);
		curY += 20.0f;
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.max_slope"), { startX, curY, halfW, 22.0f }, m_MaxSlope, 0.5f, 0.0f, 89.0f, theme))
		{
			m_MaxSlope = std::clamp(m_MaxSlope, 0.0f, 89.0f);
			if (boundCompPtr) boundCompPtr->MaxSlope = m_MaxSlope;
			host.MarkDocumentDirty();
		}
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.moisture"), { startX + halfW + theme.PadSmall, curY, halfW, 22.0f }, m_MoistureThreshold, 0.02f, 0.0f, 1.0f, theme))
		{
			m_MoistureThreshold = std::clamp(m_MoistureThreshold, 0.0f, 1.0f);
			if (boundCompPtr) boundCompPtr->MoistureThreshold = m_MoistureThreshold;
			host.MarkDocumentDirty();
		}
		curY += 26.0f;

		// 伪随机种子
		Wui::Label(ctx, { startX, curY + 2.0f }, "生态程序化伪随机种子 (Seed):", theme.Text, theme.FontSizeSmall);
		int64_t seedVal = static_cast<int64_t>(m_Seed);
		if (Wui::DragInt(ctx, Wui::HashId("pcg.seed"), { startX + 160.0f, curY, contentW - 160.0f, 22.0f }, seedVal, 0, 9999999, theme))
		{
			m_Seed = static_cast<int32_t>(seedVal);
			if (boundCompPtr) boundCompPtr->Seed = static_cast<uint32_t>(m_Seed);
			host.MarkDocumentDirty();
		}
		curY += 32.0f;

		// 动态可扩展规则图层操作栏
		BiomePCGComponent dummyComp;
		dummyComp.MinElevation = m_MinElevation;
		dummyComp.MaxElevation = m_MaxElevation;
		dummyComp.MaxSlope = m_MaxSlope;
		dummyComp.MoistureThreshold = m_MoistureThreshold;
		dummyComp.Seed = static_cast<uint32_t>(m_Seed);
		dummyComp.AutoDeformTerrain = m_AutoDeformTerrain;

		PCGRuleGraphDesc& graph = PCGBiomeGenerator::GetMutableRuleGraph();
		if (graph.Layers.empty())
		{
			PCGBiomeGenerator::ResetRuleGraphToDefault(boundCompPtr ? *boundCompPtr : dummyComp);
		}

		Wui::Label(ctx, { startX, curY + 2.0f }, "动态生态图层配置 (Dynamic Rule Layers):", theme.Accent, theme.FontSizeSmall);
		curY += 22.0f;

		// 顶部图层操作按钮组: [+ 新增规则层] [↺ 恢复默认规则]
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.add_layer"), { startX, curY, halfW, 26.0f }, "+ 新增生态规则层 (Add Layer)", theme))
		{
			PCGBiomeLayerDesc newLayer;
			newLayer.LayerName = "自定义生态层 " + std::to_string(graph.Layers.size() + 1);
			newLayer.Category = BiomeType::Meadow;
			newLayer.Priority = 1.0f;
			newLayer.ElevationBand = { 0.1f, 0.6f, 0.05f };
			newLayer.SlopeDegrees = { 0.0f, 35.0f, 5.0f };
			newLayer.MoistureBand = { 0.2f, 0.8f, 0.05f };
			graph.Layers.push_back(newLayer);
			m_ExpandedLayerIdx = static_cast<int>(graph.Layers.size() - 1);
			m_StatusFeedback = "已新增生态规则层：" + newLayer.LayerName;
			host.MarkDocumentDirty();
		}
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.reset_rules"), { startX + halfW + theme.PadSmall, curY, halfW, 26.0f }, "↺ 恢复默认五生态规则 (Reset Rules)", theme))
		{
			PCGBiomeGenerator::ResetRuleGraphToDefault(boundCompPtr ? *boundCompPtr : dummyComp);
			m_ExpandedLayerIdx = 0;
			m_StatusFeedback = "已恢复为自适应五大生态默认决策图层。";
			host.MarkDocumentDirty();
		}
		curY += 32.0f;

		// 绘制各个图层卡片
		const char* categoryNames[5] = { "🌲 Forest 森林", "🌼 Meadow 草甸", "⛰️ Rocky 岩山", "🏜️ Desert 荒漠", "❄️ SnowPeak 雪峰" };
		const Wui::WuiColor categoryColors[5] = {
			Wui::WuiColor{ 0.15f, 0.65f, 0.30f, 0.90f },
			Wui::WuiColor{ 0.45f, 0.75f, 0.20f, 0.90f },
			Wui::WuiColor{ 0.55f, 0.55f, 0.55f, 0.90f },
			Wui::WuiColor{ 0.80f, 0.65f, 0.25f, 0.90f },
			Wui::WuiColor{ 0.20f, 0.80f, 0.95f, 0.90f }
		};

		int layerToDelete = -1;
		for (size_t i = 0; i < graph.Layers.size(); ++i)
		{
			auto& layer = graph.Layers[i];
			const bool isExpanded = (m_ExpandedLayerIdx == static_cast<int>(i));
			const int catIdx = std::clamp(static_cast<int>(layer.Category), 0, 4);

			const float cardH = isExpanded ? 180.0f : 36.0f;
			const Wui::WuiRect layerCard { startX, curY, contentW, cardH };
			Wui::PanelBackground(ctx, layerCard, Wui::WuiColor { 0.10f, 0.13f, 0.17f, 0.90f }, 4.0f);
			Wui::HighlightOutline(ctx, layerCard, categoryColors[catIdx], 4.0f, isExpanded ? 1.5f : 1.0f);

			float cardRowY = curY + 6.0f;

			// 标题行：展开/折叠、名称标签、Category徽标、删除按钮
			const Wui::WuiRect foldBtnRect { startX + 6.0f, cardRowY, 20.0f, 22.0f };
			auto foldTheme = theme;
			foldTheme.ButtonBg = { 0, 0, 0, 0 };
			foldTheme.Border = { 0, 0, 0, 0 };
			foldTheme.Text = isExpanded ? theme.Accent : theme.TextMuted;
			if (Wui::Button(ctx, Wui::HashId(("pcg.layer.fold." + std::to_string(i)).c_str()), foldBtnRect, isExpanded ? "▼" : "▶", foldTheme))
			{
				m_ExpandedLayerIdx = isExpanded ? -1 : static_cast<int>(i);
			}

			char titleBuf[128];
			std::snprintf(titleBuf, sizeof(titleBuf), "[%zu] %s", i + 1, layer.LayerName.c_str());
			Wui::Label(ctx, { startX + 30.0f, cardRowY + 2.0f }, titleBuf, theme.Text, theme.FontSizeSmall);

			// 类别徽标
			const float badgeW = 95.0f;
			const Wui::WuiRect catBadgeRect { startX + contentW - badgeW - 55.0f, cardRowY + 1.0f, badgeW, 20.0f };
			Wui::Badge(ctx, catBadgeRect, categoryNames[catIdx], Wui::WuiColor{ 0.15f, 0.20f, 0.25f, 0.6f }, categoryColors[catIdx], theme, 10.0f, true, 2.0f);

			// 删除按钮
			const Wui::WuiRect delBtnRect { startX + contentW - 48.0f, cardRowY, 42.0f, 22.0f };
			auto delTheme = theme;
			delTheme.Text = Wui::WuiColor{ 0.95f, 0.35f, 0.30f, 1.0f };
			if (Wui::Button(ctx, Wui::HashId(("pcg.layer.del." + std::to_string(i)).c_str()), delBtnRect, "删除", delTheme))
			{
				layerToDelete = static_cast<int>(i);
			}

			// 展开后的编辑内容
			if (isExpanded)
			{
				cardRowY += 28.0f;

				// 图层名称输入框
				Wui::Label(ctx, { startX + 10.0f, cardRowY + 2.0f }, "图层名称:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::TextField(ctx, Wui::HashId(("pcg.layer.name." + std::to_string(i)).c_str()),
					{ startX + 70.0f, cardRowY, 150.0f, 22.0f }, layer.LayerName, theme))
				{
					host.MarkDocumentDirty();
				}

				// 生态类型选择
				std::vector<std::string> catOptions = { "Forest", "Meadow", "RockyMountain", "Desert", "SnowPeak" };
				int curCat = static_cast<int>(layer.Category);
				Wui::Label(ctx, { startX + 230.0f, cardRowY + 2.0f }, "生态类型:", theme.TextMuted, theme.FontSizeSmall);
				if (Wui::Combo(ctx, Wui::HashId(("pcg.layer.cat." + std::to_string(i)).c_str()),
					{ startX + 295.0f, cardRowY, contentW - 305.0f, 22.0f }, "", catOptions, curCat, theme))
				{
					layer.Category = static_cast<BiomeType>(curCat);
					host.MarkDocumentDirty();
				}
				cardRowY += 26.0f;

				// 归一化标高带 Min / Max / Falloff
				const float rowColW = (contentW - 20.0f) / 3.0f;
				Wui::Label(ctx, { startX + 10.0f, cardRowY + 2.0f }, "归一化标高 (Elev 0..1):", theme.TextMuted, theme.FontSizeSmall);
				cardRowY += 18.0f;
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.elev_min." + std::to_string(i)).c_str()), { startX + 10.0f, cardRowY, rowColW - 4.0f, 20.0f }, layer.ElevationBand.Min, 0.02f, 0.0f, 1.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.elev_max." + std::to_string(i)).c_str()), { startX + 10.0f + rowColW, cardRowY, rowColW - 4.0f, 20.0f }, layer.ElevationBand.Max, 0.02f, 0.0f, 1.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.elev_fal." + std::to_string(i)).c_str()), { startX + 10.0f + rowColW * 2.0f, cardRowY, rowColW - 4.0f, 20.0f }, layer.ElevationBand.Falloff, 0.01f, 0.0f, 0.5f, theme);
				cardRowY += 24.0f;

				// 坡度区间 Slope Min / Max
				Wui::Label(ctx, { startX + 10.0f, cardRowY + 2.0f }, "坡度区间 (Slope 0..85°):", theme.TextMuted, theme.FontSizeSmall);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.slope_min." + std::to_string(i)).c_str()), { startX + 160.0f, cardRowY, 90.0f, 20.0f }, layer.SlopeDegrees.Min, 0.5f, 0.0f, 85.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.slope_max." + std::to_string(i)).c_str()), { startX + 260.0f, cardRowY, 90.0f, 20.0f }, layer.SlopeDegrees.Max, 0.5f, 0.0f, 85.0f, theme);
				cardRowY += 24.0f;

				// 湿度区间 Moisture Min / Max 与 优先级 Priority
				Wui::Label(ctx, { startX + 10.0f, cardRowY + 2.0f }, "湿度区间 (0..1) 与 优先级:", theme.TextMuted, theme.FontSizeSmall);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.moist_min." + std::to_string(i)).c_str()), { startX + 170.0f, cardRowY, 65.0f, 20.0f }, layer.MoistureBand.Min, 0.02f, 0.0f, 1.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.moist_max." + std::to_string(i)).c_str()), { startX + 240.0f, cardRowY, 65.0f, 20.0f }, layer.MoistureBand.Max, 0.02f, 0.0f, 1.0f, theme);
				Wui::DragFloat(ctx, Wui::HashId(("pcg.layer.prio." + std::to_string(i)).c_str()), { startX + 315.0f, cardRowY, contentW - 325.0f, 20.0f }, layer.Priority, 0.1f, 0.1f, 5.0f, theme);
			}

			curY += cardH + 8.0f;
		}

		if (layerToDelete >= 0 && layerToDelete < static_cast<int>(graph.Layers.size()))
		{
			graph.Layers.erase(graph.Layers.begin() + layerToDelete);
			if (m_ExpandedLayerIdx >= static_cast<int>(graph.Layers.size()))
				m_ExpandedLayerIdx = static_cast<int>(graph.Layers.size()) - 1;
			m_StatusFeedback = "已移除生态规则层。";
			host.MarkDocumentDirty();
		}
	}

	void PCGEditorPanel::RenderScatterTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 2.0f }, "生态散布流水线 (PCG Scatter Pipeline)", theme.Accent, theme.FontSizeBody);
		curY += 24.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		// 散布密度倍数 与 采样步长计算
		auto& graph = PCGBiomeGenerator::GetMutableRuleGraph();
		Wui::Label(ctx, { startX, curY + 2.0f }, "散布密度倍数 (Density):", theme.Text, theme.FontSizeSmall);
		if (Wui::DragFloat(ctx, Wui::HashId("pcg.density"), { startX + 130.0f, curY, halfW - 130.0f, 22.0f }, m_Density, 0.1f, 0.1f, 10.0f, theme))
		{
			m_Density = std::clamp(m_Density, 0.1f, 10.0f);
			graph.SampleSpacing = std::clamp(2.5f / std::sqrt(std::max(0.1f, m_Density)), 0.8f, 10.0f);
			host.MarkDocumentDirty();
		}

		Wui::Label(ctx, { startX + halfW + theme.PadSmall, curY + 2.0f }, "尺度扰动系数:", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.scale_mult"), { startX + halfW + theme.PadSmall + 95.0f, curY, halfW - 95.0f, 22.0f }, m_ScaleMultiplier, 0.05f, 0.2f, 3.0f, theme);
		curY += 26.0f;

		// 真实采样步长提示
		char spacingBuf[128];
		std::snprintf(spacingBuf, sizeof(spacingBuf), "当前实际空间采样步长: %.2f 米 (Grid Sample Spacing)", graph.SampleSpacing);
		Wui::Label(ctx, { startX, curY }, spacingBuf, theme.TextMuted, theme.FontSizeSmall);
		curY += 22.0f;

		// 保护手绘植被复选框
		bool preserve = m_PreserveHandPainted;
		if (Wui::Checkbox(ctx, Wui::HashId("pcg.chk.preserve_hand"), { startX, curY, contentW, 22.0f },
			"☑ 保护手绘植被 (增量烘焙仅替换 PCG 实例，不改动笔刷手绘植物)", preserve, theme))
		{
			m_PreserveHandPainted = preserve;
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 按钮 1: 解算生态点云并同步视口预览
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.generate_cloud"), { startX, curY, halfW, 26.0f }, "✦ 1. 解算点云并同步视口 (Preview)", theme))
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
				pcgComp.MinElevation = m_MinElevation;
				pcgComp.MaxElevation = m_MaxElevation;
				pcgComp.MaxSlope = m_MaxSlope;
				pcgComp.MoistureThreshold = m_MoistureThreshold;
				pcgComp.Seed = static_cast<uint32_t>(m_Seed);
				pcgComp.AutoDeformTerrain = m_AutoDeformTerrain;

				PCGBiomeGenerator::GenerateBiomePoints(*terrain, pcgComp, m_CachedPoints, terrainWorld);

				// 同步给视口共享预览点云，3D 视口立即全局绘制五彩生态点云
				PCGBiomeGenerator::GetSharedPreviewPoints() = m_CachedPoints;
				PCGBiomeGenerator::SetSharedPreviewActive(true);

				for (int b = 0; b < 5; ++b) m_DominantCounts[b] = 0;
				for (const auto& pt : m_CachedPoints)
				{
					int idx = static_cast<int>(pt.Biome);
					if (idx >= 0 && idx < 5) m_DominantCounts[idx]++;
				}
				m_StatusFeedback = "点云解算完成：成功生成 " + std::to_string(m_CachedPoints.size()) + " 个生态空间采样点并已同步视口！";
				host.MarkDocumentDirty();
			}
		}

		// 按钮 2: 烘焙至场景植被层
		if (Wui::Button(ctx, Wui::HashId("pcg.btn.bake_foliage"), { startX + halfW + theme.PadSmall, curY, halfW, 26.0f }, "✦ 2. 烘焙至场景植被层 (Bake)", theme))
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
						pcgComp.MinElevation = m_MinElevation;
						pcgComp.MaxElevation = m_MaxElevation;
						pcgComp.MaxSlope = m_MaxSlope;
						pcgComp.MoistureThreshold = m_MoistureThreshold;
						pcgComp.Seed = static_cast<uint32_t>(m_Seed);
						pcgComp.AutoDeformTerrain = m_AutoDeformTerrain;

						PCGBiomeGenerator::GenerateBiomePoints(*terrain, pcgComp, m_CachedPoints, terrainWorld);
						PCGBiomeGenerator::GetSharedPreviewPoints() = m_CachedPoints;
						PCGBiomeGenerator::SetSharedPreviewActive(true);
					}

					const uint32_t spawned = PCGBiomeGenerator::PopulateFoliageFromBiomePoints(
						m_CachedPoints, *folData, m_ScaleMultiplier, m_PreserveHandPainted);
					m_StatusFeedback = "烘焙完成：已将 " + std::to_string(spawned) + " 处植被实例写入场景（手绘植被已受保护）！";
					host.MarkDocumentDirty();
				}
				else
				{
					m_StatusFeedback = "未在场景中检测到有效植被组件。";
				}
			}
		}
		curY += 32.0f;

		// 两个独立清理按钮
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
				m_StatusFeedback = "已清除 " + std::to_string(pcgCleared) + " 处 PCG 生成实例，手绘植物完好无损。";
				host.MarkDocumentDirty();
			}
		}

		if (Wui::Button(ctx, Wui::HashId("pcg.btn.clear_all"), { startX + halfW + theme.PadSmall, curY, halfW, 24.0f }, "✖ 清空全图所有植被与点云", theme))
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
				for (int b = 0; b < 5; ++b) m_DominantCounts[b] = 0;
				m_StatusFeedback = "已清空全图所有植被实例与视口预览点云。";
				host.MarkDocumentDirty();
			}
		}
		curY += 30.0f;
	}

	void PCGEditorPanel::RenderSplineRoadsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host)
	{
		Wui::Label(ctx, { startX, curY + 2.0f }, "样条道路地形自动填挖平整与走廊净空", theme.Accent, theme.FontSizeBody);
		curY += 24.0f;

		const float halfW = (contentW - theme.PadSmall) * 0.5f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "道路净宽 (Road Width m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.road.width"), { startX + 130.0f, curY, halfW - 130.0f, 22.0f }, m_RoadWidth, 0.5f, 1.0f, 50.0f, theme);

		Wui::Label(ctx, { startX + halfW + theme.PadSmall, curY + 2.0f }, "开槽切深 (Cut Depth m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.road.cut"), { startX + halfW + theme.PadSmall + 130.0f, curY, halfW - 130.0f, 22.0f }, m_RoadCutDepth, 0.1f, 0.0f, 10.0f, theme);
		curY += 26.0f;

		Wui::Label(ctx, { startX, curY + 2.0f }, "边坡过渡放坡宽度 (m):", theme.Text, theme.FontSizeSmall);
		Wui::DragFloat(ctx, Wui::HashId("pcg.road.slope"), { startX + 130.0f, curY, halfW - 130.0f, 22.0f }, m_RoadSlopeTransition, 0.5f, 0.5f, 30.0f, theme);

		bool clearFoliage = m_ClearFoliageOnRoad;
		if (Wui::Checkbox(ctx, Wui::HashId("pcg.road.clear_foliage"), { startX + halfW + theme.PadSmall, curY, halfW, 22.0f }, "自动清除路面植被走廊", clearFoliage, theme))
		{
			m_ClearFoliageOnRoad = clearFoliage;
			host.MarkDocumentDirty();
		}
		curY += 28.0f;

		// 检查场景中的样条与子控制点
		auto activeScene = host.GetActiveScene();
		size_t detectedSplines = 0;
		size_t totalKnots = 0;
		bool splineLacksKnots = false;
		entt::entity emptySplineEntity = entt::null;

		if (activeScene)
		{
			for (auto [sh, sc] : activeScene->GetRegistry().view<SplineComponent>().each())
			{
				detectedSplines++;
				const auto& children = Hierarchy::ChildrenOf(activeScene->GetRegistry(), sh);
				totalKnots += children.size();
				if (children.empty())
				{
					splineLacksKnots = true;
					emptySplineEntity = sh;
				}
			}
		}

		if (splineLacksKnots)
		{
			const Wui::WuiRect warnRect { startX, curY, contentW, 40.0f };
			Wui::PanelBackground(ctx, warnRect, Wui::WuiColor{ 0.25f, 0.18f, 0.08f, 0.85f }, 4.0f);
			Wui::HighlightOutline(ctx, warnRect, Wui::WuiColor{ 0.95f, 0.70f, 0.20f, 0.85f }, 4.0f, 1.0f);
			Wui::Label(ctx, { startX + 8.0f, curY + 4.0f }, "⚠️ 检测到场景样条道路尚未关联子实体控制点 (Road Knots)！", theme.Warning, theme.FontSizeSmall);

			if (Wui::Button(ctx, Wui::HashId("pcg.btn.gen_default_knots"), { startX + contentW - 170.0f, curY + 18.0f, 160.0f, 20.0f }, "+ 自动生成 3 个控制点", theme))
			{
				if (activeScene && emptySplineEntity != entt::null)
				{
					const glm::vec3 defaultKnotLocs[3] = {
						glm::vec3(-25.0f, 0.5f, -18.0f),
						glm::vec3(0.0f, 1.2f, -12.0f),
						glm::vec3(25.0f, 2.0f, -6.0f)
					};
					for (int k = 0; k < 3; ++k)
					{
						Entity knot = Entity::CreateEntity(activeScene.get(), "Road Knot K" + std::to_string(k));
						knot.AddComponent<TransformComponent>().Location = defaultKnotLocs[k];
						auto& hc = knot.AddComponent<HierarchyComponent>();
						hc.Parent = emptySplineEntity;
						hc.InheritTransform = true;
					}
					m_StatusFeedback = "已为样条道路自动生成 3 个默认控制点实体！";
					host.MarkDocumentDirty();
				}
			}
			curY += 46.0f;
		}

		char splineStatBuf[128];
		std::snprintf(splineStatBuf, sizeof(splineStatBuf), "场景样条道路: %zu 条  |  子实体控制点总数: %zu 处", detectedSplines, totalKnots);
		Wui::Label(ctx, { startX, curY }, splineStatBuf, theme.TextMuted, theme.FontSizeSmall);
		curY += 22.0f;

		if (Wui::Button(ctx, Wui::HashId("pcg.btn.exec_road_grading"), { startX, curY, contentW, 26.0f }, "✦ 沿场景所有样条道路执行地形挖填与净空 (Execute Road Grading)", theme))
		{
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
					glm::mat4 splineWorldMat(1.0f);
					if (const auto* parentXf = activeScene->GetRegistry().try_get<TransformComponent>(sh))
						splineWorldMat = parentXf->GetLocalMatrix();
					if (const auto* parentWxf = activeScene->GetRegistry().try_get<WorldTransformComponent>(sh))
						splineWorldMat = parentWxf->Matrix;

					const auto& children = Hierarchy::ChildrenOf(activeScene->GetRegistry(), sh);
					for (entt::entity ch : children)
					{
						if (const auto* xf = activeScene->GetRegistry().try_get<TransformComponent>(ch))
							knots.push_back(glm::vec3(splineWorldMat * glm::vec4(xf->Location, 1.0f)));
					}
					if (knots.size() >= 2)
					{
						for (const auto& k : knots) spline.AddPoint(k);
						spline.SetClosedLoop(splineComp.ClosedLoop != 0);
						spline.UpdateTangents();

						if (terrain)
						{
							PCGBiomeGenerator::DeformTerrainAlongSpline(
								*terrain, spline, m_RoadWidth, m_RoadCutDepth, m_RoadSlopeTransition, terrainWorld);
						}
						if (folData && m_ClearFoliageOnRoad)
						{
							foliageCleared += PCGBiomeGenerator::ClearFoliageAlongSpline(*folData, spline, m_RoadWidth + 1.5f);
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
		Wui::Label(ctx, { startX, curY + 2.0f }, "生态群落全局遥测与预算分析 (Biome Telemetry)", theme.Accent, theme.FontSizeBody);
		curY += 24.0f;

		const size_t totalPoints = m_CachedPoints.size();
		char summaryBuf[128];
		std::snprintf(summaryBuf, sizeof(summaryBuf), "当前解算采样点总数: %zu 处  |  随机种子: %d", totalPoints, m_Seed);
		Wui::Label(ctx, { startX, curY }, summaryBuf, theme.Text, theme.FontSizeSmall);
		curY += 22.0f;

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
