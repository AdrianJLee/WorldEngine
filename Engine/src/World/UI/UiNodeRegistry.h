#pragma once

// 游戏 UI 框架(GameUI)— 节点类型注册表(工作包 M2)。
//
// 契约:`contract.ui-runtime` §10(类型/属性注册制)、`contract.wui-component-library`
// (新控件必须进 WuiComponentRegistry)、`contract.ui-document-format` §6(未知 Type 必须报错)。
//
// 职责边界:`UiNodeRegistry` 只做三件事 ——
//   ① `.wui` 的 `Type` 名 → **既有** `Wui::WuiComponentRegistry` 登记项(`ComponentId`)的映射;
//   ② 该类型画进 `WuiContext` 的绘制入口(`Paint`)与无障碍 `Role`;
//   ③ 属性元数据的查询转发(不复制第二份属性表)。
// 它**不新增控件命名**:`ComponentId` 必须能被 `WuiComponentRegistry::Find()` 命中,
// 否则 `RegisterType` 直接失败(禁止绕过组件登记表另起一套命名)。

#include "World/Core/Export.h"
#include "World/WUI/WuiComponentRegistry.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace World::UI
{
	struct UiNodePaintContext;   // 定义在 UiPainter.h(注册表只存函数指针,不依赖绘制实现)
	struct UiNodeInstance;       // 定义在 UiScreen.h(同上:只存函数指针,不依赖实例实现)

	// 一件 `.wui` 节点类型的绘制入口。与 `WuiComponentDesc::Showcase` 同口径:
	// 必须是真实绘制路径(命令进 `WuiContext`),不是复刻 demo。
	using UiNodePaintFn = void (*)(UiNodePaintContext& context);

	// 滚动内容尺寸(M10):程序化列表/网格的内容不是文档子节点,不能靠"子的包围盒"求内容尺寸,
	// 由类型自报(设计空间)。返回 false = 不自报,调用方(UiScreen)退回子的包围盒。
	using UiNodeContentSizeFn = bool (*)(const UiNodeInstance& node, glm::vec2& outSize);

	struct UiNodeTypeDesc
	{
		std::string Type;                  // 规范 `.wui` Type 名(如 "Button");文档里写它
		std::string ComponentId;           // WuiComponentRegistry 的 id(如 "button");必须是既有 id
		std::string Role;                  // 无障碍 role(写进 WuiAccessNode::Role)
		std::vector<std::string> Aliases;  // 兼容别名(可含组件 TypeName,如 "WuiButton")
		bool Interactive = false;          // 是否可被输入/AI 激活(决定 Actions="click")
		UiNodePaintFn Paint = nullptr;     // 绘制入口;空 = 该类型不可绘制(登记失败)
		// 本层消费、但组件登记属性表里没有的属性名(如 Panel 的 bg/title)。
		// 只用于"未知属性"可读提示,不改变绘制语义。
		std::vector<std::string> ExtraProps;
		// M10:滚动内容尺寸自报入口(仅对滚动容器有意义);空 = 用子的包围盒。追加在末尾,
		// 既有聚合初始化不受影响。
		UiNodeContentSizeFn ContentSize = nullptr;
		// M41:一行功能介绍(英文源文;UI 侧用 `wui.component.<ComponentId>.doc` 本地化覆盖)。
		// 面向"用控件的人",不是面向改引擎的人(A11yNotes/SizeNotes 是技术说明,别混)。
		std::string Doc;
		// M41:设计器新建该类型时的默认尺寸(设计单位)。0 = 无偏好(调用方回落)。
		glm::vec2 DefaultSize { 0.0f, 0.0f };
	};

	class WLD_API UiNodeRegistry
	{
	public:
		// 注册/覆盖一件类型(同 Type 幂等覆盖)。
		// 失败(返回 false + 可读 error)的两种情形:Type 为空、ComponentId 未在
		// WuiComponentRegistry 登记、Paint 为空。失败时不写入登记表。
		static bool RegisterType(UiNodeTypeDesc desc, std::string* error = nullptr);

		// 按 `.wui` Type 查:匹配 Type / Aliases / ComponentId / 组件登记项 TypeName。
		// 未登记 → nullptr(调用方可读报错,不静默画空气)。
		// 返回的指针在下一次 RegisterType 之前有效。
		static const UiNodeTypeDesc* Find(std::string_view type);

		// 全部登记项(内置类型已就位)。
		static const std::vector<UiNodeTypeDesc>& All();

		// 该类型的属性元数据 = 组件登记项(分组/类型/默认值/状态/交互契约的事实源)。
		static const Wui::WuiComponentDesc* Component(std::string_view type);

		// 属性名是否被该类型声明(组件属性表 ∪ ExtraProps);未知键据此给可读提示。
		static bool DeclaresProperty(const UiNodeTypeDesc& type, std::string_view property);

		// 引擎内置类型;幂等。首次查询时自动调用。
		static void EnsureBuiltins();

	private:
		static std::vector<UiNodeTypeDesc>& Types();
	};

	// 内置类型登记实现(UiPainter.cpp:绘制函数与类型表同文件,避免注册表依赖绘制实现)。
	WLD_API void RegisterBuiltinUiNodeTypes();
}
