#include "wldpch.h"
#include "SavePanel.h"

#include "World/Gameplay/SaveService.h"
#include "World/WUI/Widgets/WuiChrome.h"

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
				"存档服务未就绪(需要项目清单与活动场景)。", theme.TextMuted, 14.0f);
			return;
		}

		std::unordered_map<uint32_t, Gameplay::SaveSlotInfo> bySlot;
		for (const Gameplay::SaveSlotInfo& info : saves->ListSaves())
			bySlot[info.Slot] = info;

		float y = rect.Y + 8.0f;
		Label(ctx, { rect.X + 10.0f, y }, "Save Slots", theme.Text, 15.0f);
		y += 26.0f;

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
					m_Status = "已保存到 slot " + std::to_string(slot);
				else
					m_Status = "保存失败: " + saves->GetLastError();
				m_StatusUntil = static_cast<double>(ctx.Frame()) + 300.0;
			}
			buttonX += actionW + 4.0f;
			if (Button(ctx, Wui::HashId(("load.slot." + std::to_string(slot)).c_str()),
				{ buttonX, buttonY, actionW, 22.0f }, "Load", theme))
			{
				if (!exists)
					m_Status = "slot " + std::to_string(slot) + " 为空";
				else if (saves->Load(slot))
				{
					const Gameplay::SaveLoadReport& report = saves->GetLastLoadReport();
					m_Status = "已读取 slot " + std::to_string(slot) + ": 更新 " + std::to_string(report.EntitiesUpdated)
						+ " / 新建 " + std::to_string(report.EntitiesCreated)
						+ " / 组件 " + std::to_string(report.ComponentsApplied)
						+ (report.ComponentsFailed ? " / 失败 " + std::to_string(report.ComponentsFailed) : "");
				}
				else
					m_Status = "读取失败: " + saves->GetLastError();
				m_StatusUntil = static_cast<double>(ctx.Frame()) + 300.0;
			}
			buttonX += actionW + 4.0f;
			if (Button(ctx, Wui::HashId(("delete.slot." + std::to_string(slot)).c_str()),
				{ buttonX, buttonY, actionW, 22.0f }, "Delete", theme))
			{
				m_Status = saves->Delete(slot) ? "已删除 slot " + std::to_string(slot) : "删除失败(槽位为空)";
				m_StatusUntil = static_cast<double>(ctx.Frame()) + 300.0;
			}
			y += rowH + 6.0f;
		}

		if (!m_Status.empty() && static_cast<double>(ctx.Frame()) < m_StatusUntil)
			Label(ctx, { rect.X + 10.0f, y + 4.0f }, m_Status, theme.TextMuted, 13.0f);

		Label(ctx, { rect.X + 10.0f, rect.Y + rect.H - 22.0f },
			"目录: " + saves->GetSaveRoot().string(), theme.TextMuted, 12.0f);
	}
}
