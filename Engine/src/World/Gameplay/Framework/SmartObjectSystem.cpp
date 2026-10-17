#include "wldpch.h"
#include "World/Gameplay/Framework/SmartObjectSystem.h"
#include "World/Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace World::Gameplay
{
	bool SmartObjectSystem::ClaimSlot(Scene& scene, entt::entity smartObject, entt::entity claimer)
	{
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
		auto* so = reg.try_get<SmartObjectComponent>(smartObject);
		if (!so || so->Enabled == 0 || so->SlotState != SmartObjectSlotState::Available)
			return false;

		so->SlotState = SmartObjectSlotState::Claimed;
		so->ClaimedByEntity = static_cast<uint32_t>(claimer);
		return true;
	}

	bool SmartObjectSystem::UseSlot(Scene& scene, entt::entity smartObject, entt::entity user)
	{
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
		auto* so = reg.try_get<SmartObjectComponent>(smartObject);
		if (!so || so->Enabled == 0)
			return false;

		uint32_t userId = static_cast<uint32_t>(user);
		if (so->SlotState == SmartObjectSlotState::Claimed && so->ClaimedByEntity == userId)
		{
			so->SlotState = SmartObjectSlotState::InUse;
			return true;
		}
		if (so->SlotState == SmartObjectSlotState::Available)
		{
			so->SlotState = SmartObjectSlotState::InUse;
			so->ClaimedByEntity = userId;
			return true;
		}

		return false;
	}

	bool SmartObjectSystem::ReleaseSlot(Scene& scene, entt::entity smartObject)
	{
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
		auto* so = reg.try_get<SmartObjectComponent>(smartObject);
		if (!so || so->SlotState == SmartObjectSlotState::Available)
			return false;

		so->ClaimedByEntity = 0;
		if (so->CooldownDuration > 0.0f)
		{
			so->SlotState = SmartObjectSlotState::Cooldown;
			so->CooldownTimer = so->CooldownDuration;
		}
		else
		{
			so->SlotState = SmartObjectSlotState::Available;
			so->CooldownTimer = 0.0f;
		}
		return true;
	}

	glm::vec3 SmartObjectSystem::GetSlotWorldPosition(const Scene& scene, entt::entity smartObject)
	{
		const auto& reg = scene.GetRegistry();
		const auto* so = reg.try_get<SmartObjectComponent>(smartObject);
		if (!so) return glm::vec3(0.0f);

		glm::vec3 worldPos(0.0f);
		if (const auto* tr = reg.try_get<TransformComponent>(smartObject))
		{
			worldPos = tr->Location + tr->Rotation * so->SlotOffset;
		}
		else
		{
			worldPos = so->SlotOffset;
		}
		return worldPos;
	}

	entt::entity SmartObjectSystem::FindNearestAvailableSlot(Scene& scene, const glm::vec3& searchPos,
	                                                        uint32_t activityTag, float maxRange)
	{
		const auto& reg = static_cast<const Scene&>(scene).GetRegistry();
		entt::entity bestEntity = entt::null;
		float bestDistSq = maxRange * maxRange;

		for (auto entity : reg.view<SmartObjectComponent>())
		{
			const auto& so = reg.get<SmartObjectComponent>(entity);
			if (so.Enabled == 0 || so.SlotState != SmartObjectSlotState::Available)
				continue;
			if (activityTag != 0 && so.ActivityTag != activityTag)
				continue;

			glm::vec3 slotPos = GetSlotWorldPosition(scene, entity);
			glm::vec3 diff = slotPos - searchPos;
			float distSq = glm::dot(diff, diff);
			if (distSq < bestDistSq)
			{
				bestDistSq = distSq;
				bestEntity = entity;
			}
		}

		return bestEntity;
	}

	void SmartObjectSystem::Update(Scene& scene, float dt)
	{
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
		for (auto entity : reg.view<SmartObjectComponent>())
		{
			auto& so = reg.get<SmartObjectComponent>(entity);
			if (so.SlotState == SmartObjectSlotState::Cooldown)
			{
				so.CooldownTimer -= dt;
				if (so.CooldownTimer <= 0.0f)
				{
					so.SlotState = SmartObjectSlotState::Available;
					so.CooldownTimer = 0.0f;
				}
			}
		}
	}
}
