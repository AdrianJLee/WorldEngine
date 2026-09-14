#pragma once

// WUI 界面骨架组件:停靠标签栏、分隔条、工具栏、右键菜单、路径面包屑、搜索框。
// 这些组件把编辑器外壳与面板里原本手写的矩形/文字/命中逻辑统一到一处,
// 主窗口停靠区、独立窗口、面板内部共用同一套外观与交互。

#include "World/WUI/WuiWidgets.h"

#include <functional>
#include <string>
#include <vector>

namespace World::Wui
{
	// ---- 停靠标签栏 ----
	struct DockTab
	{
		WuiId Id = 0;
		std::string Title;
		bool Active = false;
	};

	struct DockTabBarResult
	{
		int Clicked = -1;  // 单击激活
		int Closed = -1;   // 点击标签上的 x
		int DragStart = -1; // 按住标签并开始拖动(用于停靠拖拽/跨窗口附加)
		int Hovered = -1;
		bool BarHovered = false;
	};

	// 绘制标签栏并处理命中/关闭/拖动起手。标签高度默认 24。
	DockTabBarResult DockTabBar(WuiContext& ctx, const WuiRect& area, const std::vector<DockTab>& tabs,
		const WuiTheme& theme, float tabHeight = 24.0f, float maxTabWidth = 150.0f);

	// ---- 分隔条 ----
	struct SplitterResult
	{
		bool Hovered = false;
		bool Active = false;   // 正在拖动
		float Position = 0.0f; // 拖动时:相对 area 的对齐位置(像素)
	};

	// 可拖动分隔条(row=true 时是竖直条,拖动改变水平位置)。thickness 建议 4。
	SplitterResult Splitter(WuiContext& ctx, const WuiRect& area, bool row, const WuiTheme& theme,
		bool dragging, float thickness = 4.0f);

	// ---- 工具栏 ----
	// 绘制工具栏底板,返回内容区域(调用方在其中摆放按钮/分隔线)。
	WuiRect Toolbar(WuiContext& ctx, const WuiRect& area, const WuiTheme& theme,
		float rounding = 6.0f, float alpha = 0.85f);
	// 工具栏图标按钮:纹理缺失时退化为按钮底色 + 标签文字。
	bool ToolbarIconButton(WuiContext& ctx, WuiId id, const WuiRect& rect, uint64_t textureId,
		const WuiRect& uv, const std::string& fallbackLabel, const WuiTheme& theme, bool enabled = true);
	void ToolbarSeparator(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme);

	// ---- 右键菜单 ----
	// Begin:在固定位置画出菜单面板并返回是否为有效菜单区域(内部已 PushOverlay)。
	bool BeginContextMenu(WuiContext& ctx, WuiId id, const glm::vec2& pinnedPos, float width,
		size_t itemCount, WuiRect* panel, const WuiTheme& theme);
	void EndContextMenu(WuiContext& ctx, WuiId id, const WuiRect& panel, const WuiTheme& theme);
	// 菜单项:普通动作项。
	bool ContextMenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		const WuiTheme& theme, bool enabled = true);
	// 菜单项:带勾选标记(可见性开关)。
	bool ContextMenuToggleItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		bool checked, const WuiTheme& theme, bool enabled = true);
	void ContextMenuSeparator(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme);

	// ---- 面包屑 ----
	// 显示以 '/' 分隔的路径;返回被点击的段索引(-1 = 未点击)。
	int Breadcrumb(WuiContext& ctx, const WuiRect& area, const std::string& path, const WuiTheme& theme);

	// ---- 搜索框 ----
	// 带放大镜占位与清除按钮的文本框;返回回车提交。
	bool SearchField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer,
		const std::string& placeholder, const WuiTheme& theme);
}
