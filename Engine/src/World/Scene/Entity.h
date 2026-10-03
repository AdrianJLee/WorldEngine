#pragma once
#include "World/Scene/Scene.h"
#include "World/Core/UUID.h"
#include <entt.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace World
{
	class Entity
	{
	public:
		static Entity CreateEntity(Scene* scene, const std::string& name = "Empty Entity", const UUID& id = UUID());
		static Entity CreateRaw(Scene* scene);
		static void DestroyEntity(Scene* scene, Entity entity);
		Entity() = default;
		Entity(Scene* scene, entt::entity handle);

		bool IsValid() const;
		bool CanAddComponent(entt::id_type component, std::string* reason = nullptr) const;
		bool CanRemoveComponent(entt::id_type component, std::string* reason = nullptr) const;

		template<typename T, typename... Args>
		decltype(auto) AddOrReplaceComponent(Args&&... args)
		{
			RequireValid();
			m_Scene->AssertStructuralWrite();
			RequireCanAdd(entt::type_id<T>().hash(), true);
			if constexpr (std::is_empty_v<T>)
			{
				m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
			}
			else
			{
				return m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
			}
		}
		template<typename T, typename... Args>
		decltype(auto) AddComponent(Args&&... args)
		{
			RequireValid();
			m_Scene->AssertStructuralWrite();
			RequireCanAdd(entt::type_id<T>().hash(), false);
			if constexpr (std::is_empty_v<T>)
			{
				m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
			}
			else
			{
				return m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
			}
		}

		template<typename T, typename... Args>
		Entity& Set(Args&&... args)
		{
			RequireValid();
			m_Scene->AssertStructuralWrite();
			RequireCanAdd(entt::type_id<T>().hash(), true);
			m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
			return *this;
		}
		template<typename T, typename... Args>
		Entity& With(Args&&... args)
		{
			RequireValid();
			m_Scene->AssertStructuralWrite();
			RequireCanAdd(entt::type_id<T>().hash(), true);
			m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
			return *this;
		}
		template<typename T>
		Entity& Without()
		{
			RemoveComponent<T>();
			return *this;
		}
		// No-data dynamic adds can be requested by Lua; data-bearing adds stay synchronous.
		void AddComponent(entt::id_type componentId, const void* data = nullptr);
		template<typename T>
		bool HasComponent() const
		{
			RequireValid();
			return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}
		bool HasComponent(entt::id_type componentId) const;
		template<typename T>
		T& GetComponent()
		{
			RequireValid();
			if (!m_Scene->m_Registry.all_of<T>(m_EntityHandle))
				throw std::logic_error("Entity does not have the requested component");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}
		void* GetComponent(entt::id_type componentId);
		template<typename T>
		void RemoveComponent() { RemoveComponent(entt::type_id<T>().hash()); }
		void RemoveComponent(entt::id_type componentId);

		operator bool() const { return IsValid(); }
		operator entt::entity() const { return m_EntityHandle; }
		operator uint32_t() const { return static_cast<uint32_t>(m_EntityHandle); }
		bool operator==(const Entity& other) const
		{
			return m_EntityHandle == other.m_EntityHandle &&
			       m_Scene == other.m_Scene &&
			       m_SceneSlot == other.m_SceneSlot &&
			       m_SceneGeneration == other.m_SceneGeneration;
		}
		bool operator!=(const Entity& other) const { return !(*this == other); }
		Scene* GetScene() const { RequireValid(); return m_Scene; }

	private:
		void RequireValid() const;
		bool CheckAdd(entt::id_type component, bool replace, bool requireDependencies, std::string* reason) const;
		void RequireCanAdd(entt::id_type component, bool replace) const;
		entt::entity m_EntityHandle = entt::null;
		uint16_t m_SceneSlot = 0;
		uint16_t m_SceneGeneration = 0;
		Scene* m_Scene = nullptr;
	};

	static_assert(sizeof(Entity) == 16, "Entity must be exactly 16 bytes");
	static_assert(std::is_trivially_copyable_v<Entity>, "Entity must be trivially copyable");
	static_assert(std::is_trivially_destructible_v<Entity>, "Entity must be trivially destructible");
}
