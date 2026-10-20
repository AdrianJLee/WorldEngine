#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Terrain/Terrain.h"

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

		// 笔刷参数查询
		TerrainToolTab GetToolTab() const { return m_ActiveTab; }
		TerrainSculptMode GetSculptMode() const { return m_SculptMode; }
		TerrainBrushFalloff GetBrushFalloff() const { return m_Falloff; }
		float GetBrushRadius() const { return m_BrushRadius; }
		float GetBrushStrength() const { return m_BrushStrength; }
		float GetTargetHeight() const { return m_TargetHeight; }
		uint32_t GetActivePaintLayer() const { return m_ActivePaintLayer; }

		// 活跃地形数据
		TerrainData* GetActiveTerrain() { return m_Terrain.get(); }
		const TerrainData* GetActiveTerrain() const { return m_Terrain.get(); }

		void CreateDefaultTerrain(uint32_t width = 256, uint32_t height = 256, float cellSize = 1.0f);
		bool LoadTerrain(const std::string& path);
		bool SaveTerrain(const std::string& path);

	private:
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

		float m_BrushRadius { 10.0f };
		float m_BrushStrength { 0.5f };
		float m_TargetHeight { 0.0f };

		// 混绘状态
		uint32_t m_ActivePaintLayer { 0 };
		int m_PaintLayerIdx { 0 };

		// 新建/设置状态
		int64_t m_NewResolution { 256 };
		float m_NewCellSize { 1.0f };

		std::shared_ptr<TerrainData> m_Terrain;
		std::string m_LoadedPath;
		std::string m_StatusFeedback;
		bool m_Dirty { false };
	};
}
