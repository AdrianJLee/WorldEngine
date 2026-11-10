#include "wldpch.h"
#include "WUI/Panels/LevelPanel.h"

#include "World/Asset/ProjectManifest.h"
#include "World/Core/Application.h"
#include "World/Level/Authoring/LevelCatalog.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include <algorithm>
#include <cctype>

namespace World
{
	namespace
	{
		// 关卡 Id 就是入口文件名(= manifest 的 Id = 内容目录名)。路径分隔符与相对段
		// 一律拒绝:关卡必须落在内容根的直接子目录里,不能借 Id 跳出内容根。
		bool IsValidLevelId(const std::string& id)
		{
			return !id.empty() && id != "." && id != ".."
				&& id.find_first_of("/\\") == std::string::npos;
		}
	}

	void LevelPanel::EnsureLoaded()
	{
		if (m_Loaded)
			return;
		m_Loaded = true;
		Reload();
	}

	void LevelPanel::Reload()
	{
		m_Levels.clear();
		m_ContentRoot.clear();
		m_StartScene.clear();
		m_ManifestRenderer.clear();
		m_ManifestContentRoot.clear();
		m_RenameId.clear();
		m_RowError.clear();

		// 与 GameHost/编辑器启动同一条定位:项目清单 → 内容根。
		std::filesystem::path manifestPath;
		if (!World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
			return;

		std::string error;
		World::Asset::ProjectManifest manifest;
		if (!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
			return;
		m_ContentRoot = manifest.ResolveContentRoot(manifestPath);
		// 改名命中启动场景时要把它同步过去 —— 同步走"只覆盖这三个字段"的既有写入口,
		// 因此这里必须记下另外两个字段的当前值,才能原样回写。
		m_ManifestRenderer = manifest.Renderer;
		m_ManifestContentRoot = manifest.ContentRoot.generic_string();
		m_StartScene = manifest.StartScene;

		m_Levels = Level::LevelCatalog::Discover(m_ContentRoot);
	}

	bool LevelPanel::IsCurrentStartScene(const std::string& id) const
	{
		if (m_StartScene.empty())
			return false;
		if (m_StartScene == id || m_StartScene == std::string(Level::LevelPackage::LevelsDirName) + "/" + id)
			return true;
		return std::filesystem::path(m_StartScene).stem().string() == id;
	}

	bool LevelPanel::RenameLevel(PanelHost& host, const std::string& oldId, const std::string& newId,
		std::string* error)
	{
		const auto fail = [error](const std::string& reason)
		{
			if (error) *error = reason;
			return false;
		};

		if (!IsValidLevelId(newId))
			return fail(Wui::Tr("panel.levels.name_invalid", "Level name cannot contain a path separator."));
		if (newId == oldId)
			return true;

		const std::filesystem::path oldEntry = Level::LevelCatalog::EntryPathFor(m_ContentRoot, oldId);
		const std::filesystem::path newEntry = Level::LevelCatalog::EntryPathFor(m_ContentRoot, newId);
		const std::filesystem::path oldContent = m_ContentRoot / Level::LevelPackage::LevelsDirName / oldId;
		const std::filesystem::path newContent = m_ContentRoot / Level::LevelPackage::LevelsDirName / newId;

		std::error_code ec;
		if (std::filesystem::exists(newEntry, ec))
			return fail("A level with that name already exists.");
		if (std::filesystem::exists(newContent, ec))
			return fail("The target content directory already exists.");

		// 成对事务:先改入口,再改内容目录;第二步失败就把第一步滚回去 ——
		// 绝不留下"入口叫新名、内容还在旧目录"这种被成对校验拒绝的残骸。
		std::filesystem::rename(oldEntry, newEntry, ec);
		if (ec)
			return fail("Cannot rename the level entry: " + ec.message());
		if (std::filesystem::is_directory(oldContent, ec))
		{
			std::filesystem::rename(oldContent, newContent, ec);
			if (ec)
			{
				std::error_code rollback;
				std::filesystem::rename(newEntry, oldEntry, rollback);
				return fail("Cannot rename the level content directory: " + ec.message()
					+ (rollback ? " (the entry file was rolled back)" : " (rollback also failed)"));
			}
		}

		// manifest 里的 `Id` 必须等于文件名 stem(LevelCatalog 的成对校验据此拒绝不一致)。
		// 用引擎自己的读写器修正,而不是手写 YAML。
		Level::LevelPackage package;
		std::string manifestError;
		if (package.Load(newEntry, &manifestError))
		{
			package.SetId(newId);
			if (!package.SaveEntry(&manifestError))
				WLD_CORE_WARN("LevelPanel: level '{0}' renamed, but its entry Id could not be rewritten: {1}",
					newId, manifestError);
		}
		else
		{
			WLD_CORE_WARN("LevelPanel: renamed level '{0}' could not be reopened: {1}", newId, manifestError);
		}

		// 启动场景指向旧入口 ⇒ 同步清单,否则下次启动就找不到这个关卡了。
		if (IsCurrentStartScene(oldId))
		{
			const std::string newStart = std::string(Level::LevelPackage::ScenesDirName) + "/" + newId
				+ Level::LevelPackage::EntryExtension;
			std::string message;
			if (!host.SaveProjectStartupSettings(m_ManifestRenderer, newStart, m_ManifestContentRoot, &message))
				WLD_CORE_WARN("LevelPanel: start_scene could not be updated after rename: {0}", message);
			else
				m_StartScene = newStart;
		}

		// 正在编辑这个关卡 ⇒ 文档跟着换到新入口(未保存改动仍由宿主的选择流程把关)。
		const std::string openLogical = host.GetOpenDocumentPath();
		if (!openLogical.empty() && std::filesystem::path(openLogical).stem().string() == oldId)
			host.OpenScene(newEntry);

		m_RenameId.clear();
		Reload();
		return true;
	}

	bool LevelPanel::DeleteLevel(PanelHost& host, const std::string& id, std::string* error)
	{
		// 两道守卫:删掉启动场景会让项目启动即失败;删掉正在编辑的关卡会让文档悬空。
		if (IsCurrentStartScene(id))
		{
			if (error) *error = "This is the project's start scene; point start_scene at another level first.";
			return false;
		}
		const std::string openLogical = host.GetOpenDocumentPath();
		if (!openLogical.empty() && std::filesystem::path(openLogical).stem().string() == id)
		{
			if (error) *error = "This level is open in the editor; open another level before deleting it.";
			return false;
		}

		const std::filesystem::path entry = Level::LevelCatalog::EntryPathFor(m_ContentRoot, id);
		const std::filesystem::path content = m_ContentRoot / Level::LevelPackage::LevelsDirName / id;

		std::error_code ec;
		if (std::filesystem::is_directory(content, ec))
		{
			std::filesystem::remove_all(content, ec);
			if (ec)
			{
				if (error) *error = "Cannot delete the level content directory: " + ec.message();
				return false;
			}
		}
		if (std::filesystem::exists(entry, ec))
		{
			std::filesystem::remove(entry, ec);
			if (ec)
			{
				if (error) *error = "Cannot delete the level entry file: " + ec.message();
				return false;
			}
		}

		Reload();
		return true;
	}

	void LevelPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		EnsureLoaded();

		// 行级动作先记账、循环后执行。
		enum class RowAction { None, Open, Delete, RenameCommit };
		RowAction pendingAction = RowAction::None;
		std::string pendingId;
		std::string pendingNewId;

		float y = rect.Y + 8.0f;
		Label(ctx, { rect.X + 10.0f, y }, "Levels", theme.Text, 15.0f);
		y += 24.0f;

		if (m_ContentRoot.empty())
		{
			Label(ctx, { rect.X + 10.0f, y },
				Wui::Tr("panel.levels.not_found", "No project manifest; cannot locate levels."),
				theme.TextMuted, 13.0f);
			return;
		}

		// ---- 新建关卡输入栏 ----
		{
			const float rowY = y;
			const Wui::WuiRect nameRect { rect.X + 8.0f, rowY, rect.W - 16.0f - 76.0f, 22.0f };
			Wui::TextFieldA11y nameA11y;
			nameA11y.Label = "New level name";
			nameA11y.Placeholder = "NewLevel";
			Wui::TextField(ctx, Wui::HashId("level.new.name"), nameRect, m_NewLevelName, theme, nullptr, &nameA11y);

			if (Button(ctx, Wui::HashId("level.new.create"),
				{ nameRect.X + nameRect.W + 4.0f, rowY, 68.0f, 22.0f }, "Create", theme))
			{
				m_NewLevelError.clear();
				m_RowError.clear();
				const std::string name = m_NewLevelName;
				if (!IsValidLevelId(name))
				{
					m_NewLevelError = Wui::Tr("panel.levels.name_invalid",
						"Level name cannot contain a path separator.");
				}
				else
				{
					const std::filesystem::path entryPath = Level::LevelCatalog::EntryPathFor(m_ContentRoot, name);
					std::string createError;
					WorldContext* context = Application::HasInstance()
						? &Application::Get().GetContext() : nullptr;
					if (context && Level::LevelPackage::CreateEmpty(*context, entryPath, name, &createError))
					{
						Reload();
					}
					else
					{
						m_NewLevelError = createError;
						WLD_CORE_ERROR("LevelPanel: creating level '{0}' failed: {1}", name, createError);
					}
				}
			}
			y += 26.0f;
			if (!m_NewLevelError.empty())
			{
				Label(ctx, { rect.X + 10.0f, y }, m_NewLevelError, theme.Danger, 12.0f);
				y += 18.0f;
			}
		}

		// ---- 关卡下拉快捷切换选择器 (Quick Level Dropdown Selector) ----
		{
			std::vector<std::string> comboItems;
			comboItems.reserve(m_Levels.size());
			const std::string openDocNow = host.GetOpenDocumentPath();
			const std::string openDocIdNow = openDocNow.empty() ? std::string() : std::filesystem::path(openDocNow).stem().string();
			int currentIdx = -1;
			for (size_t i = 0; i < m_Levels.size(); ++i)
			{
				const std::string name = m_Levels[i].DisplayName.empty() ? m_Levels[i].Id : (m_Levels[i].DisplayName + "  (" + m_Levels[i].Id + ")");
				comboItems.push_back(name);
				if (m_Levels[i].Id == openDocIdNow)
					currentIdx = static_cast<int>(i);
			}

			const Wui::WuiRect comboRect { rect.X + 8.0f, y, rect.W - 16.0f, 22.0f };
			const std::string comboDisplayText = currentIdx >= 0 ? ("Current: " + m_Levels[currentIdx].Id) : "Select Level to open...";
			int selectedIdx = currentIdx;
			if (Wui::Combo(ctx, Wui::HashId("level.quick.combo"), comboRect, "", comboItems, selectedIdx, theme, comboDisplayText))
			{
				if (selectedIdx >= 0 && selectedIdx < static_cast<int>(m_Levels.size()))
				{
					pendingAction = RowAction::Open;
					pendingId = m_Levels[selectedIdx].Id;
				}
			}
			Wui::Tooltip(ctx, comboRect, "下拉快速切换打开关卡 (Quick switch level dropdown)");
			y += 26.0f;
		}

		// ---- 搜索过滤栏 ----
		{
			const Wui::WuiRect searchRect { rect.X + 8.0f, y, rect.W - 16.0f, 22.0f };
			Wui::TextFieldA11y searchA11y;
			searchA11y.Label = "Filter levels";
			searchA11y.Placeholder = "Search levels...";
			Wui::TextField(ctx, Wui::HashId("level.search.filter"), searchRect, m_SearchFilter, theme, nullptr, &searchA11y);
			y += 26.0f;
		}

		if (m_Levels.empty())
		{
			Label(ctx, { rect.X + 10.0f, y },
				Wui::Tr("panel.levels.empty",
					"No levels yet. A level is an entry file under scenes/ plus its content under levels/."),
				theme.TextMuted, 13.0f);
			return;
		}

		// 过滤候选关卡
		std::vector<const Level::LevelInfo*> filteredLevels;
		std::string lowerFilter = m_SearchFilter;
		std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		for (const auto& lvl : m_Levels)
		{
			if (lowerFilter.empty())
			{
				filteredLevels.push_back(&lvl);
				continue;
			}
			std::string lowerId = lvl.Id;
			std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::string lowerName = lvl.DisplayName;
			std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lowerId.find(lowerFilter) != std::string::npos || lowerName.find(lowerFilter) != std::string::npos)
			{
				filteredLevels.push_back(&lvl);
			}
		}

		const std::string openDoc = host.GetOpenDocumentPath();
		const std::string openDocId = openDoc.empty() ? std::string() : std::filesystem::path(openDoc).stem().string();

		const float footerH = 22.0f;
		const float listTopY = y;
		const float listBottomY = rect.Y + rect.H - footerH - 4.0f;
		const float listH = std::max(40.0f, listBottomY - listTopY);
		const Wui::WuiRect listRect { rect.X + 4.0f, listTopY, rect.W - 8.0f, listH };

		constexpr float rowH = 46.0f;
		constexpr float rowGap = 4.0f;
		const float totalContentHeight = static_cast<float>(filteredLevels.size()) * (rowH + rowGap) + 40.0f;
		const bool hasScrollbar = totalContentHeight > listH;
		constexpr float scrollbarWidth = 10.0f;
		const float rowWidth = hasScrollbar ? (listRect.W - scrollbarWidth - 4.0f) : (listRect.W - 4.0f);

		if (filteredLevels.empty() && !m_SearchFilter.empty())
		{
			Label(ctx, { rect.X + 10.0f, y + 8.0f },
				"No matching levels found.", theme.TextMuted, 13.0f);
		}
		else
		{
			Wui::BeginScrollArea(ctx, listRect, totalContentHeight, m_ScrollY, theme, Wui::HashId("level.scrollarea"));

			for (size_t index = 0; index < filteredLevels.size(); ++index)
			{
				const Level::LevelInfo& level = *filteredLevels[index];
				const float rowY = listRect.Y + 2.0f + static_cast<float>(index) * (rowH + rowGap) - m_ScrollY;
				const Wui::WuiRect row { listRect.X + 2.0f, rowY, rowWidth, rowH };

				// 视口视野外几何裁剪:避免不可见的深层行产生多余绘制与误交互
				if (row.Y + row.H < listRect.Y || row.Y > listRect.Y + listRect.H)
					continue;

				const bool isHovered = ctx.IsHovered(row);
				const bool isCurrentOpen = (level.Id == openDocId) || IsCurrentStartScene(level.Id);

				const Wui::WuiColor rowBg = isCurrentOpen
					? (isHovered ? theme.PanelBg : theme.PanelHeader)
					: (isHovered ? theme.ButtonHover : theme.PanelHeader);
				Wui::PanelBackground(ctx, row, rowBg, 2.0f);
				if (isCurrentOpen)
				{
					Wui::PanelBackground(ctx, { row.X, row.Y, 3.0f, row.H }, theme.Accent, 1.0f);
				}

				// 双击行直接打开对应关卡
				if (ctx.IsDoubleClicked(row) && m_RenameId != level.Id)
				{
					pendingAction = RowAction::Open;
					pendingId = level.Id;
				}

				const float right = row.X + row.W - 6.0f;
				// 右侧按钮区占用 132px 空间,文字区域严格限宽,彻底根除长文本穿透重叠按钮的问题
				const float buttonZoneW = 132.0f;
				const float maxTextWidth = std::max(30.0f, row.W - buttonZoneW - 14.0f);

				// 关卡主标题 (末尾限宽省略)
				std::string fullTitle = level.DisplayName.empty() ? level.Id : (level.DisplayName + "  (" + level.Id + ")");
				std::string shownTitle = Wui::EllipsizeMiddleToWidth(ctx, fullTitle, maxTextWidth, 13.0f);
				Label(ctx, { row.X + 8.0f, row.Y + 4.0f },
					shownTitle, isCurrentOpen ? theme.TextBright : theme.Text, 13.0f);

				// 关卡相对路径 (中间限宽省略)
				std::string fullPath = std::string(Level::LevelPackage::ScenesDirName) + "/" + level.Id
					+ Level::LevelPackage::EntryExtension;
				std::string shownPath = Wui::EllipsizeMiddleToWidth(ctx, fullPath, maxTextWidth, 11.0f);
				Label(ctx, { row.X + 8.0f, row.Y + 22.0f },
					shownPath, theme.TextMuted, 11.0f);

				// 悬停气泡提示完整标题与路径
				if (isHovered && m_RenameId != level.Id)
				{
					Wui::Tooltip(ctx, row, fullTitle + "\n" + fullPath + "\n(Double-click to open)");
				}

				if (m_RenameId == level.Id)
				{
					const float fieldWidth = std::clamp(row.W - 120.0f, 60.0f, 160.0f);
					const Wui::WuiRect field { row.X + 6.0f, row.Y + row.H - 26.0f, fieldWidth, 22.0f };
					Wui::TextFieldA11y renameA11y;
					renameA11y.Label = "Rename level to";
					Wui::TextField(ctx, Wui::HashId(("level.rename.field." + level.Id).c_str()), field,
						m_RenameText, theme, nullptr, &renameA11y);
					if (Button(ctx, Wui::HashId(("level.rename.ok." + level.Id).c_str()),
						{ field.X + field.W + 4.0f, field.Y, 44.0f, 22.0f }, "OK", theme))
					{
						pendingAction = RowAction::RenameCommit;
						pendingId = level.Id;
						pendingNewId = m_RenameText;
					}
					if (Button(ctx, Wui::HashId(("level.rename.cancel." + level.Id).c_str()),
						{ field.X + field.W + 52.0f, field.Y, 56.0f, 22.0f }, "Cancel", theme))
						m_RenameId.clear();
				}
				else
				{
					if (Button(ctx, Wui::HashId(("level.open." + level.Id).c_str()),
						{ right - 60.0f, row.Y + 2.0f, 60.0f, 20.0f }, "Open", theme))
					{
						pendingAction = RowAction::Open;
						pendingId = level.Id;
					}
					if (Button(ctx, Wui::HashId(("level.rename." + level.Id).c_str()),
						{ right - 126.0f, row.Y + 2.0f, 62.0f, 20.0f }, "Rename", theme))
					{
						m_RenameId = level.Id;
						m_RenameText = level.Id;
						m_RowError.clear();
					}
					if (Button(ctx, Wui::HashId(("level.delete." + level.Id).c_str()),
						{ right - 126.0f, row.Y + 24.0f, 62.0f, 20.0f }, "Delete", theme))
					{
						pendingAction = RowAction::Delete;
						pendingId = level.Id;
					}
				}
			}

			Wui::EndScrollArea(ctx);

			// 纵向交互滚动条
			if (hasScrollbar)
			{
				const Wui::WuiRect scrollbarRect { listRect.X + listRect.W - scrollbarWidth, listRect.Y, scrollbarWidth, listRect.H };
				Wui::ScrollBar(ctx, Wui::HashId("level.scrollbar"), scrollbarRect, totalContentHeight, listRect.H, m_ScrollY, theme, false);
			}
		}

		// ---- 行级动作延迟处理 ----
		if (pendingAction != RowAction::None)
		{
			m_RowError.clear();
			m_NewLevelError.clear();
			std::string actionError;
			switch (pendingAction)
			{
			case RowAction::Open:
				if (const Level::LevelInfo* info = Level::LevelCatalog::Find(m_Levels, pendingId))
					host.OpenScene(info->EntryPath);
				break;
			case RowAction::RenameCommit:
				if (!RenameLevel(host, pendingId, pendingNewId, &actionError))
					m_RowError = actionError;
				break;
			case RowAction::Delete:
				if (!DeleteLevel(host, pendingId, &actionError))
					m_RowError = actionError;
				break;
			case RowAction::None:
				break;
			}
		}

		if (!m_RowError.empty())
		{
			Label(ctx, { rect.X + 10.0f, rect.Y + rect.H - footerH - 18.0f }, m_RowError, theme.Danger, 12.0f);
		}

		// 底部显示内容目录物理路径
		Label(ctx, { rect.X + 10.0f, rect.Y + rect.H - 18.0f },
			(m_ContentRoot / Level::LevelPackage::ScenesDirName).string(), theme.TextMuted, 11.0f);
	}
}



