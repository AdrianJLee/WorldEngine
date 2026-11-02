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
		Rules = 0,       // 规则管线 (Rules: 用户图层与模型条目)
		Scatter = 1,     // 空间散布 (Scatter: 点云解算与烘焙)
		SplineRoads = 2, // 道路放坡 (Splines: 样条避让与地形成型)
		Scripting = 3,   // 脚本算子 (Scripting: Luau自定义算法沙盒)
		Telemetry = 4    // 遥测诊断 (Stats: 密度与内存)
	};

	class PCGEditorPanel final : public EditorPanel
	{
	public:
		PCGEditorPanel();
		~PCGEditorPanel() override = default;

		const char* Id() const override { return "pcg_editor"; }
		const char* Title() const override { return "PCG 生态生成器工作台 (PCG Graph Workbench)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		// 资产与实体加载公共方法 (实现全场景闭环跳转)
		void LoadGraphAsset(const std::string& logicalOrPhysicalPath);
		void BindEntity(entt::entity e);

		// 资产下拉选缓存清单管理 (面板构造/显式刷新时扫描，杜绝每帧遍历磁盘 I/O 导致卡顿)
		void RefreshAssetCaches();

	private:
		float EstimateTabContentHeight() const;

		void RenderRulesTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderScatterTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderSplineRoadsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderScriptingTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderTelemetryTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);

		PCGEditorTab m_ActiveTab { PCGEditorTab::Rules };
		int m_ActiveTabIdx { 0 };

		// 场景双向绑定实体
		entt::entity m_BoundEntity { entt::null };

		// 规则图与脚本算子路径
		std::string m_GraphAssetPath { "assets/pcg/custom_rules.wpcggraph" };
		std::string m_ScriptOperatorPath { "assets/scripts/pcg/custom_filter.luau" };

		// 空间控制参数 (双向绑定)
		float m_BoundsRadius { 50.0f };
		float m_BoundsHeight { 40.0f };
		float m_DensityMultiplier { 1.0f };
		float m_ClearanceMargin { 0.0f };
		int32_t m_Seed { 1337 };
		uint8_t m_AutoDeformTerrain { 0 };
		uint8_t m_LivePreview { 1 };

		// 滚动条偏移 (支持任意多图层与模型条目流畅滚动)
		float m_TabScrollY { 0.0f };

		// 动态图层折叠与选中索引
		int m_ExpandedLayerIdx { 0 };

		// 散布流水线参数
		float m_ScaleMultiplier { 1.0f };
		bool m_PreserveHandPainted { true };

		// 样条道路平整参数
		float m_RoadWidth { 6.0f };
		float m_RoadCutDepth { 0.5f };
		float m_RoadSlopeTransition { 4.0f };
		bool m_ClearFoliageOnRoad { true };

		// 缓存数据与遥测
		std::vector<PCGSpawnPoint> m_CachedPoints;
		std::string m_StatusFeedback;

		// 资产下拉选缓存清单 (在构造/刷新时加载一次，绝不每帧扫描磁盘)
		std::vector<std::string> m_CachedMeshOptions;
		std::vector<std::string> m_CachedMaterialOptions;
		std::vector<std::string> m_CachedRuleGraphOptions;
		std::vector<std::string> m_CachedScriptOptions;
		bool m_AssetCachesInitialized { false };
	};
}
