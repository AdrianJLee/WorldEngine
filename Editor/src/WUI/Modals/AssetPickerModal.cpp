#include "wldpch.h"
#include "WUI/Modals/AssetPickerModal.h"
#include "WUI/Common/EditorPanel.h"
#include "World/WUI/WuiWidgets.h"
#include <algorithm>
#include <cctype>

namespace World
{
	AssetPickerModal::AssetPickerModal()
		: m_CategoryNames({ "All", "Texture", "Material", "Mesh", "Prefab", "Script", "Audio", "Level" })
	{
		PopulateDefaultAssets();
	}

	void AssetPickerModal::PopulateDefaultAssets()
	{
		m_AllAssets = {
			{ "projects/default/assets/textures/checkerboard.png", "checkerboard.png", AssetCategory::Texture },
			{ "projects/default/assets/textures/noise.png",        "noise.png",        AssetCategory::Texture },
			{ "projects/default/assets/materials/standard.wmat",   "standard.wmat",   AssetCategory::Material },
			{ "projects/default/assets/materials/emissive.wmat",   "emissive.wmat",   AssetCategory::Material },
			{ "projects/default/assets/models/cube.obj",           "cube.obj",         AssetCategory::Mesh },
			{ "projects/default/assets/models/sphere.obj",         "sphere.obj",       AssetCategory::Mesh },
			{ "projects/default/assets/prefabs/player.wprefab",    "player.wprefab",   AssetCategory::Prefab },
			{ "projects/default/assets/prefabs/enemy.wprefab",     "enemy.wprefab",    AssetCategory::Prefab },
			{ "projects/default/assets/scripts/player_controller.luau", "player_controller.luau", AssetCategory::Script },
			{ "projects/default/assets/scripts/game_loop.luau",    "game_loop.luau",   AssetCategory::Script },
			{ "projects/default/assets/audio/jump.wav",            "jump.wav",         AssetCategory::Audio },
			{ "projects/default/assets/audio/hit.wav",             "hit.wav",          AssetCategory::Audio },
			{ "projects/default/assets/levels/main.wlevel",        "main.wlevel",      AssetCategory::Level }
		};

		// 登记到全局缩略图缓存中
		auto& cache = AssetThumbnailCache::Get();
		for (const auto& item : m_AllAssets)
		{
			ThumbnailInfo info;
			info.Path = item.Path;
			info.Category = item.Category;
			info.Width = 64;
			info.Height = 64;
			info.IsReady = true;
			cache.RegisterThumbnail(item.Path, info);
		}
	}

	void AssetPickerModal::Open(std::string_view title, AssetCategory filterCat, std::function<void(const std::string&)> onSelect)
	{
		m_IsOpen = true;
		m_Title = title.empty() ? "Select Asset" : std::string(title);
		m_FilterCategory = filterCat;
		m_OnSelect = std::move(onSelect);
		m_SearchQuery.clear();
		m_SelectedItem.clear();

		// 根据入参设定初始类别过滤
		if (m_FilterCategory != AssetCategory::Unknown)
		{
			m_SelectedCategoryIndex = static_cast<int>(m_FilterCategory);
		}
		else
		{
			m_SelectedCategoryIndex = 0; // All
		}
	}

	void AssetPickerModal::Close()
	{
		m_IsOpen = false;
		m_SelectedItem.clear();
	}

	static bool CaseInsensitiveFind(std::string_view haystack, std::string_view needle)
	{
		if (needle.empty())
		{
			return true;
		}
		auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
			[](char ch1, char ch2) {
				return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2));
			});
		return it != haystack.end();
	}

	bool AssetPickerModal::MatchesFilter(const AssetItem& item) const
	{
		// 类别匹配
		if (m_SelectedCategoryIndex > 0)
		{
			AssetCategory targetCat = static_cast<AssetCategory>(m_SelectedCategoryIndex);
			if (item.Category != targetCat)
			{
				return false;
			}
		}

		// 模糊搜索关键词匹配
		if (!m_SearchQuery.empty())
		{
			if (!CaseInsensitiveFind(item.Name, m_SearchQuery) &&
				!CaseInsensitiveFind(item.Path, m_SearchQuery))
			{
				return false;
			}
		}

		return true;
	}

	void AssetPickerModal::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		if (!m_IsOpen)
		{
			return;
		}

		const Wui::WuiTheme& theme = host.Theme();
		Wui::WuiId baseId = 0x530000;

		// 居中模态对话框外框 (640x440)
		float modalW = std::min(rect.W - 32.0f, 640.0f);
		float modalH = std::min(rect.H - 32.0f, 440.0f);
		float modalX = rect.X + (rect.W - modalW) * 0.5f;
		float modalY = rect.Y + (rect.H - modalH) * 0.5f;
		Wui::WuiRect modalRect { modalX, modalY, modalW, modalH };

		// 绘制模态框外框
		Wui::Panel(ctx, modalRect, m_Title, theme);

		float curY = modalRect.Y + 36.0f;

		// 搜索栏与视图切换 (4px 栅格)
		Wui::Label(ctx, { modalRect.X + 16.0f, curY + 4.0f }, "Search:", theme.TextMuted, theme.FontSizeBody);
		Wui::WuiRect searchRect { modalRect.X + 76.0f, curY, modalRect.W - 190.0f, 24.0f };
		Wui::TextField(ctx, baseId++, searchRect, m_SearchQuery, theme);

		Wui::WuiRect viewToggleRect { modalRect.X + modalRect.W - 104.0f, curY, 88.0f, 24.0f };
		if (Wui::Button(ctx, baseId++, viewToggleRect, (m_ViewMode == 0 ? "Grid View" : "List View"), theme))
		{
			m_ViewMode = 1 - m_ViewMode;
		}
		curY += 32.0f;

		// 类别选择分段器
		Wui::WuiRect catRect { modalRect.X + 16.0f, curY, modalRect.W - 32.0f, 24.0f };
		Wui::Segmented(ctx, baseId++, catRect, m_CategoryNames, m_SelectedCategoryIndex, theme);
		curY += 32.0f;

		// 资产内容列表区域
		const float listAreaHeight = modalRect.H - 150.0f;
		Wui::WuiRect listRect { modalRect.X + 16.0f, curY, modalRect.W - 32.0f, listAreaHeight };

		// 区域边框线
		Wui::LineSegment(ctx, { listRect.X, listRect.Y }, { listRect.X + listRect.W, listRect.Y }, theme.Border, 1.0f);
		Wui::LineSegment(ctx, { listRect.X, listRect.Y + listRect.H }, { listRect.X + listRect.W, listRect.Y + listRect.H }, theme.Border, 1.0f);

		std::vector<const AssetItem*> visibleAssets;
		for (const auto& item : m_AllAssets)
		{
			if (MatchesFilter(item))
			{
				visibleAssets.push_back(&item);
			}
		}

		if (m_ViewMode == 0) // 网格视图 (Grid View)
		{
			float cardW = 92.0f;
			float cardH = 68.0f;
			float pad = 8.0f;
			int cols = std::max(1, static_cast<int>((listRect.W - pad) / (cardW + pad)));

			float startX = listRect.X + pad;
			float startY = listRect.Y + pad;

			for (size_t i = 0; i < visibleAssets.size(); ++i)
			{
				int col = static_cast<int>(i) % cols;
				int row = static_cast<int>(i) / cols;
				float itemX = startX + static_cast<float>(col) * (cardW + pad);
				float itemY = startY + static_cast<float>(row) * (cardH + pad);

				if (itemY + cardH > listRect.Y + listRect.H)
				{
					break; // 越界裁剪
				}

				const auto* item = visibleAssets[i];
				bool isSelected = (m_SelectedItem == item->Path);

				Wui::WuiRect cardRect { itemX, itemY, cardW, cardH };
				std::string cardLabel = item->Name;
				if (cardLabel.size() > 10)
				{
					cardLabel = cardLabel.substr(0, 8) + "..";
				}

				if (Wui::Button(ctx, baseId++, cardRect, cardLabel, theme))
				{
					m_SelectedItem = item->Path;
				}

				// 选中高亮边框
				if (isSelected)
				{
					Wui::LineSegment(ctx, { cardRect.X, cardRect.Y }, { cardRect.X + cardRect.W, cardRect.Y }, theme.Accent, 2.0f);
					Wui::LineSegment(ctx, { cardRect.X, cardRect.Y + cardRect.H }, { cardRect.X + cardRect.W, cardRect.Y + cardRect.H }, theme.Accent, 2.0f);
					Wui::LineSegment(ctx, { cardRect.X, cardRect.Y }, { cardRect.X, cardRect.Y + cardRect.H }, theme.Accent, 2.0f);
					Wui::LineSegment(ctx, { cardRect.X + cardRect.W, cardRect.Y }, { cardRect.X + cardRect.W, cardRect.Y + cardRect.H }, theme.Accent, 2.0f);
				}

				Wui::Tooltip(ctx, cardRect, item->Path);
			}
		}
		else // 列表视图 (List View)
		{
			float rowY = listRect.Y + 4.0f;
			float rowH = 24.0f;

			for (const auto* item : visibleAssets)
			{
				if (rowY + rowH > listRect.Y + listRect.H)
				{
					break;
				}

				bool isSelected = (m_SelectedItem == item->Path);
				Wui::WuiRect rowRect { listRect.X + 4.0f, rowY, listRect.W - 8.0f, rowH };

				if (Wui::Button(ctx, baseId++, rowRect, item->Name, theme))
				{
					m_SelectedItem = item->Path;
				}

				if (isSelected)
				{
					Wui::LineSegment(ctx, { rowRect.X, rowY }, { rowRect.X, rowY + rowH }, theme.Accent, 3.0f);
				}

				Wui::Tooltip(ctx, rowRect, item->Path);
				rowY += rowH + 2.0f;
			}
		}

		// 底部操作栏 (Footer)
		float footerY = modalRect.Y + modalRect.H - 42.0f;
		Wui::LineSegment(ctx, { modalRect.X + 12.0f, footerY - 4.0f }, { modalRect.X + modalRect.W - 12.0f, footerY - 4.0f }, theme.Border, 1.0f);

		std::string statusText = m_SelectedItem.empty() ? "No asset selected" : m_SelectedItem;
		if (statusText.size() > 45)
		{
			statusText = "..." + statusText.substr(statusText.size() - 42);
		}
		Wui::Label(ctx, { modalRect.X + 16.0f, footerY + 6.0f }, statusText, theme.TextMuted, theme.FontSizeSmall);

		// 取消按钮
		Wui::WuiRect cancelRect { modalRect.X + modalRect.W - 168.0f, footerY, 72.0f, 26.0f };
		if (Wui::Button(ctx, baseId++, cancelRect, "Cancel", theme))
		{
			Close();
		}

		// 选择确认按钮
		Wui::WuiRect selectRect { modalRect.X + modalRect.W - 88.0f, footerY, 72.0f, 26.0f };
		if (Wui::Button(ctx, baseId++, selectRect, "Select", theme))
		{
			if (!m_SelectedItem.empty())
			{
				if (m_OnSelect)
				{
					m_OnSelect(m_SelectedItem);
				}
				Close();
			}
		}
	}
}
