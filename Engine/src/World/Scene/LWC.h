#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	struct WLD_API WorldTransform64
	{
		glm::dvec3 DoublePosition { 0.0 };
		glm::quat Rotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale { 1.0f };

		// 计算以相机为中心的 32 位相对变换矩阵 (Camera-Relative Rendering)
		glm::mat4 ToCameraRelativeMatrix(const glm::dvec3& cameraDoublePos) const;

		// 相对坐标向量转换
		static glm::vec3 ToCameraRelative(const glm::dvec3& worldDoublePos, const glm::dvec3& cameraDoublePos);
	};
}
