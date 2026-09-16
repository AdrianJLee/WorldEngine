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
		WuiRect Rect;             // 窗口客户区坐标
		bool Enabled = true;
		bool Focused = false;
		bool Interactive = true;  // 是否可被 ui.invoke 点击
		// 节点的中心是否落在窗口客户区之内。看不见的控件不允许被脚本点击
		// (否则会出现"AI 能点到用户点不到的东西",掩盖真实的布局问题)。
		bool Visible = true;
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
