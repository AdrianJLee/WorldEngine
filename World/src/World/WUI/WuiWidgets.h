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

	void Panel(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiTheme& theme);
	void Label(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const WuiColor& color, float fontSize);
	// 标签列的默认宽度预算(设计单位)。旧调用点(不传 width)按它裁剪,而不是让文字压到
	// 右侧控件上;面板有更精确的列宽时显式传 width。
	// 150 = 现有面板最紧的标签列(SettingsPanel 的控件列在 x+170,标签列到 164)+ 余量。
	inline constexpr float LabelDefaultWidth = 150.0f;
	// P4-UX1 术语对照:主文案后追加英文术语(Caption + 次要色)。用于中文界面下的有歧义条目;
	// 英文界面传空 term 即可。P4-UX5 起显式裁剪:超出 width(设计单位,= 标签列宽)时按优先级降级 ——
	// ① 术语省略号、② 去掉术语只留主文案、③ 主文案本身按 EllipsizeToWidth 语义截断。
	// width 省略时用 LabelDefaultWidth;传 0 = 不做宽度裁剪(仅按自身文本绘制)。
	void LabelWithTerm(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const std::string& term,
		const WuiColor& color, float fontSize, const WuiTheme& theme, float width);
	// 兼容重载:旧调用点(Editor 侧 12 处)语义不变,内部按 LabelDefaultWidth 裁剪。
	void LabelWithTerm(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const std::string& term,
		const WuiColor& color, float fontSize, const WuiTheme& theme);

	// P4-UX4 悬停提示(用户 2026-09-20:"鼠标悬停在一个设置里的选项是不是得有介绍"):
	//  - `Tooltip(ctx, hoverRect, text)`:鼠标在该矩形内时登记文本(后登记覆盖先登记);
	//  - `DrawTooltip(ctx, theme)`:宿主在**画完所有面板之后**调用一次,画到 overlay 层
	//    (不受面板裁剪、永远在最上层);文本支持 '\n' 与按宽度自动换行。
	// 文案约定(见 skill engine-ui-patterns):名称 + 一句用途 + 默认值/范围 + 生效时机 + 禁用原因。
	void Tooltip(WuiContext& ctx, const WuiRect& hoverRect, const std::string& text);
	void DrawTooltip(WuiContext& ctx, const WuiTheme& theme);
	// P4-UX6 焦点环:id 是当前焦点控件时,按主题令牌画 1.5px 圆角描边(颜色 = theme.FocusRing)。
	// 控件在**自身绘制末尾**调用;环走 overlay 命令层(与 tooltip 同一机制),因此后画的兄弟控件
	// 不会盖住它。没有焦点时什么都不画,既有控件的命令流逐字节不变。
	void DrawFocusRing(WuiContext& ctx, const WuiRect& rect, WuiId id, const WuiTheme& theme);
	bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);
	bool Toggle(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);
	// P4-UX6 单行标签条:每个标签等分 rect.W,活跃标签 = ActiveBg 填充 + 底部 2px Accent 下划线;
	// 每个标签登记无障碍节点 kind="tab"、value=(active==i),可被 ui.invoke 点击。
	// 返回值 = 本帧选中项是否变化;中键点击某个标签把它的下标写进 *closeRequested(可选),
	// 控件**不自行关闭**(由调用方决定:关面板 / 提示未保存 / 忽略)。
	bool TabBar(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::vector<std::string>& tabs,
		int& active, const WuiTheme& theme, int* closeRequested = nullptr);
	// P4-UX6 分段按钮(用法约定 2–4 项,代码对任意数量都按等分处理,空表直接返回 false):
	// 同一圆角外壳内等分,选中项 ActiveBg + Accent 文本。无障碍节点 kind="segmented"(组,value=下标)
	// + 子项 kind="segmented-option"(value=(selected==i))。返回值 = 选中项是否变化。
	bool Segmented(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::vector<std::string>& options,
		int& selected, const WuiTheme& theme);
	// 值驱动的勾选框(供 Inspector 等绑定外部状态):状态写回 value;
	// 返回值 = 本帧是否被点击改值(与 Combo/DragInt/DragFloat 同约定)。
	bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool& value, const WuiTheme& theme);
	// 同上,带英文术语对照(中文界面下显示 "垂直同步  Vertical Sync")。
	bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const std::string& term,
		bool& value, const WuiTheme& theme);
	// P4-UX6 多选行的三态勾选:mixed=true 画水平短横(—)而不是勾;点击后按"全部选中"处理
	// (mixed 是按值传入的,本控件内部按 mixed=false、value=true 绘制并返回 true,由调用方把 value
	// 写给它管辖的多个对象)。无障碍节点 value = "mixed" / "true" / "false",kind 与 Checkbox 相同。
	bool CheckboxMixed(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		bool& value, bool mixed, const WuiTheme& theme);
	void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme);
	// 数值编辑:点击进入文本输入,按住左右拖动微调;Enter 提交,Escape 取消。
	bool DragFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float speed, float min, float max, const WuiTheme& theme);
	bool DragInt(WuiContext& ctx, WuiId id, const WuiRect& rect, int64_t& value, int64_t min, int64_t max, const WuiTheme& theme);
	// 文本输入:UTF-8 追加/退格;回车提交返回 true,Escape 失焦。
	// P4-U5a(2026-09-21):无障碍补充信息。**为什么要显式传**:占位提示是各面板自己画的 Label,
	// 读屏/脚本读不到 —— 实测 search 框的节点 label/value 双空(用户口径:两者不能同时为空)。
	// Label = 控件名(如 "Search assets");Placeholder = 空输入时当作 value 的占位文案(与画进框里的同一句)。
	struct TextFieldA11y
	{
		std::string Label;
		std::string Placeholder;
	};
	bool TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme,
		bool* cancelledOut = nullptr, const TextFieldA11y* a11y = nullptr);
	// P4-UX6 带行内错误的文本字段:error 非空时描边用 theme.Danger,并在控件下方画一行 Caption
	// 字号的 Danger 说明(调用方负责给这一行留高度;文案超宽按省略号裁剪)。
	// 返回值与 TextField 相同(回车提交)。无障碍节点 value 追加 " error=<文本>"。
	bool TextFieldEx(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer,
		const WuiTheme& theme, const std::string& error, const TextFieldA11y* a11y = nullptr);
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

	// ---- P4-UX7 / U2B:表头排序 / 颜色字段 / 分隔条 ----
	// 表头 + 排序:一行常驻表头,列矩形按 TableCell 口径用 columnWidths 累计排布。
	//  绘制:列名左对齐(右侧 14px 留给排序箭头,超宽按省略号裁剪);排序列文字 theme.Text 且右侧
	//  画 ▲(升)/▼(降),其余列 theme.TextMuted;悬停列底色 theme.HoverBg;底部 1px 线用
	//  theme.BorderStrong(与"表头常驻"的表格约定一致)。
	//  交互:点击列 = sortColumn 切到该列;再点同一列 = ascending 取反(换列不重置 ascending);
	//  焦点列上 Enter/Space 等价于点击。
	//  无障碍:每列一个节点 kind="table-header"、label=列名、value="asc"/"desc"/""、interactive=true,
	//  节点 id 由父 id 派生(稳定);脚本 ui.invoke 注入坐标后走与鼠标同一条 ctx.IsClicked 路径。
	//  返回 true = 本次改动了排序状态(sortColumn 或 ascending)。
	bool TableHeader(WuiContext& ctx, WuiId id, const WuiRect& table, const std::vector<std::string>& columns,
		const std::vector<float>& columnWidths, int& sortColumn, bool& ascending, const WuiTheme& theme);
	// 颜色字段:折叠态 = rect(建议 22 高) 内左侧 6px 圆角色块(棋盘格底 + 当前色)+ 右侧色值文本。
	//  点击(或焦点上 Enter/Space)用 ctx.OpenPopup/ClosePopup 展开 220×132 弹层:R/G/B/A 四条
	//  SliderFloat(0..1,两位小数)+ 顶部 hex 输入(#RRGGBB / #RRGGBBAA,允许省略 '#' 与大小写;
	//  6 位 = 不透明)。非法输入保持原值,并由 TextFieldEx 标红边 + 行内说明,失焦回到规范值。
	//  无障碍:字段本身 kind="color-field"、value="#RRGGBB"(带 alpha 时 8 位);弹层内的滑杆与
	//  hex 输入各自登记节点(走控件自身的登记,不需要额外节点)。
	//  返回 true = 本次通过滑杆或 hex 改动了 rgba。
	bool ColorField(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec4& rgba, const WuiTheme& theme);
	// 分隔条:vertical=true = 竖向条(左右拖动改 value,光标 ResizeEW);false = 横向条(上下拖动,
	//  ResizeNS)。命中带宽固定 6px、居中于传入 rect 的轴线;视觉线默认 1px(theme.Border),
	//  悬停 3px(theme.BorderStrong),拖动中 3px(theme.Accent)。拖动按"按下时的 value + 轴向位移"
	//  累加并 clamp 到 [minValue,maxValue];焦点上按轴向箭头 ±theme.Pad。双击复位由调用方决定。
	//  无障碍:kind="splitter"、value=当前值字符串、interactive=true。
	//  返回 true = 本次改动了 value。
	bool Splitter(WuiContext& ctx, WuiId id, const WuiRect& rect, bool vertical, float& value,
		float minValue, float maxValue, const WuiTheme& theme);

	// ---- P4-UX12 / U2C:向量字段 / 空状态 ----
	// 向量字段:三个分量同格(`X [ ]  Y [ ]  Z [ ]`),点进去输入、按住拖动微调(复用 DragFloat 的手感)。
	//  layout 0 = 横排(默认,放得下时用):三个分量按 rect.W 等分,分量之间留 theme.PadSmall;
	//  layout 1 = 竖排(窄面板):每行"标签 + 输入",三行等分 rect.H。
	//  每个分量左侧 12px 画 X/Y/Z 标签(theme.TextMuted + Caption 字号);拖动中的分量用 theme.Accent
	//  高亮(轴标签 + 输入框描边 + 数值文本)。
	//  数值文本与编辑缓冲都是两位小数(同一套文本,避免"看到的数"和"点进去的数"不一致)。
	//  交互:按下即取焦点;水平位移 > 1.5px 进入拖动(值 = 按下瞬间的值 + 位移 × speed,夹在
	//  [minValue,maxValue]);没拖动就松手 = 进入文本编辑(Enter 提交 / Escape 取消 / 点别处提交,
	//  复用 DragFloat 的同一套实现);焦点在整体上且不在编辑态时,Up/Down 按 |speed| 调"当前轴"
	//  (最近一次按下的分量,默认第一个)。minValue >= maxValue = 无界(与 DragFloat 的哨兵约定一致)。
	//  无障碍:整体 kind="vec3-field"、value="x,y,z";三个分量各登记一个 kind="vec3-axis" 子节点
	//  (id = DerivedChildId(id, ".axis.", i)、label="X"/"Y"/"Z"、value = 该分量的字符串化数值、
	//  interactive=true、不是独立焦点项)。返回值 = 本帧 value 是否真的被改动。
	bool Vec3Field(WuiContext& ctx, WuiId id, const WuiRect& rect, glm::vec3& value, float speed,
		float minValue, float maxValue, const WuiTheme& theme, int layout = 0);
	// 空状态(列表/面板没有内容时的统一表达):垂直居中依次排 glyph(可空,FontSizeHeading +
	//  theme.TextMuted)→ title(FontSizeTitle + theme.Text)→ hint(可空,FontSizeSmall + theme.TextMuted,
	//  按 rect 宽度自动换行、最多两行、超宽按 EllipsizeToWidth 语义补 '…')→ action 按钮(可空,
	//  居中,宽 = 文案宽 + 24px 内边距)。左右各留 24px 安全边距;内容装不下时从 rect 顶部开始。
	//  视觉克制:**不画**外框/背景块(面板自己已经有背景),只画文字与按钮。
	//  无障碍:一个 kind="empty-state" 节点(label=title、value=hint、interactive=false);有 action 时
	//  按钮由 Button 自己登记(可脚本点击)。返回值 = 用户点了 action 按钮。
	bool EmptyState(WuiContext& ctx, const WuiRect& rect, const std::string& glyph, const std::string& title,
		const std::string& hint, const std::string& actionLabel, WuiId actionId, const WuiTheme& theme);

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
