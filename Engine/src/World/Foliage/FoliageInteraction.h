#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>

namespace World
{
	class WLD_API FoliageInteractionSystem
	{
	public:
		// 计算实体推挤倒伏位移
		static void ComputePushDisplacement(
			const glm::vec3& foliagePos,
			const glm::vec3& pusherPos,
			const glm::vec3& pusherVelocity,
			float pushRadius,
			float pushStrength,
			glm::vec3& outDisplacement);

		// 阻尼谐振弹性回弹更新
		static void UpdateSpringRecovery(
			glm::vec3& displacement,
			float recoverySpeed,
			float dt);
	};
}
