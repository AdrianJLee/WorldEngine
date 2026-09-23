#include "wldpch.h"
#include "World/Gameplay/Prefab.h"

#include "World/Core/Log.h"
#include "World/Scene/Components.h"

#include <memory>

namespace World::Gameplay
{
	namespace
	{
		// 按先序把子树展平成实体列表(实例与 prefab 同构,顺序一一对应)。
		void Flatten(const entt::registry& registry, entt::entity root, std::vector<entt::entity>& out)
		{
			std::vector<entt::entity> stack { root };
			while (!stack.empty())
			{
				const entt::entity current = stack.back();
				stack.pop_back();
				if (!registry.valid(current))
					continue;
				out.push_back(current);
				const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
				if (!hierarchy)
					continue;
				for (auto it = hierarchy->Children.rbegin(); it != hierarchy->Children.rend(); ++it)
					stack.push_back(*it);
			}
		}

		template <typename T>
		void RestoreComponentIfPresent(const entt::registry& source, entt::registry& destination,
			entt::entity from, entt::entity to)
		{
			if (const auto* component = source.try_get<T>(from))
				destination.emplace_or_replace<T>(to, *component);
		}

		void RestoreBuiltinComponents(const entt::registry& source, entt::registry& destination,
			entt::entity from, entt::entity to)
		{
			RestoreComponentIfPresent<TagComponent>(source, destination, from, to);
			RestoreComponentIfPresent<TransformComponent>(source, destination, from, to);
			RestoreComponentIfPresent<SpriteComponent>(source, destination, from, to);
			RestoreComponentIfPresent<CircleRendererComponent>(source, destination, from, to);
			RestoreComponentIfPresent<MeshRendererComponent>(source, destination, from, to);
			RestoreComponentIfPresent<CameraComponent>(source, destination, from, to);
			// 同 Prefab.cpp:物理运行时句柄属于各自的物理世界,回滚时也置空重建。
			if (const auto* body = source.try_get<RigidBody2DComponent>(from))
			{
				RigidBody2DComponent copy = *body;
				copy.RuntimeBodyId = b2_nullBodyId;
				destination.emplace_or_replace<RigidBody2DComponent>(to, copy);
			}
			RestoreComponentIfPresent<BoxCollider2DComponent>(source, destination, from, to);
			RestoreComponentIfPresent<CircleCollider2DComponent>(source, destination, from, to);
		}
	}

	void MarkOverride(PrefabInstanceRecord& record, entt::entity entity, const std::string& field)
	{
		if (entity == entt::null || field.empty())
			return;
		auto& fields = record.Overrides[static_cast<uint32_t>(entity)];
		if (std::find(fields.begin(), fields.end(), field) == fields.end())
			fields.push_back(field);
	}

	bool HasOverride(const PrefabInstanceRecord& record, entt::entity entity)
	{
		const auto it = record.Overrides.find(static_cast<uint32_t>(entity));
		return it != record.Overrides.end() && !it->second.empty();
	}

	size_t GetOverrideCount(const PrefabInstanceRecord& record)
	{
		size_t count = 0;
		for (const auto& [entity, fields] : record.Overrides)
			count += fields.size();
		return count;
	}

	void ClearOverrides(PrefabInstanceRecord& record)
	{
		record.Overrides.clear();
	}

	bool CanRevert(const PrefabInstanceRecord& record, const Scene& scene)
	{
		return record.IsValid() && !record.PrefabPath.empty() && scene.GetRegistry().valid(record.Root);
	}

	bool UnpackInstance(PrefabInstanceRecord& record)
	{
		if (!record.IsValid())
			return false;
		ClearOverrides(record);
		record.PrefabPath.clear();
		WLD_CORE_INFO("Prefab::UnpackInstance: subtree at entity {0} is no longer a prefab instance",
			static_cast<uint32_t>(record.Root));
		return true;
	}

	bool RevertInstance(PrefabInstanceRecord& record, Scene& scene)
	{
		if (!record.IsValid() || record.PrefabPath.empty())
		{
			WLD_CORE_WARN("Prefab::RevertInstance: record has no prefab source");
			return false;
		}
		if (!scene.GetRegistry().valid(record.Root))
		{
			WLD_CORE_WARN("Prefab::RevertInstance: instance root is no longer valid");
			return false;
		}

		// 重新实例化的临时场景(与 InstantiateFromFile 同一路径,保证"回滚 = 回到 prefab 原值")。
		const Ref<Scene> fresh = CreateRef<Scene>(scene.GetContext());
		const PrefabInstanceResult staged = InstantiateFromFile(record.PrefabPath, *fresh);
		if (!staged.IsValid())
			return false;

		std::vector<entt::entity> sourceOrder;
		std::vector<entt::entity> instanceOrder;
		const entt::registry& sourceRegistry = fresh->GetRegistry();
		entt::registry& instanceRegistry = scene.GetRegistry();
		Flatten(sourceRegistry, static_cast<entt::entity>(staged.Root), sourceOrder);
		Flatten(instanceRegistry, record.Root, instanceOrder);
		if (sourceOrder.size() != instanceOrder.size())
		{
			WLD_CORE_WARN("Prefab::RevertInstance: instance structure differs from prefab ({0} vs {1} entities)",
				instanceOrder.size(), sourceOrder.size());
			return false;
		}

		for (size_t i = 0; i < sourceOrder.size(); ++i)
			RestoreBuiltinComponents(sourceRegistry, instanceRegistry, sourceOrder[i], instanceOrder[i]);

		ClearOverrides(record);
		WLD_CORE_INFO("Prefab::RevertInstance: {0} entities restored from '{1}'",
			instanceOrder.size(), record.PrefabPath);
		return true;
	}
}
