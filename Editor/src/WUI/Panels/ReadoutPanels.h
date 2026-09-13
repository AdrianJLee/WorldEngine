#pragma once

#include "EditorPanel.h"

namespace World
{
	// 只读统计面板:无本地状态,每帧从渲染/内存/操作日志读取。
	class StatsPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "stats"; }
		const char* Title() const override { return "Stats"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
	};

	class MemoryPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "memory"; }
		const char* Title() const override { return "Memory Analyzer"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
	};

	class OperationsPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "operations"; }
		const char* Title() const override { return "Operations"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
	};
}
