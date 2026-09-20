#include "wldpch.h"
#include "LevelPanel.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Gameplay/LevelList.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
	void LevelPanel::EnsureLoaded()
	{
		if (m_Loaded)
			return;
		m_Loaded = true;

		// 与 GameHost 相同的定位顺序:内容根上一级 → 内容根 → 当前目录(Game/levels.welevel 是常见位置)。
		std::filesystem::path manifestPath;
		if (!World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
			return;

		std::string error;
		World::Asset::ProjectManifest manifest;
		if (!World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
			return;
		m_ContentRoot = manifest.ResolveContentRoot(manifestPath);

		const std::filesystem::path candidates[] = {
			m_ContentRoot.parent_path() / "levels.welevel",
			m_ContentRoot / "levels.welevel",
			std::filesystem::current_path() / "levels.welevel",
		};
		for (const std::filesystem::path& candidate : candidates)
		{
			if (!std::filesystem::exists(candidate))
				continue;
			Gameplay::LevelList list;
			if (Gameplay::LevelList::Load(candidate, &list, &error))
			{
				m_ListPath = candidate;
				m_Entries = list.Entries();
				break;
			}
			WLD_CORE_WARN("LevelPanel: failed to load '{0}': {1}", candidate.string(), error);
		}
	}

	void LevelPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		EnsureLoaded();

		float y = rect.Y + 8.0f;
		Label(ctx, { rect.X + 10.0f, y }, "Levels", theme.Text, 15.0f);
		y += 24.0f;

		if (m_Entries.empty())
		{
			Label(ctx, { rect.X + 10.0f, y },
				m_ListPath.empty()
					? Wui::Tr("panel.levels.not_found",
						"levels.welevel not found (the level list next to the project manifest).")
					: Wui::Tr("panel.levels.empty", "The level list is empty."), theme.TextMuted, 13.0f);
			return;
		}

		const float rowH = 24.0f;
		for (size_t index = 0; index < m_Entries.size(); ++index)
		{
			const Gameplay::LevelEntry& entry = m_Entries[index];
			const Wui::WuiRect row { rect.X + 8.0f, y, rect.W - 16.0f, rowH };
			Wui::PanelBackground(ctx, row, theme.PanelHeader, 1.0f);

			Label(ctx, { row.X + 6.0f, row.Y + 5.0f },
				entry.DisplayName + "  (" + entry.Id + ")", theme.Text, 13.0f);
			Label(ctx, { row.X + 6.0f, row.Y + rowH - 4.0f }, entry.ScenePath, theme.TextMuted, 11.0f);

			if (Button(ctx, Wui::HashId(("level.open." + entry.Id).c_str()),
				{ row.X + row.W - 70.0f, row.Y + 2.0f, 62.0f, 20.0f }, "Open", theme))
			{
				// 编辑器里的关卡切换 = 打开该关卡的场景资产(Play/Simulate 仍由视图口驱动)。
				const std::filesystem::path scenePath = m_ContentRoot / entry.ScenePath;
				if (std::filesystem::exists(scenePath))
					host.OpenScene(scenePath);
				else
					WLD_CORE_ERROR("LevelPanel: scene asset not found: {0}", scenePath.string());
			}
			y += rowH + 4.0f;
		}

		Label(ctx, { rect.X + 10.0f, rect.Y + rect.H - 20.0f },
			m_ListPath.string(), theme.TextMuted, 11.0f);
	}
}
