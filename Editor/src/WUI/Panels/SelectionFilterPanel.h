#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Scene/SelectionFilter.h"
#include <vector>

namespace World
{
	class SelectionFilterPanel final : public EditorPanel
	{
	public:
		SelectionFilterPanel();
		~SelectionFilterPanel() override = default;

		const char* Id() const override { return "selection_filter"; }
		const char* Title() const override { return "Selection Filter"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		const SelectionFilter& GetFilter() const { return m_Filter; }
		SelectionFilter& GetFilterMut() { return m_Filter; }
		void SetFilter(const SelectionFilter& filter) { m_Filter = filter; }

	private:
		struct LayerItem
		{
			const char* Name;
			uint32_t Mask;
			const char* Description;
		};

		SelectionFilter m_Filter;
		std::vector<LayerItem> m_Layers;
	};
}
