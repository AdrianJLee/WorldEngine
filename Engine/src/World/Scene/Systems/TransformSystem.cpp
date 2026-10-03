#include "World/Scene/Systems/TransformSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"

#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

namespace World
{
	glm::mat4 TransformSystem::Compose(const glm::vec3& location, const glm::quat& rotationQuat, const glm::vec3& scale)
	{
		glm::mat4 rotationMatrix = glm::mat4_cast(rotationQuat);
		glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), scale);
		glm::mat4 translationMatrix = glm::translate(glm::mat4(1.0f), location);
		return translationMatrix * rotationMatrix * scaleMatrix;
	}

	void TransformSystem::Decompose(const glm::mat4& transform, glm::vec3& outLocation, glm::quat& outRotationQuat, glm::vec3& outRotationEuler, glm::vec3& outScale)
	{
		outLocation = glm::vec3(transform[3]);
		outScale.x = glm::length(glm::vec3(transform[0]));
		outScale.y = glm::length(glm::vec3(transform[1]));
		outScale.z = glm::length(glm::vec3(transform[2]));
		if (glm::determinant(transform) < 0.0f)
			outScale.x *= -1.0f;

		glm::mat4 rotationMatrix = transform;
		if (outScale.x != 0.0f) rotationMatrix[0] /= outScale.x;
		if (outScale.y != 0.0f) rotationMatrix[1] /= outScale.y;
		if (outScale.z != 0.0f) rotationMatrix[2] /= outScale.z;
		rotationMatrix[3] = glm::vec4(0, 0, 0, 1);

		outRotationQuat = glm::quat_cast(rotationMatrix);
		outRotationEuler = glm::eulerAngles(outRotationQuat);
	}

	void TransformSystem::Recalculate(TransformComponent& transform)
	{
		transform.Transform = Compose(transform.Location, transform.RotationQuat, transform.Scale);
	}

	void TransformSystem::SetTransform(TransformComponent& transform, const glm::vec3& location, const glm::vec3& rotation, const glm::vec3& scale)
	{
		transform.Location = location;
		transform.Rotation = rotation;
		transform.Scale = scale;
		transform.RotationQuat = glm::quat(rotation);
		Recalculate(transform);
	}

	void TransformSystem::SetTransform(TransformComponent& transform, const glm::mat4& matrix)
	{
		transform.Transform = matrix;
		Decompose(matrix, transform.Location, transform.RotationQuat, transform.Rotation, transform.Scale);
	}

	void TransformSystem::SetLocation(TransformComponent& transform, const glm::vec3& location)
	{
		transform.Location = location;
		Recalculate(transform);
	}

	void TransformSystem::SetRotation(TransformComponent& transform, const glm::vec3& rotation)
	{
		transform.Rotation = rotation;
		transform.RotationQuat = glm::quat(rotation);
		Recalculate(transform);
	}

	void TransformSystem::SetRotationQuat(TransformComponent& transform, const glm::quat& rotationQuat)
	{
		transform.RotationQuat = rotationQuat;
		transform.Rotation = glm::eulerAngles(rotationQuat);
		Recalculate(transform);
	}

	void TransformSystem::SetScale(TransformComponent& transform, const glm::vec3& scale)
	{
		transform.Scale = scale;
		Recalculate(transform);
	}

	void TransformSystem::UpdateLocalTransforms(entt::registry& registry)
	{
		auto view = registry.view<TransformComponent>();
		for (auto entity : view)
		{
			auto& transform = view.get<TransformComponent>(entity);
			Recalculate(transform);
		}
	}

	uint32_t TransformSystem::UpdateWorldTransforms(entt::registry& registry)
	{
		return Hierarchy::UpdateWorldTransforms(registry);
	}
}
