#pragma once
#include "World/Scene/Scene.h"
#include "World/Core/UUID.h"

#include <entt.hpp>

namespace World
{
	class Entity
	{
	public:
		static Entity CreateEntity(Scene* scene, const std::string& name = "Empty Entity", const UUID& id = UUID());
		static void DestroyEntity(Scene* scene, Entity entity);


	public:
		Entity() = default;
		Entity(Scene* scene, entt::entity handle);

		template<typename T, typename... Args>
		T& AddOrReplaceComponent(Args&&... args)
		{
			WLD_CORE_ASSERT(m_Scene, "Scene is null!");

			return m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
		}

		template<typename T, typename... Args>
		T& AddComponent(Args&&...args)
		{
			WLD_CORE_ASSERT(m_Scene, "Scene is null!");
			return m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
		}

		void AddComponent(entt::id_type componentId, const void* data = nullptr)
		{
			WLD_CORE_ASSERT(m_Scene, "Scene is null!");
			auto storage = m_Scene->m_Registry.storage(componentId);
			WLD_CORE_ASSERT(storage, "Component storage not found for component ID: {0}", componentId);

			if (data)
			{
				storage->push(m_EntityHandle, data);
			}
			else
			{
				storage->push(m_EntityHandle);
			}
		}


		template<typename T>
		bool HasComponent()
		{
			WLD_CORE_ASSERT(m_Scene, "Scene is null!");
			return m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		};

		bool HasComponent(entt::id_type componentId)
		{
			WLD_CORE_ASSERT(m_Scene, "Scene is null!");
			return m_Scene->m_Registry.storage(componentId) && m_Scene->m_Registry.storage(componentId)->contains(m_EntityHandle);
		}

		template<typename T>
		T& GetComponent()
		{
			WLD_CORE_ASSERT(m_Scene, "Scene is null!");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		void* GetComponent(entt::id_type componentId)
		{
			WLD_CORE_ASSERT(m_Scene, "Scene is null!");
			auto storage = m_Scene->m_Registry.storage(componentId);
			return storage->value(m_EntityHandle);
		}

		template <typename T>
		void RemoveComponent()
		{
			if (HasComponent<T>())
			{
				m_Scene->m_Registry.remove<T>(m_EntityHandle);
			}
		}

		operator bool() const { return m_EntityHandle != entt::null && m_Scene != nullptr; }
		operator entt::entity() const { return m_EntityHandle; }
		operator uint32_t() const { return (uint32_t)m_EntityHandle; }

		bool operator==(const Entity& other) const
		{
			return m_EntityHandle == other.m_EntityHandle && m_Scene == other.m_Scene;
		}

		bool operator!=(const Entity& other) const
		{
			return !(*this == other);
		}

		inline Scene* GetScene() const { return m_Scene; }
	private:
		entt::entity m_EntityHandle { entt::null };
		Scene* m_Scene;
	};
}

