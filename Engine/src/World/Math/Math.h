#pragma once
#include <glm/glm.hpp>
namespace World::Math
{
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outLocation, glm::vec3& outRotation, glm::vec3& outScale);

	void MultiplyMat4ByVec4_SIMD_x4(const glm::mat4& transform, const glm::vec4* vertices, glm::vec3* outPositions);
}