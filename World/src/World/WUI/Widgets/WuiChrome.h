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

	// ---- 列表视图 ----
	struct ListViewItem
	{
		WuiId Id = 0;
		std::string Label;
		std::string SubLabel;      // 可选:右侧灰色副标题(如文件大小/类型)
		uint64_t Icon = 0;         // 可选图标纹理
		WuiRect Uv { 0, 0, 1, 1 };
		bool Selected = false;
		bool Disabled = false;
	};

	struct ListViewResult
	{
		int Clicked = -1;         // 单击(用于选中)
		int DoubleClicked = -1;   // 双击(用于打开)
		int ContextClicked = -1;  // 右键(用于上下文菜单)
		int Hovered = -1;
		std::vector<WuiRect> ItemRects; // 与 items 一一对应(调用方据此保留拖拽/重命名等自定义交互)
	};

	// 行列表:自带滚动裁剪与滚轮滚动(scrollY 由调用方持有)。
	ListViewResult ListView(WuiContext& ctx, const WuiRect& area, const std::vector<ListViewItem>& items,
		float rowHeight, float& scrollY, const WuiTheme& theme);

	// ---- 网格视图 ----
	struct GridViewItem
	{
		WuiId Id = 0;
		std::string Label;
		uint64_t Icon = 0;
		WuiRect Uv { 0, 0, 1, 1 };
		bool Selected = false;
		bool Disabled = false;
	};

	struct GridViewResult
	{
		int Clicked = -1;
		int DoubleClicked = -1;
		int ContextClicked = -1;
		int Hovered = -1;
		std::vector<WuiRect> ItemRects;
	};

	// 图标网格:按 cellWidth/cellHeight 自动换行,自带滚动裁剪。
	GridViewResult GridView(WuiContext& ctx, const WuiRect& area, const std::vector<GridViewItem>& items,
		float cellWidth, float cellHeight, float& scrollY, const WuiTheme& theme);

	// ---- 树视图 ----
	struct TreeViewItem
	{
		WuiId Id = 0;
		std::string Label;
		int Depth = 0;
		bool HasChildren = false;
		bool Expanded = false;
		bool Selected = false;
		bool Disabled = false;
	};

	struct TreeViewResult
	{
		std::vector<WuiRect> ItemRects;   // 与 items 索引对齐(不可见项也占位)
		std::vector<WuiRect> ArrowRects;  // 展开箭头区域(无子节点为零矩形)
		int ClickedArrow = -1;            // 点击展开箭头(调用方切换 Expanded)
		int Clicked = -1;                 // 点击行(调用方导航/选中)
		int DoubleClicked = -1;
		int ContextClicked = -1;
	};

	// 目录/层级树:行 = 缩进 + 展开箭头 + 标题,自带滚动裁剪。
	TreeViewResult TreeView(WuiContext& ctx, const WuiRect& area, const std::vector<TreeViewItem>& items,
		float rowHeight, float& scrollY, const WuiTheme& theme);
}
