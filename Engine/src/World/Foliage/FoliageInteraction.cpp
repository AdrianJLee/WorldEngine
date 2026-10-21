#include "wldpch.h"
#include "World/Foliage/FoliageInteraction.h"
#include <algorithm>
#include <cmath>

namespace World
{
	void FoliageInteractionSystem::ComputePushDisplacement(
		const glm::vec3& foliagePos,
		const glm::vec3& pusherPos,
		const glm::vec3& pusherVelocity,
		float pushRadius,
		float pushStrength,
		glm::vec3& outDisplacement)
	{
		const glm::vec3 delta = foliagePos - pusherPos;
		const float dist2D = std::sqrt(delta.x * delta.x + delta.z * delta.z);

		if (dist2D >= pushRadius || pushRadius <= 0.001f)
		{
			outDisplacement = glm::vec3(0.0f);
			return;
		}

		const float factor = 1.0f - (dist2D / pushRadius);
		glm::vec3 pushDir = (dist2D > 0.001f) ? glm::normalize(glm::vec3(delta.x, 0.0f, delta.z)) : glm::vec3(1.0f, 0.0f, 0.0f);

		// 融合速度动力学冲击
		if (glm::length(pusherVelocity) > 0.01f)
		{
			pushDir = glm::normalize(pushDir + glm::normalize(pusherVelocity) * 0.5f);
		}

		outDisplacement = pushDir * (factor * pushStrength);
	}

	void FoliageInteractionSystem::UpdateSpringRecovery(
		glm::vec3& displacement,
		float recoverySpeed,
		float dt)
	{
		if (glm::length(displacement) < 1e-4f)
		{
			displacement = glm::vec3(0.0f);
			return;
		}

		// 阻尼指数衰减回弹至原位
		const float decay = std::exp(-recoverySpeed * dt);
		displacement *= decay;
	}
}
