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

	// 工作台"属性编辑器"由这些元数据自动生成 —— 加控件只改登记,不改工作台。
	struct WuiComponentProperty
	{
		enum class Kind : uint8_t { Bool = 0, Float = 1, Int = 2, Enum = 3, Text = 4 };

		std::string Name;                     // 属性名(工作台显示)
		Kind Type = Kind::Bool;
		float Min = 0.0f;                     // Float/Int 用
		float Max = 1.0f;
		float Step = 0.01f;
		std::vector<std::string> Options;     // Enum 用
		std::string DefaultText;              // Text 用(如"较长的示例文案")
	};

	// 可被工作台强制切换的视觉/交互状态(适用者登记,不适用的不列)。
	struct WuiComponentState
	{
		std::string Id;      // "default" / "hover" / "focus" / "pressed" / "disabled" / "error" / "loading"
		std::string Label;   // 工作台显示名
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
		void (*Showcase)(const WuiComponentDraw& draw) = nullptr;
	};

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
