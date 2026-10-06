#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/Widgets/WuiPlot.h"

#include <memory>
#include <string>
#include <vector>

namespace World
{
	class SystemsPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "systems"; }
		const char* Title() const override { return "Systems Pipeline"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::vector<std::shared_ptr<Wui::WuiLabel>> m_Lines;
		// WP6:每行一个启用勾选框(值存 unique_ptr<bool> 以获得稳定地址,交给 WuiCheckbox::Value)。
		std::vector<std::unique_ptr<bool>> m_Toggles;
		std::vector<std::string> m_ToggleNames;
		std::vector<std::shared_ptr<Wui::WuiBox>> m_Rows;
	};

	class MemoryPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "memory"; }
		const char* Title() const override { return "Memory Analyzer"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		// **每行一个整宽标签**(单字符串),不再是"5 列 + 进度条"的横向组合。
		// 为什么:(实测)内存面板通常只有 ~240px 宽,而原来 5 个横向子项的期望宽度
		// 加起来 ~316px ⇒ 子项横向溢出 ⇒ 文字被裁掉/挤到面板外,面板看起来"没有数据"。
		// 单标签整宽布局在窄面板下也一定可见(标签自带省略号裁剪)。
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::shared_ptr<Wui::WuiPlot> m_Trend;
		std::vector<std::shared_ptr<Wui::WuiLabel>> m_Lines;
		// 内存趋势:数据由引擎每帧驱动采集(Telemetry::EndFrame -> MemoryTrack::Tick),
		// 面板只是把环形缓冲取出来画 ⇒ 面板关掉再打开,趋势依然完整。
		static constexpr size_t kTrendFrames = 600;
		// 两条序列:活跃字节(绿)与显存驻留(橙)。同图对比便于一眼看出"是堆涨了还是显存涨了"。
		float m_TrendHistory[kTrendFrames] = {};
		float m_TrendHistoryGpu[kTrendFrames] = {};
		size_t m_TrendCount = 0;
		size_t m_BuiltLines = 0;
	};

	class OperationsPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "operations"; }
		const char* Title() const override { return "Operations"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::shared_ptr<Wui::WuiScrollArea> m_Scroll;
		std::shared_ptr<Wui::WuiBox> m_Content;
		size_t m_BuiltCount = 0;
	};
}
