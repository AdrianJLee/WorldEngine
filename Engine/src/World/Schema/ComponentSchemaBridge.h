#pragma once

// 组件/脚本 schema 与 EnTT/Entity 之间的桥接。
// Schema 核心不依赖 entt/Scene;这里提供 MakeComponentStorage/MakeScriptBinding,
// 由 schema-compiler 生成的注册 TU 调用。只做类型转换,不持状态。

#include "World/Schema/Schema.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"

#include <entt.hpp>
#include <type_traits>
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
	// 2026-09-26 重写:绑定 = 工厂(创建 + 销毁),不再往组件里写函数指针。
	// 断言 T 真的是脚本类型(有虚析构,可安全经 ScriptableEntity* 删除)。
	inline ScriptBinding MakeScriptBinding()
	{
		static_assert(std::is_base_of_v<World::ScriptableEntity, T>,
			"Category==Script 的类型必须继承 ScriptableEntity");
		ScriptBinding binding;
		binding.Create = []() -> World::ScriptableEntity*
		{
			return static_cast<World::ScriptableEntity*>(WLD_POOL_NEW(T));
		};
		binding.Destroy = [](World::ScriptableEntity* instance)
		{
			if (!instance)
				return;
			WLD_POOL_DELETE(T, World::PoolTag::General, instance);
		};
		return binding;
	}
}
