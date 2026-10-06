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
// 滚动(M10):`UiScreen::IsScrollContainer` 为真的节点在绘制前压入裁剪矩形(命令流
//   `ClipPush` + `WuiContext::PushClipRect` 同进同出),其整个子树画完再弹;完全落在
//   裁剪之外的节点既不画也不登记(`UiPaintResult::ClippedNodes`)—— 与 `UiScreen::HitTest`
//   的裁剪链同一口径(溢出部分不可见也不可点)。
//
// 边界:本层不处理输入/焦点/绑定/动画(M3/M6);不做命中路由 —— `UiScreen::HitTest` 是命中的唯一实现。
//
// 主题令牌(M14):节点属性值形如 `$tokenId` 时在**本层**解析(`Tokens` 优先 / `Styles` 回退,
//   允许多级引用,检测环);非令牌值走原路径(零拷贝,旧文档行为不变)。未定义 / 环 / 引用非法
//   ⇒ 记一条可读错误(`UiPaintResult::Errors`,不阻断绘制)且该属性按"未设置"处理(用调用方默认值)。
//   登记进无障碍的 `Label`/`Value` 是**解析后**的文本(`$` 绝不泄漏给 AI/读屏)。
//   布局不解析令牌 —— `UiScreen::Layout` 只读数值属性;口径见 `UiTypes.h`。

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
	// ---- M26:运行态属性覆盖表(绑定求值结果 → 绘制前覆盖)----
	//
	// 键 = (节点稳定 Id, 属性名)。宿主(引擎里是 `UiHost`)每帧在 `UiBindingTable::Refresh`
	// 之后填;绘制期读取属性时**优先**取覆盖值(见 `UiPainter.cpp` 的 `ResolvedProp`)。
	// 覆盖**不改** `.wui` 文档、不改布局口径(`UiScreen::Layout` 不读它),只影响绘制命令与
	// 无障碍文本 —— 与 `UiBinding` 的"只读表现层"边界一致。
	struct UiPropertyOverride
	{
		std::string NodeId;
		std::string Property;
		std::string Value;   // 属性文本协议(与 `UiProp::Value` 同编码)
	};

	class WLD_API UiPropertyOverrideTable
	{
	public:
		void Clear() { m_Entries.clear(); }
		// 同键 = 覆盖(不追加第二份);空节点 Id / 空属性名 = 忽略(不静默写入不可查的键)。
		void Set(std::string nodeId, std::string property, std::string value);
		// 查覆盖值;无该键 = nullptr(指针在下次 Set/Clear 之前有效)。
		const std::string* Find(std::string_view nodeId, std::string_view property) const;

		std::size_t Count() const { return m_Entries.size(); }
		bool Empty() const { return m_Entries.empty(); }
		const std::vector<UiPropertyOverride>& Entries() const { return m_Entries; }

	private:
		std::vector<UiPropertyOverride> m_Entries;
	};

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
		// M26:运行态属性覆盖表(按节点 Id + 属性名);非空 ⇒ 绘制读取属性时优先取覆盖值。
		// 表由宿主持有并每帧重建;本层只读(不改、不清)。
		const UiPropertyOverrideTable* Overrides = nullptr;
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

		// ---- M14:主题令牌(解析在本层做,引擎内唯一一处)----
		//
		// `Document` = 令牌表来源(`Tokens` 优先 / `Styles` 回退;空 = 无令牌表);
		// `TokenErrors` = 解析失败出口(可读错误)。
		const UiDocument* Document = nullptr;
		std::vector<UiPaintError>* TokenErrors = nullptr;

		// 令牌表查询;`Document` 为空 = 无令牌表(一切照字面值)。
		const std::string* FindToken(std::string_view tokenId, std::string_view propName) const
		{
			return Document != nullptr ? FindUiToken(*Document, tokenId, propName) : nullptr;
		}

		// 解析一个属性值:非令牌值 = 原样;令牌值 = 沿链解析(多级 + 环检测)。
		// 解析失败 ⇒ 记一条可读错误并返回 false —— 调用方**必须**用默认值(按"未设置"处理,
		// 不得当成空串/0)。同 (节点, 属性, 消息) 只记一次,避免每帧重复刷屏。
		bool ResolveTokenValue(std::string_view propName, std::string_view value, std::string& out) const
		{
			const UiTokenResult resolved = ResolveUiTokenValue(value, propName,
				[this](std::string_view tokenId, std::string_view name)
				{
					return FindToken(tokenId, name);
				});
			if (resolved.Ok)
			{
				out = resolved.Value;
				return true;
			}

			if (TokenErrors != nullptr)
			{
				const std::string message = "property '" + std::string(propName) + "': " +
					(resolved.Error.empty() ? std::string("invalid token reference") : resolved.Error) +
					" (property treated as unset)";
				bool reported = false;
				for (const UiPaintError& existing : *TokenErrors)
				{
					if (existing.Path == Path && existing.Message == message)
					{
						reported = true;
						break;
					}
				}
				if (!reported)
					TokenErrors->push_back(UiPaintError { Path, Type.Type, message });
			}
			return false;
		}
	};

	struct UiPaintResult
	{
		std::size_t DrawnNodes = 0;    // 实际发出命令的节点数
		std::size_t SkippedNodes = 0;  // 因未知类型跳过的节点数
		std::size_t Commands = 0;      // 本次追加到 WuiContext 的命令数
		std::size_t AccessNodes = 0;   // 登记的无障碍节点数(无障碍关闭时为 0)
		// M10:完全落在滚动容器裁剪之外、既不画也不登记的节点数(与命中同一裁剪口径)。
		std::size_t ClippedNodes = 0;
		// 可读错误:未知类型(节点被跳过)/ 未解析的令牌(节点照画,仅该属性按"未设置"处理)。
		std::vector<UiPaintError> Errors;
		std::vector<UiPaintWarning> Warnings;

		// 无错误(未知类型或未解析令牌);未解析令牌不影响其它属性/节点与绘制。
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
