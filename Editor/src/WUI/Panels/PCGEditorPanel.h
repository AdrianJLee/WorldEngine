#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Environment/PCGBiome.h"
#include "World/Scene/Entity.h"

#include <memory>
#include <string>
#include <vector>

namespace World
{
	enum class PCGEditorTab : uint8_t
	{
		BiomeRules = 0,    // 生态群落规则矩阵
		Scatter = 1,       // 程序化散布流水线
		SplineRoads = 2,   // 样条道路地形平整与走廊
		Telemetry = 3      // 生态遥测与性能预算
	};

	class PCGEditorPanel final : public EditorPanel
	{
	public:
		PCGEditorPanel();
		~PCGEditorPanel() override = default;

		const char* Id() const override { return "pcg_editor"; }
		const char* Title() const override { return "PCG 生态与世界生成 (PCG Biome Generator)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		void RenderBiomeRulesTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderScatterTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderSplineRoadsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderTelemetryTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);

		PCGEditorTab m_ActiveTab { PCGEditorTab::BiomeRules };
		int m_ActiveTabIdx { 0 };

		// 生态规则参数
		float m_MinElevation { 0.0f };
		float m_MaxElevation { 1000.0f };
		float m_MaxSlope { 35.0f };
		float m_MoistureThreshold { 0.35f };
		int32_t m_Seed { 1337 };

		// 散布流水线参数
		float m_Density { 1.0f };
		float m_ScaleMultiplier { 1.0f };

		// 样条道路平整参数
		float m_RoadWidth { 6.0f };
		float m_RoadCutDepth { 0.5f };
		float m_RoadSlopeTransition { 4.0f };
		bool m_ClearFoliageOnRoad { true };

		// 缓存数据与遥测
		std::vector<PCGSpawnPoint> m_CachedPoints;
		size_t m_DominantCounts[5] { 0 };
		std::string m_StatusFeedback;
	};
}
