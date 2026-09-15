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

	uint32_t UpdateWorldTransforms(entt::registry& registry)
	{
		// 先收集根节点(无父或父已失效),再逐个先序遍历。
		std::vector<entt::entity> roots;
		auto view = registry.view<TransformComponent>();
		for (const entt::entity entity : view)
		{
			const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
			if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
				roots.push_back(entity);
		}

		uint32_t solved = 0;
		// 用显式栈做先序,避免深层级递归爆栈;父矩阵先算好再压子节点。
		std::vector<std::pair<entt::entity, glm::mat4>> stack;
		stack.reserve(roots.size());
		for (const entt::entity root : roots)
			stack.emplace_back(root, glm::mat4(1.0f));

		while (!stack.empty())
		{
			const auto [entity, parentWorld] = stack.back();
			stack.pop_back();
			if (!registry.valid(entity))
				continue;

			const auto* transform = registry.try_get<TransformComponent>(entity);
			if (!transform)
				continue;

			auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
			const bool inherit = !hierarchy || hierarchy->InheritTransform;
			const glm::mat4 world = inherit ? parentWorld * transform->Transform : transform->Transform;
			registry.get_or_emplace<WorldTransformComponent>(entity).Matrix = world;
			solved++;

			if (hierarchy)
				for (const entt::entity child : hierarchy->Children)
					if (registry.valid(child))
						stack.emplace_back(child, world);
		}
		return solved;
	}
}
