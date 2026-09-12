#pragma once

// 组件/脚本 schema 与 EnTT/Entity 之间的桥接。
// Schema 核心不依赖 entt/Scene;这里提供 MakeComponentStorage/MakeScriptBinding,
// 由 schema-compiler 生成的注册 TU 调用。只做类型转换,不持状态。

#include "World/Schema/Schema.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"

#include <entt.hpp>
#include <unordered_map>

namespace World::Schema
{
	inline bool IsTagComponent(uint32_t componentId)
	{
		return componentId == static_cast<uint32_t>(entt::type_id<World::UUIDComponent>().hash());
	}

	inline World::UUID GetEntityUUID(entt::registry& registry, entt::entity entity)
	{
		return registry.get<World::UUIDComponent>(entity).ID;
	}

	template <typename T>
	StorageBinding MakeComponentStorage()
	{
		StorageBinding binding;
		binding.ComponentId = static_cast<uint32_t>(entt::type_id<T>().hash());
		binding.Add = [](void* rawEntity)
		{
			static_cast<World::Entity*>(rawEntity)->AddComponent<T>();
		};
		binding.Copy = IsTagComponent(binding.ComponentId) ? nullptr : +[](void* rawDst, void* rawSrc)
		{
			auto* dst = static_cast<World::Entity*>(rawDst);
			auto* src = static_cast<World::Entity*>(rawSrc);
			dst->AddOrReplaceComponent<T>(World::CloneComponentConfiguration(src->GetComponent<T>()));
		};
		binding.CopyAll = [](void* rawDstRegistry, void* rawSrcRegistry, const void* rawEntityMap)
		{
			auto& dstRegistry = *static_cast<entt::registry*>(rawDstRegistry);
			auto& srcRegistry = *static_cast<entt::registry*>(rawSrcRegistry);
			const auto& entityMap = *static_cast<const std::unordered_map<World::UUID, entt::entity>*>(rawEntityMap);
			for (auto entity : srcRegistry.view<T>())
			{
				const World::UUID entityId = GetEntityUUID(srcRegistry, entity);
				const auto it = entityMap.find(entityId);
				if (it != entityMap.end())
					dstRegistry.emplace_or_replace<T>(it->second, World::CloneComponentConfiguration(srcRegistry.get<T>(entity)));
			}
		};
		return binding;
	}

	template <typename T>
	ScriptBinding MakeScriptBinding()
	{
		ScriptBinding binding;
		binding.Bind = [](void* rawScript)
		{
			static_cast<World::NativeScriptComponent*>(rawScript)->Bind<T>();
		};
		return binding;
	}
}
