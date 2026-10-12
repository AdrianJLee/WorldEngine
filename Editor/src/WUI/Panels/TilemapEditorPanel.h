#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Renderer/Tilemap.h"
#include "World/Renderer/TextureAtlas.h"
#include "World/RHI/Rhi.h"
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <algorithm>

namespace World
{
	enum class TilePaintTool : uint8_t
	{
		Brush = 0,
		Eraser = 1,
		FloodFill = 2,
		RectFill = 3,
		CollisionToggle = 4
	};

	class TilemapEditorPanel final : public EditorPanel
	{
	public:
		explicit TilemapEditorPanel(std::string logicalPath);
		~TilemapEditorPanel() override = default;

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		const std::string& LogicalPath() const noexcept { return m_LogicalPath; }
		bool HasUnsavedChanges() const noexcept { return m_Dirty; }

		bool Reload(std::string* message = nullptr);
		bool Save(std::string* message = nullptr);

		void PaintCell(uint32_t x, uint32_t y, uint16_t tileId, bool setSolid = false);
		void FillRectCells(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint16_t tileId, bool setSolid = false);
		void FloodFillCells(uint32_t startX, uint32_t startY, uint16_t tileId, bool setSolid = false);
		void ClearAll();

		std::shared_ptr<Tilemap> GetTilemap() const noexcept { return m_Tilemap; }
		std::shared_ptr<TextureAtlas> GetAtlas() const noexcept { return m_Atlas; }

		int GetSelectedPaletteTile() const noexcept { return m_SelectedTileId; }
		void SetSelectedPaletteTile(int tileId) noexcept { m_SelectedTileId = tileId; }

		TilePaintTool GetActiveTool() const noexcept { return m_ActiveTool; }
		void SetActiveTool(TilePaintTool tool) noexcept { m_ActiveTool = tool; }

		float GetZoom() const noexcept { return m_Zoom; }
		void SetZoom(float zoom) noexcept { m_Zoom = std::clamp(zoom, 0.2f, 8.0f); }
		void ResetView() noexcept { m_Zoom = 1.0f; m_PanX = 0.0f; m_PanY = 0.0f; }

	private:
		std::filesystem::path ResolvePhysicalPath() const;
		void EnsureAtlasAndTextureLoaded();

		std::string m_LogicalPath;
		std::string m_PanelId;
		std::string m_PanelTitle;

		std::shared_ptr<Tilemap> m_Tilemap;
		std::shared_ptr<TextureAtlas> m_Atlas;

		std::string m_EditName;
		std::string m_EditAtlasPath;
		int64_t m_EditGridW { 16 };
		int64_t m_EditGridH { 12 };
		float m_EditCellW { 1.0f };
		float m_EditCellH { 1.0f };

		int m_SelectedTileId { 1 }; // 1..N = SubTexture index + 1
		TilePaintTool m_ActiveTool { TilePaintTool::Brush };
		bool m_PaintSolid { false };
		bool m_ShowCollisionOverlay { true };
		bool m_Dirty { false };
		std::string m_StatusFeedback;

		// 2D 视口画布交互: 缩放与平移 (Zoom & Pan)
		float m_Zoom { 1.0f };
		float m_PanX { 0.0f };
		float m_PanY { 0.0f };
		bool m_IsPanning { false };
		glm::vec2 m_PanStartMouse { 0.0f, 0.0f };
		float m_PanStartPanX { 0.0f };
		float m_PanStartPanY { 0.0f };

		bool m_HasRectStart { false };
		uint32_t m_RectStartX { 0 };
		uint32_t m_RectStartY { 0 };

		float m_PaletteScrollY { 0.0f };
		uint64_t m_UiTextureId { 0 };
		Rhi::Handle<Rhi::Texture> m_GpuTexture { nullptr };
		std::string m_LoadedAtlasPath;
		std::string m_LoadedTexturePath;
	};
}