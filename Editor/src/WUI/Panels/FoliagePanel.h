#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Foliage/Foliage.h"
#include "World/Scene/Entity.h"

#include <memory>
#include <string>
#include <vector>

namespace World
{
	class TerrainData;

	enum class FoliageBrushTool : uint8_t
	{
		Paint = 0,
		Erase = 1,
		Settings = 2
	};

	enum class FoliageEraseTarget : uint8_t
	{
		AllSpecies = 0,
		ActiveTypeOnly = 1
	};

	class FoliagePanel final : public EditorPanel
	{
	public:
		FoliagePanel();
		~FoliagePanel() override = default;

		const char* Id() const override { return "foliage_editor"; }
		const char* Title() const override { return "3D 植被与散布 (Foliage & Scatter)"; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		FoliageBrushTool GetTool() const { return m_Tool; }
		void SetTool(FoliageBrushTool tool) { m_Tool = tool; m_ToolIdx = static_cast<int>(tool); }
		float GetBrushRadius() const { return m_BrushRadius; }
		void SetBrushRadius(float r) { m_BrushRadius = std::clamp(r, 0.5f, 100.0f); }
		float GetBrushDensity() const { return m_BrushDensity; }
		void SetBrushDensity(float d) { m_BrushDensity = std::clamp(d, 0.1f, 10.0f); }
		size_t GetActiveTypeIndex() const { return static_cast<size_t>(std::max(0, m_ActiveTypeIdx)); }
		void SetActiveTypeIndex(size_t idx) { m_ActiveTypeIdx = static_cast<int>(idx); }

		// 多物种混刷与擦除模式查询
		std::vector<size_t> GetActivePaintingTypes() const;
		FoliageEraseTarget GetEraseTarget() const { return m_EraseTarget; }
		void SetEraseTarget(FoliageEraseTarget target) { m_EraseTarget = target; }

		FoliageData* GetActiveFoliage() { return m_Foliage.get(); }
		const FoliageData* GetActiveFoliage() const { return m_Foliage.get(); }

		void ResetDefaultPalette();
		bool LoadFoliageAsset(const std::string& path);
		bool SaveFoliageAsset(const std::string& path);
		bool SaveCurrentFoliage();
		const std::string& GetLoadedPath() const { return m_LoadedPath; }

		// 批处理与统计
		size_t GetInstanceCountByType(size_t typeIndex) const;
		void RealignToTerrain(const TerrainData* terrain, const glm::mat4& terrainWorld);

	private:
		void RenderPaintTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderEraseTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderSettingsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);

		FoliageBrushTool m_Tool { FoliageBrushTool::Paint };
		int m_ToolIdx { 0 };
		FoliageEraseTarget m_EraseTarget { FoliageEraseTarget::AllSpecies };

		// 笔刷参数
		float m_BrushRadius { 15.0f };
		float m_BrushDensity { 2.0f };
		int m_ActiveTypeIdx { 0 };

		// 属性编辑内联缓存
		int m_CachedTypeIdx { -1 };
		std::string m_EditName;
		std::string m_EditMeshPath;
		std::string m_EditMatPath;

		Entity m_BoundEntity;
		std::shared_ptr<FoliageData> m_Foliage;
		std::string m_LoadedPath;
		std::string m_StatusFeedback;
	};
}
