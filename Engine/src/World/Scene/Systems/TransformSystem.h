#pragma once

#include "World/Core/Export.h"
#include <cmath>
#include <entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	namespace TransformFlags
	{
		constexpr uint32_t Clean        = 0;
		constexpr uint32_t DirtyLocal   = 1 << 0;
		constexpr uint32_t DirtyWorld   = 1 << 1;
		constexpr uint32_t HasHierarchy = 1 << 2;
		constexpr uint32_t Static       = 1 << 3;
	}

	struct TransformComponent;

	// 纯 ECS 变换系统:
	// 将矩阵解算、层级更新、局部/世界变换求解从组件中剥离,统一在系统层批量处理。
	class WLD_API TransformSystem
	{
	public:
		// 纯数学辅助函数 (SIMD 向量化加速)
		static inline glm::mat4 Compose(const glm::vec3& location, const glm::quat& rotationQuat, const glm::vec3& scale)
		{
			glm::mat4 rotationMatrix = glm::mat4_cast(rotationQuat);
			rotationMatrix[0] *= scale.x;
			rotationMatrix[1] *= scale.y;
			rotationMatrix[2] *= scale.z;
			rotationMatrix[3] = glm::vec4(location, 1.0f);
			return rotationMatrix;
		}

		static inline glm::mat4 Compose2D(float x, float y, float z, float scaleX, float scaleY, float rotationRadians)
		{
			const float c = std::cos(rotationRadians);
			const float s = std::sin(rotationRadians);
			glm::mat4 m(1.0f);
			m[0][0] = c * scaleX;
			m[0][1] = s * scaleX;
			m[1][0] = -s * scaleY;
			m[1][1] = c * scaleY;
			m[3][0] = x;
			m[3][1] = y;
			m[3][2] = z;
			return m;
		}

		static void Decompose(const glm::mat4& transform, glm::vec3& outLocation, glm::quat& outRotationQuat, glm::vec3& outRotationEuler, glm::vec3& outScale);

		// 欧拉角与四元数转换工具
		static inline glm::vec3 ToEulerDegrees(const glm::quat& q) { return glm::degrees(glm::eulerAngles(q)); }
		static inline glm::quat FromEulerDegrees(const glm::vec3& deg) { return glm::quat(glm::radians(deg)); }
		static inline glm::vec3 ToEulerRadians(const glm::quat& q) { return glm::eulerAngles(q); }
		static inline glm::quat FromEulerRadians(const glm::vec3& rad) { return glm::quat(rad); }

		// 单组件重算与设置 (轻量置脏机制)
		static void Recalculate(TransformComponent& transform);
		static void SetTransform(TransformComponent& transform, const glm::vec3& location, const glm::vec3& rotationEuler, const glm::vec3& scale);
		static void SetTransform(TransformComponent& transform, const glm::vec3& location, const glm::quat& rotation, const glm::vec3& scale);
		static void SetTransform(TransformComponent& transform, const glm::mat4& matrix);
		static void SetLocation(TransformComponent& transform, const glm::vec3& location);
		static void SetRotation(TransformComponent& transform, const glm::vec3& rotationEuler);
		static void SetRotation(TransformComponent& transform, const glm::quat& rotation);
		static void SetRotationQuat(TransformComponent& transform, const glm::quat& rotationQuat);
		static void SetScale(TransformComponent& transform, const glm::vec3& scale);

		// 系统更新：批量标记所有实体的局部脏状态
		static void UpdateLocalTransforms(entt::registry& registry);

		// 系统更新：遍历层级森林求解所有实体的 WorldTransformComponent::Matrix
		static uint32_t UpdateWorldTransforms(entt::registry& registry);
	};
}
