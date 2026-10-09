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

		// 与 GameHost/编辑器启动同一条定位:项目清单 → 内容根。
		std::filesystem::path manifestPath;
		if (!World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
			return;

		std::string error;
		World::Asset::ProjectManifest manifest;
		if (!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
			return;
		m_ContentRoot = manifest.ResolveContentRoot(manifestPath);

		m_Levels = Level::LevelCatalog::Discover(m_ContentRoot);
	}

	void LevelPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		EnsureLoaded();

		float y = rect.Y + 8.0f;
		Label(ctx, { rect.X + 10.0f, y }, "Levels", theme.Text, 15.0f);
		y += 24.0f;

		// ---- 新建关卡:名称 + Create。成功即出现在下面的列表里,可直接 Open。----
		if (!m_ContentRoot.empty())
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
				const std::string name = m_NewLevelName;
				if (name.empty())
				{
					m_NewLevelError = Wui::Tr("panel.levels.name_empty", "Enter a level name.");
				}
				else if (name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
				{
					m_NewLevelError = Wui::Tr("panel.levels.name_invalid",
						"Level name cannot contain a path separator.");
				}
				else
				{
					const std::filesystem::path packageDir = m_ContentRoot / "levels" / name;
					std::string createError;
					// 起手实体走同一个"场景 → 包"捕获路径,因此新关卡打开就有相机/灯/方块,
					// 而不是一片黑让人怀疑没加载上。上下文取应用级(编辑器总有一个),
					// 不依赖"当前是否已打开场景"。
					WorldContext* context = Application::HasInstance()
						? &Application::Get().GetContext() : nullptr;
					if (context && Level::LevelPackage::CreateEmpty(*context, packageDir, name, &createError))
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
				m_ContentRoot.empty()
					? Wui::Tr("panel.levels.not_found", "No project manifest; cannot locate levels.")
					: Wui::Tr("panel.levels.empty",
						"No levels yet. Create one above (a level is a folder under assets/levels/)."),
				theme.TextMuted, 13.0f);
			return;
		}

		const float rowH = 24.0f;
		for (size_t index = 0; index < m_Levels.size(); ++index)
		{
			const Level::LevelInfo& level = m_Levels[index];
			const Wui::WuiRect row { rect.X + 8.0f, y, rect.W - 16.0f, rowH };
			Wui::PanelBackground(ctx, row, theme.PanelHeader, 1.0f);

			Label(ctx, { row.X + 6.0f, row.Y + 5.0f },
				level.DisplayName + "  (" + level.Id + ")", theme.Text, 13.0f);
			Label(ctx, { row.X + 6.0f, row.Y + rowH - 4.0f },
				"levels/" + level.Id, theme.TextMuted, 11.0f);

			if (Button(ctx, Wui::HashId(("level.open." + level.Id).c_str()),
				{ row.X + row.W - 70.0f, row.Y + 2.0f, 62.0f, 20.0f }, "Open", theme))
			{
				// 打开关卡包目录:EditorDocument 按目录分派到 Level 2.0 读路径。
				host.OpenScene(level.PackageDir);
			}
			y += rowH + 4.0f;
		}

		Label(ctx, { rect.X + 10.0f, rect.Y + rect.H - 20.0f },
			(m_ContentRoot / "levels").string(), theme.TextMuted, 11.0f);
	}
}
