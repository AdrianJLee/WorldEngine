#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <entt.hpp>
#include <glm/glm.hpp>

namespace World::Gameplay
{
	class WLD_API SmartObjectSystem
	{
	public:
		static bool ClaimSlot(Scene& scene, entt::entity smartObject, entt::entity claimer);
		static bool UseSlot(Scene& scene, entt::entity smartObject, entt::entity user);
		static bool ReleaseSlot(Scene& scene, entt::entity smartObject);
		static entt::entity FindNearestAvailableSlot(Scene& scene, const glm::vec3& searchPos,
		                                             uint32_t activityTag, float maxRange = 50.0f);
		static glm::vec3 GetSlotWorldPosition(const Scene& scene, entt::entity smartObject);
		static void Update(Scene& scene, float dt);
	};
}
