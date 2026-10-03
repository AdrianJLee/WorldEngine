#include "wldpch.h"
#include "World/Scene/Hierarchy.h"

#include "World/Core/Log.h"
#include "World/Scene/Components.h"

#include <algorithm>

namespace World::Hierarchy
{
	namespace
	{
		// 循环保护:深度超过该阈值即视为非法层级(与 SetParent 的环路检测形成双保险)。
		constexpr uint32_t kMaxDepth = 512;

		// 线程局部复用缓冲:消除 UpdateWorldTransforms 逐帧堆内存分配
		thread_local std::vector<entt::entity> s_Roots;
		thread_local std::vector<std::pair<entt::entity, glm::mat4>> s_Stack;
	}

	bool IsAncestorOf(const entt::registry& registry, entt::entity ancestor, entt::entity node)
	{
		if (ancestor == entt::null || node == entt::null)
			return false;

		entt::entity current = node;
		uint32_t guard = 0;
		while (current != entt::null && guard++ < kMaxDepth)
		{
			if (current == ancestor)
				return true;
			const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
			current = hierarchy ? hierarchy->Parent : entt::null;
		}
		return false;
	}

	uint32_t GetDepth(const entt::registry& registry, entt::entity node)
	{
		uint32_t depth = 0;
		entt::entity current = node;
		while (current != entt::null && depth < kMaxDepth)
		{
			const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
			current = hierarchy ? hierarchy->Parent : entt::null;
			if (current != entt::null)
				depth++;
		}
		return depth;
	}

	bool SetParent(entt::registry& registry, entt::entity child, entt::entity parent)
	{
		if (child == entt::null || !registry.valid(child))
			return false;
		if (parent == child)
			return false;   // 自己不能当自己的父
		if (parent != entt::null && !registry.valid(parent))
			return false;
		// 把 child 挂到自己的后代上会形成环。
		if (parent != entt::null && IsAncestorOf(registry, child, parent))
		{
			WLD_CORE_WARN("Hierarchy::SetParent rejected: would create a cycle (child is an ancestor of parent)");
			return false;
		}
		if (GetDepth(registry, parent) + 1 > kMaxDepth)
		{
			WLD_CORE_WARN("Hierarchy::SetParent rejected: hierarchy deeper than {0}", kMaxDepth);
			return false;
		}

		HierarchyComponent& hierarchy = registry.get_or_emplace<HierarchyComponent>(child);
		if (hierarchy.Parent != entt::null)
		{
			if (auto* oldParent = registry.try_get<HierarchyComponent>(hierarchy.Parent))
				oldParent->Children.erase(
					std::remove(oldParent->Children.begin(), oldParent->Children.end(), child),
					oldParent->Children.end());
		}

		hierarchy.Parent = parent;
		if (parent != entt::null)
		{
			HierarchyComponent& parentHierarchy = registry.get_or_emplace<HierarchyComponent>(parent);
			if (std::find(parentHierarchy.Children.begin(), parentHierarchy.Children.end(), child) ==
				parentHierarchy.Children.end())
				parentHierarchy.Children.push_back(child);
		}
		registry.get_or_emplace<WorldTransformComponent>(child);
		return true;
	}

	void ClearParent(entt::registry& registry, entt::entity child)
	{
		SetParent(registry, child, entt::null);
	}

	int32_t GetChildIndex(const entt::registry& registry, entt::entity child)
	{
		const auto* hierarchy = registry.try_get<HierarchyComponent>(child);
		if (!hierarchy || hierarchy->Parent == entt::null)
			return -1;
		const auto* parent = registry.try_get<HierarchyComponent>(hierarchy->Parent);
		if (!parent)
			return -1;
		for (size_t i = 0; i < parent->Children.size(); ++i)
			if (parent->Children[i] == child)
				return static_cast<int32_t>(i);
		return -1;
	}

	bool InsertChild(entt::registry& registry, entt::entity child, entt::entity parent, size_t index)
	{
		if (!SetParent(registry, child, parent))
			return false;
		if (parent == entt::null)
			return true;   // 根节点顺序由显示侧决定(见面板说明)

		auto* parentHierarchy = registry.try_get<HierarchyComponent>(parent);
		if (!parentHierarchy)
			return true;
		auto& children = parentHierarchy->Children;
		const auto it = std::find(children.begin(), children.end(), child);
		if (it == children.end())
			return true;
		const size_t current = static_cast<size_t>(std::distance(children.begin(), it));
		if (index > children.size())
			index = children.size();
		if (current == index)
			return true;
		children.erase(it);
		// 删除后目标位置可能前移一位。
		const size_t insertAt = current < index ? index - 1 : index;
		children.insert(children.begin() + static_cast<std::ptrdiff_t>(std::min(insertAt, children.size())), child);
		return true;
	}

	uint32_t UpdateWorldTransforms(entt::registry& registry)
	{
		s_Roots.clear();
		auto view = registry.view<TransformComponent>();
		for (const entt::entity entity : view)
		{
			const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
			if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
				s_Roots.push_back(entity);
		}

		uint32_t solved = 0;
		s_Stack.clear();
		s_Stack.reserve(s_Roots.size() * 2);
		for (const entt::entity root : s_Roots)
			s_Stack.emplace_back(root, glm::mat4(1.0f));

		while (!s_Stack.empty())
		{
			const auto [entity, parentWorld] = s_Stack.back();
			s_Stack.pop_back();
			if (!registry.valid(entity))
				continue;

			auto* transform = registry.try_get<TransformComponent>(entity);
			if (!transform)
				continue;

			auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
			const bool inherit = !hierarchy || hierarchy->InheritTransform;
			const glm::mat4 local = TransformSystem::Compose(transform->Location, transform->Rotation, transform->Scale);
			const glm::mat4 world = inherit ? parentWorld * local : local;
			registry.get_or_emplace<WorldTransformComponent>(entity).Matrix = world;
			transform->Flags &= ~(TransformFlags::DirtyLocal | TransformFlags::DirtyWorld);
			solved++;

			if (hierarchy)
				for (const entt::entity child : hierarchy->Children)
					if (registry.valid(child))
						s_Stack.emplace_back(child, world);
		}
		return solved;
	}
}
