#include "MovementSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Query.h"
#include "World/Scene/TransformSystem.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World
{
	void MovementSystem::Update(Scene& scene, Timestep dt)
	{
		const float delta = dt.GetSeconds();
		if (delta <= 0.0f)
			return;

		scene.Query<TransformComponent, const VelocityComponent>()
			.Each([delta](TransformComponent& transform, const VelocityComponent& vel) {
				if (glm::length2(vel.Linear) > 0.0f)
				{
					transform.Location += vel.Linear * delta;
				}
				if (glm::length2(vel.Angular) > 0.0f)
				{
					const glm::quat rotDelta = glm::quat(vel.Angular * delta);
					transform.RotationQuat = glm::normalize(rotDelta * transform.RotationQuat);
					transform.Rotation = glm::eulerAngles(transform.RotationQuat);
				}
				TransformSystem::Recalculate(transform);
			});
	}
}
