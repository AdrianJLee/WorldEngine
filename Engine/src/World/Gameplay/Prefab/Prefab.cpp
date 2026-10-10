#include "wldpch.h"
#include "World/Gameplay/Prefab/Prefab.h"

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
		// 复制单个实体的组件 —— **schema 驱动**,不再是手写清单。
		//
		// 手写清单曾漏掉 3D 物理、灯光、蒙皮、动画以及**所有自定义组件**
		// (Game.dll / 插件注册的组件在 prefab 实例化时静默消失)。
		// 这里改为遍历 schema 注册表,与 `Scene::DuplicateEntity` 同一条机制。
		//
		// 排除两类:
		//   * `UUIDComponent`  —— 预制体实例必须**重新生成身份**,不继承模板的 UUID;
		//   * `NotInPrefab`     —— 关卡/世界层专有组件(如数据层归属),预制体不该携带。
		//     `HierarchyComponent` 由调用方单独重建,不在此处复制。
		void CopySchemaComponents(const Scene& source, Scene& destination,
			entt::entity from, entt::entity to)
		{
			Entity sourceEntity(const_cast<Scene*>(&source), from);
			Entity destinationEntity(&destination, to);
			const auto& schemas = source.GetContext().Schemas();
			for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
			{
				if (!schema || !schema->Storage || !schema->Storage->Copy)
					continue;
				if (schema->NotInPrefab || schema->Id.Name == "World::UUIDComponent"
					|| schema->Id.Name == "World::HierarchyComponent")
					continue;
				if (!sourceEntity.HasComponent(schema->Storage->ComponentId))
					continue;
				schema->Storage->Copy(static_cast<void*>(&destinationEntity),
					static_cast<void*>(&sourceEntity));
			}
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
				std::vector<entt::entity> children = Hierarchy::ChildrenOf(registry, current);
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
			CopySchemaComponents(source, destination, from, to);
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
