#pragma once

#include "World/WUI/WuiContext.h"

namespace World::Wui
{
	// 默认主题;应用可覆盖后传入控件。
	struct WuiTheme
	{
		WuiColor PanelBg { 0.12f, 0.125f, 0.13f, 1.0f };
		WuiColor PanelHeader { 0.16f, 0.165f, 0.17f, 1.0f };
		WuiColor Border { 0.25f, 0.26f, 0.28f, 1.0f };
		WuiColor Text { 0.82f, 0.84f, 0.87f, 1.0f };
		WuiColor TextMuted { 0.55f, 0.58f, 0.62f, 1.0f };
		WuiColor ButtonBg { 0.20f, 0.21f, 0.23f, 1.0f };
		WuiColor ButtonHover { 0.27f, 0.28f, 0.31f, 1.0f };
		WuiColor Accent { 0.30f, 0.50f, 0.90f, 1.0f };
	};

	void Panel(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiTheme& theme);
	void Label(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const WuiColor& color, float fontSize);
	bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);
	bool Toggle(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);
	// 值驱动的勾选框(供 Inspector 等绑定外部状态)。
	bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool& value, const WuiTheme& theme);
	void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme);
	// 数值编辑:点击进入文本输入,按住左右拖动微调;Enter 提交,Escape 取消。
	bool DragFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float speed, float min, float max, const WuiTheme& theme);
	bool DragInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme);
	// 文本输入:UTF-8 追加/退格;回车提交返回 true,Escape 失焦。
	bool TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, bool* cancelledOut = nullptr);
	void Image(WuiContext& ctx, const WuiRect& rect, uint64_t textureId, const WuiRect& uv, const WuiTheme& theme);
	bool Combo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		const std::vector<std::string>& options, int& selected, const WuiTheme& theme);
	bool TreeNode(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool leaf, const WuiTheme& theme);

	// 菜单与弹窗
	bool BeginMenuBar(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme);
	void EndMenuBar(WuiContext& ctx);
	bool BeginMenu(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);
	void EndMenu(WuiContext& ctx, WuiId id, const WuiRect& panel, const WuiTheme& theme);
	// 弹出面板背景(在条目之前调用,避免背景盖住文字)。
	void DrawPanelSurface(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme);
	bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool enabled, const WuiTheme& theme);
	// checked:在标签前绘制勾选标记。
	bool MenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool checked, bool enabled, const WuiTheme& theme);
	bool BeginModal(WuiContext& ctx, WuiId id, const std::string& title, const glm::vec2& size, WuiRect* panel, const WuiTheme& theme);
	void EndModal(WuiContext& ctx, WuiId id);

	// 滚动区:Begin 后子项按 -scrollY 偏移绘制,End 恢复裁剪。
	bool BeginScrollArea(WuiContext& ctx, const WuiRect& viewport, float contentHeight, float& scrollY, const WuiTheme& theme);
	void EndScrollArea(WuiContext& ctx);

	// 表格单元矩形(按列宽累计)。
	WuiRect TableCell(const WuiRect& table, const std::vector<float>& columns, size_t row, size_t column, float rowHeight);

	// 标准窗口控制(最小化/最大化或还原/关闭),绘制在 bar 右侧;返回被点击的按钮。
	enum class WindowControl : uint8_t
	{
		None = 0,
		Minimize = 1,
		Maximize = 2,
		Close = 4,
	};
	WindowControl WindowControls(WuiContext& ctx, const WuiRect& bar, const WuiTheme& theme, bool maximized = false);
}
