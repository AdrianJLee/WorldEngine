#pragma once

#include "World/WUI/WuiContext.h"

namespace World::Wui
{
	// P4-UX1:主题模式。默认暗色;浅色与"跟随系统"已可用(系统判定在 WuiTheme.cpp)。
	enum class WuiThemeMode : uint8_t
	{
		Dark = 0,
		Light = 1,
		System = 2,
	};

	// 设计令牌(P4-UX1)。控件只读这里,不再自带魔法数字。
	// 结构保持向后兼容:旧字段(PanelBg/PanelHeader/Border/Text/TextMuted/ButtonBg/
	// ButtonHover/Accent)继续有效,新增字段带默认值。
	struct WuiTheme
	{
		// ---- 颜色:中性色阶 ----
		WuiColor WindowBg { 0.059f, 0.067f, 0.082f, 1.0f };      // #0F1115 面板之间的空隙
		WuiColor PanelBg { 0.078f, 0.090f, 0.110f, 1.0f };       // #14171C
		WuiColor PanelHeader { 0.102f, 0.118f, 0.141f, 1.0f };   // #1A1E24
		WuiColor ContentBg { 0.063f, 0.075f, 0.094f, 1.0f };     // #101318 列表/输入框
		WuiColor HoverBg { 0.133f, 0.153f, 0.184f, 1.0f };       // #22272F
		WuiColor ActiveBg { 0.165f, 0.188f, 0.220f, 1.0f };      // #2A3038
		WuiColor Border { 0.169f, 0.192f, 0.220f, 1.0f };        // #2B3138
		WuiColor BorderStrong { 0.227f, 0.259f, 0.298f, 1.0f };  // #3A424C
		WuiColor Text { 0.843f, 0.863f, 0.890f, 1.0f };          // #D7DCE3
		WuiColor TextMuted { 0.545f, 0.584f, 0.639f, 1.0f };     // #8B95A3
		WuiColor TextDisabled { 0.353f, 0.384f, 0.427f, 1.0f };  // #5A626D
		// ---- 颜色:语义 ----
		WuiColor Accent { 0.298f, 0.553f, 1.0f, 1.0f };          // #4C8DFF
		WuiColor Success { 0.247f, 0.725f, 0.314f, 1.0f };       // #3FB950
		WuiColor Warning { 0.824f, 0.600f, 0.133f, 1.0f };       // #D29922
		WuiColor Danger { 0.973f, 0.318f, 0.286f, 1.0f };        // #F85149
		WuiColor Selection { 0.298f, 0.553f, 1.0f, 0.22f };      // Accent @22%
		WuiColor FocusRing { 0.298f, 0.553f, 1.0f, 1.0f };
		// ---- 兼容字段(旧调用点仍在使用) ----
		WuiColor ButtonBg { 0.133f, 0.153f, 0.184f, 1.0f };
		WuiColor ButtonHover { 0.165f, 0.188f, 0.220f, 1.0f };
		// ---- 排版与尺寸 ----
		float FontSizeCaption = 11.0f;
		float FontSizeSmall = 12.0f;
		float FontSizeBody = 14.0f;
		float FontSizeTitle = 15.0f;
		float FontSizeHeading = 18.0f;
		float RowHeight = 26.0f;        // 常规密度行高(紧凑 22 / 舒适 30)
		float ControlHeight = 24.0f;
		float Pad = 8.0f;
		float PadSmall = 4.0f;
		float Radius = 4.0f;
		float AnimFastMs = 120.0f;
		float AnimPanelMs = 180.0f;
	};

	// 当前主题与模式。`WLD_UI_THEME=dark|light|system` 覆盖默认(暗色)。
	WLD_API const WuiTheme& CurrentTheme();
	// 解析模式 → 主题(system 会查询操作系统设置)。
	WLD_API WuiTheme ResolveTheme(WuiThemeMode mode);
	WLD_API void SetThemeMode(WuiThemeMode mode);
	WLD_API WuiThemeMode GetThemeMode();
	// 模式每变一次 +1;宿主(EditorShell/独立窗口)据此刷新自己缓存的主题副本。
	WLD_API uint32_t ThemeGeneration();
	WLD_API bool IsDarkTheme();
	// UI 字号缩放(编辑器偏好"UI 缩放";`WLD_UI_SCALE` 覆盖,范围 0.8..1.5,默认 1.15)。
	// 只作用于文字绘制与度量(布局数值不变),因此不会改变面板结构。
	WLD_API float UiFontScale();
	WLD_API void SetUiFontScale(float scale);

	void Panel(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiTheme& theme);
	void Label(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const WuiColor& color, float fontSize);
	// P4-UX1 术语对照:主文案后追加英文术语(Caption + 次要色),右侧空间不足时自动省略。
	// 用于中文界面下的有歧义条目;英文界面传空 term 即可。
	void LabelWithTerm(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const std::string& term,
		const WuiColor& color, float fontSize, const WuiTheme& theme);
	bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);
	bool Toggle(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);
	// 值驱动的勾选框(供 Inspector 等绑定外部状态):状态写回 value;
	// 返回值 = 本帧是否被点击改值(与 Combo/DragInt/DragFloat 同约定)。
	bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool& value, const WuiTheme& theme);
	// 同上,带英文术语对照(中文界面下显示 "垂直同步  Vertical Sync")。
	bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const std::string& term,
		bool& value, const WuiTheme& theme);
	void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme);
	// 数值编辑:点击进入文本输入,按住左右拖动微调;Enter 提交,Escape 取消。
	bool DragFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float speed, float min, float max, const WuiTheme& theme);
	bool DragInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme);
	// 文本输入:UTF-8 追加/退格;回车提交返回 true,Escape 失焦。
	bool TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme, bool* cancelledOut = nullptr);
	void Image(WuiContext& ctx, const WuiRect& rect, uint64_t textureId, const WuiRect& uv, const WuiTheme& theme);
	bool Combo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		const std::vector<std::string>& options, int& selected, const WuiTheme& theme);
	// 可搜索下拉(资源选择用):点击/输入展开带输入框的弹层,按子串过滤选项,
	// 滚轮滚动结果列表,回车选中第一个匹配项。返回 true 表示本帧选了新值(写入 selected)。
	bool SearchableCombo(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
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
