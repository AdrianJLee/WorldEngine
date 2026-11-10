#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/Widgets/WuiPlot.h"

#include <memory>
#include <vector>

namespace World
{
	// 性能剖析面板:帧时间曲线 + 分位数 + 采样控制 + 内存概览。
	//
	// 设计约束(与 knowledge/contracts/telemetry-schema 的一致性规则):
	//   * 只读 `Telemetry::GetStats()` / `CollectFrameHistory()` —— **不自己算指标**,
	//     面板 / AI 命令 / 导出文件 / 无障碍节点看到的必须是同一份数字;
	//   * 刷新**零堆分配**:帧历史用定长栈缓冲,文本用 `char[]` + snprintf;
	//   * 关键数字同时注册无障碍节点,自动化无需解析像素。
	//
	// 它能回答的问题(人):这一帧慢不慢、稳不稳、慢在哪(分位数曲线 + 掉帧阈值线)。
	class ProfilerPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "profiler"; }
		const char* Title() const override { return "Profiler"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		// 曲线保留的帧数(与 Telemetry 的历史环形上限一致)。
		static constexpr size_t kHistoryFrames = 600;

	private:
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::shared_ptr<Wui::WuiPlot> m_Plot;
		std::vector<std::shared_ptr<Wui::WuiLabel>> m_Lines;
		float m_History[kHistoryFrames] = {};
		size_t m_HistoryCount = 0;
	};
}
