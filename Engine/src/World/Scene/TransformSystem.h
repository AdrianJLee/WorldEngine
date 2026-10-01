#pragma once

#include "World/Core/Export.h"
#include <entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	struct TransformComponent;

	// 纯 ECS 变换系统:
	// 将矩阵解算、层级更新、局部/世界变换求解从组件中剥离,统一在系统层批量处理。
	class WLD_API TransformSystem
	{
	public:
		// 纯数学辅助函数
		static glm::mat4 Compose(const glm::vec3& location, const glm::quat& rotationQuat, const glm::vec3& scale);
		static void Decompose(const glm::mat4& transform, glm::vec3& outLocation, glm::quat& outRotationQuat, glm::vec3& outRotationEuler, glm::vec3& outScale);

		// 单组件重算与设置
		static void Recalculate(TransformComponent& transform);
		static void SetTransform(TransformComponent& transform, const glm::vec3& location, const glm::vec3& rotation, const glm::vec3& scale);
		static void SetTransform(TransformComponent& transform, const glm::mat4& matrix);
		static void SetLocation(TransformComponent& transform, const glm::vec3& location);
		static void SetRotation(TransformComponent& transform, const glm::vec3& rotation);
		static void SetRotationQuat(TransformComponent& transform, const glm::quat& rotationQuat);
		static void SetScale(TransformComponent& transform, const glm::vec3& scale);

		// 系统更新：批量重新计算所有实体的局部 Transform 矩阵
		static void UpdateLocalTransforms(entt::registry& registry);

		// 系统更新：遍历层级森林求解所有实体的 WorldTransformComponent::Matrix
		static uint32_t UpdateWorldTransforms(entt::registry& registry);
	};
}
