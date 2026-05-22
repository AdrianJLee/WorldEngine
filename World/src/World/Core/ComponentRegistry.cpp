#include "wldpch.h"
#include "ComponentRegistry.h"
#include "World/Scene/Components.h"
namespace World
{
	bool TypeDescDataComponent::IsTagComponent(entt::id_type componentId)
	{
		return componentId == entt::type_id<UUIDComponent>().hash();
	}
	UUID TypeDescDataComponent::GetEntityUUID(entt::registry& registry, entt::entity entity)
	{
		return registry.get<UUIDComponent>(entity).ID;
	}

}