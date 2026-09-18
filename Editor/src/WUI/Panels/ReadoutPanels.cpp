#include "wldpch.h"
#include "ReadoutPanels.h"

#include "World/Core/Memory/MemoryTracker.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Renderer/Renderer3D.h"
#include "World/WUI/WuiWidget.h"

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

	void StatsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost&)
	{
		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Gap = 2;
			for (int i = 0; i < 10; ++i)
			{
				auto label = std::make_shared<Wui::WuiLabel>();
				label->FontSize = 14;
				m_Root->Add(label);
				m_Lines.push_back(label);
			}
		}

		const auto& stats = Renderer2D::GetStats();
		m_Lines[0]->Text = "Application " + std::to_string(ctx.Input().FPS) + " FPS";
		m_Lines[1]->Text = "Renderer2D Stats:";
		m_Lines[1]->Color = { 0.55f, 0.58f, 0.62f, 1 };
		m_Lines[2]->Text = "Draw Calls: " + std::to_string(stats.DrawCalls);
		m_Lines[3]->Text = "Quads: " + std::to_string(stats.QuadCount);
		m_Lines[4]->Text = "Circles: " + std::to_string(stats.CircleCount);
		m_Lines[5]->Text = "Vertices: " + std::to_string(stats.GetTotalVertexCount());
		m_Lines[6]->Text = "Indices: " + std::to_string(stats.GetTotalIndexCount());

		// P1b D4:光照数量上限与阴影耗时可见(验收条款)。
		const auto& stats3d = Renderer3D::GetStats();
		m_Lines[7]->Text = "Renderer3D Stats:";
		m_Lines[7]->Color = { 0.55f, 0.58f, 0.62f, 1 };
		m_Lines[8]->Text = "Lights: " + std::to_string(stats3d.Lights) + "/" + std::to_string(stats3d.MaxLights)
			+ (stats3d.DroppedLights ? " (+" + std::to_string(stats3d.DroppedLights) + " dropped)" : "");
		char shadowText[64] = {};
		std::snprintf(shadowText, sizeof(shadowText), "Shadow pass: %.2f ms", stats3d.ShadowPassMilliseconds);
		m_Lines[9]->Text = shadowText;

		Wui::LayoutWidgetTree(m_Root, { rect.X + 8, rect.Y + 8, rect.W - 16, rect.H - 16 });
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);
	}

	void MemoryPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost&)
	{
		const std::vector<AllocatorStats> snapshots = MemoryTracker::Get().GetFullSnapshot();
		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_Root->Gap = 4;
		}
		if (m_Rows.size() != snapshots.size())
		{
			m_Root->Invalidate();
			m_Rows.clear();
			for (size_t i = 0; i < snapshots.size(); ++i)
			{
				auto row = std::make_shared<Wui::WuiBox>();
				row->Direction = Wui::WuiDirection::Row;
				row->Gap = 8;
				row->AlignCross = Wui::WuiAlign::Center;

				Row entry;
				entry.Name = std::make_shared<Wui::WuiLabel>();
				entry.Type = std::make_shared<Wui::WuiLabel>();
				entry.Usage = std::make_shared<Wui::WuiLabel>();
				entry.Bar = std::make_shared<Wui::WuiProgress>();
				entry.Allocs = std::make_shared<Wui::WuiLabel>();
				for (auto& label : { entry.Name, entry.Type, entry.Usage, entry.Allocs })
					label->FontSize = 13;
				row->Add(entry.Name, { rect.W * 0.26f, rect.W * 0.26f, 0, 1e30f, 0 });
				row->Add(entry.Type, { 70, 70, 0, 1e30f, 0 });
				row->Add(entry.Usage, { rect.W * 0.2f, rect.W * 0.2f, 0, 1e30f, 0 });
				row->Add(entry.Bar, { rect.W * 0.32f, rect.W * 0.32f, 0, 1e30f, 1 });
				row->Add(entry.Allocs, { 60, 60, 0, 1e30f, 0 });
				m_Root->Add(row, { 0, 1e30f, 0, 24, 0 });
				m_Rows.push_back(std::move(entry));
			}
		}

		for (size_t i = 0; i < m_Rows.size(); ++i)
		{
			const AllocatorStats& snapshot = snapshots[i];
			m_Rows[i].Name->Text = snapshot.Name;
			m_Rows[i].Type->Text = AllocatorTypeName(snapshot.Type);
			m_Rows[i].Type->Color = { 0.55f, 0.58f, 0.62f, 1 };
			m_Rows[i].Usage->Text = FormatBytes(snapshot.UsedBytes) + " / " + FormatBytes(snapshot.TotalReserved);
			m_Rows[i].Bar->Fraction = snapshot.TotalReserved > 0
				? static_cast<float>(snapshot.UsedBytes) / static_cast<float>(snapshot.TotalReserved) : 0;
			m_Rows[i].Allocs->Text = std::to_string(snapshot.NumAllocations);
		}
		MemoryTracker::Get().ClearEphemeralStats();

		Wui::LayoutWidgetTree(m_Root, { rect.X + 8, rect.Y + 8, rect.W - 16, rect.H - 16 });
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);
	}

	void OperationsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost&)
	{
		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_Root->Gap = 4;
			m_Scroll = std::make_shared<Wui::WuiScrollArea>();
			m_Content = std::make_shared<Wui::WuiBox>();
			m_Content->Gap = 1;
			m_Scroll->Child = m_Content;
			m_Root->Add(m_Scroll, { 0, 1e30f, 0, 1e30f, 1 });

			auto clear = std::make_shared<Wui::WuiButton>();
			clear->Label = "Clear";
			clear->OnClick = [&ctx] { ctx.Ops().Clear(); ctx.RecordOp("ops", "clear", "", ""); };
			m_Root->Add(clear);
		}

		const std::vector<Wui::WuiOpRecord>& records = ctx.Ops().Records();
		if (m_Content->Children().size() != records.size())
		{
			// 操作日志频繁变化:内容子树按需重建(容器布局仍被缓存)。
			m_Content = std::make_shared<Wui::WuiBox>();
			m_Content->Gap = 1;
			for (size_t i = 0; i < records.size(); ++i)
				m_Content->Add(std::make_shared<Wui::WuiLabel>());
			m_Scroll->Child = m_Content;
			m_Root->Invalidate();
		}
		for (size_t i = 0; i < m_Content->Children().size() && i < records.size(); ++i)
		{
			const auto& record = records[records.size() - 1 - i];
			auto label = std::static_pointer_cast<Wui::WuiLabel>(m_Content->Children()[i].Widget);
			label->Text = "F" + std::to_string(record.Frame) + " [" + record.Category + "] " + record.Action +
				(record.Target.empty() ? "" : " " + record.Target) + (record.Detail.empty() ? "" : " " + record.Detail);
			label->FontSize = 12;
			label->FixedHeight = 16;
		}
		m_Scroll->ContentHeight = records.size() * 17.0f + 8;

		Wui::LayoutWidgetTree(m_Root, { rect.X + 8, rect.Y + 8, rect.W - 16, rect.H - 16 });
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);
	}
}
