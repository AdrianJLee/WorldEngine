#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Renderer/TextureAtlas.h"
#include "World/RHI/Rhi.h"
#include <string>
#include <vector>
#include <memory>

namespace World
{
	class SpriteAtlasEditorPanel final : public EditorPanel
	{
	public:
		explicit SpriteAtlasEditorPanel(std::string logicalPath);
		~SpriteAtlasEditorPanel() override = default;

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		const std::string& LogicalPath() const noexcept { return m_LogicalPath; }
		bool HasUnsavedChanges() const noexcept { return m_Dirty; }

		bool Reload(std::string* message = nullptr);
		bool Save(std::string* message = nullptr);

		void AddSprite(const std::string& name, const glm::vec4& pixelRect,
			const glm::vec2& pivot = { 0.5f, 0.5f }, const glm::vec4& border = { 0.0f, 0.0f, 0.0f, 0.0f });
		bool DeleteSprite(size_t index);
		void AutoGridSlice(float cellW, float cellH);

		std::shared_ptr<TextureAtlas> GetAtlas() const noexcept { return m_Atlas; }

	private:
		void SyncFromSelected();
		void CommitToSelected();
		std::filesystem::path ResolvePhysicalPath() const;
		void EnsureTextureLoaded();

		std::string m_LogicalPath;
		std::string m_PanelId;
		std::string m_PanelTitle;

		std::shared_ptr<TextureAtlas> m_Atlas;
		int m_SelectedIndex { -1 };
		bool m_Dirty { false };
		std::string m_StatusFeedback;

		std::string m_EditTexturePath;
		int64_t m_EditAtlasW { 256 };
		int64_t m_EditAtlasH { 256 };
		float m_GridCellW { 64.0f };
		float m_GridCellH { 64.0f };

		std::string m_EditName;
		float m_EditRectX { 0.0f };
		float m_EditRectY { 0.0f };
		float m_EditRectW { 32.0f };
		float m_EditRectH { 32.0f };
		float m_EditPivotX { 0.5f };
		float m_EditPivotY { 0.5f };
		float m_EditBorderL { 0.0f };
		float m_EditBorderR { 0.0f };
		float m_EditBorderT { 0.0f };
		float m_EditBorderB { 0.0f };

		float m_ListScrollY { 0.0f };
		uint64_t m_UiTextureId { 0 };
		Rhi::Handle<Rhi::Texture> m_GpuTexture { nullptr };
		std::string m_LoadedTexturePath;
	};
}
