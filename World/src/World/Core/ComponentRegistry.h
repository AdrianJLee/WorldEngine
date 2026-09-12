#pragma once
#include "World/Scene/Entity.h"
#include "entt.hpp"
#include <type_traits>
#include <any>

namespace World
{
	struct NativeScriptComponent;
	struct LuaScriptComponent;
	struct RigidBody2DComponent;
	// Clone editor configuration without changing EnTT's ordinary move/copy semantics.
	NativeScriptComponent CloneComponentConfiguration(const NativeScriptComponent& source);
	LuaScriptComponent CloneComponentConfiguration(const LuaScriptComponent& source);
	RigidBody2DComponent CloneComponentConfiguration(const RigidBody2DComponent& source);
	template<typename T>
	T CloneComponentConfiguration(const T& source) { return source; }

	template <typename T, typename = void>
	struct has_ui_logic : std::false_type {};
	template <typename T>
	struct has_ui_logic<T, std::void_t<decltype(T::ComponentPropertiesUI)>> : std::true_type {};

	class TypeDescDataComponent
	{
	public:
		// entt::id_type 是 entt 中用于唯一标识类型的哈希值，通常通过 entt::type_id<T>().hash() 获取。它在运行时用于识别和区分不同的组件类型。
		entt::id_type Id;

		// entt::meta_type 是 entt 的反射系统中的一个核心类型，代表了一个注册的类型信息。它提供了访问类型成员、函数、属性等反射信息的接口。
		entt::meta_type Type;

		void (*AddFunc)(Entity) = nullptr;
		void (*CopyFunc)(Entity dest, Entity src) = nullptr;
		void (*CopyComponentFunc)(entt::registry& destRegistry, entt::registry& srcRegistry, const std::unordered_map<UUID, entt::entity>& entityMap) = nullptr;
		bool (*ComponentPropertiesUI)(Entity) = nullptr;

		template<typename T>
		static void Register(std::any& userData)
		{
			entt::id_type id = entt::type_id<T>().hash();
			entt::meta_type type = entt::resolve(id);
			entt::meta_factory<T>().type(id).template ctor<>();

			// 只有当 T 定义了 ComponentPropertiesUI 时才引用它，避免编译期错误
			bool (*uiFunc)(Entity) = nullptr;
			if constexpr (has_ui_logic<T>::value)
				uiFunc = T::ComponentPropertiesUI;

			// 决定是否提供绑定复制的函数
			void (*copyFunc)(Entity dest, Entity src) = nullptr;
			void (*copyComponentFunc)(entt::registry & destRegistry, entt::registry & srcRegistry, const std::unordered_map<UUID, entt::entity>&entityMap) = nullptr;
			if (!IsTagComponent(id))
			{
				copyFunc = [](Entity dest, Entity src)
					{
						dest.AddOrReplaceComponent<T>(CloneComponentConfiguration(src.GetComponent<T>()));
					};

			}
			copyComponentFunc = [](entt::registry& destRegistry, entt::registry& srcRegistry, const std::unordered_map<UUID, entt::entity>& entityMap)
				{
					auto srcView = srcRegistry.view<T>();
					for (auto entity : srcView)
					{
						UUID entityId = GetEntityUUID(srcRegistry, entity);
						if (entityMap.find(entityId) != entityMap.end())
						{
							entt::entity destEntity = entityMap.at(entityId);
							destRegistry.emplace_or_replace<T>(destEntity, CloneComponentConfiguration(srcRegistry.get<T>(entity)));
						}
					}

				};


			userData = std::make_any<TypeDescDataComponent>(TypeDescDataComponent { id, type,
				[](Entity e) { e.AddComponent<T>(); },
				copyFunc,
				copyComponentFunc,
				uiFunc });
		}

	private:
		static bool IsTagComponent(entt::id_type componentId);
		static UUID GetEntityUUID(entt::registry& registry, entt::entity entity);
	};


	class TypeDescDataScript
	{
	public:
		entt::id_type Id;
		void (*BindFunc)(struct NativeScriptComponent&) = nullptr;

		template<typename T>
		static void Register(std::any& userData)
		{
			entt::id_type id = entt::type_id<T>().hash();
			userData = std::make_any<TypeDescDataScript>(TypeDescDataScript { id,
				[](NativeScriptComponent& nativeScript)
				{
					nativeScript.Bind<T>();
				}
				});
		}
	};

	#define COMPONENT_UI() \
	static bool ComponentPropertiesUI(Entity);
}
