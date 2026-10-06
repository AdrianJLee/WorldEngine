#include "wldpch.h"
#include "World/UI/UiScreen.h"

#include <algorithm>

namespace World::UI
{
	namespace
	{
		void FlattenNode(const UiNode& node, int parent, int depth,
			std::vector<UiNodeInstance>& out)
		{
			const int index = static_cast<int>(out.size());
			UiNodeInstance instance;
			instance.Id = node.Id;
			instance.Type = node.Type;
			instance.Source = &node;
			instance.Parent = parent;
			instance.Depth = depth;
			out.push_back(std::move(instance));

			if (parent >= 0)
				out[static_cast<std::size_t>(parent)].Children.push_back(index);

			for (const UiNode& child : node.Children)
				FlattenNode(child, index, depth + 1, out);
		}
	}

	bool UiScreen::Build(const UiDocument& doc, std::string* error)
	{
		std::vector<UiValidationIssue> issues;
		if (!ValidateUiDocument(doc, &issues))
		{
			if (error)
			{
				std::string message = "cannot build UI screen: ";
				for (std::size_t i = 0; i < issues.size() && i < 3; ++i)
				{
					if (i)
						message += "; ";
					message += (issues[i].Path.empty() ? std::string("<root>") : issues[i].Path) + ": " + issues[i].Message;
				}
				*error = message;
			}
			return false;
		}

		m_Document = doc;
		m_Nodes.clear();
		m_Nodes.reserve(m_Document.NodeCount());
		for (const UiNode& root : m_Document.Nodes)
			FlattenNode(root, -1, 0, m_Nodes);
		return true;
	}

	int UiScreen::IndexOf(std::string_view id) const
	{
		for (std::size_t i = 0; i < m_Nodes.size(); ++i)
		{
			if (m_Nodes[i].Id == id)
				return static_cast<int>(i);
		}
		return -1;
	}

	const UiNodeInstance* UiScreen::Find(std::string_view id) const
	{
		const int index = IndexOf(id);
		return index >= 0 ? &m_Nodes[static_cast<std::size_t>(index)] : nullptr;
	}

	Wui::WuiRect UiScreen::RectOf(std::string_view id) const
	{
		const UiNodeInstance* node = Find(id);
		return node ? node->Rect : Wui::WuiRect { 0, 0, 0, 0 };
	}

	void UiScreen::SetNodeRect(std::size_t index, const Wui::WuiRect& rect)
	{
		if (index < m_Nodes.size())
			m_Nodes[index].Rect = rect;
	}

	void UiScreen::PlaceNode(int index, const Wui::WuiRect& rect)
	{
		m_Nodes[static_cast<std::size_t>(index)].Rect = rect;

		const std::vector<int> children = m_Nodes[static_cast<std::size_t>(index)].Children;
		if (children.empty())
			return;

		const UiNode& source = *m_Nodes[static_cast<std::size_t>(index)].Source;
		const UiLayoutSpec& spec = source.Layout;
		const std::size_t count = children.size();

		switch (spec.Kind)
		{
		case UiLayoutKind::Absolute:
		{
			for (const int child : children)
			{
				const UiNode& childSource = *m_Nodes[static_cast<std::size_t>(child)].Source;
				PlaceNode(child, ResolveAnchor(childSource.Anchor, rect));
			}
			return;
		}
		case UiLayoutKind::Overlay:
		{
			for (const int child : children)
				PlaceNode(child, rect);
			return;
		}
		case UiLayoutKind::Flex:
		{
			const Wui::WuiRect inner = ApplyPadding(spec, rect);
			const Wui::WuiFlexLayout layout {
				spec.RowMajor ? Wui::WuiDirection::Row : Wui::WuiDirection::Column,
				Wui::WuiAlign::Start, Wui::WuiAlign::Stretch, spec.Gap };
			const float boundedMain = spec.RowMajor ? inner.W : inner.H;
			const float availableMain = std::max(0.0f, boundedMain - spec.Gap * static_cast<float>(count - 1));
			const float each = availableMain / static_cast<float>(count);

			std::vector<Wui::WuiFlexItem> items(count);
			for (std::size_t i = 0; i < count; ++i)
			{
				items[i].MinMain = 0.0f;
				items[i].MaxMain = each;
				items[i].MinCross = 0.0f;
				items[i].MaxCross = spec.RowMajor ? inner.H : inner.W;
				items[i].Grow = 1.0f;
			}

			const Wui::WuiConstraints constraints { inner.X, inner.Y, inner.X + inner.W, inner.Y + inner.H };
			const Wui::WuiFlexResult result = Wui::SolveFlex(layout, items, constraints);
			for (std::size_t i = 0; i < count; ++i)
			{
				const Wui::WuiRect slot {
					inner.X + result.Rects[i].X, inner.Y + result.Rects[i].Y,
					result.Rects[i].W, result.Rects[i].H };
				const UiNode& childSource = *m_Nodes[static_cast<std::size_t>(children[i])].Source;
				PlaceNode(children[i], IsDefaultAnchor(childSource.Anchor)
					? slot : ResolveAnchor(childSource.Anchor, slot));
			}
			return;
		}
		case UiLayoutKind::Column:
		case UiLayoutKind::Row:
		case UiLayoutKind::Grid:
		default:
		{
			for (std::size_t i = 0; i < count; ++i)
			{
				const Wui::WuiRect slot = SolveChildSlot(spec, rect, i, count);
				const UiNode& childSource = *m_Nodes[static_cast<std::size_t>(children[i])].Source;
				PlaceNode(children[i], IsDefaultAnchor(childSource.Anchor)
					? slot : ResolveAnchor(childSource.Anchor, slot));
			}
			return;
		}
		}
	}

	bool UiScreen::Layout(const UiViewport& viewport)
	{
		if (!(viewport.Scale > 0.0f))
			return false;
		m_Viewport = viewport;

		for (std::size_t i = 0; i < m_Nodes.size(); ++i)
		{
			if (m_Nodes[i].Parent != -1)
				continue;
			const UiAnchor& anchor = m_Nodes[i].Source->Anchor;
			// 根节点相对视口内容矩形(已扣安全区)锚定;RelativeToSafeArea 对根是冗余声明。
			PlaceNode(static_cast<int>(i), ResolveAnchor(anchor, viewport.ContentRect));
		}
		return true;
	}

	const UiNodeInstance* UiScreen::HitTest(glm::vec2 physicalPoint) const
	{
		// 后序 = 绘制顺序的逆序:最后画的先命中(最上层赢)。
		for (std::size_t i = m_Nodes.size(); i-- > 0;)
		{
			if (HitTestDesign(m_Nodes[i].Rect, m_Viewport, physicalPoint))
				return &m_Nodes[i];
		}
		return nullptr;
	}
}
