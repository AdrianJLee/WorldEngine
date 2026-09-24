#pragma once

#include "World/Core/Export.h"
#include "World/WUI/WuiCore.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace World::Wui
{
	struct WuiContext;   // 前向声明:showcase 只画不持有上下文
	struct WuiTheme;
	struct WuiRect;

	// 组件在库里的成熟度。`Approved` = 该提交下的外观与行为基线(见 plan P1);改动它必须重新过验收。
	enum class WuiComponentStatus : uint8_t
	{
		Draft = 0,
		Approved = 1,
		Deprecated = 2,
	};

	// WUI-P1.5:属性分组(UE 式三块 + Behavior)。属性面板按它分区/折叠;
	// 旧登记项不写这个字段 = Content(既有 47 条语义不变)。
	enum class WuiComponentPropertyGroup : uint8_t
	{
		Content = 0,    // 文本 / 图标 / 纹理 id —— 交给控件的内容
		Style = 1,      // 字号 / 粗细 / 内边距 / 按状态的颜色
		Layout = 2,     // 首选尺寸 / 填充与对齐
		Behavior = 3,   // 行为开关(逻辑属性,如 disabled)
	};

	// 工作台"属性编辑器"由这些元数据自动生成 —— 加控件只改登记,不改工作台。
	struct WuiComponentProperty
	{
		// WUI-P1.5:新增 `Color` / `Size2` —— 值编码见文件末尾的 ParseComponentColor / ParseComponentSize。
		enum class Kind : uint8_t { Bool = 0, Float = 1, Int = 2, Enum = 3, Text = 4, Color = 5, Size2 = 6 };

		std::string Name;                     // 属性名(工作台显示)
		Kind Type = Kind::Bool;
		float Min = 0.0f;                     // Float/Int 用
		float Max = 1.0f;
		float Step = 0.01f;
		std::vector<std::string> Options;     // Enum 用
		// Text 用(如"较长的示例文案");Color/Size2 也填规范写法(#RRGGBB / WxH 的文本),
		// 这样尚未升级的文本行编辑器(只认 DefaultText)照样能编辑这两类属性。
		std::string DefaultText;

		// ---- WUI-P1.5 加值(全部带默认值:现有登记条目一个都不改也照样编译、行为不变)----
		WuiComponentPropertyGroup Group = WuiComponentPropertyGroup::Content;  // 分组
		std::string Unit;                     // 数值单位后缀("px" / "°" / "×");空 = 无单位
		std::string Doc;                      // 一行说明(属性面板的悬停提示;英文源文,可 i18n 覆盖)
		bool StateScoped = false;             // true = 值按状态分槽,Name 形如 "bg.hover"(面板配状态选择器)
		float DefaultNumber = 0.0f;           // Float/Int 的类型化默认(面板初值 / Reset 的目标)
		WuiColor DefaultColor {};             // Color 的类型化默认(暗色主题令牌值;文本编码 #RRGGBB[AA])
		glm::vec2 DefaultSize { 0.0f, 0.0f }; // Size2 的类型化默认(设计单位;文本编码 "WxH")
	};

	// 可被工作台强制切换的视觉/交互状态(适用者登记,不适用的不列)。
	struct WuiComponentState
	{
		std::string Id;      // "default" / "hover" / "focus" / "pressed" / "disabled" / "error" / "loading"
		std::string Label;   // 工作台显示名
	};

	// WUI-P1.5:交互契约 —— 一件组件"能怎么被用"的机器可读声明。工作台 Play 模式按它自动跑
	// 场景、探针按它生成输入脚本(不再每件手写鼠标/键盘脚本);静态件(label/image/spacer)声明空表。
	// 这条数据只在编辑器侧消费,不进游戏运行时(绘制路径零新增)。
	enum class WuiInteractionKind : uint8_t
	{
		Hover = 0,    // 鼠标移入/移出
		Click = 1,    // 按下 + 抬起
		Drag = 2,     // 按下 + 位移 + 抬起
		Type = 3,     // 文本/键盘字符注入
		Scroll = 4,   // 滚轮
		Key = 5,      // 具名按键(Enter/Space/Tab/箭头…)
	};

	// 期望的**可观测**证据 —— Play 模式自动跑完每件后按它断言。
	enum class WuiInteractionExpect : uint8_t
	{
		ValueChange = 0,   // a11y 节点的 value 变了
		PixelChange = 1,   // 画布像素哈希变了
		Event = 2,         // 产生了一次事件(操作记录/回调/命令计数)
		StateChange = 3,   // 视觉/交互状态迁移(如 focus 归属改变)
	};

	struct WuiComponentInteraction
	{
		WuiInteractionKind Kind = WuiInteractionKind::Click;            // 动作类型
		std::string TargetId;                                           // 目标 a11y id(可哈希文本,或派生子节点规则)
		WuiInteractionExpect Expect = WuiInteractionExpect::ValueChange; // 期望观测
		std::string Steps;                                              // 步骤(人/机可读)
		std::string Note;                                               // 备注(边界/前置条件)
	};

	// 工作台一次"画一件"所需的全部输入。属性值用字符串统一承载(控件自己解析),
	// 这样登记表与工作台都不需要知道各控件的字段类型。
	struct WuiComponentDraw
	{
		WuiContext* Context = nullptr;
		const WuiTheme* Theme = nullptr;
		WuiRect Rect {};                                  // 画布给这件组件的矩形
		std::string State = "default";                    // 见 WuiComponentState::Id
		std::vector<std::pair<std::string, std::string>> Properties;  // 覆盖值(名 → 文本)
		float UiScale = 1.0f;                             // 缩放(100% = 1.0)
		float Density = 1.0f;                             // 1.0 = comfortable,<1 = compact
		std::string Locale = "en";                        // "en" / "zh-CN"
	};

	// 一件组件在库里的登记。`Showcase` 必须是**真实控件代码路径**(不是复刻 demo),
	// 这样"库里通过 ⇒ 界面里也这样"才成立。
	struct WuiComponentDesc
	{
		std::string Id;                    // 唯一 id,如 "button.primary"、"dragfloat"
		// 该条登记包装的控件结构体名(如 "WuiButton"、"WuiDragFloat")。
		// 门禁用它比对"面板用到的控件类型是否都已在库里登记"(空 = 未声明,门禁会报缺口)。
		std::string TypeName;
		std::string DisplayName;           // 工作台显示名(默认英文;可用 i18n key 覆盖)
		std::string Category;              // 分组,如 "Buttons" / "Inputs" / "Containers"
		WuiComponentStatus Status = WuiComponentStatus::Draft;
		std::string SourceFile;            // 实现文件(追责与文档)
		std::string A11yNotes;             // role / 命名来源 / id 约定
		std::string SizeNotes;             // 尺寸约束:"min 24x24; preferred 96x28"
		std::vector<std::string> ExtraA11yIds;  // 该组件用到的稳定 a11y id(探针断言用)
		std::vector<WuiComponentState> States;
		std::vector<WuiComponentProperty> Properties;
		std::vector<WuiComponentInteraction> Interactions;   // 交互契约(P1.6);静态件为空
		void (*Showcase)(const WuiComponentDraw& draw) = nullptr;
	};

	// ---- 属性覆盖的值编码协议(WUI-P1.5;面板与 showcase 共用同一份实现)----
	// 覆盖值仍旧走 `name → text` 这一条通道,规范写法:
	//   · 颜色 = "#RRGGBB" / "#RRGGBBAA"(可省 '#',大小写不限;6 位 = 不透明)
	//   · 尺寸 = "<宽>x<高>"(设计单位;允许空格与 'X' 分开写,如 "128x24" / "200 X 32")
	//   · 其余类型沿既有文本写法(Bool = 0/1/true/false;Float/Int = 十进制;Enum/Text = 原文)
	// 解析失败一律返回 false 且不改 out(调用方保留当前值:不抛异常、不崩)。
	WLD_API bool ParseComponentColor(const std::string& text, WuiColor& out);
	WLD_API bool ParseComponentSize(const std::string& text, float& width, float& height);
	// 规范写法(面板回显/写回用):颜色 → "#RRGGBB"(A=1)或 "#RRGGBBAA";尺寸 → "WxH"(去尾零)。
	WLD_API std::string FormatComponentColor(const WuiColor& color);
	WLD_API std::string FormatComponentSize(float width, float height);

	// 只读查询接口:工作台、探针、门禁脚本都从这里取事实源。
	class WLD_API WuiComponentRegistry
	{
	public:
		// 全部登记项(按 Category 再按 DisplayName 排序;注册顺序无意义)。
		static const std::vector<WuiComponentDesc>& All();
		// 按 id 查找;不存在返回 nullptr。
		static const WuiComponentDesc* Find(const std::string& id);
		// 供控件实现文件在静态初始化期登记(幂等:同 id 重复登记 = 覆盖并记一条告警)。
		static void Register(WuiComponentDesc desc);
		// 注册项数量(探针/门禁断言用)。
		static size_t Count();
	};
}
