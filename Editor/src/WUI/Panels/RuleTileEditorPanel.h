#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Renderer/RuleTile.h"
#include "World/Renderer/TextureAtlas.h"
#include "World/Renderer/Tilemap.h"
#include <string>
#include <memory>
#include <vector>

namespace World
{
	class RuleTileEditorPanel final : public EditorPanel
	{
	public:
		explicit RuleTileEditorPanel(std::string logicalPath);
		~RuleTileEditorPanel() override = default;

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		bool Save(std::string* message = nullptr);
		bool Reload(std::string* message = nullptr);

	private:
		std::filesystem::path ResolvePhysicalPath() const;
		void EnsureAtlasLoaded();

		std::string m_LogicalPath;
		std::string m_PanelId;
		std::string m_PanelTitle;

		std::shared_ptr<RuleTile> m_RuleTile;
		std::shared_ptr<TextureAtlas> m_Atlas;
		uint64_t m_AtlasTextureId { 0 };
		std::string m_LoadedAtlasPath;

		int m_SelectedTileId { 1 };
		int m_SelectedRuleIndex { 0 };
		float m_RulesScroll { 0.0f };
		float m_PaletteScroll { 0.0f };
		bool m_Dirty { false };
		std::string m_StatusFeedback;

		// 8x8 交互式测试沙盒
		uint8_t m_Sandbox[8 * 8] { 0 };
		void InitSandbox();
	};
}
