#include "wldpch.h"
#include "ReadoutPanels.h"

#include "World/Core/Memory/MemoryTracker.h"
#include "World/Renderer/Renderer2D.h"
#include "World/WUI/WuiWidgets.h"

namespace World
{
	namespace
	{
		std::string FormatBytes(size_t bytes)
		{
			const char* units[] = { "B", "KB", "MB", "GB", "TB" };
			double value = static_cast<double>(bytes);
			int unit = 0;
			while (value >= 1024 && unit < 4)
			{
				value /= 1024;
				++unit;
			}
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);
			return buffer;
		}

		const char* AllocatorTypeName(AllocatorType type)
		{
			switch (type)
			{
				case AllocatorType::Stack: return "Stack";
				case AllocatorType::Pool: return "Pool";
				case AllocatorType::DualTrack: return "DualTrack";
				case AllocatorType::Linear: return "Linear";
				default: return "Unknown";
			}
		}
	}

	void StatsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		float y = rect.Y + 8;
		const auto& stats = Renderer2D::GetStats();
		Label(ctx, { rect.X + 8, y }, "Application " + std::to_string(ctx.Input().FPS) + " FPS", theme.Text, 14.0f);
		y += 20;
		Label(ctx, { rect.X + 8, y }, "Renderer2D Stats:", theme.TextMuted, 14.0f);
		y += 20;
		Label(ctx, { rect.X + 8, y }, "Draw Calls: " + std::to_string(stats.DrawCalls), theme.Text, 14.0f);
		y += 18;
		Label(ctx, { rect.X + 8, y }, "Quads: " + std::to_string(stats.QuadCount), theme.Text, 14.0f);
		y += 18;
		Label(ctx, { rect.X + 8, y }, "Circles: " + std::to_string(stats.CircleCount), theme.Text, 14.0f);
		y += 18;
		Label(ctx, { rect.X + 8, y }, "Vertices: " + std::to_string(stats.GetTotalVertexCount()), theme.Text, 14.0f);
		y += 18;
		Label(ctx, { rect.X + 8, y }, "Indices: " + std::to_string(stats.GetTotalIndexCount()), theme.Text, 14.0f);
	}

	void MemoryPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const std::vector<AllocatorStats> snapshots = MemoryTracker::Get().GetFullSnapshot();
		const std::vector<float> columns { rect.W * 0.28f, 80, rect.W * 0.2f, rect.W * 0.36f, 70 };
		for (size_t row = 0; row < snapshots.size(); ++row)
		{
			const Wui::WuiRect name = TableCell(rect, columns, row, 0, 24);
			const Wui::WuiRect type = TableCell(rect, columns, row, 1, 24);
			const Wui::WuiRect usage = TableCell(rect, columns, row, 2, 24);
			const Wui::WuiRect bar = TableCell(rect, columns, row, 3, 24);
			const Wui::WuiRect allocs = TableCell(rect, columns, row, 4, 24);
			Label(ctx, { name.X + 6, name.Y + 4 }, snapshots[row].Name, theme.Text, 13.0f);
			Label(ctx, { type.X + 6, type.Y + 4 }, AllocatorTypeName(snapshots[row].Type), theme.TextMuted, 13.0f);
			Label(ctx, { usage.X + 6, usage.Y + 4 }, FormatBytes(snapshots[row].UsedBytes) + " / " + FormatBytes(snapshots[row].TotalReserved), theme.Text, 13.0f);
			const float fraction = snapshots[row].TotalReserved > 0 ? static_cast<float>(snapshots[row].UsedBytes) / static_cast<float>(snapshots[row].TotalReserved) : 0;
			const Wui::WuiRect track { bar.X + 4, bar.Y + 8, bar.W - 8, 8 };
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, track, theme.ButtonBg, 3.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { track.X, track.Y, track.W * fraction, track.H }, theme.Accent, 3.0f });
			Label(ctx, { allocs.X + 6, allocs.Y + 4 }, std::to_string(snapshots[row].NumAllocations), theme.Text, 13.0f);
		}
		MemoryTracker::Get().ClearEphemeralStats();
	}

	void OperationsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const std::vector<Wui::WuiOpRecord>& records = ctx.Ops().Records();
		const float rowHeight = 16.0f;
		float scrollY = 0;
		BeginScrollArea(ctx, rect, records.size() * rowHeight + 8.0f, scrollY, theme);
		float y = rect.Y + 6 - scrollY;
		for (auto it = records.rbegin(); it != records.rend(); ++it)
		{
			Wui::WuiColor color = theme.TextMuted;
			if (it->Category == "dock") color = theme.Accent;
			else if (it->Category == "undo") color = { 0.35f, 0.8f, 0.45f, 1 };
			else if (it->Category == "drag") color = theme.Text;
			const std::string line = "F" + std::to_string(it->Frame) + " [" + it->Category + "] " + it->Action +
				(it->Target.empty() ? "" : " " + it->Target) + (it->Detail.empty() ? "" : " " + it->Detail);
			Label(ctx, { rect.X + 8, y }, line, color, 12.0f);
			y += rowHeight;
		}
		EndScrollArea(ctx);
		if (Button(ctx, Wui::HashId("ops.clear"), { rect.X + rect.W - 70, rect.Y + 4, 60, 20 }, "Clear", theme))
		{
			ctx.Ops().Clear();
			ctx.RecordOp("ops", "clear", "", "");
		}
	}
}
