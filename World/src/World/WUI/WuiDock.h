#pragma once

#include "World/WUI/WuiCore.h"

#include <optional>
#include <string>
#include <vector>

namespace World::Wui
{
	using PanelId = std::string;
	class JsonValue;

	enum class DropZone : uint8_t
	{
		Center,
		Left,
		Right,
		Top,
		Bottom,
	};

	// 停靠布局树:split(方向+比例)与 tab 组两种节点。
	struct DockNode
	{
		enum class Type : uint8_t
		{
			Tabs,
			Split,
		};

		Type Type = Type::Tabs;
		WuiDirection Direction = WuiDirection::Row;
		float Ratio = 0.5f;
		std::vector<DockNode> Children; // Split
		std::vector<PanelId> Panels;    // Tabs
		size_t Active = 0;

		bool IsTabs() const { return Type == Type::Tabs; }
	};

	// 浮动面板窗口:脱离停靠树、可自由移动与缩放,每窗承载一个面板。
	struct DockFloat
	{
		PanelId Panel;
		WuiRect Rect { 220, 140, 480, 320 };
	};

	class DockLayout
	{
	public:
		static DockLayout Default(const std::vector<PanelId>& panels);

		// 计算每个 tab 组在给定区域内的矩形(递归按 Ratio 切分)。
		void ComputeRects(const WuiRect& area, std::vector<std::pair<PanelId, WuiRect>>* out) const;

		// 面板是否在停靠树中 / 是否处于浮动窗口。
		bool Contains(const PanelId& panel) const;
		bool IsFloating(const PanelId& panel) const;
		DockFloat* FindFloat(const PanelId& panel);
		const DockFloat* FindFloat(const PanelId& panel) const;
		bool IsActive(const PanelId& panel) const;

		// zone=Center 并入目标 tab 组;其余在目标外侧新建 split。
		bool AddTab(const PanelId& panel, const PanelId& target, DropZone zone);
		// 移动已存在的面板:摘除→挂载,源组塌缩后回退锚点;返回是否生效。
		bool MoveTab(const PanelId& panel, const PanelId& target, DropZone zone);
		// 停靠到整个编辑器边缘:在根级新建横跨全区的分栏(Left/Right/Top/Bottom),
		// 用于拖到编辑器四边时生成全局停靠区,而不是只切分某个面板组。
		bool DockToRoot(const PanelId& panel, DropZone zone);
		bool RemoveTab(const PanelId& panel);
		// 从停靠树摘除并转为浮动窗口(原 tab 组按 RemoveTab 规则塌缩)。
		bool Float(const PanelId& panel, const WuiRect& rect);
		// 浮动窗口落回停靠:先摘除浮动记录,再挂到目标/根级。
		bool DockFloating(const PanelId& panel, const PanelId& target, DropZone zone);
		bool DockFloatingToRoot(const PanelId& panel, DropZone zone);
		// 隐藏浮动面板(仅移除浮动记录,不改变停靠树)。
		bool CloseFloating(const PanelId& panel);
		// 浮动窗口提到最前(最后绘制 = 最上层)。
		void BringFloatToFront(const PanelId& panel);
		// 记录/查询独立窗口的上次屏幕矩形(跨会话记忆)。
		void RememberFloat(const DockFloat& entry);
		bool FindFloatMemory(const PanelId& panel, WuiRect* out) const;
		bool Activate(const PanelId& panel);
		// 同一 tab 组内除 panel 外的另一个面板;无则返回空。
		PanelId FindSibling(const PanelId& panel) const;
		// 深度优先找到的第一个面板。
		PanelId FirstPanel() const;
		// 深度优先收集全部停靠面板(树序),主窗口全局标签栏用。
		void AllPanels(std::vector<PanelId>* out) const;

		std::string Serialize() const;
		static bool Deserialize(const std::string& text, DockLayout* out, std::string* error);

		DockNode Root;
		std::vector<DockFloat> Floating;
		// 曾经作为独立窗口存在过的面板及其最后屏幕矩形(跨会话记忆):
		// 从 Window 菜单重新打开时据此恢复为独立窗口,而不是塞进停靠树。
		std::vector<DockFloat> FloatMemory;

	private:
		static DockNode* FindTabNode(DockNode& node, const PanelId& panel, std::vector<DockNode*>* path);
		static const DockNode* FindTabNode(const DockNode& node, const PanelId& panel);
		static bool RemoveFromTree(DockNode& parent, DockNode* child);
		static bool SerializeNode(const DockNode& node, JsonValue* out);
		static bool DeserializeNode(const JsonValue& value, DockNode* out, std::string* error);
	};
}
