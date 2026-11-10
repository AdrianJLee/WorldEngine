#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Level/Core/WorldSpace.h"
#include "World/Level/Runtime/DataLayerManager.h"

namespace World
{
	class LevelStreamingPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "level_streaming"; }
		const char* Title() const override { return "Level Streaming"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		void EnsureInitialized();

		Level::WorldSpace m_WorldSpace;
		Level::DataLayerManager m_DataLayerManager;
		bool m_Initialized = false;
	};
}
