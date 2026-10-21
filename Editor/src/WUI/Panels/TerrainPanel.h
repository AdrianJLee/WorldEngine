#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Terrain/Terrain.h"
#include "World/Terrain/TerrainRenderer.h"
#include "World/Scene/Entity.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace World
{
	enum class TerrainToolTab : uint8_t
	{
		Sculpt = 0,
		Paint = 1,
		Settings = 2
	};

	class TerrainPanel final : public EditorPanel
	{
	public:
		TerrainPanel();
		~TerrainPanel() override = default;

		const char* Id() const override { return "terrain_editor"; }
		const char* Title() const override { return "3D 地貌与地形 (Terrain & Landscape)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		// 笔刷参数查询与配置
		TerrainToolTab GetToolTab() const { return m_ActiveTab; }
		void SetToolTab(TerrainToolTab tab) { m_ActiveTab = tab; m_ActiveTabIdx = static_cast<int>(tab); }

		TerrainSculptMode GetSculptMode() const { return m_SculptMode; }
		void SetSculptMode(TerrainSculptMode mode) { m_SculptMode = mode; m_SculptModeIdx = static_cast<int>(mode); }

		TerrainBrushFalloff GetBrushFalloff() const { return m_Falloff; }
		void SetBrushFalloff(TerrainBrushFalloff falloff) { m_Falloff = falloff; m_FalloffIdx = static_cast<int>(falloff); }

		float GetBrushRadius() const { return m_BrushRadius; }
		void SetBrushRadius(float radius) { m_BrushRadius = std::clamp(radius, 0.5f, 200.0f); }

		float GetBrushStrength() const { return m_BrushStrength; }
		void SetBrushStrength(float strength) { m_BrushStrength = std::clamp(strength, 0.01f, 1.0f); }

		float GetBrushFalloffFactor() const { return m_BrushFalloffFactor; }
		void SetBrushFalloffFactor(float f) { m_BrushFalloffFactor = std::clamp(f, 0.0f, 1.0f); }

		float GetTargetHeight() const { return m_TargetHeight; }
		void SetTargetHeight(float h) { m_TargetHeight = h; }

		uint32_t GetActivePaintLayer() const { return m_ActivePaintLayer; }
		void SetActivePaintLayer(uint32_t layer) { m_ActivePaintLayer = layer; m_PaintLayerIdx = static_cast<int>(layer); }

		// 活跃地形数据与场景实体绑定
		Entity GetBoundEntity() const { return m_BoundEntity; }
		TerrainData* GetActiveTerrain() { return m_Terrain.get(); }
		const TerrainData* GetActiveTerrain() const { return m_Terrain.get(); }
		std::shared_ptr<TerrainData> GetActiveTerrainPtr() { return m_Terrain; }

		void CreateDefaultTerrain(uint32_t width = 256, uint32_t height = 256, float cellSize = 1.0f);
		bool LoadTerrain(const std::string& path);
		bool SaveTerrain(const std::string& path);

	private:
		void RenderWizard(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderSculptTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderPaintTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderSettingsTab(Wui::WuiContext& ctx, float startX, float& currentY, float contentWidth, const Wui::WuiTheme& theme, PanelHost& host);

		TerrainToolTab m_ActiveTab { TerrainToolTab::Sculpt };
		int m_ActiveTabIdx { 0 };

		// 雕刻状态
		TerrainSculptMode m_SculptMode { TerrainSculptMode::Raise };
		int m_SculptModeIdx { 0 };
		TerrainBrushFalloff m_Falloff { TerrainBrushFalloff::Smooth };
		int m_FalloffIdx { 1 };

		float m_BrushRadius { 15.0f };
		float m_BrushStrength { 0.5f };
		float m_BrushFalloffFactor { 0.5f };
		float m_TargetHeight { 0.0f };

		// 混绘状态
		uint32_t m_ActivePaintLayer { 0 };
		int m_PaintLayerIdx { 0 };

		// 新建向导状态
		int64_t m_WizardResolution { 255 };
		float m_WizardCellSize { 1.0f };
		float m_WizardHeightScale { 50.0f };

		Entity m_BoundEntity;
		std::shared_ptr<TerrainData> m_Terrain;
		std::string m_LoadedPath;
		std::string m_StatusFeedback;
		bool m_Dirty { false };
	};
}
