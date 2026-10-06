#pragma once

// 游戏 UI 框架(GameUI)— 绘制与无障碍登记(工作包 M2)。
//
// 契约:`contract.ui-runtime`(§2 坐标一致:绘制与命中共用同一 `UiViewport` 映射;
// §8 无障碍:每个可见交互节点登记 Role/Path/Label/Value/Rect;§10 门禁:绘制只走 `WuiContext`)。
//
// 流程:对 `UiScreen::Nodes()` 按绘制顺序(前序 = 父先子后)逐个节点
//   ① 用 `UiNodeRegistry::Find(type)` 解析 `.wui` Type → 既有组件登记项;
//      未登记 = 可读错误 + 跳过该节点(绝不静默画空气);
//   ② 把设计空间矩形经 `viewport.DesignRectToPhysical` 映射成物理矩形(**与命中同一映射**);
//   ③ 调用类型的 `Paint` 把命令写进 `WuiContext`;
//   ④ 把节点登记进 `WuiAccessibility`(窗口坐标 = 物理坐标)。
//
// 边界:本层不处理输入/焦点/绑定/动画(M3/M6);不做命中路由 —— `UiScreen::HitTest` 是命中的唯一实现。

#include "World/Core/Export.h"
#include "World/UI/UiNodeRegistry.h"
#include "World/UI/UiScreen.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiWidgets.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace World::UI
{
	struct UiPaintOptions
	{
		// 无障碍节点归属窗口 key(与宿主 `WuiAccessibility::BeginFrame` 的 key 一致)。
		std::string WindowKey = "main";
		// 面板 id;空 = 沿用无障碍当前面板(宿主已 SetPanel),再空 = 文档 Screen 名。
		std::string PanelId;
		// 页面名;空 = 文档 Screen 名。
		std::string Page;
		// 层级(页面栈/模态叠加用;M3 导航填,这里原样进无障碍节点)。
		int Layer = 0;
		// 是否登记无障碍节点(控制通道关闭时宿主可传 false)。
		bool RegisterAccessibility = true;
	};

	struct UiPaintError
	{
		std::string Path;      // 稳定节点路径(如 "root.hp")
		std::string Type;      // 出问题的 `.wui` Type
		std::string Message;   // 可读说明(含节点路径与类型名)
	};

	// 非致命提示(如属性名不在该类型的登记属性表里)。不影响 Ok(),不阻断绘制。
	struct UiPaintWarning
	{
		std::string Path;
		std::string Message;
	};

	// 单个节点的绘制上下文。绘制函数只发命令;Label/Value/States/Disabled 回传给
	// `UiPainter` 用于**集中**登记无障碍节点(Role/Path/Page/Layer 由外层统一填)。
	struct UiNodePaintContext
	{
		Wui::WuiContext& Context;
		const UiScreen& Screen;
		const UiNodeInstance& Node;
		const UiNodeTypeDesc& Type;
		Wui::WuiRect Rect { 0, 0, 0, 0 };   // 物理像素(窗口客户区);命中用同一映射
		float Scale = 1.0f;                 // 设计单位 → 物理像素
		const Wui::WuiTheme& Theme;
		const UiPaintOptions& Options;
		std::string Path;                   // 稳定节点路径

		// ---- 绘制函数回传 ----
		std::string Label;                  // 人类可读标签(已本地化)
		std::string Value;                  // 当前值文本
		std::string States;                 // 逗号分隔,如 "checked,disabled"
		bool Disabled = false;

		// ---- 非致命提示出口(可为空)----
		std::vector<UiPaintWarning>* Warnings = nullptr;

		void Warn(std::string message) const
		{
			if (Warnings != nullptr)
				Warnings->push_back(UiPaintWarning { Path, std::move(message) });
		}
	};

	struct UiPaintResult
	{
		std::size_t DrawnNodes = 0;    // 实际发出命令的节点数
		std::size_t SkippedNodes = 0;  // 因未知类型跳过的节点数
		std::size_t Commands = 0;      // 本次追加到 WuiContext 的命令数
		std::size_t AccessNodes = 0;   // 登记的无障碍节点数(无障碍关闭时为 0)
		std::vector<UiPaintError> Errors;
		std::vector<UiPaintWarning> Warnings;

		bool Ok() const { return Errors.empty(); }
	};

	class WLD_API UiPainter
	{
	public:
		// 把 `UiScreen`(需已 `Layout`)画进 `ctx`,并按 `options` 登记无障碍节点。
		// 期望调用方已完成 `ctx.BeginFrame(...)`(命令每帧清零由它负责)。
		static UiPaintResult Paint(Wui::WuiContext& ctx, const UiScreen& screen,
			const UiPaintOptions& options = UiPaintOptions {});
	};
}
