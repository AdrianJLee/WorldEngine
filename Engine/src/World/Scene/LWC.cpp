#include "wldpch.h"
#include "World/Scene/LWC.h"
#include <glm/gtc/matrix_transform.hpp>

namespace World
{
	glm::vec3 WorldTransform64::ToCameraRelative(const glm::dvec3& worldDoublePos, const glm::dvec3& cameraDoublePos)
	{
		const glm::dvec3 delta = worldDoublePos - cameraDoublePos;
		return glm::vec3(static_cast<float>(delta.x), static_cast<float>(delta.y), static_cast<float>(delta.z));
	}

	glm::mat4 WorldTransform64::ToCameraRelativeMatrix(const glm::dvec3& cameraDoublePos) const
	{
		const glm::vec3 relativePos = ToCameraRelative(DoublePosition, cameraDoublePos);
		glm::mat4 m = glm::translate(glm::mat4(1.0f), relativePos);
		m *= glm::mat4_cast(Rotation);
		m = glm::scale(m, Scale);
		return m;
	}
}
