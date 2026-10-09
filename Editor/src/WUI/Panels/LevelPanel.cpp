#include "wldpch.h"
#include "WUI/Panels/LevelPanel.h"

#include "World/Asset/ProjectManifest.h"
#include "World/Level/Authoring/LevelCatalog.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
	void LevelPanel::EnsureLoaded()
	{
		if (m_Loaded)
			return;
		m_Loaded = true;

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

		if (m_Levels.empty())
		{
			Label(ctx, { rect.X + 10.0f, y },
				m_ContentRoot.empty()
					? Wui::Tr("panel.levels.not_found", "No project manifest; cannot locate levels.")
					: Wui::Tr("panel.levels.empty",
						"No levels yet. Add a folder under assets/levels/ (a manifest.yaml plus entities/)."),
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
