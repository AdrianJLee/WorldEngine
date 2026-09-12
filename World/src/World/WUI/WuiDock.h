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

	// 停靠布局树:split(方向+比例)与 tab 组两种节点;v1 不支持自由浮动。
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

	class DockLayout
	{
	public:
		static DockLayout Default(const std::vector<PanelId>& panels);

		// 计算每个 tab 组在给定区域内的矩形(递归按 Ratio 切分)。
		void ComputeRects(const WuiRect& area, std::vector<std::pair<PanelId, WuiRect>>* out) const;

		bool Contains(const PanelId& panel) const;
		bool IsActive(const PanelId& panel) const;

		// zone=Center 并入目标 tab 组;其余在目标外侧新建 split。
		bool AddTab(const PanelId& panel, const PanelId& target, DropZone zone);
		bool RemoveTab(const PanelId& panel);
		bool Activate(const PanelId& panel);

		std::string Serialize() const;
		static bool Deserialize(const std::string& text, DockLayout* out, std::string* error);

		DockNode Root;

	private:
		static DockNode* FindTabNode(DockNode& node, const PanelId& panel, std::vector<DockNode*>* path);
		static const DockNode* FindTabNode(const DockNode& node, const PanelId& panel);
		static bool RemoveFromTree(DockNode& parent, DockNode* child);
		static bool SerializeNode(const DockNode& node, JsonValue* out);
		static bool DeserializeNode(const JsonValue& value, DockNode* out, std::string* error);
	};
}
