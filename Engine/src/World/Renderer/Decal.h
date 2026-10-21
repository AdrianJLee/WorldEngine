#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>

namespace World
{
	class WLD_API DecalProjector
	{
	public:
		// 计算贴花局部立方体空间投影
		static bool ProjectWorldPoint(
			const glm::mat4& invDecalTransform,
			const glm::vec3& worldPos,
			const glm::vec3& surfaceNormal,
			const glm::vec3& decalDirection,
			float normalCutoffDegrees,
			glm::vec2& outUV,
			float& outFade);
	};
}
