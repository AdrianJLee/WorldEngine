#include "wldpch.h"
#include "WUI/Panels/ProfilerPanel.h"

#include "World/Profiling/MemoryTrack.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Profiling/Telemetry.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidget.h"

#include <cstdio>

namespace World
{
	namespace
	{
		// 行数 = 实际 setLine 次数(**逐条对齐**;数错会越界,见 trap.count-mismatch-heap-corruption)。
		//
		// 布局原则(用户反馈"信息有点乱、有些看不懂"后的改版):
		//   1. **按问题分组**:帧是否健康 / 渲染画了什么 / 内存 / 采集 — 每组一个标题;
		//   2. **标签写全**:不用 `R draws`、`tris`、`obj 0/0`、`pk`、`w3600` 这类只有作者看得懂的缩写;
		//   3. **斜杠要说明**:`8/8` 改成 `8 of 8 visible`,读者不必猜分子分母;
		//   4. **面板里只放数据**:环境变量、配置路径这类"怎么开"的说明移到文档与 tooltip,
		//      不再和数字挤在一个列表里(那是"乱"的主要来源)。
		constexpr int kLineCount = 13;
	}

	void ProfilerPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost&)
	{
		// 数据源:唯一真相(与 prof.stats / 导出 / 无障碍同源)。
		const Profiling::Telemetry::StatsSnapshot snapshot = Profiling::Telemetry::GetStats();

		if (!m_Root)
		{
			m_Root = std::make_shared<Wui::WuiBox>();
			m_Root->Direction = Wui::WuiDirection::Column;
			m_Root->Gap = 4;

			// 帧时间曲线:数据由本面板持有(WuiPlot 只读指针,不拷贝)。
			m_Plot = std::make_shared<Wui::WuiPlot>();
			m_Plot->Style = Wui::WuiPlot::Kind::Line;
			m_Plot->LineColor = { 0.35f, 0.75f, 1.0f, 1.0f };
			m_Plot->FillColor = { 0.35f, 0.75f, 1.0f, 0.16f };
			m_Plot->ThresholdColor = { 1.0f, 0.45f, 0.35f, 0.9f };
			m_Root->Add(m_Plot, { 0, 1e30f, 0, 120, 0 });

			for (int i = 0; i < kLineCount; ++i)
			{
				auto label = std::make_shared<Wui::WuiLabel>();
				label->FontSize = 13;
				m_Root->Add(label);
				m_Lines.push_back(label);
			}
		}

		// 帧历史(定长缓冲,零分配)。
		m_HistoryCount = Profiling::Telemetry::CollectFrameHistory(m_History, kHistoryFrames);
		m_Plot->Samples = m_History;
		m_Plot->SampleCount = m_HistoryCount;
		// 显式量程:0..100ms —— 固定量程让"看得见曲线在动"而不是每帧被自动定标拉平。
		m_Plot->MinValue = 0.0f;
		m_Plot->MaxValue = 100.0f;
		// 掉帧阈值线:引擎给的判据(2× 中位数),不是面板自己编的。
		m_Plot->WarnThreshold = snapshot.Frame.JankThresholdMs > 0.0f
			? snapshot.Frame.JankThresholdMs : -1.0f;

		// 分位数参考线(p50/p95/p99)。与下面的文本行**同一个 StatsSnapshot** ——
		// 图上看到的线和文字里的数字必然一致,不会出现"图说一套字说一套"。
		// 颜色分工:p50 绿(=典型帧)、p95 黄(=偏慢)、p99 红(=尾部);越界由 WuiPlot 跳过。
		{
			uint32_t guide = 0;
			if (snapshot.Frame.P50Ms > 0.0f)
				m_Plot->Guides[guide++] = { snapshot.Frame.P50Ms, { 0.45f, 0.85f, 0.45f, 0.55f } };
			if (snapshot.Frame.P95Ms > 0.0f)
				m_Plot->Guides[guide++] = { snapshot.Frame.P95Ms, { 0.95f, 0.85f, 0.35f, 0.55f } };
			if (snapshot.Frame.P99Ms > 0.0f)
				m_Plot->Guides[guide++] = { snapshot.Frame.P99Ms, { 0.95f, 0.45f, 0.40f, 0.55f } };
			m_Plot->GuideCount = guide;
		}

		const auto setLine = [this](int index, const char* text, const Wui::WuiColor& color)
		{
			m_Lines[index]->Text = text;
			m_Lines[index]->Color = color;
		};
		const Wui::WuiColor normal { 0.85f, 0.88f, 0.92f, 1.0f };
		const Wui::WuiColor muted { 0.55f, 0.58f, 0.62f, 1.0f };
		const Wui::WuiColor good { 0.45f, 0.85f, 0.45f, 1.0f };
		// 分节标题:与内存面板同一套(-- X -- 的蓝灰),跨面板保持一致。
		const Wui::WuiColor head { 0.62f, 0.72f, 0.88f, 1.0f };

		char buffer[200];

		// 标签一律走本地化(默认英文;中文等走 catalog)。标签都取得很短(≤15 字符),
		// MSVC 的 SSO 装得下 ⇒ 不会因为每帧 Tr() 产生堆分配(这条面板一直守着零分配)。
		const auto section = [&](int index, const char* key, const char* fallback)
		{
			// "-- X --" 与内存面板同一形态:两个面板的分节看起来是一套东西。
			std::snprintf(buffer, sizeof(buffer), "-- %s --", Wui::Tr(key, fallback).c_str());
			setLine(index, buffer, head);
		};

		// ---- 帧:这一帧健康吗 ----
		{
			const double gpuMs = Renderer3D::GetSceneStatistics().GpuMilliseconds;
			if (gpuMs > 0.0)
				std::snprintf(buffer, sizeof(buffer), "%s %llu      %.2f ms      GPU %.2f ms",
					Wui::Tr("profiler.frame", "Frame").c_str(),
					static_cast<unsigned long long>(snapshot.FrameIndex),
					snapshot.Frame.LastMs, gpuMs);
			else
				// GPU 关着时写明"未测",不要让人以为是 0ms(缺失 ≠ 零)。
				std::snprintf(buffer, sizeof(buffer), "%s %llu      %.2f ms      GPU %s",
					Wui::Tr("profiler.frame", "Frame").c_str(),
					static_cast<unsigned long long>(snapshot.FrameIndex),
					snapshot.Frame.LastMs, Wui::Tr("profiler.notMeasured", "not measured").c_str());
			setLine(0, buffer, normal);

			std::snprintf(buffer, sizeof(buffer), "%.1f FPS average", snapshot.Frame.EmaFps);
			setLine(1, buffer, normal);

			// 分位数:写明"最近 N 帧",避免被当成全程统计。
			std::snprintf(buffer, sizeof(buffer),
				"typical %.1f   slow %.1f   worst 1%% %.1f   max %.1f ms",
				snapshot.Frame.P50Ms, snapshot.Frame.P95Ms, snapshot.Frame.P99Ms, snapshot.Frame.MaxMs);
			setLine(2, buffer, normal);
			std::snprintf(buffer, sizeof(buffer), "%s %u %s",
				Wui::Tr("profiler.lastFrames", "percentiles over last").c_str(),
				snapshot.Frame.WindowFrames,
				Wui::Tr("profiler.frames", "frames").c_str());
			setLine(3, buffer, muted);

			// 掉帧:说清"多少帧、超过什么线",而不是只给一个数字。
			if (snapshot.Frame.JankThresholdMs > 0.0f)
				std::snprintf(buffer, sizeof(buffer), "%u %s over %.1f ms (2x typical)",
					snapshot.Frame.JankCount, Wui::Tr("profiler.jankFrames", "slow frames").c_str(),
					snapshot.Frame.JankThresholdMs);
			else
				std::snprintf(buffer, sizeof(buffer), "%u %s (%s)",
					snapshot.Frame.JankCount, Wui::Tr("profiler.jankFrames", "slow frames").c_str(),
					Wui::Tr("profiler.thresholdPending", "baseline not ready").c_str());
			setLine(4, buffer, snapshot.Frame.JankCount > 0
				? Wui::WuiColor { 1.0f, 0.7f, 0.3f, 1.0f } : good);
		}

		// ---- 渲染:画了多少东西 ----
		section(5, "profiler.section.render", "RENDER");
		{
			const Renderer3D::SceneStatistics scene = Renderer3D::GetSceneStatistics();
			const Renderer3D::Statistics r3 = Renderer3D::GetStats();

			std::snprintf(buffer, sizeof(buffer), "%s %u      %s %u",
				Wui::Tr("profiler.draws", "Draws").c_str(), scene.DrawCalls,
				Wui::Tr("profiler.triangles", "Triangles").c_str(), scene.Triangles);
			setLine(6, buffer, normal);

			// 斜杠改成 "of ... visible",读者不用猜分子分母。
			std::snprintf(buffer, sizeof(buffer), "%u of %u %s      %s %u",
				scene.Submitted, scene.Objects, Wui::Tr("profiler.visible", "visible").c_str(),
				Wui::Tr("profiler.culled", "Culled").c_str(), scene.Culled);
			setLine(7, buffer, normal);

			std::snprintf(buffer, sizeof(buffer), "%s %u of %u      %s %.2f ms",
				Wui::Tr("profiler.lights", "Lights").c_str(), r3.Lights, r3.MaxLights,
				Wui::Tr("profiler.shadow", "Shadow").c_str(), r3.ShadowPassMilliseconds);
			const bool overflow = r3.DroppedLights > 0 || scene.DroppedObjects > 0;
			setLine(8, buffer, overflow ? Wui::WuiColor { 1.0f, 0.7f, 0.3f, 1.0f } : normal);
		}

		// ---- 内存 ----
		section(9, "profiler.section.memory", "MEMORY");
		std::snprintf(buffer, sizeof(buffer), "%s %.1f MB (%s %.1f)      %s %llu",
			Wui::Tr("profiler.heap", "Heap").c_str(),
			static_cast<double>(snapshot.Memory.CpuTotalBytes) / (1024.0 * 1024.0),
			Wui::Tr("profiler.peak", "peak").c_str(),
			static_cast<double>(snapshot.Memory.CpuPeakBytes) / (1024.0 * 1024.0),
			Wui::Tr("profiler.allocations", "Allocations").c_str(),
			static_cast<unsigned long long>(Profiling::MemoryTrack::LiveAllocationCount()));
		setLine(10, buffer, normal);
		std::snprintf(buffer, sizeof(buffer), "%s %.1f MB      %s %.1f MB",
			Wui::Tr("profiler.vram", "VRAM").c_str(),
			static_cast<double>(snapshot.Memory.GpuResidentBytes) / (1024.0 * 1024.0),
			Wui::Tr("profiler.pools", "Pools").c_str(),
			static_cast<double>(Profiling::MemoryTrack::PoolUsedBytes()) / (1024.0 * 1024.0));
		setLine(11, buffer, normal);

		// ---- 采集:只报状态。用法说明不进面板(它属于文档/tooltip,不属于数字列表) ----
		{
			if (snapshot.Capture.Active)
				std::snprintf(buffer, sizeof(buffer), "%s  %u / %u %s",
					Wui::Tr("profiler.capturing", "Capturing").c_str(),
					snapshot.Capture.FramesCaptured, snapshot.Capture.FramesRequested,
					Wui::Tr("profiler.frames", "frames").c_str());
			else if (snapshot.Capture.HasResult)
				std::snprintf(buffer, sizeof(buffer), "%s  %s",
					Wui::Tr("profiler.captureSaved", "Captured").c_str(),
					snapshot.Capture.OutputPath.c_str());
			else
				std::snprintf(buffer, sizeof(buffer), "%s  %s",
					Wui::Tr("profiler.capture", "Capture").c_str(),
					Wui::Tr("profiler.idle", "idle").c_str());
			setLine(12, buffer, snapshot.Capture.DroppedEvents > 0
				? Wui::WuiColor { 1.0f, 0.5f, 0.4f, 1.0f } : muted);
		}
		// 无障碍节点:自动化可直接断言这组数字(键名与数据字典一致)。
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("profiler.summary");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "status";
			node.Label = "profiler summary";
			char value[256];
			std::snprintf(value, sizeof(value),
				"frame=%llu lastMs=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f jank=%u "
				"fps=%.2f heapLive=%lld gpuResident=%lld",
				static_cast<unsigned long long>(snapshot.FrameIndex),
				snapshot.Frame.LastMs, snapshot.Frame.P50Ms, snapshot.Frame.P95Ms,
				snapshot.Frame.P99Ms, snapshot.Frame.MaxMs, snapshot.Frame.JankCount,
				snapshot.Frame.EmaFps,
				static_cast<long long>(snapshot.Memory.CpuTotalBytes),
				static_cast<long long>(snapshot.Memory.GpuResidentBytes));
			node.Value = value;
			node.Rect = { rect.X + 8.0f, rect.Y + 8.0f, rect.W - 16.0f, 16.0f };
			node.Interactive = false;
			// 面板里放不下的解释放这里(不占视觉空间,但 AI/读屏能拿到):
			// 曲线三条参考线的颜色对应关系、以及 GPU 计时怎么打开。
			node.Tooltip =
				"chart guides: green = p50 (typical), yellow = p95, red = p99, orange = 2x p50 threshold. "
				"GPU ms requires project setting rendering.gpu_timing = on (then it lags ~3 frames). "
				"Capture: set WLD_TELEMETRY_CAPTURE=<frames> (and WLD_TELEMETRY_OUT=<path>).";
			Wui::WuiAccessibility::Get().Register(node);
		}

		Wui::LayoutWidgetTree(m_Root, { rect.X + 8, rect.Y + 8, rect.W - 16, rect.H - 16 });
		Wui::WuiPaintContext paint(ctx);
		m_Root->Paint(paint);
	}
}
