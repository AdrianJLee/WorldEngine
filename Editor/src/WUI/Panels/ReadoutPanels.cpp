#include "wldpch.h"
#include <cstring>
#include "WUI/Panels/ReadoutPanels.h"

#include "World/Profiling/MemoryTrack.h"
#include "World/Profiling/Telemetry.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Renderer/Renderer3D.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"

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

	void SystemsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Ref<Scene> scene = host.GetActiveScene();

		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_Root->Gap = 2;
		}

		if (!scene)
		{
			if (m_Lines.size() != 1)
			{
				m_Root->Clear();
				m_Root->Invalidate();
				m_Lines.clear();
				auto label = std::make_shared<Wui::WuiLabel>();
				label->FontSize = 14;
				m_Root->Add(label);
				m_Lines.push_back(label);
			}
			m_Lines[0]->Text = "No active scene.";
			m_Lines[0]->Color = { 0.55f, 0.58f, 0.62f, 1.0f };
		}
		else
		{
			const auto& timings = scene->GetFrameSystemTimings();
			if (timings.empty())
			{
				if (m_Lines.size() != 2)
				{
					m_Root->Clear();
					m_Root->Invalidate();
					m_Lines.clear();
					for (int i = 0; i < 2; ++i)
					{
						auto label = std::make_shared<Wui::WuiLabel>();
						label->FontSize = 14;
						m_Root->Add(label);
						m_Lines.push_back(label);
					}
				}
				m_Lines[0]->Text = "Systems Pipeline:";
				m_Lines[0]->Color = { 0.55f, 0.58f, 0.62f, 1.0f };
				m_Lines[1]->Text = "No system timings available (idle or unsimulated).";
				m_Lines[1]->Color = { 0.55f, 0.58f, 0.62f, 1.0f };
			}
			else
			{
				const size_t requiredLines = timings.size() + 3;
				if (m_Lines.size() != requiredLines)
				{
					m_Root->Clear();
					m_Root->Invalidate();
					m_Lines.clear();
					m_Lines.reserve(requiredLines);
					m_Toggles.clear();
					m_ToggleNames.clear();
					m_Rows.clear();
					const size_t systemRows = timings.size();
					for (size_t i = 0; i < requiredLines; ++i)
					{
						if (i >= 2 && i < 2 + systemRows)
						{
							auto rowBox = std::make_shared<Wui::WuiBox>();
							rowBox->Direction = Wui::WuiDirection::Row;
							rowBox->Gap = 6;
							auto toggle = std::make_unique<bool>(true);
							auto check = std::make_shared<Wui::WuiCheckbox>();
							check->Value = toggle.get();
							rowBox->Add(check);
							auto label = std::make_shared<Wui::WuiLabel>();
							label->FontSize = 14;
							rowBox->Add(label);
							m_Root->Add(rowBox);
							m_Rows.push_back(rowBox);
							m_Toggles.push_back(std::move(toggle));
							m_ToggleNames.emplace_back();
							m_Lines.push_back(label);
						}
						else
						{
							auto label = std::make_shared<Wui::WuiLabel>();
							label->FontSize = 14;
							m_Root->Add(label);
							m_Lines.push_back(label);
						}
					}
				}

				m_Lines[0]->Text = "Systems Pipeline:";
				m_Lines[0]->Color = { 0.55f, 0.58f, 0.62f, 1.0f };

				m_Lines[1]->Text = "Systems Count: " + std::to_string(timings.size());
				m_Lines[1]->Color = { 1.0f, 1.0f, 1.0f, 1.0f };

				double totalMs = 0.0;
				size_t disabledCount = 0;
				for (size_t i = 0; i < timings.size(); ++i)
				{
					const auto& timing = timings[i];
					totalMs += timing.Milliseconds;
					if (!timing.Enabled)
						++disabledCount;

					// WP6:把勾选框与场景的真实启用态对齐;用户改了勾选框就写回(scene 是运行态
					// 唯一权威,面板只做视图)。SetFrameSystemEnabled 幂等。
					if (i < m_Toggles.size() && scene && m_Toggles[i])
					{
						if (m_ToggleNames[i] != timing.Name)
						{
							// 首次绑定:勾选框取场景的真实启用态。
							m_ToggleNames[i] = timing.Name;
							*m_Toggles[i] = scene->IsFrameSystemEnabled(timing.Name);
						}
						else if (*m_Toggles[i] != scene->IsFrameSystemEnabled(timing.Name))
						{
							// 用户改了勾选框 ⇒ 写回场景(面板只做视图)。
							scene->SetFrameSystemEnabled(timing.Name, *m_Toggles[i]);
						}
					}

					char buffer[256];
					std::snprintf(buffer, sizeof(buffer), "[%s] %s: %s (%s)%s - %.3f ms",
						Gameplay::SystemPhaseName(timing.Phase),
						timing.Owner.c_str(),
						timing.Name.c_str(),
						timing.ParallelSafe ? "Parallel" : "Main Thread",
						timing.Enabled ? "" : " (off)",
						timing.Milliseconds);

					m_Lines[2 + i]->Text = buffer;
					m_Lines[2 + i]->Color = { 0.85f, 0.88f, 0.92f, 1.0f };
				}

				char summaryBuffer[128];
				std::snprintf(summaryBuffer, sizeof(summaryBuffer), "Total Pipeline Time: %.3f ms", totalMs);
				m_Lines[2 + timings.size()]->Text = summaryBuffer;
				m_Lines[2 + timings.size()]->Color = { 0.45f, 0.85f, 0.45f, 1.0f };

				// 无障碍节点:自动化 / 压测可直接读取
				{
					Wui::WuiAccessNode node;
					node.Id = Wui::HashId("systems.pipeline");
					node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
					node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
					node.Kind = "status";
					node.Label = "systems pipeline";
					node.Value = "count=" + std::to_string(timings.size()) +
						" disabled=" + std::to_string(disabledCount) +
						" totalMs=" + std::to_string(totalMs);
					node.Rect = { rect.X + 8.0f, rect.Y + 8.0f, rect.W - 16.0f, 16.0f };
					node.Interactive = false;
					Wui::WuiAccessibility::Get().Register(node);
				}
			}
		}

		Wui::LayoutWidgetTree(m_Root, { rect.X + 8, rect.Y + 8, rect.W - 16, rect.H - 16 });
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);
	}

	void MemoryPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost&)
	{
		// 数据来自 MemoryTrack 的**引擎每帧驱动**快照:面板不开也有峰值与趋势。
		// 用固定栈缓冲收集,不构造 vector/string(避免观察者效应)。
		Profiling::MemoryTrack::AllocatorStat allocators[Profiling::MemoryTrack::kMaxAllocators];
		const size_t allocatorCount =
			Profiling::MemoryTrack::CollectAllocators(allocators, Profiling::MemoryTrack::kMaxAllocators);

		Profiling::MemoryTrack::TagStat tags[Profiling::MemoryTrack::kMaxTags];
		const size_t tagCount = Profiling::MemoryTrack::CollectTags(tags, Profiling::MemoryTrack::kMaxTags);

		Profiling::MemoryTrack::GpuOwnerStat gpu[Profiling::MemoryTrack::kMaxGpuOwners];
		const size_t gpuCount = Profiling::MemoryTrack::CollectGpuOwners(gpu, Profiling::MemoryTrack::kMaxGpuOwners);

		// 3 个分节标题 + 2 个汇总行,再加各分组条目 —— 与实际写入行数**逐条对齐**。
		// 基准行数 = 实际写入行数逐条对齐:Heap / Pools / 三个分节标题 = 5。
		static constexpr size_t kBaseLines = 6;   // Heap / Pools / 图例 / 三个分节标题 = 6
		const size_t requiredLines = kBaseLines + allocatorCount + tagCount + gpuCount;

		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_Root->Gap = 2;

			// 趋势曲线放最上面:内存问题几乎都是"看曲线"而不是"看某一帧的数字"。
			// 高度由控件自己的 MinHeight 决定(不靠 flex 项,避免误用轴向)。
			m_Trend = std::make_shared<Wui::WuiPlot>();
			m_Trend->Style = Wui::WuiPlot::Kind::Line;
			m_Trend->LineColor = { 0.55f, 0.85f, 0.55f, 1.0f };
			m_Trend->FillColor = { 0.55f, 0.85f, 0.55f, 0.16f };
			// 第二序列 = 显存驻留(橙);不填充,避免遮住主序列。
			m_Trend->LineColor2 = { 1.0f, 0.72f, 0.35f, 1.0f };
			m_Trend->FillSecond = false;
			m_Trend->MinHeight = 96.0f;
			m_Root->Add(m_Trend);
		}

		if (m_Lines.size() != requiredLines)
		{
			m_Root->Invalidate();
			m_Root->Clear();
			m_Lines.clear();
			// Clear() 会连趋势曲线一起清掉 —— 清完立刻补回(否则曲线永远不显示)。
			m_Root->Add(m_Trend);
			for (size_t i = 0; i < requiredLines; ++i)
			{
				auto label = std::make_shared<Wui::WuiLabel>();
				label->FontSize = 13;
				// 默认 flex 项:整宽、高度取自身。窄面板下也一定可见。
				m_Root->Add(label);
				m_Lines.push_back(label);
			}
			m_BuiltLines = requiredLines;
		}

		const auto setLine = [this](size_t index, const char* text, const Wui::WuiColor& color)
		{
			if (index >= m_Lines.size())   // 护栏:数错了只少画一行,不越界。
				return;
			m_Lines[index]->Text = text;
			m_Lines[index]->Color = color;
		};

		const Wui::WuiColor normal { 0.85f, 0.88f, 0.92f, 1.0f };
		const Wui::WuiColor muted { 0.55f, 0.58f, 0.62f, 1.0f };
		const Wui::WuiColor section { 0.62f, 0.72f, 0.88f, 1.0f };

		// 趋势:从引擎的帧环形缓冲取(面板关着的时候引擎也在采)。
		{
			uint64_t liveTrend[MemoryPanel::kTrendFrames] = {};
			uint64_t gpuTrend[MemoryPanel::kTrendFrames] = {};
			const size_t count = Profiling::MemoryTrack::CollectTrend(
				liveTrend, gpuTrend, MemoryPanel::kTrendFrames);
			m_TrendCount = count;
			float peakMb = 0.0f;
			for (size_t i = 0; i < count; ++i)
			{
				const float mb = static_cast<float>(static_cast<double>(liveTrend[i]) / (1024.0 * 1024.0));
				m_TrendHistory[i] = mb;
				if (mb > peakMb)
					peakMb = mb;
				m_TrendHistoryGpu[i] = static_cast<float>(static_cast<double>(gpuTrend[i]) / (1024.0 * 1024.0));
			}
			m_Trend->Samples = m_TrendHistory;
			m_Trend->SampleCount = m_TrendCount;
			m_Trend->Samples2 = m_TrendHistoryGpu;
			m_Trend->SampleCount2 = m_TrendCount;
			// 两条序列共用一条量程(否则"谁高谁低"不可比)⇒ 峰值取二者较大者。
			for (size_t i = 0; i < count; ++i)
				if (m_TrendHistoryGpu[i] > peakMb)
					peakMb = m_TrendHistoryGpu[i];
			// 量程:贴着数据取(min 取"最低点稍微下探"),这样**看得见波动**;
			// 用 0..峰值 会把曲线压成贴着顶的一条直线(实测就是这个观感)。
			float minMb = peakMb;
			for (size_t i = 0; i < count; ++i)
				if (m_TrendHistory[i] < minMb)
					minMb = m_TrendHistory[i];
			if (count == 0 || peakMb - minMb < 0.05f)
			{
				minMb = peakMb > 0.5f ? (peakMb - 0.5f) : 0.0f;
				peakMb = minMb + 1.0f;
			}
			m_Trend->MinValue = (minMb > 0.0f) ? (minMb * 0.98f) : 0.0f;
			m_Trend->MaxValue = peakMb * 1.02f;
			m_Trend->WarnThreshold = -1.0f;
		}

		size_t cursor = 0;
		char buffer[192];
		// 窄面板(~250px)下把名字裁到固定宽度并加省略号,保证后面的数字可见。
		const auto fitName = [](char* out, size_t outSize, const char* name)
		{
			const char* source = name ? name : "?";
			// 18 字符:够放 "EngineAllocator_SOS…" 这种前缀相同的内部名,
			// 截断后仍能互相区分(16 字符时两个不同分配器会截成同一个样子)。
			const size_t kField = 18;
			const size_t length = std::strlen(source);
			if (length <= kField)
			{
				std::snprintf(out, outSize, "%-16s", source);
				return;
			}
			// 截断到 15 字节再补 "…"(避免切坏 UTF-8 续字节)。
			size_t cut = kField - 1;
			while (cut > 0 && (static_cast<unsigned char>(source[cut]) & 0xC0u) == 0x80u)
				--cut;
			char head[22] = {};
			std::memcpy(head, source, cut);
			std::snprintf(out, outSize, "%s…", head);
		};

		// 1) 汇总(与 StatsSnapshot.Memory 同源)。
		//    上一版这里有 "Pools" 又紧跟一个 "-- Pools --" 分节标题,同一个词出现两次 ——
		//    用户反馈"有点乱"的一部分就来自这种重复。汇总行只留**总量级**两项,明细归分节。
		std::snprintf(buffer, sizeof(buffer), "%s %.1f MB (%s %.1f)   %s %llu",
			Wui::Tr("memory.heap", "Heap").c_str(),
			static_cast<double>(Profiling::MemoryTrack::HeapLiveBytes()) / (1024.0 * 1024.0),
			Wui::Tr("memory.peak", "peak").c_str(),
			static_cast<double>(Profiling::MemoryTrack::HeapPeakBytes()) / (1024.0 * 1024.0),
			Wui::Tr("memory.allocations", "Allocations").c_str(),
			static_cast<unsigned long long>(Profiling::MemoryTrack::LiveAllocationCount()));
		setLine(cursor++, buffer, normal);

		std::snprintf(buffer, sizeof(buffer), "%s %.1f MB",
			Wui::Tr("memory.vram", "VRAM resident").c_str(),
			static_cast<double>(Profiling::MemoryTrack::GpuResidentBytes()) / (1024.0 * 1024.0));
		setLine(cursor++, buffer, normal);

		// 图例:曲线两条线的颜色 ↔ 指标。放在汇总区最后一行,紧挨图表。
		std::snprintf(buffer, sizeof(buffer), "%s",
			Wui::Tr("memory.curveLegend", "chart: green = heap, orange = VRAM").c_str());
		setLine(cursor++, buffer, muted);

		// 2) 池(分配给器)。
		std::snprintf(buffer, sizeof(buffer), "-- %s (%zu) --",
			Wui::Tr("memory.section.pools", "Pools by allocator").c_str(), allocatorCount);
		setLine(cursor++, buffer, section);
		for (size_t i = 0; i < allocatorCount; ++i)
		{
			const auto& stat = allocators[i];
			char name[24];
			fitName(name, sizeof(name), stat.Name);
			// 行内只留"用了多少 / 预留多少"(用户最需要的一对);峰值在汇总行与无障碍值里。
			std::snprintf(buffer, sizeof(buffer), "%s %.2f / %.1f MB",
				name,
				static_cast<double>(stat.UsedBytes) / (1024.0 * 1024.0),
				static_cast<double>(stat.TotalReserved) / (1024.0 * 1024.0));
			setLine(cursor++, buffer, normal);
		}

		// 3) 标签(按子系统)。
		std::snprintf(buffer, sizeof(buffer), "-- %s (%zu) --",
			Wui::Tr("memory.section.tags", "Heap by subsystem").c_str(), tagCount);
		setLine(cursor++, buffer, section);
		for (size_t i = 0; i < tagCount; ++i)
		{
			const auto& stat = tags[i];
			char name[24];
			fitName(name, sizeof(name), stat.Name);
			std::snprintf(buffer, sizeof(buffer), "%s %.3f MB  (%llu)",
				name,
				static_cast<double>(stat.LiveBytes) / (1024.0 * 1024.0),
				static_cast<unsigned long long>(stat.LiveCount));
			setLine(cursor++, buffer, stat.LiveBytes > 0 ? normal : muted);
		}

		// 4) 显存(按所有者)。
		std::snprintf(buffer, sizeof(buffer), "-- %s (%zu) --",
			Wui::Tr("memory.section.gpu", "VRAM by owner").c_str(), gpuCount);
		setLine(cursor++, buffer, section);
		for (size_t i = 0; i < gpuCount; ++i)
		{
			const auto& stat = gpu[i];
			char name[24];
			fitName(name, sizeof(name), stat.Name);
			std::snprintf(buffer, sizeof(buffer), "%s %.2f MB  (%llu)",
				name,
				static_cast<double>(stat.Bytes) / (1024.0 * 1024.0),
				static_cast<unsigned long long>(stat.LiveCount));
			setLine(cursor++, buffer, normal);
		}

		// 无障碍节点:自动化可直接断言这组数字(键名与数据字典一致)。
		{
			char value[256];
			std::snprintf(value, sizeof(value),
				"trendFrames=%llu heapLive=%llu heapPeak=%llu liveAllocs=%llu pools=%llu gpuResident=%llu untrackedFrees=%llu",
				static_cast<unsigned long long>(m_TrendCount),
				static_cast<unsigned long long>(Profiling::MemoryTrack::HeapLiveBytes()),
				static_cast<unsigned long long>(Profiling::MemoryTrack::HeapPeakBytes()),
				static_cast<unsigned long long>(Profiling::MemoryTrack::LiveAllocationCount()),
				static_cast<unsigned long long>(Profiling::MemoryTrack::PoolUsedBytes()),
				static_cast<unsigned long long>(Profiling::MemoryTrack::GpuResidentBytes()),
				static_cast<unsigned long long>(Profiling::MemoryTrack::UnknownFrees()));
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("memory.summary");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "status";
			node.Label = "memory summary";
			node.Value = value;
			node.Rect = { rect.X + 8.0f, rect.Y + 8.0f, rect.W - 16.0f, 16.0f };
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		}

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
