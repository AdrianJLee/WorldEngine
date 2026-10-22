#include <unordered_set>
#include "wldpch.h"
#include "WUI/Modals/AssetPickerModal.h"
#include "WUI/Common/AssetThumbnailRenderer.h"
#include "WUI/Common/EditorPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Utils/Paths.h"
#include "World/Core/KeyCodes.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

namespace World
{
	namespace
	{
		double GetCurrentTimeSeconds()
		{
			using Clock = std::chrono::steady_clock;
			static const auto start = Clock::now();
			return std::chrono::duration<double>(Clock::now() - start).count();
		}

		bool CaseInsensitiveFind(std::string_view haystack, std::string_view needle)
		{
			if (needle.empty()) return true;
			auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
				[](char ch1, char ch2) {
					return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2));
				});
			return it != haystack.end();
		}

		Wui::WuiColor CategoryColor(AssetCategory cat, const Wui::WuiTheme& theme)
		{
			switch (cat)
			{
			case AssetCategory::Texture:  return theme.Info;
			case AssetCategory::Material: return theme.Accent;
			case AssetCategory::Mesh:     return theme.Success;
			case AssetCategory::Prefab:   return theme.Warning;
			case AssetCategory::Script:   return theme.Warning;
			case AssetCategory::Audio:    return theme.Info;
			case AssetCategory::Level:    return theme.Danger;
			case AssetCategory::Curve:    return theme.Info;
			case AssetCategory::Shader:   return theme.Accent;
			case AssetCategory::UI:       return theme.Accent;
			case AssetCategory::VFX:      return theme.Warning;
			default:                      return theme.TextMuted;
			}
		}

		const char* CategoryShortName(AssetCategory cat)
		{
			switch (cat)
			{
			case AssetCategory::Texture:  return "TEX";
			case AssetCategory::Material: return "MAT";
			case AssetCategory::Mesh:     return "3D";
			case AssetCategory::Prefab:   return "PREFAB";
			case AssetCategory::Script:   return "LUA";
			case AssetCategory::Audio:    return "AUDIO";
			case AssetCategory::Level:    return "LEVEL";
			case AssetCategory::Curve:    return "CURVE";
			case AssetCategory::Shader:   return "SHADER";
			case AssetCategory::UI:       return "UI";
			case AssetCategory::VFX:      return "VFX";
			default:                      return "MISC";
			}
		}
	}

	AssetPickerModal::AssetPickerModal()
		: m_CategoryNames({
			"全部 (All)",
			"贴图 (Texture)",
			"材质 (Material)",
			"网格 (Mesh)",
			"预制体 (Prefab)",
			"脚本 (Script)",
			"音频 (Audio)",
			"关卡 (Level)",
			"曲线 (Curve)",
			"着色器 (Shader)",
			"界面 (UI)",
			"特效 (VFX)"
		})
	{
		PopulateProjectAssets();
	}

	void AssetPickerModal::PopulateProjectAssets()
	{
		m_AllAssets.clear();
		std::error_code ec;
		std::vector<std::filesystem::path> searchRoots;

		std::filesystem::path primary = World::Paths::AssetRoot();
		if (std::filesystem::exists(primary, ec))
			searchRoots.push_back(primary);

		std::filesystem::path projAssets = std::filesystem::current_path() / "projects" / "assets";
		if (std::filesystem::exists(projAssets, ec))
			searchRoots.push_back(projAssets);

		std::filesystem::path templateAssets = std::filesystem::current_path() / "templates" / "project-example" / "assets";
		if (std::filesystem::exists(templateAssets, ec))
			searchRoots.push_back(templateAssets);

		std::filesystem::path engineRoot = World::Paths::EngineContentRoot();
		if (std::filesystem::exists(engineRoot, ec))
			searchRoots.push_back(engineRoot);

		if (searchRoots.empty())
			return;

		auto& thumbCache = AssetThumbnailCache::Get();
		std::unordered_set<std::string> seenPaths;

		for (const auto& root : searchRoots)
		{
			for (const auto& entry : std::filesystem::recursive_directory_iterator(root,
				std::filesystem::directory_options::skip_permission_denied, ec))
			{
				if (ec || !entry.is_regular_file(ec))
					continue;

				const std::filesystem::path rel = std::filesystem::relative(entry.path(), root, ec);
				if (ec || rel.empty())
					continue;

				std::string logical = (root == engineRoot) ? ("engine/" + rel.generic_string()) : rel.generic_string();
				if (seenPaths.count(logical))
					continue;
				seenPaths.insert(logical);

				AssetCategory cat = AssetThumbnailCache::DetectCategory(logical);
				if (cat == AssetCategory::Unknown)
					continue;

				AssetItem item;
				item.Path = logical;
				item.Name = rel.filename().string();
				item.Category = cat;
				m_AllAssets.push_back(std::move(item));

				thumbCache.GetOrCreateThumbnail(logical);
			}
		}

		std::sort(m_AllAssets.begin(), m_AllAssets.end(), [](const AssetItem& a, const AssetItem& b) {
			if (a.Category != b.Category)
				return static_cast<int>(a.Category) < static_cast<int>(b.Category);
			return a.Name < b.Name;
		});
	}

	void AssetPickerModal::Open(std::string_view title, AssetCategory filterCat, std::function<void(const std::string&)> onSelect)
	{
		PopulateProjectAssets();
		m_IsOpen = true;
		m_Title = title.empty() ? "Select Asset" : std::string(title);
		m_FilterCategory = filterCat;
		m_OnSelect = std::move(onSelect);
		m_SearchQuery.clear();
		m_SelectedItem.clear();
		m_LastClickedItem.clear();
		m_LastClickTime = 0.0;
		m_ScrollOffset = 0.0f;

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
		m_LastClickedItem.clear();
	}

	bool AssetPickerModal::MatchesFilter(const AssetItem& item) const
	{
		if (m_SelectedCategoryIndex > 0)
		{
			AssetCategory targetCat = static_cast<AssetCategory>(m_SelectedCategoryIndex);
			if (item.Category != targetCat)
				return false;
		}

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
			return;

		const Wui::WuiTheme& theme = host.Theme();
		Wui::WuiId baseId = 0x530000;

		auto commitSelection = [this, &host]() {
			if (m_SelectedItem.empty())
				return;

			if (m_OnSelect)
			{
				m_OnSelect(m_SelectedItem);
			}
			else
			{
				host.OpenAsset(m_SelectedItem);
			}
			Close();
		};

		// 快捷键支持: Escape 取消，Enter 确认
		if (ctx.IsKeyPressed(KeyCodes::Escape))
		{
			Close();
			return;
		}
		if (ctx.IsKeyPressed(KeyCodes::Enter) && !m_SelectedItem.empty())
		{
			commitSelection();
			return;
		}

		// 模态弹窗全部命令推入 overlay 顶层渲染通道
		ctx.PushOverlay();

		// 半透明全屏遮罩 + 封锁下层命中
		Wui::PanelBackground(ctx, rect, theme.ModalBackdrop, 0.0f);
		ctx.RegisterOverlayRect(rect);

		// 模态外框居中排布 (720x520)
		const float modalW = std::min(rect.W - 32.0f, 720.0f);
		const float modalH = std::min(rect.H - 32.0f, 520.0f);
		const float modalX = rect.X + (rect.W - modalW) * 0.5f;
		const float modalY = rect.Y + (rect.H - modalH) * 0.5f;
		const Wui::WuiRect modalRect { modalX, modalY, modalW, modalH };

		Wui::DrawPanelSurface(ctx, modalRect, theme);
		ctx.RegisterOverlayRect(modalRect);

		float curY = modalRect.Y + 8.0f;

		// 顶部标题栏
		Wui::Label(ctx, { modalRect.X + 16.0f, curY + 2.0f }, m_Title, theme.Text, theme.FontSizeTitle);

		// 右上角关闭 X 按钮
		const Wui::WuiRect closeBtnRect { modalRect.X + modalRect.W - 32.0f, curY, 24.0f, 22.0f };
		if (Wui::Button(ctx, baseId++, closeBtnRect, "✕", theme))
		{
			Close();
			ctx.PopOverlay();
			return;
		}
		curY += 32.0f;

		// 搜索栏与下拉选择器 + 视图切换 (第一行)
		Wui::Label(ctx, { modalRect.X + 16.0f, curY + 4.0f }, "搜索:", theme.TextMuted, theme.FontSizeBody);
		const float catComboW = 160.0f;
		const float viewToggleW = 86.0f;
		const float searchW = modalRect.W - 32.0f - 52.0f - catComboW - viewToggleW - 16.0f;

		const Wui::WuiRect searchRect { modalRect.X + 56.0f, curY, searchW, 24.0f };
		Wui::TextField(ctx, baseId++, searchRect, m_SearchQuery, theme);

		const Wui::WuiRect catComboRect { searchRect.X + searchRect.W + 8.0f, curY, catComboW, 24.0f };
		Wui::Combo(ctx, baseId++, catComboRect, "分类", m_CategoryNames, m_SelectedCategoryIndex, theme);

		const Wui::WuiRect viewToggleRect { catComboRect.X + catComboRect.W + 8.0f, curY, viewToggleW, 24.0f };
		if (Wui::Button(ctx, baseId++, viewToggleRect, (m_ViewMode == 0 ? "网格视图" : "列表视图"), theme))
		{
			m_ViewMode = 1 - m_ViewMode;
		}
		curY += 28.0f;

		// 快速分类过滤胶囊标签栏 (第二行: 12 大分类一览无余)
		const char* shortLabels[] = {
			"全部", "贴图", "材质", "网格", "预制体", "脚本", "音频", "关卡", "曲线", "着色器", "界面", "特效"
		};
		const float btnPad = 4.0f;
		const float totalW = modalRect.W - 32.0f;
		const float pillW = (totalW - btnPad * 11.0f) / 12.0f;
		for (int i = 0; i < 12; ++i)
		{
			const Wui::WuiRect pillRect { modalRect.X + 16.0f + static_cast<float>(i) * (pillW + btnPad), curY, pillW, 22.0f };
			const bool isSelected = (m_SelectedCategoryIndex == i);
			if (isSelected)
			{
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, pillRect, theme.Accent, 3.0f });
			}
			if (Wui::Button(ctx, baseId++, pillRect, shortLabels[i], theme))
			{
				m_SelectedCategoryIndex = i;
			}
		}
		curY += 28.0f;

		// 资产内容列表显示区 (带剪裁与滚动条)
		const float listAreaHeight = modalRect.H - 146.0f;
		const Wui::WuiRect listRect { modalRect.X + 16.0f, curY, modalRect.W - 32.0f, listAreaHeight };

		// 边框分割线
		Wui::LineSegment(ctx, { listRect.X, listRect.Y }, { listRect.X + listRect.W, listRect.Y }, theme.Border, 1.0f);
		Wui::LineSegment(ctx, { listRect.X, listRect.Y + listRect.H }, { listRect.X + listRect.W, listRect.Y + listRect.H }, theme.Border, 1.0f);

		std::vector<const AssetItem*> visibleAssets;
		for (const auto& item : m_AllAssets)
		{
			if (MatchesFilter(item))
				visibleAssets.push_back(&item);
		}

		if (visibleAssets.empty())
		{
			Wui::Label(ctx, { listRect.X + listRect.W * 0.5f - 110.0f, listRect.Y + listRect.H * 0.5f - 10.0f },
				"没有找到匹配的资产 (No assets found)", theme.TextMuted, 13.0f);
		}
		else if (m_ViewMode == 0) // 网格视图 (Grid View - 真实缩略图)
		{
			const float cardW = 96.0f;
			const float cardH = 92.0f;
			const float pad = 8.0f;
			const int cols = std::max(1, static_cast<int>((listRect.W - pad) / (cardW + pad)));
			const size_t rowCount = (visibleAssets.size() + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols);
			const float totalContentH = pad + static_cast<float>(rowCount) * (cardH + pad);

			Wui::BeginScrollArea(ctx, listRect, totalContentH, m_ScrollOffset, theme);

			const float startX = listRect.X + pad;
			const float startY = listRect.Y + pad;
			const double now = GetCurrentTimeSeconds();
			auto& thumbCache = AssetThumbnailCache::Get();

			for (size_t i = 0; i < visibleAssets.size(); ++i)
			{
				const int col = static_cast<int>(i) % cols;
				const int row = static_cast<int>(i) / cols;
				const float itemX = startX + static_cast<float>(col) * (cardW + pad);
				const float itemY = startY + static_cast<float>(row) * (cardH + pad) - m_ScrollOffset;

				// 视口外剔除
				if (itemY + cardH < listRect.Y || itemY > listRect.Y + listRect.H)
					continue;

				const auto* item = visibleAssets[i];
				const bool isSelected = (m_SelectedItem == item->Path);
				const Wui::WuiRect cardRect { itemX, itemY, cardW, cardH };
				const bool hovered = ctx.IsHovered(cardRect);

				// 卡片底色 (选中 > 悬停 > 默认)
				Wui::PanelBackground(ctx, cardRect, isSelected ? theme.Selection : (hovered ? theme.HoverBg : theme.ButtonBg), theme.Radius);

				// 实时多类型缩略图渲染
				const Wui::WuiRect imgRect { itemX + 5.0f, itemY + 5.0f, cardW - 10.0f, 56.0f };
				Editor::AssetThumbnailRenderer::DrawThumbnail(ctx, imgRect, item->Path, item->Category, theme);

				// 文件名称 (底部单行截断)
				std::string cardLabel = item->Name;
				if (cardLabel.size() > 11)
					cardLabel = cardLabel.substr(0, 9) + "..";
				Wui::Label(ctx, { itemX + 6.0f, itemY + 66.0f }, cardLabel, isSelected ? theme.Text : theme.TextMuted, 11.0f);

				// 交互与双击确认 (严禁用全卡片透明 Button 覆盖!)
				if (hovered && ctx.Input().MouseDown[0])
				{
					m_SelectedItem = item->Path;
				}
				if (hovered && ctx.Input().MouseDoubleClicked[0])
				{
					m_SelectedItem = item->Path;
					commitSelection();
					Wui::EndScrollArea(ctx);
					ctx.PopOverlay();
					return;
				}

				if (isSelected)
				{
					Wui::Badge(ctx, { itemX + cardW - 22.0f, itemY + 4.0f, 18.0f, 16.0f },
						"✓", theme.Accent, theme.TextBright, theme, 9.0f);
				}

				// 边框描边
				Wui::HighlightOutline(ctx, cardRect, isSelected ? theme.Accent : (hovered ? theme.BorderStrong : theme.Border),
					theme.Radius, isSelected ? 2.0f : 1.0f);

				Wui::Tooltip(ctx, cardRect, item->Path);
			}

			Wui::EndScrollArea(ctx);
		}
		else // 列表视图 (List View)
		{
			const float rowH = 26.0f;
			const float totalContentH = static_cast<float>(visibleAssets.size()) * (rowH + 2.0f) + 4.0f;

			Wui::BeginScrollArea(ctx, listRect, totalContentH, m_ScrollOffset, theme);

			float rowY = listRect.Y + 4.0f - m_ScrollOffset;

			for (const auto* item : visibleAssets)
			{
				if (rowY + rowH >= listRect.Y && rowY <= listRect.Y + listRect.H)
				{
					const bool isSelected = (m_SelectedItem == item->Path);
					const Wui::WuiRect rowRect { listRect.X + 4.0f, rowY, listRect.W - 8.0f, rowH };
					const bool hovered = ctx.IsHovered(rowRect);

					// 行底色
					if (isSelected)
						Wui::PanelBackground(ctx, rowRect, theme.Selection, 2.0f);
					else if (hovered)
						Wui::PanelBackground(ctx, rowRect, theme.HoverBg, 2.0f);

					// 类别胶囊徽标
					const Wui::WuiColor badgeCol = CategoryColor(item->Category, theme);
					Wui::Badge(ctx, { rowRect.X + 4.0f, rowY + 3.0f, 50.0f, 20.0f },
						CategoryShortName(item->Category), badgeCol, theme.TextBright, theme, 10.0f);

					// 文件名与相对路径
					Wui::Label(ctx, { rowRect.X + 62.0f, rowY + 5.0f }, item->Name, isSelected ? theme.Text : theme.TextMuted, 12.0f);
					Wui::Label(ctx, { rowRect.X + 230.0f, rowY + 5.0f }, item->Path, theme.TextDisabled, 11.0f);

					if (hovered && ctx.Input().MouseDown[0])
					{
						m_SelectedItem = item->Path;
					}
					if (hovered && ctx.Input().MouseDoubleClicked[0])
					{
						m_SelectedItem = item->Path;
						commitSelection();
						Wui::EndScrollArea(ctx);
						ctx.PopOverlay();
						return;
					}

					if (isSelected)
					{
						Wui::LineSegment(ctx, { rowRect.X, rowY }, { rowRect.X, rowY + rowH }, theme.Accent, 3.0f);
					}

					Wui::Tooltip(ctx, rowRect, item->Path);
				}
				rowY += rowH + 2.0f;
			}

			Wui::EndScrollArea(ctx);
		}

		// 底部操作栏 (Footer)
		const float footerY = modalRect.Y + modalRect.H - 38.0f;
		Wui::LineSegment(ctx, { modalRect.X + 12.0f, footerY - 4.0f }, { modalRect.X + modalRect.W - 12.0f, footerY - 4.0f }, theme.Border, 1.0f);

		std::string statusText = m_SelectedItem.empty() ? "请在上方点选或双击资产 (Click or double-click to select an asset)" : ("已选择: " + m_SelectedItem);
		if (statusText.size() > 56)
			statusText = "已选择: ..." + statusText.substr(statusText.size() - 50);
		Wui::Label(ctx, { modalRect.X + 16.0f, footerY + 5.0f }, statusText, m_SelectedItem.empty() ? theme.TextMuted : theme.Text, theme.FontSizeSmall);

		// 定位目录 (Reveal in Explorer)
		const float btnRevealW = 76.0f;
		const Wui::WuiRect revealRect { modalRect.X + modalRect.W - 270.0f, footerY, btnRevealW, 26.0f };
		if (!m_SelectedItem.empty())
		{
			Wui::Tooltip(ctx, revealRect, "在内容浏览器或文件资源管理器中定位当前选中资产所在目录");
			if (Wui::Button(ctx, baseId++, revealRect, "定位文件", theme))
			{
				if (host.RevealAssetInContentBrowser(m_SelectedItem))
				{
					host.Notify("已在内容浏览器中定位: " + m_SelectedItem);
				}
				else
				{
					std::string p = m_SelectedItem;
					if (p.rfind("engine/", 0) == 0 || p.rfind("Engine/", 0) == 0)
						p = p.substr(7);
					std::filesystem::path full = World::Paths::AssetRoot() / m_SelectedItem;
					if (!std::filesystem::exists(full))
						full = World::Paths::EngineContentRoot() / p;
					if (!std::filesystem::exists(full))
						full = World::Paths::ProjectDir() / m_SelectedItem;
					std::string cmd = "start "" explorer.exe /select,"" + full.string() + """;
					system(cmd.c_str());
					host.Notify("已在文件资源管理器中定位: " + m_SelectedItem);
				}
			}
		}

		// 取消按钮
		const Wui::WuiRect cancelRect { modalRect.X + modalRect.W - 186.0f, footerY, 72.0f, 26.0f };
		if (Wui::Button(ctx, baseId++, cancelRect, "取消", theme))
		{
			Close();
			ctx.PopOverlay();
			return;
		}

		// 确认/打开按钮
		const float btnSelectW = m_OnSelect ? 76.0f : 100.0f;
		const Wui::WuiRect selectRect { modalRect.X + modalRect.W - 106.0f, footerY, btnSelectW, 26.0f };
		auto selectTheme = theme;
		if (!m_SelectedItem.empty())
		{
			selectTheme.ButtonBg = theme.ActiveBg;
			selectTheme.Border = theme.Accent;
		}
		const std::string selectBtnLabel = m_OnSelect ? "确认选择" : "打开资产";
		if (Wui::Button(ctx, baseId++, selectRect, selectBtnLabel, selectTheme))
		{
			if (!m_SelectedItem.empty())
			{
				commitSelection();
				ctx.PopOverlay();
				return;
			}
		}

		ctx.PopOverlay();
	}
}
