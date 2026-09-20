#include "wldpch.h"
#include "SavePanel.h"

#include "World/Gameplay/SaveService.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <ctime>

namespace World
{
	namespace
	{
		constexpr uint32_t kSlotCount = 5;   // 面板固定展示 5 个槽位(0..4)

		std::string FormatTimestamp(uint64_t seconds)
		{
			if (seconds == 0)
				return "-";
			const std::time_t raw = static_cast<std::time_t>(seconds);
			std::tm local {};
			if (localtime_s(&local, &raw) != 0)
				return "-";
			char buffer[32] = {};
			std::strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &local);
			return buffer;
		}
	}

	void SavePanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Gameplay::SaveService* saves = host.GetSaveService();
		if (!saves)
		{
			Label(ctx, { rect.X + 10.0f, rect.Y + 10.0f },
				Wui::Tr("panel.save.service_unavailable",
					"Save service unavailable (needs project manifest and active scene)."), theme.TextMuted, 14.0f);
			return;
		}

		std::unordered_map<uint32_t, Gameplay::SaveSlotInfo> bySlot;
		for (const Gameplay::SaveSlotInfo& info : saves->ListSaves())
			bySlot[info.Slot] = info;

		float y = rect.Y + 8.0f;
		Label(ctx, { rect.X + 10.0f, y }, "Save Slots", theme.Text, 15.0f);
		y += 26.0f;

		// 底部状态行 + 存档目录:空状态分支与槽位分支共用(正常分支的绘制顺序不变)。
		const auto drawFooter = [&]()
		{
			if (!m_Status.empty() && static_cast<double>(ctx.Frame()) < m_StatusUntil)
				Label(ctx, { rect.X + 10.0f, y + 4.0f }, m_Status, theme.TextMuted, 13.0f);
			Label(ctx, { rect.X + 10.0f, rect.Y + rect.H - 22.0f },
				Wui::Tr("panel.save.dir", "Directory: ") + saves->GetSaveRoot().string(), theme.TextMuted, 12.0f);
		};

		// ---- U2d:一个存档都没有 → 统一空状态 ----
		// 槽位行只在真的有存档内容时出现(否则"5 行 (empty)"与空状态是两套重复表达)。
		if (bySlot.empty())
		{
			const float emptyTop = y;
			const float emptyBottom = rect.Y + rect.H - 26.0f;   // 底部目录行上方
			const Wui::WuiRect emptyRect { rect.X + 8.0f, emptyTop, std::max(0.0f, rect.W - 16.0f),
				std::max(0.0f, emptyBottom - emptyTop) };
			const bool saveRequested = Wui::EmptyState(ctx, emptyRect, std::string(),
				Wui::Tr("panel.save.empty.title", "No saves yet"),
				Wui::Tr("panel.save.empty.hint",
					"Save the current scene to create the first save slot."),
				Wui::Tr("panel.save.empty.action", "Save to Slot 0"),
				Wui::HashId("save.empty.create"), theme);
			if (saveRequested)
			{
				// 与槽位行的 Save 按钮同一条路径。
				if (saves->Save(0, "current"))
					m_Status = Wui::Tr("panel.save.status.saved", "Saved to slot ") + std::to_string(0);
				else
					m_Status = Wui::Tr("panel.save.status.save_failed", "Save failed: ") + saves->GetLastError();
				m_StatusUntil = static_cast<double>(ctx.Frame()) + 300.0;
			}
			y = rect.Y + rect.H - 46.0f;   // 状态行落在目录行上方
			drawFooter();
			return;
		}

		const float actionW = 62.0f;
		const float rowH = 26.0f;
		for (uint32_t slot = 0; slot < kSlotCount; ++slot)
		{
			const auto found = bySlot.find(slot);
			const bool exists = found != bySlot.end();
			const bool valid = exists && found->second.Valid;

			const Wui::WuiRect row { rect.X + 8.0f, y, rect.W - 16.0f, rowH };
			Wui::PanelBackground(ctx, row, theme.PanelHeader, 1.0f);

			std::string label = "Slot " + std::to_string(slot) + "  ";
			if (!exists)
				label += "(empty)";
			else if (!valid)
				label += "(invalid: " + found->second.Error + ")";
			else
				label += found->second.Header.LevelId + "  v" + std::to_string(found->second.Header.Version)
					+ "  " + FormatTimestamp(found->second.Header.Timestamp);
			Label(ctx, { row.X + 6.0f, row.Y + 6.0f }, label,
				valid ? theme.Text : theme.TextMuted, 13.0f);

			const float buttonY = row.Y + 2.0f;
			float buttonX = row.X + row.W - actionW * 3.0f - 10.0f;
			if (Button(ctx, Wui::HashId(("save.slot." + std::to_string(slot)).c_str()),
				{ buttonX, buttonY, actionW, 22.0f }, "Save", theme))
			{
				const std::string before = saves->GetLastError();
				(void)before;
				if (saves->Save(slot, "current"))
					m_Status = Wui::Tr("panel.save.status.saved", "Saved to slot ") + std::to_string(slot);
				else
					m_Status = Wui::Tr("panel.save.status.save_failed", "Save failed: ") + saves->GetLastError();
				m_StatusUntil = static_cast<double>(ctx.Frame()) + 300.0;
			}
			buttonX += actionW + 4.0f;
			if (Button(ctx, Wui::HashId(("load.slot." + std::to_string(slot)).c_str()),
				{ buttonX, buttonY, actionW, 22.0f }, "Load", theme))
			{
				if (!exists)
					m_Status = Wui::Tr("panel.save.status.empty_slot", "Slot ") + std::to_string(slot)
						+ Wui::Tr("panel.save.status.empty_slot_suffix", " is empty");
				else if (saves->Load(slot))
				{
					const Gameplay::SaveLoadReport& report = saves->GetLastLoadReport();
					m_Status = Wui::Tr("panel.save.status.loaded", "Loaded slot ") + std::to_string(slot)
						+ Wui::Tr("panel.save.status.loaded_stats", ": updated ") + std::to_string(report.EntitiesUpdated)
						+ Wui::Tr("panel.save.status.loaded_created", " / created ") + std::to_string(report.EntitiesCreated)
						+ Wui::Tr("panel.save.status.loaded_components", " / components ") + std::to_string(report.ComponentsApplied)
						+ (report.ComponentsFailed ? Wui::Tr("panel.save.status.loaded_failed", " / failed ")
							+ std::to_string(report.ComponentsFailed) : "");
				}
				else
					m_Status = Wui::Tr("panel.save.status.load_failed", "Load failed: ") + saves->GetLastError();
				m_StatusUntil = static_cast<double>(ctx.Frame()) + 300.0;
			}
			buttonX += actionW + 4.0f;
			if (Button(ctx, Wui::HashId(("delete.slot." + std::to_string(slot)).c_str()),
				{ buttonX, buttonY, actionW, 22.0f }, "Delete", theme))
			{
				m_Status = saves->Delete(slot)
					? Wui::Tr("panel.save.status.deleted", "Deleted slot ") + std::to_string(slot)
					: Wui::Tr("panel.save.status.delete_failed", "Delete failed (slot is empty)");
				m_StatusUntil = static_cast<double>(ctx.Frame()) + 300.0;
			}
			y += rowH + 6.0f;
		}

		drawFooter();
	}
}
