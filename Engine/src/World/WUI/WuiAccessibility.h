#pragma once

#include "World/Core/Export.h"
#include "World/WUI/WuiCore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace World::Wui
{
	// 一个"可被 AI 读到/点到"的界面节点。
	// 登记发生在**控件绘制时**,所以它的矩形、启用态、值都与用户看到的完全一致;
	// 坐标是**所属窗口的客户区坐标**(主窗口 = 主窗口客户区,独立窗口 = 该窗口客户区),
	// 配合 Window 字段就能把注入的点击送到正确的窗口。
	struct WuiAccessNode
	{
		WuiId Id = 0;
		std::string Window;       // "main" 或 "float:<面板>"
		std::string Panel;        // 面板 id(停靠面板 / 独立窗口内的面板)
		std::string Kind;         // button/slider/combo/text-field/menu-item/...
		std::string Label;        // 人类可读名称(通常就是控件文字)
		std::string Value;        // 当前值(数值/选项/文本)
		// 悬停说明(P4-UX7):只能靠鼠标悬停看到的信息(用途/默认值/生效时机/禁用原因)
		// 必须同时进节点 —— 脚本与读屏拿不到 tooltip 的话,设置类控件就是"不可读"的。
		std::string Tooltip;
		WuiRect Rect;             // 窗口客户区坐标
		bool Enabled = true;
		bool Focused = false;
		bool Interactive = true;  // 是否可被 ui.invoke 点击
		// 节点的中心是否落在窗口客户区之内。看不见的控件不允许被脚本点击
		// (否则会出现"AI 能点到用户点不到的东西",掩盖真实的布局问题)。
		bool Visible = true;

		// ---- GameUI(M2):节点语义扩展(**全部追加在末尾** + 默认值,保持既有聚合初始化兼容)----
		// 契约:`contract.ui-runtime` §8 —— 每个可见交互节点要能回答"它是什么角色、什么状态、
		// 能做什么、在页面/层级的哪个位置"。这些字段只描述,不改变命中与输入路径。

		// 语义角色("button"/"text"/"img"/"progressbar"/"group"/"checkbox"/...)。
		std::string Role;
		// 状态集(逗号分隔,如 "checked,disabled");空 = 无特殊状态。
		std::string States;
		// 可执行动作(逗号分隔,如 "click,type");空 = 静态件。
		std::string Actions;
		// 稳定节点路径(`.wui` 的 "父.子" 口径,如 "root.hp");与坐标/索引无关。
		std::string Path;
		// 所属页面(导航层);空 = 未知/单页。
		std::string Page;
		// 层级(页面栈/模态叠加深度;0 = 常态页面)。
		int Layer = 0;
	};

	// UI 无障碍树:让 AI 能"像读 DOM 一样"找到控件,而不是靠猜坐标。
	// 单例的理由与 WuiTextureRegistry 相同:多个窗口(WuiContext 实例)共享一份树,
	// 每帧由各窗口各自 BeginFrame(窗口 key) 后重新登记。
	class WLD_API WuiAccessibility
	{
	public:
		static WuiAccessibility& Get();

		// 总开关:控制通道关闭时完全不登记节点(正常编辑零开销)。
		void SetEnabled(bool enabled) { m_Enabled = enabled; if (!enabled) Clear(); }
		bool Enabled() const { return m_Enabled; }
		// 每个窗口在构建 UI 前调用:清掉本窗口上一帧的节点(clientSize = 窗口客户区尺寸)。
		void BeginFrame(const std::string& windowKey, glm::vec2 clientSize = { 0.0f, 0.0f });
		// 面板内容绘制前调用:此后登记的节点归属该面板(rect 为窗口客户区坐标)。
		void SetPanel(const std::string& panelId);
		// 丢弃某个窗口的全部节点(P4-UX7):独立窗口"隐藏复用"后不再渲染,它上一帧登记的
		// 节点会一直留在树里 —— ui.tree 出现幽灵行,ui.invoke 会把点击投给已经不存在的窗口。
		// 与 BeginFrame 的区别:不动"当前窗口/面板"状态,可以在任意时刻调用。
		void ClearWindow(const std::string& windowKey);
		// 追加式(M9):只清某个窗口里**归属指定面板**的节点,不动同窗口的其它面板。
		// 用途:编辑器 Play 退出时清游戏 UI(UiHost::SharedChannel),而**不能**把同窗口的
		// 编辑器节点一起清掉(`ClearWindow` 会;`ClearPanel` 不会)。`ClearWindow` 语义不变。
		void ClearPanel(const std::string& windowKey, const std::string& panelId);
		// 控件绘制时调用(重复 id 以最后一次为准)。
		void Register(const WuiAccessNode& node);

		const std::vector<WuiAccessNode>& Nodes() const { return m_Nodes; }
		const WuiAccessNode* Find(WuiId id) const;
		// 便捷匹配:label 全等,或 "kind:label" 形式。
		const WuiAccessNode* FindByLabel(const std::string& label, const std::string& kind = std::string()) const;
		void Clear();

		// 序列化成 JSON 数组文本(供 ui.tree / state.dump 使用)。
		std::string Serialize() const;

		const std::string& CurrentWindow() const { return m_Window; }
		const std::string& CurrentPanel() const { return m_Panel; }

	private:
		std::vector<WuiAccessNode> m_Nodes;
		std::string m_Window;
		std::string m_Panel;
		glm::vec2 m_ClientSize { 0.0f, 0.0f };
		bool m_Enabled = false;
	};
}
