#pragma once

#include "EditorPanel.h"
#include "World/WUI/WuiWidget.h"

#include <memory>

namespace World
{
	// 只读统计面板:无本地状态,每帧从渲染/内存/操作日志读取。
	class StatsPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "stats"; }
		const char* Title() const override { return "Stats"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::vector<std::shared_ptr<Wui::WuiLabel>> m_Lines;
	};

	class MemoryPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "memory"; }
		const char* Title() const override { return "Memory Analyzer"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		struct Row
		{
			std::shared_ptr<Wui::WuiLabel> Name;
			std::shared_ptr<Wui::WuiLabel> Type;
			std::shared_ptr<Wui::WuiLabel> Usage;
			std::shared_ptr<Wui::WuiProgress> Bar;
			std::shared_ptr<Wui::WuiLabel> Allocs;
		};
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::vector<Row> m_Rows;
		size_t m_BuiltRows = 0;
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
