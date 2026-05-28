#include "wldpch.h"
#include "Entity.h"

#include "World/Scene/Components.h"

namespace World
{

	Entity Entity::CreateEntity(Scene* scene, const std::string& name, const UUID& id)
	{
		WLD_CORE_ASSERT(scene, "Scene is null!");

		auto entity = Entity(scene, scene->m_Registry.create());
		entity.AddComponent<TagComponent>(name);
		entity.AddComponent<UUIDComponent>(id);
		return entity;

	}
	void Entity::DestroyEntity(Scene* scene, Entity entity)
	{
		scene->m_Registry.destroy(entity);
	}

	Entity::Entity(Scene* scene, entt::entity handle)
		: m_EntityHandle(handle), m_Scene(scene)
	{

	}
}