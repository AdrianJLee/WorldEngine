#include "wldpch.h"
#include "World/UI/UiDocument.h"

#include <unordered_set>

namespace World::UI
{
	namespace
	{
		std::string NodePath(const std::string& parentPath, const std::string& id)
		{
			return parentPath.empty() ? id : parentPath + "." + id;
		}
	}

	const UiProp* UiNode::FindProp(std::string_view name) const
	{
		for (const UiProp& prop : Props)
		{
			if (prop.Name == name)
				return &prop;
		}
		return nullptr;
	}

	// ---- 遍历 / 查找 ----

	void UiDocument::ForEachNode(const std::function<void(UiNode&, const std::string&)>& fn)
	{
		const std::function<void(UiNode&, const std::string&)> visit =
			[&](UiNode& node, const std::string& parentPath)
			{
				const std::string path = NodePath(parentPath, node.Id);
				fn(node, path);
				for (UiNode& child : node.Children)
					visit(child, path);
			};
		for (UiNode& node : Nodes)
			visit(node, std::string());
	}

	void UiDocument::ForEachNode(const std::function<void(const UiNode&, const std::string&)>& fn) const
	{
		const std::function<void(const UiNode&, const std::string&)> visit =
			[&](const UiNode& node, const std::string& parentPath)
			{
				const std::string path = NodePath(parentPath, node.Id);
				fn(node, path);
				for (const UiNode& child : node.Children)
					visit(child, path);
			};
		for (const UiNode& node : Nodes)
			visit(node, std::string());
	}

	const UiNode* UiDocument::FindNode(std::string_view id) const
	{
		const UiNode* found = nullptr;
		ForEachNode([&](const UiNode& node, const std::string&)
		{
			if (!found && node.Id == id)
				found = &node;
		});
		return found;
	}

	std::size_t UiDocument::NodeCount() const
	{
		std::size_t count = 0;
		ForEachNode([&](const UiNode&, const std::string&) { ++count; });
		return count;
	}

	// ---- 校验 ----

	bool ValidateUiDocument(const UiDocument& doc, std::vector<UiValidationIssue>* issues)
	{
		bool ok = true;
		std::unordered_set<std::string> seen;

		const auto report = [&](const std::string& path, const std::string& message)
		{
			ok = false;
			if (issues)
				issues->push_back({ path, message });
		};

		if (doc.FormatVersion != kUiDocumentFormatVersion)
			report("", "FormatVersion must be " + std::to_string(kUiDocumentFormatVersion));
		if (doc.Screen.empty())
			report("", "'Screen' must not be empty");
		if (doc.Design.Resolution.x <= 0.0f || doc.Design.Resolution.y <= 0.0f)
			report("", "Design.Resolution must be positive");

		doc.ForEachNode([&](const UiNode& node, const std::string& path)
		{
			if (!IsValidUiNodeId(node.Id))
				report(path, "invalid node Id");
			if (!seen.insert(node.Id).second)
				report(path, "duplicate node Id '" + node.Id + "'");
			if (node.Type.empty())
				report(path, "node Type must not be empty");
			for (const UiProp& prop : node.Props)
			{
				if (prop.Name.empty())
					report(path, "property name must not be empty");
			}
			for (const UiBindingDecl& binding : node.Bind)
			{
				if (binding.Target.empty() || binding.Source.empty())
					report(path, "binding needs both a target and a source");
			}
			for (const UiCommandDecl& command : node.On)
			{
				if (command.Event.empty() || command.Command.empty())
					report(path, "command needs both an event and a command");
			}
			if (node.Layout.Columns < 1)
				report(path, "Layout.Columns must be >= 1");
			if (node.World.Enabled && node.World.Target.empty())
				report(path, "World anchor needs a non-empty Target");
		});

		return ok;
	}

	// ---- 等价比较(round-trip 断言用;忽略 IdWasGenerated)----

	namespace
	{
		bool VecEq(const glm::vec2& a, const glm::vec2& b) { return a.x == b.x && a.y == b.y; }

		bool AnchorEq(const UiAnchor& a, const UiAnchor& b)
		{
			return VecEq(a.Min, b.Min) && VecEq(a.Max, b.Max) && VecEq(a.Pivot, b.Pivot) &&
				VecEq(a.Offset, b.Offset) && VecEq(a.Size, b.Size) &&
				a.RelativeToSafeArea == b.RelativeToSafeArea;
		}

		bool LayoutEq(const UiLayoutSpec& a, const UiLayoutSpec& b)
		{
			if (a.Kind != b.Kind || a.Gap != b.Gap || a.Columns != b.Columns)
				return false;
			for (int i = 0; i < 4; ++i)
			{
				if (a.Padding[i] != b.Padding[i])
					return false;
			}
			return true;
		}

		bool PropsEq(const std::vector<UiProp>& a, const std::vector<UiProp>& b)
		{
			if (a.size() != b.size())
				return false;
			for (std::size_t i = 0; i < a.size(); ++i)
			{
				if (a[i].Name != b[i].Name || a[i].Value != b[i].Value)
					return false;
			}
			return true;
		}

		bool NodeEq(const UiNode& a, const UiNode& b)
		{
			if (a.Id != b.Id || a.Type != b.Type || !PropsEq(a.Props, b.Props) || !AnchorEq(a.Anchor, b.Anchor) ||
				!LayoutEq(a.Layout, b.Layout) || a.Bind.size() != b.Bind.size() || a.On.size() != b.On.size() ||
				a.Children.size() != b.Children.size())
				return false;
			if (a.World.Enabled != b.World.Enabled || a.World.Target != b.World.Target ||
				a.World.Offset != b.World.Offset || a.World.KeepOnScreen != b.World.KeepOnScreen)
				return false;
			for (std::size_t i = 0; i < a.Bind.size(); ++i)
			{
				if (a.Bind[i].Target != b.Bind[i].Target || a.Bind[i].Source != b.Bind[i].Source)
					return false;
			}
			for (std::size_t i = 0; i < a.On.size(); ++i)
			{
				if (a.On[i].Event != b.On[i].Event || a.On[i].Command != b.On[i].Command)
					return false;
			}
			for (std::size_t i = 0; i < a.Children.size(); ++i)
			{
				if (!NodeEq(a.Children[i], b.Children[i]))
					return false;
			}
			return true;
		}
	}

	bool UiDocumentsEquivalent(const UiDocument& a, const UiDocument& b)
	{
		if (a.FormatVersion != b.FormatVersion || a.Screen != b.Screen || a.Theme != b.Theme)
			return false;
		if (!VecEq(a.Design.Resolution, b.Design.Resolution) || a.Design.ScaleMode != b.Design.ScaleMode ||
			a.Design.Match != b.Design.Match)
			return false;
		if (a.SafeArea.Enabled != b.SafeArea.Enabled || a.SafeArea.Top != b.SafeArea.Top ||
			a.SafeArea.Bottom != b.SafeArea.Bottom || a.SafeArea.Left != b.SafeArea.Left ||
			a.SafeArea.Right != b.SafeArea.Right)
			return false;
		if (a.Parameters.size() != b.Parameters.size() || a.Styles.size() != b.Styles.size() ||
			a.Nodes.size() != b.Nodes.size())
			return false;
		for (std::size_t i = 0; i < a.Parameters.size(); ++i)
		{
			if (a.Parameters[i].Name != b.Parameters[i].Name || a.Parameters[i].Type != b.Parameters[i].Type ||
				a.Parameters[i].Default != b.Parameters[i].Default)
				return false;
		}
		for (std::size_t i = 0; i < a.Styles.size(); ++i)
		{
			if (a.Styles[i].Id != b.Styles[i].Id || !PropsEq(a.Styles[i].Props, b.Styles[i].Props))
				return false;
		}
		for (std::size_t i = 0; i < a.Nodes.size(); ++i)
		{
			if (!NodeEq(a.Nodes[i], b.Nodes[i]))
				return false;
		}
		return true;
	}
}
