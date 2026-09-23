#include "wldpch.h"
#include "World/Gameplay/Prefab.h"

#include "World/Core/Log.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace World::Gameplay
{
	namespace
	{
		// 复制单个实体的内置组件(HierarchyComponent 单独重建,避免复制到源场景的句柄)。
		template <typename T>
		void CopyComponentIfPresent(const entt::registry& source, entt::registry& destination,
			entt::entity from, entt::entity to)
		{
			if (const auto* component = source.try_get<T>(from))
				destination.emplace_or_replace<T>(to, *component);
		}

		void CopyBuiltinComponents(const entt::registry& source, entt::registry& destination,
			entt::entity from, entt::entity to)
		{
			CopyComponentIfPresent<TagComponent>(source, destination, from, to);
			CopyComponentIfPresent<TransformComponent>(source, destination, from, to);
			CopyComponentIfPresent<SpriteComponent>(source, destination, from, to);
			CopyComponentIfPresent<CircleRendererComponent>(source, destination, from, to);
			CopyComponentIfPresent<MeshRendererComponent>(source, destination, from, to);
			CopyComponentIfPresent<CameraComponent>(source, destination, from, to);
			// 物理组件的配置要带过去,但**运行时句柄不能跨场景共享**:
			// b2BodyId 属于创建它的物理世界,直接拷贝会让两个场景持有同一个 body,
			// 退出时的物理销毁路径就会重复销毁 -> 崩溃。这里复制配置并把句柄置空,
			// 由目标场景在自己的物理世界启动时重新创建 body。
			if (const auto* body = source.try_get<RigidBody2DComponent>(from))
			{
				RigidBody2DComponent copy = *body;
				copy.RuntimeBodyId = b2_nullBodyId;
				destination.emplace_or_replace<RigidBody2DComponent>(to, copy);
			}
			CopyComponentIfPresent<BoxCollider2DComponent>(source, destination, from, to);
			CopyComponentIfPresent<CircleCollider2DComponent>(source, destination, from, to);
		}

		// 先序收集子树:记录"副本内应使用的父节点"(实例根始终为 null,父由调用方指定)。
		void CollectSubtree(const entt::registry& registry, entt::entity root,
			std::vector<entt::entity>& out, std::unordered_map<entt::entity, entt::entity>& parents)
		{
			std::vector<entt::entity> stack { root };
			while (!stack.empty())
			{
				const entt::entity current = stack.back();
				stack.pop_back();
				if (!registry.valid(current) || parents.find(current) != parents.end())
					continue;

				const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
				const entt::entity sourceParent =
					(hierarchy && hierarchy->Parent != entt::null && registry.valid(hierarchy->Parent))
						? hierarchy->Parent : entt::null;
				parents[current] = current == root ? entt::null : sourceParent;
				out.push_back(current);

				if (!hierarchy)
					continue;
				std::vector<entt::entity> children = hierarchy->Children;
				std::reverse(children.begin(), children.end());   // 栈序 → 保持源顺序
				for (const entt::entity child : children)
					stack.push_back(child);
			}
		}
	}

	PrefabInstanceResult Instantiate(const Scene& source, Entity sourceRoot,
		Scene& destination, entt::entity parent)
	{
		PrefabInstanceResult result;
		if (!sourceRoot.IsValid() || sourceRoot.GetScene() != &source)
		{
			WLD_CORE_WARN("Prefab::Instantiate: source entity is not valid in the given scene");
			return result;
		}

		const entt::registry& sourceRegistry = source.GetRegistry();
		entt::registry& destinationRegistry = destination.GetRegistry();
		const entt::entity sourceRootHandle = static_cast<entt::entity>(sourceRoot);

		std::vector<entt::entity> subtree;
		std::unordered_map<entt::entity, entt::entity> sourceParents;
		CollectSubtree(sourceRegistry, sourceRootHandle, subtree, sourceParents);
		if (subtree.empty())
			return result;

		// 1) 建实体 + 复制内置组件 + 重新生成 UUID。
		std::unordered_map<entt::entity, entt::entity> remap;
		for (const entt::entity from : subtree)
		{
			const entt::entity to = destinationRegistry.create();
			remap[from] = to;
			CopyBuiltinComponents(sourceRegistry, destinationRegistry, from, to);
			destinationRegistry.emplace_or_replace<UUIDComponent>(to, UUID());
		}

		// 2) 重建层级:先按源关系连,再把实例根挂到目标 parent 下。
		for (const entt::entity from : subtree)
		{
			const auto it = sourceParents.find(from);
			if (it == sourceParents.end() || it->second == entt::null)
				continue;
			const auto parentRemap = remap.find(it->second);
			if (parentRemap != remap.end())
				Hierarchy::SetParent(destinationRegistry, remap[from], parentRemap->second);
		}
		if (parent != entt::null && destinationRegistry.valid(parent))
			Hierarchy::SetParent(destinationRegistry, remap[sourceRootHandle], parent);

		Hierarchy::UpdateWorldTransforms(destinationRegistry);
		result.Root = Entity(&destination, remap[sourceRootHandle]);
		result.EntityCount = static_cast<uint32_t>(subtree.size());
		return result;
	}
}
