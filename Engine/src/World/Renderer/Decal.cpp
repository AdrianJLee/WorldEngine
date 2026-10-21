#include "wldpch.h"
#include "World/Renderer/Decal.h"
#include <algorithm>
#include <cmath>

namespace World
{
	bool DecalProjector::ProjectWorldPoint(
		const glm::mat4& invDecalTransform,
		const glm::vec3& worldPos,
		const glm::vec3& surfaceNormal,
		const glm::vec3& decalDirection,
		float normalCutoffDegrees,
		glm::vec2& outUV,
		float& outFade)
	{
		const glm::vec4 localPos4 = invDecalTransform * glm::vec4(worldPos, 1.0f);
		const glm::vec3 localPos = glm::vec3(localPos4);

		// 局部盒体 [-1, 1]^3 边界裁剪
		if (std::abs(localPos.x) > 1.0f || std::abs(localPos.y) > 1.0f || std::abs(localPos.z) > 1.0f)
			return false;

		// 法线掠射角防拉伸消隐 (Normal Cutoff)
		const float cosTheta = glm::dot(glm::normalize(surfaceNormal), glm::normalize(-decalDirection));
		const float cosCutoff = std::cos(glm::radians(std::clamp(normalCutoffDegrees, 0.0f, 89.0f)));
		if (cosTheta < cosCutoff)
			return false;

		// 投影 UV: local X/Z 映射到 [0, 1]
		outUV = glm::vec2(localPos.x * 0.5f + 0.5f, localPos.z * 0.5f + 0.5f);

		// 综合边缘深度与法线夹角平滑衰减
		const float normalFade = std::clamp((cosTheta - cosCutoff) / std::max(1.0f - cosCutoff, 1e-4f), 0.0f, 1.0f);
		const float depthFade = 1.0f - std::abs(localPos.y);
		outFade = normalFade * depthFade;

		return true;
	}
}
