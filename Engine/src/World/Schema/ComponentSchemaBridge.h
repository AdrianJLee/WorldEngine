#pragma once

// 组件/脚本 schema 与 EnTT/Entity 之间的桥接。
// Schema 核心不依赖 entt/Scene;这里提供 MakeComponentStorage/MakeScriptBinding,
// 由 schema-compiler 生成的注册 TU 调用。只做类型转换,不持状态。

#include "World/Core/Timestep.h"
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

	// ---- 生命周期方法的编译期探测(SFINAE)——脚本类不继承任何基类 ----
	// 判据是"表达式能否编译",不是"有没有虚函数";没有对应方法 = 该槽位留空。

	template <typename T, typename = void>
	struct HasOnCreateWithEntity : std::false_type {};
	template <typename T>
	struct HasOnCreateWithEntity<T, std::void_t<decltype(std::declval<T&>().OnCreate(std::declval<const World::Entity&>()))>> : std::true_type {};

	template <typename T, typename = void>
	struct HasOnCreatePlain : std::false_type {};
	template <typename T>
	struct HasOnCreatePlain<T, std::void_t<decltype(std::declval<T&>().OnCreate())>> : std::true_type {};

	template <typename T, typename = void>
	struct HasOnUpdate : std::false_type {};
	template <typename T>
	struct HasOnUpdate<T, std::void_t<decltype(std::declval<T&>().OnUpdate(std::declval<World::Timestep>()))>> : std::true_type {};

	template <typename T, typename = void>
	struct HasOnDestroyWithEntity : std::false_type {};
	template <typename T>
	struct HasOnDestroyWithEntity<T, std::void_t<decltype(std::declval<T&>().OnDestroy(std::declval<const World::Entity&>()))>> : std::true_type {};

	template <typename T, typename = void>
	struct HasOnDestroyPlain : std::false_type {};
	template <typename T>
	struct HasOnDestroyPlain<T, std::void_t<decltype(std::declval<T&>().OnDestroy())>> : std::true_type {};

	inline const World::Entity& EntityFromRaw(const void* rawEntity)
	{
		static const World::Entity kEmpty;
		return rawEntity ? *static_cast<const World::Entity*>(rawEntity) : kEmpty;
	}

	template <typename T>
	// 2026-10-01 重写:绑定 = 工厂(创建 + 生命周期 + 销毁)。脚本类**不继承任何基类**:
	// 按需提供 OnCreate/OnUpdate/OnDestroy 同名方法即可,SFINAE 探测后填表;没有的槽位留空。
	inline ScriptBinding MakeScriptBinding()
	{
		ScriptBinding binding;
		binding.Create = []() -> void*
		{
			return static_cast<void*>(WLD_POOL_NEW(T));
		};
		binding.Destroy = [](void* instance)
		{
			if (!instance)
				return;
			WLD_POOL_DELETE(T, World::PoolTag::General, static_cast<T*>(instance));
		};

		if constexpr (HasOnCreateWithEntity<T>::value)
		{
			binding.OnCreate = [](void* instance, void* rawEntity)
			{
				static_cast<T*>(instance)->OnCreate(EntityFromRaw(rawEntity));
			};
		}
		else if constexpr (HasOnCreatePlain<T>::value)
		{
			binding.OnCreate = [](void* instance, void*)
			{
				static_cast<T*>(instance)->OnCreate();
			};
		}

		if constexpr (HasOnUpdate<T>::value)
		{
			binding.OnUpdate = [](void* instance, float deltaSeconds)
			{
				static_cast<T*>(instance)->OnUpdate(World::Timestep(deltaSeconds));
			};
		}

		if constexpr (HasOnDestroyWithEntity<T>::value)
		{
			binding.OnDestroy = [](void* instance, void* rawEntity)
			{
				static_cast<T*>(instance)->OnDestroy(EntityFromRaw(rawEntity));
			};
		}
		else if constexpr (HasOnDestroyPlain<T>::value)
		{
			binding.OnDestroy = [](void* instance, void*)
			{
				static_cast<T*>(instance)->OnDestroy();
			};
		}

		return binding;
	}
}
