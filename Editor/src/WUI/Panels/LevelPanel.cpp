#include "wldpch.h"
#include "WUI/Panels/LevelPanel.h"

#include "World/Asset/ProjectManifest.h"
#include "World/Core/Application.h"
#include "World/Level/Authoring/LevelCatalog.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

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

		// 先删内容、后删入口:入口还在而内容没了 ⇒ 成对校验会把该关卡跳过(可读告警);
		// 反过来则会留下一个入口指向已消失内容目录的悬空关卡。
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

		// ---- 新建:名称 + Create。成功即出现在下面的列表里,可直接 Open。----
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
					// 入口与内容目录一次成型(`scenes/<Id>.wlevel` + `levels/<Id>/`),
					// 起手实体走同一个"场景 → 包"捕获路径 ⇒ 新关卡打开就有相机/灯/方块,
					// 而不是一片黑让人怀疑没加载上。上下文取应用级(编辑器总有一个),
					// 不依赖"当前是否已打开场景"。
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

		if (m_Levels.empty())
		{
			Label(ctx, { rect.X + 10.0f, y },
				Wui::Tr("panel.levels.empty",
					"No levels yet. A level is an entry file under scenes/ plus its content under levels/."),
				theme.TextMuted, 13.0f);
			return;
		}

		constexpr float rowH = 46.0f;
		for (size_t index = 0; index < m_Levels.size(); ++index)
		{
			const Level::LevelInfo& level = m_Levels[index];
			const Wui::WuiRect row { rect.X + 8.0f, y, rect.W - 16.0f, rowH };
			Wui::PanelBackground(ctx, row, theme.PanelHeader, 1.0f);

			Label(ctx, { row.X + 6.0f, row.Y + 4.0f },
				level.DisplayName + "  (" + level.Id + ")", theme.Text, 13.0f);
			Label(ctx, { row.X + 6.0f, row.Y + 22.0f },
				std::string(Level::LevelPackage::ScenesDirName) + "/" + level.Id
					+ Level::LevelPackage::EntryExtension, theme.TextMuted, 11.0f);

			const float right = row.X + row.W - 8.0f;
			if (m_RenameId == level.Id)
			{
				const Wui::WuiRect field { right - 264.0f, row.Y + row.H - 28.0f, 150.0f, 22.0f };
				Wui::TextFieldA11y renameA11y;
				renameA11y.Label = "New level name";
				Wui::TextField(ctx, Wui::HashId(("level.rename.field." + level.Id).c_str()), field,
					m_RenameText, theme, nullptr, &renameA11y);
				if (Button(ctx, Wui::HashId(("level.rename.ok." + level.Id).c_str()),
					{ field.X + field.W + 4.0f, field.Y, 46.0f, 22.0f }, "OK", theme))
				{
					m_RowError.clear();
					m_NewLevelError.clear();
					std::string renameError;
					if (!RenameLevel(host, level.Id, m_RenameText, &renameError))
						m_RowError = renameError;
				}
				if (Button(ctx, Wui::HashId(("level.rename.cancel." + level.Id).c_str()),
					{ field.X + field.W + 54.0f, field.Y, 60.0f, 22.0f }, "Cancel", theme))
					m_RenameId.clear();
			}
			else
			{
				if (Button(ctx, Wui::HashId(("level.open." + level.Id).c_str()),
					{ right - 62.0f, row.Y + 2.0f, 62.0f, 20.0f }, "Open", theme))
				{
					// 打开关卡入口:EditorDocument 按 `.wlevel` 后缀走 Level 2.0 读路径。
					host.OpenScene(level.EntryPath);
				}
				if (Button(ctx, Wui::HashId(("level.rename." + level.Id).c_str()),
					{ right - 128.0f, row.Y + 2.0f, 62.0f, 20.0f }, "Rename", theme))
				{
					m_RenameId = level.Id;
					m_RenameText = level.Id;
					m_RowError.clear();
				}
				if (Button(ctx, Wui::HashId(("level.delete." + level.Id).c_str()),
					{ right - 128.0f, row.Y + 24.0f, 62.0f, 20.0f }, "Delete", theme))
				{
					m_RowError.clear();
					m_NewLevelError.clear();
					std::string deleteError;
					if (!DeleteLevel(host, level.Id, &deleteError))
						m_RowError = deleteError;
				}
			}
			y += rowH + 4.0f;
		}

		if (!m_RowError.empty())
		{
			Label(ctx, { rect.X + 10.0f, y + 2.0f }, m_RowError, theme.Danger, 12.0f);
			y += 18.0f;
		}

		Label(ctx, { rect.X + 10.0f, rect.Y + rect.H - 20.0f },
			(m_ContentRoot / Level::LevelPackage::ScenesDirName).string(), theme.TextMuted, 11.0f);
	}
}
