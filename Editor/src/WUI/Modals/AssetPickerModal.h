#pragma once

#include "World/WUI/WuiContext.h"
#include "World/Asset/AssetThumbnailCache.h"

#include <string>
#include <string_view>
#include <vector>
#include <functional>

namespace World
{
	class PanelHost;

	// 资产选择器模态弹窗 (AssetPickerModal)
	// 真实项目资产驱动，支持按类别过滤、模糊关键词搜索、实时大图缩略图网格与列表双视图
	class AssetPickerModal
	{
	public:
		AssetPickerModal();
		~AssetPickerModal() = default;

		void Open(std::string_view title, AssetCategory filterCat, std::function<void(const std::string&)> onSelect);
		void Close();
		bool IsOpen() const { return m_IsOpen; }

		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);

		const std::string& GetSelectedItem() const { return m_SelectedItem; }
		size_t AssetCount() const { return m_AllAssets.size(); }

	private:
		struct AssetItem
		{
			std::string Path;
			std::string Name;
			AssetCategory Category = AssetCategory::Unknown;
		};

		void PopulateProjectAssets();
		bool MatchesFilter(const AssetItem& item) const;

		bool m_IsOpen = false;
		std::string m_Title = "Select Asset";
		AssetCategory m_FilterCategory = AssetCategory::Unknown;
		std::function<void(const std::string&)> m_OnSelect;

		std::string m_SearchQuery;
		std::string m_SelectedItem;
		int m_SelectedCategoryIndex = 0;
		int m_ViewMode = 0; // 0 = Grid, 1 = List

		std::vector<AssetItem> m_AllAssets;
		std::vector<std::string> m_CategoryNames;
		double m_LastClickTime = 0.0;
		std::string m_LastClickedItem;
		float m_ScrollOffset = 0.0f;
	};
}
