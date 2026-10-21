#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Foliage/Foliage.h"

#include <memory>
#include <string>
#include <vector>

namespace World
{
	enum class FoliageBrushTool : uint8_t
	{
		Paint = 0,
		Erase = 1,
		Settings = 2
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
		size_t GetActiveTypeIndex() const { return static_cast<size_t>(m_ActiveTypeIdx); }
		void SetActiveTypeIndex(size_t idx) { m_ActiveTypeIdx = static_cast<int>(idx); }

		FoliageData* GetActiveFoliage() { return m_Foliage.get(); }
		const FoliageData* GetActiveFoliage() const { return m_Foliage.get(); }

		void ResetDefaultPalette();
		bool LoadFoliageAsset(const std::string& path);
		bool SaveFoliageAsset(const std::string& path);

	private:
		void RenderPaintTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderEraseTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);
		void RenderSettingsTab(Wui::WuiContext& ctx, float startX, float& curY, float contentW, const Wui::WuiTheme& theme, PanelHost& host);

		FoliageBrushTool m_Tool { FoliageBrushTool::Paint };
		int m_ToolIdx { 0 };

		// 笔刷参数
		float m_BrushRadius { 15.0f };
		float m_BrushDensity { 2.0f };
		int m_ActiveTypeIdx { 0 };

		// 选中的植被类型参数微调
		float m_ScaleMin { 0.8f };
		float m_ScaleMax { 1.2f };
		float m_SlopeMax { 40.0f };
		float m_CullDist { 150.0f };
		bool m_AlignNormal { true };

		std::shared_ptr<FoliageData> m_Foliage;
		std::string m_LoadedPath;
		std::string m_StatusFeedback;
	};
}
