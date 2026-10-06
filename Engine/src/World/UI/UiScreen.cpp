#include "wldpch.h"
#include "World/UI/UiScreen.h"

#include "World/UI/UiNodeRegistry.h"

#include <algorithm>
#include <cmath>
#include <utility>

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

		// M10:滚动是运行态(不在文档里),按稳定 Id 存;重建实例树时清空。
		// 热重载"按 Id 迁移运行态"由宿主决定,不在这里隐式保留。
		m_ScrollOffsets.clear();
		m_ScrollContentSizes.clear();
		m_AppliedScrollOffsets.assign(m_Nodes.size(), glm::vec2 { 0.0f, 0.0f });
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

	void UiScreen::TranslateSubtree(std::size_t index, glm::vec2 delta)
	{
		TranslateDescendants(index, delta);
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

		// M10:布局重建后,把运行态滚动偏移重新作用到滚动容器的后代(增量基准清零 ⇒ 幂等)。
		m_AppliedScrollOffsets.assign(m_Nodes.size(), glm::vec2 { 0.0f, 0.0f });
		ApplyScrollOffsets();
		return true;
	}

	const UiNodeInstance* UiScreen::HitTest(glm::vec2 physicalPoint) const
	{
		const glm::vec2 designPoint = m_Viewport.PhysicalToDesign(physicalPoint);
		// 后序 = 绘制顺序的逆序:最后画的先命中(最上层赢)。
		for (std::size_t i = m_Nodes.size(); i-- > 0;)
		{
			if (!HitTestDesign(m_Nodes[i].Rect, m_Viewport, physicalPoint))
				continue;
			// M10:滚动容器裁剪之外的部分不可命中(与 `UiPainter` 压的裁剪矩形同一口径)。
			if (!PointInsideScrollAncestors(static_cast<int>(i), designPoint))
				continue;
			return &m_Nodes[i];
		}
		return nullptr;
	}

	// ---- 滚动(M10)----

	bool UiScreen::IsScrollContainerIndex(std::size_t index) const
	{
		return index < m_Nodes.size() && IsScrollContainer(m_Nodes[index]);
	}

	bool UiScreen::IsScrollContainer(std::string_view id) const
	{
		const int index = IndexOf(id);
		return index >= 0 && IsScrollContainer(m_Nodes[static_cast<std::size_t>(index)]);
	}

	bool UiScreen::IsScrollContainer(const UiNodeInstance& node) const
	{
		if (node.Source == nullptr)
			return false;
		const auto valueOf = [&node](std::string_view name) -> std::string_view {
			const UiProp* prop = node.Source->FindProp(name);
			return prop != nullptr ? std::string_view(prop->Value) : std::string_view();
		};
		// 与 `UiInputRouter::IsScrollContainer` 同一份规则(见 UiTypes 的同一实现)。
		return IsUiScrollContainerProps(valueOf("scrollable"), valueOf("scroll"), valueOf("overflow"));
	}

	glm::vec2 UiScreen::ScrollOffset(std::string_view id) const
	{
		const auto it = m_ScrollOffsets.find(std::string(id));
		return it != m_ScrollOffsets.end() ? it->second : glm::vec2 { 0.0f, 0.0f };
	}

	glm::vec2 UiScreen::ScrollContentSize(std::string_view id) const
	{
		const int index = IndexOf(id);
		if (index < 0)
			return glm::vec2 { 0.0f, 0.0f };
		const UiNodeInstance& node = m_Nodes[static_cast<std::size_t>(index)];

		const auto explicitIt = m_ScrollContentSizes.find(std::string(id));
		if (explicitIt != m_ScrollContentSizes.end())
			return explicitIt->second;

		// 类型自报(程序化列表/网格:行不是文档子节点,子的包围盒量不到内容)。
		const UiNodeTypeDesc* type = UiNodeRegistry::Find(node.Type);
		glm::vec2 reported { 0.0f, 0.0f };
		if (type != nullptr && type->ContentSize != nullptr && type->ContentSize(node, reported))
			return reported;

		// 子的包围盒(设计空间),**以容器左上角为内容原点**:内容从容器原点起算,
		// 越界只向正方向延伸(负偏移的子节点 = 内容起点之上,随偏移 0 被裁剪)。
		// 以容器自身为下界 ⇒ 无子节点/内容更小 ⇒ 内容 = 容器(偏移恒 0)。
		//
		// 关键:子节点的矩形**已经被滚动偏移平移过**(ApplyScrollOffsets 的增量是净位移
		// `-applied`)。若直接量已平移的矩形,偏移一旦生效内容就会"缩回"容器内,下一次
		// 钳位又把偏移压回 0(实测:滚 9999 → 150,再滚 10 → 0)。因此这里把本容器**已应用
		// 的位移加回去**,量的是"偏移为 0 时"的内容包围盒。
		const glm::vec2 applied = (static_cast<std::size_t>(index) < m_AppliedScrollOffsets.size())
			? m_AppliedScrollOffsets[static_cast<std::size_t>(index)]
			: glm::vec2 { 0.0f, 0.0f };
		float right = node.Rect.X + node.Rect.W;
		float bottom = node.Rect.Y + node.Rect.H;
		for (const int child : node.Children)
		{
			const Wui::WuiRect& rect = m_Nodes[static_cast<std::size_t>(child)].Rect;
			right = std::max(right, rect.X + rect.W + applied.x);
			bottom = std::max(bottom, rect.Y + rect.H + applied.y);
		}
		return glm::vec2 { std::max(right - node.Rect.X, 0.0f), std::max(bottom - node.Rect.Y, 0.0f) };
	}

	float UiScreen::MaxScrollOffset(std::string_view id, int axis) const
	{
		const int index = IndexOf(id);
		if (index < 0)
			return 0.0f;
		const UiNodeInstance& node = m_Nodes[static_cast<std::size_t>(index)];
		const glm::vec2 content = ScrollContentSize(id);
		const float extent = axis == 0 ? content.x : content.y;
		const float container = axis == 0 ? node.Rect.W : node.Rect.H;
		return std::max(extent - container, 0.0f);
	}

	void UiScreen::SetScrollOffset(std::string_view id, glm::vec2 offset)
	{
		const int index = IndexOf(id);
		if (index < 0 || !IsScrollContainerIndex(static_cast<std::size_t>(index)))
			return;

		const UiNodeInstance& node = m_Nodes[static_cast<std::size_t>(index)];
		const glm::vec2 content = ScrollContentSize(id);
		const glm::vec2 clamped {
			ClampUiScrollOffset(offset.x, content.x, node.Rect.W),
			ClampUiScrollOffset(offset.y, content.y, node.Rect.H) };

		const std::string key(id);
		if (clamped.x == 0.0f && clamped.y == 0.0f)
			m_ScrollOffsets.erase(key);
		else
			m_ScrollOffsets[key] = clamped;

		ApplyScrollOffsets();
	}

	void UiScreen::SetScrollContentSize(std::string_view id, glm::vec2 size)
	{
		if (IndexOf(id) < 0)
			return;
		m_ScrollContentSizes[std::string(id)] =
			glm::vec2 { std::max(size.x, 0.0f), std::max(size.y, 0.0f) };
	}

	bool UiScreen::EnsureScrollVisible(std::string_view containerId, std::string_view targetId)
	{
		const int containerIndex = IndexOf(containerId);
		const int targetIndex = IndexOf(targetId);
		if (containerIndex < 0 || targetIndex < 0 ||
			!IsScrollContainerIndex(static_cast<std::size_t>(containerIndex)) ||
			!IsDescendantOf(targetIndex, containerIndex))
			return false;

		const UiNodeInstance& container = m_Nodes[static_cast<std::size_t>(containerIndex)];
		const UiNodeInstance& target = m_Nodes[static_cast<std::size_t>(targetIndex)];
		glm::vec2 want = ScrollOffset(containerId);

		if (MaxScrollOffset(containerId, 1) > 0.0f)
		{
			const float top = target.Rect.Y - container.Rect.Y;
			const float bottom = top + target.Rect.H;
			if (top - want.y < 0.0f)
				want.y = top;
			else if (bottom - want.y > container.Rect.H)
				want.y = bottom - container.Rect.H;
		}
		if (MaxScrollOffset(containerId, 0) > 0.0f)
		{
			const float left = target.Rect.X - container.Rect.X;
			const float right = left + target.Rect.W;
			if (left - want.x < 0.0f)
				want.x = left;
			else if (right - want.x > container.Rect.W)
				want.x = right - container.Rect.W;
		}

		const glm::vec2 before = ScrollOffset(containerId);
		SetScrollOffset(containerId, want);
		return ScrollOffset(containerId) != before;
	}

	void UiScreen::ApplyScrollOffsets()
	{
		if (m_AppliedScrollOffsets.size() != m_Nodes.size())
			m_AppliedScrollOffsets.assign(m_Nodes.size(), glm::vec2 { 0.0f, 0.0f });

		// 前序 = 外层容器先于内层。外层先把整个家族(含内层容器)平移,内层再在
		// 已平移的家族里平移自己的后代;位移是纯加法 ⇒ 增量式可重复调用。
		for (std::size_t i = 0; i < m_Nodes.size(); ++i)
		{
			if (!IsScrollContainerIndex(i))
				continue;

			const std::string& id = m_Nodes[i].Id;
			const glm::vec2 content = ScrollContentSize(id);
			const glm::vec2 requested = ScrollOffset(id);
			const glm::vec2 desired {
				ClampUiScrollOffset(requested.x, content.x, m_Nodes[i].Rect.W),
				ClampUiScrollOffset(requested.y, content.y, m_Nodes[i].Rect.H) };

			const glm::vec2 applied = m_AppliedScrollOffsets[i];
			const glm::vec2 delta { applied.x - desired.x, applied.y - desired.y };
			if (delta.x != 0.0f || delta.y != 0.0f)
				TranslateDescendants(i, delta);
			m_AppliedScrollOffsets[i] = desired;
		}
	}

	void UiScreen::TranslateDescendants(std::size_t index, glm::vec2 delta)
	{
		if (index >= m_Nodes.size())
			return;
		for (const int child : m_Nodes[index].Children)
		{
			const std::size_t childIndex = static_cast<std::size_t>(child);
			m_Nodes[childIndex].Rect.X += delta.x;
			m_Nodes[childIndex].Rect.Y += delta.y;
			TranslateDescendants(childIndex, delta);
		}
	}

	bool UiScreen::PointInsideScrollAncestors(int index, glm::vec2 designPoint) const
	{
		for (int parent = m_Nodes[static_cast<std::size_t>(index)].Parent; parent >= 0;
			parent = m_Nodes[static_cast<std::size_t>(parent)].Parent)
		{
			const std::size_t p = static_cast<std::size_t>(parent);
			if (IsScrollContainerIndex(p) && !m_Nodes[p].Rect.Contains(designPoint))
				return false;
		}
		return true;
	}

	bool UiScreen::IsDescendantOf(int index, int ancestor) const
	{
		if (index < 0 || ancestor < 0 || index >= static_cast<int>(m_Nodes.size()))
			return false;
		for (int parent = m_Nodes[static_cast<std::size_t>(index)].Parent; parent >= 0;
			parent = m_Nodes[static_cast<std::size_t>(parent)].Parent)
		{
			if (parent == ancestor)
				return true;
		}
		return false;
	}
}
