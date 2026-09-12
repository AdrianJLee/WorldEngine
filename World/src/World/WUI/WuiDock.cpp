#include "wldpch.h"
#include "World/WUI/WuiDock.h"
#include "World/WUI/WuiJson.h"

#include <algorithm>

namespace World::Wui
{
	DockLayout DockLayout::Default(const std::vector<PanelId>& panels)
	{
		DockLayout layout;
		DockNode leftColumn;
		leftColumn.Type = DockNode::Type::Split;
		leftColumn.Direction = WuiDirection::Column;
		leftColumn.Ratio = 0.58f;
		DockNode hierarchyTabs;
		DockNode contentTabs;
		DockNode viewTabs;
		DockNode miscTabs;
		for (const PanelId& panel : panels)
		{
			if (panel == "view") viewTabs.Panels.push_back(panel);
			else if (panel == "stats" || panel == "memory" || panel == "operations") miscTabs.Panels.push_back(panel);
			else if (panel == "content_browser") contentTabs.Panels.push_back(panel);
			else hierarchyTabs.Panels.push_back(panel);
		}
		leftColumn.Children.push_back(std::move(hierarchyTabs));
		leftColumn.Children.push_back(std::move(contentTabs));
		DockNode rightColumn;
		rightColumn.Type = DockNode::Type::Split;
		rightColumn.Direction = WuiDirection::Column;
		rightColumn.Ratio = 0.82f;
		rightColumn.Children.push_back(std::move(viewTabs));
		rightColumn.Children.push_back(std::move(miscTabs));
		layout.Root.Type = DockNode::Type::Split;
		layout.Root.Direction = WuiDirection::Row;
		layout.Root.Ratio = 0.24f;
		layout.Root.Children.push_back(std::move(leftColumn));
		layout.Root.Children.push_back(std::move(rightColumn));
		return layout;
	}

	DockNode* DockLayout::FindTabNode(DockNode& node, const PanelId& panel, std::vector<DockNode*>* path)
	{
		if (path)
			path->push_back(&node);
		if (node.IsTabs())
			return std::find(node.Panels.begin(), node.Panels.end(), panel) != node.Panels.end() ? &node : nullptr;
		for (DockNode& child : node.Children)
		{
			DockNode* found = FindTabNode(child, panel, path);
			if (found)
				return found;
		}
		if (path)
			path->pop_back();
		return nullptr;
	}

	const DockNode* DockLayout::FindTabNode(const DockNode& node, const PanelId& panel)
	{
		if (node.IsTabs())
			return std::find(node.Panels.begin(), node.Panels.end(), panel) != node.Panels.end() ? &node : nullptr;
		for (const DockNode& child : node.Children)
		{
			const DockNode* found = FindTabNode(child, panel);
			if (found)
				return found;
		}
		return nullptr;
	}

	bool DockLayout::Contains(const PanelId& panel) const
	{
		return FindTabNode(Root, panel) != nullptr;
	}

	void DockLayout::ComputeRects(const WuiRect& area, std::vector<std::pair<PanelId, WuiRect>>* out) const
	{
		if (!out)
			return;
		if (Root.IsTabs())
		{
			for (const PanelId& panel : Root.Panels)
				out->push_back({ panel, area });
			return;
		}
		float cursor = 0;
		for (const DockNode& child : Root.Children)
		{
			const bool row = Root.Direction == WuiDirection::Row;
			const float total = row ? area.W : area.H;
			const float firstSize = (Root.Children.size() == 2) ? total * Root.Ratio : total / static_cast<float>(Root.Children.size());
			const float size = (&child == &Root.Children.front()) ? firstSize : total - firstSize;
			WuiRect childArea = row
				? WuiRect { area.X + cursor, area.Y, size, area.H }
				: WuiRect { area.X, area.Y + cursor, area.W, size };
			cursor += size;
			if (child.IsTabs())
			{
				for (const PanelId& panel : child.Panels)
					out->push_back({ panel, childArea });
			}
			else
			{
				DockLayout nested;
				nested.Root = child;
				nested.ComputeRects(childArea, out);
			}
		}
	}

	bool DockLayout::IsActive(const PanelId& panel) const
	{
		const DockNode* node = FindTabNode(Root, panel);
		if (!node)
			return false;
		return node->Active < node->Panels.size() && node->Panels[node->Active] == panel;
	}

	bool DockLayout::AddTab(const PanelId& panel, const PanelId& target, DropZone zone)
	{
		if (Contains(panel) || !Contains(target))
			return false;

		if (zone == DropZone::Center)
		{
			DockNode* targetNode = FindTabNode(Root, target, nullptr);
			targetNode->Panels.push_back(panel);
			targetNode->Active = targetNode->Panels.size() - 1;
			return true;
		}

		std::vector<DockNode*> path;
		DockNode* targetNode = FindTabNode(Root, target, &path);
		DockNode newTabs;
		newTabs.Panels.push_back(panel);

		const bool before = zone == DropZone::Left || zone == DropZone::Top;
		const WuiDirection direction = (zone == DropZone::Left || zone == DropZone::Right)
			? WuiDirection::Row : WuiDirection::Column;

		if (path.size() <= 1)
		{
			// 目标是根 tab 组:包一层 split。
			DockNode split;
			split.Type = DockNode::Type::Split;
			split.Direction = direction;
			split.Ratio = 0.5f;
			if (before)
			{
				split.Children.push_back(std::move(newTabs));
				split.Children.push_back(std::move(Root));
			}
			else
			{
				split.Children.push_back(std::move(Root));
				split.Children.push_back(std::move(newTabs));
			}
			Root = std::move(split);
			return true;
		}

		DockNode* parent = path[path.size() - 2];
		auto it = std::find_if(parent->Children.begin(), parent->Children.end(), [&](const DockNode& child)
			{
				return &child == targetNode;
			});
		if (it == parent->Children.end())
			return false;

		DockNode existing = std::move(*it);
		DockNode split;
		split.Type = DockNode::Type::Split;
		split.Direction = direction;
		split.Ratio = 0.5f;
		if (before)
		{
			split.Children.push_back(std::move(newTabs));
			split.Children.push_back(std::move(existing));
		}
		else
		{
			split.Children.push_back(std::move(existing));
			split.Children.push_back(std::move(newTabs));
		}
		*it = std::move(split);
		return true;
	}

	bool DockLayout::RemoveFromTree(DockNode& parent, DockNode* child)
	{
		auto it = std::find_if(parent.Children.begin(), parent.Children.end(), [&](const DockNode& node)
			{
				return &node == child;
			});
		if (it == parent.Children.end())
			return false;
		parent.Children.erase(it);
		if (parent.Children.size() == 1)
		{
			DockNode remaining = std::move(parent.Children.front());
			parent = std::move(remaining);
		}
		return true;
	}

	bool DockLayout::RemoveTab(const PanelId& panel)
	{
		std::vector<DockNode*> path;
		DockNode* node = FindTabNode(Root, panel, &path);
		if (!node)
			return false;
		auto it = std::find(node->Panels.begin(), node->Panels.end(), panel);
		if (it == node->Panels.end())
			return false;
		node->Panels.erase(it);
		if (node->Active >= node->Panels.size())
			node->Active = node->Panels.empty() ? 0 : node->Panels.size() - 1;
		if (!node->Panels.empty())
			return true;
		if (path.size() <= 1)
		{
			Root = DockNode {};
			return true;
		}
		return RemoveFromTree(*path[path.size() - 2], node);
	}

	bool DockLayout::Activate(const PanelId& panel)
	{
		DockNode* node = FindTabNode(Root, panel, nullptr);
		if (!node)
			return false;
		auto it = std::find(node->Panels.begin(), node->Panels.end(), panel);
		if (it == node->Panels.end())
			return false;
		node->Active = static_cast<size_t>(it - node->Panels.begin());
		return true;
	}

	PanelId DockLayout::FindSibling(const PanelId& panel) const
	{
		const DockNode* node = FindTabNode(Root, panel);
		if (!node)
			return {};
		for (const PanelId& candidate : node->Panels)
			if (candidate != panel)
				return candidate;
		return {};
	}

	PanelId DockLayout::FirstPanel() const
	{
		std::function<PanelId(const DockNode&)> walk = [&](const DockNode& node) -> PanelId
		{
			if (node.IsTabs())
				return node.Panels.empty() ? PanelId {} : node.Panels.front();
			for (const DockNode& child : node.Children)
			{
				const PanelId found = walk(child);
				if (!found.empty())
					return found;
			}
			return {};
		};
		return walk(Root);
	}

	bool DockLayout::SerializeNode(const DockNode& node, JsonValue* out)
	{
		if (node.IsTabs())
		{
			JsonValue object;
			object.type = JsonValue::Type::Object;
			object.Object.push_back({ "type", JsonValue::MakeString("tabs") });
			JsonValue panels;
			panels.type = JsonValue::Type::Array;
			for (const PanelId& panel : node.Panels)
				panels.Array.push_back(JsonValue::MakeString(panel));
			object.Object.push_back({ "panels", std::move(panels) });
			object.Object.push_back({ "active", JsonValue::MakeNumber(static_cast<double>(node.Active)) });
			*out = std::move(object);
			return true;
		}

		JsonValue object;
		object.type = JsonValue::Type::Object;
		object.Object.push_back({ "type", JsonValue::MakeString("split") });
		object.Object.push_back({ "direction", JsonValue::MakeString(node.Direction == WuiDirection::Row ? "row" : "column") });
		object.Object.push_back({ "ratio", JsonValue::MakeNumber(node.Ratio) });
		JsonValue children;
		children.type = JsonValue::Type::Array;
		for (const DockNode& child : node.Children)
		{
			JsonValue serialized;
			if (!SerializeNode(child, &serialized))
				return false;
			children.Array.push_back(std::move(serialized));
		}
		object.Object.push_back({ "children", std::move(children) });
		*out = std::move(object);
		return true;
	}

	std::string DockLayout::Serialize() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "version", JsonValue::MakeNumber(1) });
		JsonValue node;
		SerializeNode(Root, &node);
		root.Object.push_back({ "root", std::move(node) });
		return root.Dump();
	}

	bool DockLayout::DeserializeNode(const JsonValue& value, DockNode* out, std::string* error)
	{
		const JsonValue* typeNode = value.Find("type");
		const std::string type = typeNode ? typeNode->AsString() : std::string();
		if (type == "tabs")
		{
			out->Type = DockNode::Type::Tabs;
			const JsonValue* panels = value.Find("panels");
			if (!panels || panels->type != JsonValue::Type::Array)
			{
				*error = "tabs node missing panels array";
				return false;
			}
			for (const JsonValue& panel : panels->Array)
				out->Panels.push_back(panel.AsString());
			const JsonValue* active = value.Find("active");
			out->Active = active ? static_cast<size_t>(active->AsNumber(0)) : 0;
			if (out->Active >= out->Panels.size())
				out->Active = out->Panels.empty() ? 0 : out->Panels.size() - 1;
			return true;
		}
		if (type == "split")
		{
			out->Type = DockNode::Type::Split;
			out->Direction = value.Find("direction") && value.Find("direction")->AsString("row") == "column"
				? WuiDirection::Column : WuiDirection::Row;
			const JsonValue* ratio = value.Find("ratio");
			out->Ratio = ratio ? static_cast<float>(ratio->AsNumber(0.5)) : 0.5f;
			const JsonValue* children = value.Find("children");
			if (!children || children->type != JsonValue::Type::Array)
			{
				*error = "split node missing children array";
				return false;
			}
			for (const JsonValue& child : children->Array)
			{
				DockNode node;
				if (!DeserializeNode(child, &node, error))
					return false;
				out->Children.push_back(std::move(node));
			}
			if (out->Children.size() < 2)
			{
				*error = "split node needs at least two children";
				return false;
			}
			return true;
		}
		*error = "unknown dock node type '" + type + "'";
		return false;
	}

	bool DockLayout::Deserialize(const std::string& text, DockLayout* out, std::string* error)
	{
		auto parsed = JsonValue::Parse(text, error);
		if (!parsed)
			return false;
		const JsonValue* root = parsed->Find("root");
		if (!root)
		{
			*error = "missing root node";
			return false;
		}
		DockLayout layout;
		if (!DeserializeNode(*root, &layout.Root, error))
			return false;
		*out = std::move(layout);
		return true;
	}
}
