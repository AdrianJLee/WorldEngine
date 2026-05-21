#pragma once
#include "World/Scene/Entity.h"
#include "entt.hpp"
#include <type_traits>

namespace World
{
	struct ComponentInfo
	{
		entt::id_type Id;
		entt::meta_type Type;
		std::string Name;
		void (*AddFunc)(Entity) = nullptr;
		void (*CopyFunc)(Entity dest, Entity src) = nullptr;
		void (*CopyComponentFunc)(entt::registry& destRegistry, entt::registry& srcRegistry, const std::unordered_map<UUID, entt::entity>& entityMap) = nullptr;
		void (*ComponentPropertiesUI)(Entity) = nullptr;
	};
	class ComponentRegistry
	{
	public:
		using RegistryMap = std::vector<ComponentInfo>;

		static RegistryMap& GetList()
		{
			static RegistryMap s_List;
			return s_List;
		}

		template<typename T>
		static void Register(const std::string& name)
		{
			entt::id_type id = entt::type_id<T>().hash();
			entt::meta_type type = entt::resolve(id);

			// 只有当 T 定义了 ComponentPropertiesUI 时才引用它，避免编译期错误
			void (*uiFunc)(Entity) = nullptr;
			if constexpr (has_ui_logic<T>::value)
				uiFunc = T::ComponentPropertiesUI;

			// 决定是否提供绑定复制的函数
			void (*copyFunc)(Entity dest, Entity src) = nullptr;
			void (*copyComponentFunc)(entt::registry & destRegistry, entt::registry & srcRegistry, const std::unordered_map<UUID, entt::entity>&entityMap) = nullptr;
			if (!IsTagComponent(id))
			{
				copyFunc = [](Entity dest, Entity src)
					{
						dest.AddOrReplaceComponent<T>(src.GetComponent<T>());
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
							destRegistry.emplace_or_replace<T>(destEntity, srcRegistry.get<T>(entity));
						}
					}

				};

			GetList().push_back({ id, type,name,
				[](Entity e) { e.AddComponent<T>(); },
				copyFunc,
				copyComponentFunc,
				uiFunc });
		}

		//// 1. 注册 (通常在程序启动时)
		//entt::meta<TransformComponent>().type("Transform"_hs); // 将 ID 和类型绑定

		//// 2. 获取
		//entt::id_type id = "Transform"_hs;
		//entt::meta_type type = entt::resolve(id); // 通过 ID 获取“类型代理”

		//if (type)
		//{
		//	// 你甚至可以在不知道类型 T 的情况下创建它
		//	entt::meta_any instance = type.construct();
		//	printf("找到了类型: %s", type.info().name().data());
		//}
	private:
		static bool IsTagComponent(entt::id_type componentId);
		static UUID GetEntityUUID(entt::registry& registry, entt::entity entity);
	};

	struct NativeScriptComponent;
	struct ScriptInfo
	{
		entt::id_type Id;
		entt::meta_type Type;
		std::string Name;
		// 绑定回调：传入目标实体上的 nativeScript，并执行 Bind<T>
		void (*BindFunc)(NativeScriptComponent&) = nullptr;
	};

	class ScriptRegistry
	{
	public:
		using ScriptRegistryMap = std::vector<ScriptInfo>;

		static ScriptRegistryMap& GetScriptList()
		{
			static ScriptRegistryMap s_ScriptList;
			return s_ScriptList;
		}

		template<typename T>
		static void Register(const std::string& name)
		{
			static_assert(std::is_base_of<ScriptableEntity, T>::value, "Registered script must inherit from ScriptableEntity!");

			entt::id_type id = entt::type_id<T>().hash();
			entt::meta_type type = entt::resolve(id);

			GetScriptList().push_back({ id, type, name,
				// 工厂函数：当通过名称查找到当前 ScriptInfo，以此回调将 T 绑定给 NativeScriptComponent
				[](NativeScriptComponent& nativeScript)
				{
					nativeScript.Bind<T>();
				}
				});
		}
	};

	#define REGISTER_COMPONENT(Type) \
    inline static bool Type##_Registered = []() { \
        ComponentRegistry::Register<Type>(#Type); \
        return true; \
    }();

	#define REGISTER_SCRIPT(Type) \
	inline static bool Type##_Registered = []() { \
		ScriptRegistry::Register<Type>(#Type); \
		return true; \
	}();


	// 探测器：检查 T 是否有名为 ComponentPropertiesUI 的成员
	template <typename T, typename = void>
	struct has_ui_logic : std::false_type {};
	template <typename T>
	struct has_ui_logic<T, std::void_t<decltype(T::ComponentPropertiesUI)>> : std::true_type {};

	#define COMPONENT_UI() \
	static void ComponentPropertiesUI(Entity);
}
