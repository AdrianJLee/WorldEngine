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
	// ---- 基础表面与高亮 ----
	// 面板/子区域底色:纯填充,统一从这里走,避免面板里散落裸绘制。
	void PanelBackground(WuiContext& ctx, const WuiRect& rect, const WuiColor& color, float radius = 0.0f);
	// 条状底板(挂靠栏/菜单栏等):填充 + 底边 1px 分隔线。
	void BarSurface(WuiContext& ctx, const WuiRect& rect, const WuiColor& fill, const WuiColor& border);
	// 行悬停/选中底色:仅在 hovered 或 selected 时绘制,返回 hovered。
	bool HoverRow(WuiContext& ctx, const WuiRect& rect, bool hovered, bool selected, const WuiTheme& theme,
		float radius = 3.0f);
	// 高亮描边:拖放目标/拖动标签等统一用强调色。
	void HighlightOutline(WuiContext& ctx, const WuiRect& rect, const WuiColor& color, float radius = 2.0f,
		float thickness = 2.0f);
	// 拖放落区预览:半透明强调色填充 + 高亮描边(边缘停靠/挂靠栏提示)。
	void DropZoneOverlay(WuiContext& ctx, const WuiRect& rect, float alpha = 0.30f, float radius = 3.0f);
	// 分节标题:标题文字 + 下方 1px 分隔线。
	void SectionHeader(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiColor& color,
		const WuiTheme& theme, float fontSize = 15.0f);

	// ---- 挂靠标签(主窗口顶栏 chip) ----
	struct AttachTagResult
	{
		bool Hovered = false;
		bool CloseHovered = false;
		bool Clicked = false;      // 单击标签本体(用于切换显示)
		bool CloseClicked = false; // 单击 × (closable 时才可能出现)
		WuiRect Rect {};
		WuiRect CloseRect {};
	};

	// 主窗口挂靠栏上的标签:活动/悬停底色 + 标题 + 可选关闭 ×。
	// 拖动状态机仍由调用方持有(本组件只负责绘制与命中)。
	AttachTagResult AttachTag(WuiContext& ctx, const WuiRect& rect, const std::string& title, bool active,
		bool closable, const WuiTheme& theme, float fontSize = 13.0f);

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
		// U2e 键盘导航(id != 0 时才有意义):控件只报告"要做什么",不动调用方的模型 ——
		// 当前项由调用方通过 items[i].Selected 传入,改选中/展开/激活都由调用方决定。
		int KeyActivate = -1;      // Enter:激活当前项(打开/重命名等,语义由调用方定)
		int KeyToggleExpand = -1;  // Left/Right:切换当前项展开态(调用方按 item.Expanded 处理)
		int KeyMoveTo = -1;        // Up/Down/Home/End:把选中移到该下标(夹取到两端 | 跳过 Disabled)
	};

	// 目录/层级树:行 = 缩进 + 展开箭头 + 标题,自带滚动裁剪。
	// id != 0 时整棵树进焦点表(Tab 可达)并画焦点环,同时吃 ↑/↓/←/→/Home/End/Enter 键,
	// 结果写进 KeyMoveTo / KeyToggleExpand / KeyActivate(旧调用点不传 id,行为不变)。
	TreeViewResult TreeView(WuiContext& ctx, const WuiRect& area, const std::vector<TreeViewItem>& items,
		float rowHeight, float& scrollY, const WuiTheme& theme, WuiId id = 0);
}
