#include "wldpch.h"
#include "World/Renderer/VirtualCameraSystem.h"
#include "World/Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace World
{
	static glm::vec3 s_CurrentVelocity { 0.0f };
	static float s_GlobalTime = 0.0f;

	glm::vec3 VirtualCameraSystem::SmoothDamp(const glm::vec3& current, const glm::vec3& target,
	                                         glm::vec3& currentVelocity, float smoothTime, float dt)
	{
		smoothTime = std::max(0.0001f, smoothTime);
		float omega = 2.0f / smoothTime;
		float x = omega * dt;
		float exp = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
		glm::vec3 change = current - target;
		glm::vec3 temp = (currentVelocity + omega * change) * dt;
		currentVelocity = (currentVelocity - omega * temp) * exp;
		glm::vec3 output = target + (change + temp) * exp;
		return output;
	}

	void VirtualCameraSystem::AddTrauma(VirtualCameraComponent& vcam, float trauma)
	{
		vcam.ScreenShakeTrauma = glm::clamp(vcam.ScreenShakeTrauma + trauma, 0.0f, 1.0f);
	}

	glm::vec3 VirtualCameraSystem::EvaluateShakeOffset(const VirtualCameraComponent& vcam, float time)
	{
		if (vcam.ScreenShakeTrauma <= 0.001f)
			return glm::vec3(0.0f);

		float shake = vcam.ScreenShakeTrauma * vcam.ScreenShakeTrauma;
		float f = vcam.ScreenShakeFrequency;
		float ox = std::sin(time * f * 1.11f) * std::cos(time * f * 0.73f);
		float oy = std::cos(time * f * 1.37f) * std::sin(time * f * 0.91f);
		float oz = std::sin(time * f * 0.83f);
		return glm::vec3(ox, oy, oz * 0.2f) * shake * 2.8f;
	}

	glm::vec2 VirtualCameraSystem::ApplyDeadZone(const glm::vec2& currentOffset, const glm::vec2& deadZoneHalfExtents)
	{
		glm::vec2 result(0.0f);
		if (std::abs(currentOffset.x) > deadZoneHalfExtents.x)
		{
			result.x = currentOffset.x > 0.0f ? (currentOffset.x - deadZoneHalfExtents.x)
			                                  : (currentOffset.x + deadZoneHalfExtents.x);
		}
		if (std::abs(currentOffset.y) > deadZoneHalfExtents.y)
		{
			result.y = currentOffset.y > 0.0f ? (currentOffset.y - deadZoneHalfExtents.y)
			                                  : (currentOffset.y + deadZoneHalfExtents.y);
		}
		return result;
	}

	void VirtualCameraSystem::Update(Scene& scene, float dt)
	{
		s_GlobalTime += dt;
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());

		entt::entity bestVcamEntity = entt::null;
		int32_t bestPriority = std::numeric_limits<int32_t>::min();

		for (auto entity : reg.view<VirtualCameraComponent>())
		{
			auto& vcam = reg.get<VirtualCameraComponent>(entity);
			if ((vcam.Flags & VirtualCameraFlags::Active) != 0 && vcam.Priority > bestPriority)
			{
				bestPriority = vcam.Priority;
				bestVcamEntity = entity;
			}
		}

		if (bestVcamEntity == entt::null)
		{
			for (auto entity : reg.view<VirtualCameraComponent>())
			{
				auto& vcam = reg.get<VirtualCameraComponent>(entity);
				if (vcam.Priority > bestPriority)
				{
					bestPriority = vcam.Priority;
					bestVcamEntity = entity;
				}
			}
		}

		if (bestVcamEntity == entt::null)
			return;

		auto& vcam = reg.get<VirtualCameraComponent>(bestVcamEntity);

		// Find target position
		glm::vec3 targetPos(0.0f);
		glm::vec3 targetVel(0.0f);
		bool hasTarget = false;

		entt::entity tEnt = static_cast<entt::entity>(vcam.FollowTarget);
		if (reg.valid(tEnt))
		{
			if (const auto* tr = reg.try_get<TransformComponent>(tEnt))
			{
				targetPos = tr->Location;
				hasTarget = true;
			}
			if (const auto* vel = reg.try_get<VelocityComponent>(tEnt))
			{
				targetVel = vel->Linear;
			}
			else if (const auto* kcc = reg.try_get<CharacterControllerComponent>(tEnt))
			{
				targetVel = kcc->Velocity;
			}
		}

		// Decay trauma
		vcam.ScreenShakeTrauma = std::max(0.0f, vcam.ScreenShakeTrauma - vcam.ScreenShakeDecay * dt);
		glm::vec3 shakeOffset = EvaluateShakeOffset(vcam, s_GlobalTime);

		// Drive CameraComponent
		for (auto camEntity : reg.view<CameraComponent, TransformComponent>())
		{
			auto& camTr = reg.get<TransformComponent>(camEntity);
			glm::vec3 currentCamPos = camTr.Location;

			glm::vec3 desiredPos = currentCamPos;
			if (hasTarget)
			{
				glm::vec3 idealTarget = targetPos + vcam.FollowOffset + targetVel * vcam.LookaheadTime;
				desiredPos = SmoothDamp(currentCamPos, idealTarget, s_CurrentVelocity, vcam.Damping.x, dt);
			}
			desiredPos += shakeOffset;

			if ((vcam.Flags & VirtualCameraFlags::LockZ) != 0)
			{
				desiredPos.z = vcam.FollowOffset.z;
			}

			camTr.SetLocation(desiredPos);
			break;
		}
	}
}
