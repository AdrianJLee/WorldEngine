#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>

namespace World::Gameplay
{
	enum class DamageType : uint8_t
	{
		Physical = 0,
		Fire,
		TrueDamage
	};

	struct DamageEvent
	{
		entt::entity Source = entt::null;
		entt::entity Target = entt::null;
		float Amount = 0.0f;
		DamageType Type = DamageType::Physical;
		glm::vec3 HitPoint { 0.0f };
		glm::vec3 HitNormal { 0.0f, 1.0f, 0.0f };
	};

	struct DeathEvent
	{
		entt::entity Entity = entt::null;
		entt::entity Killer = entt::null;
	};

	class WLD_API DamageSystem
	{
	public:
		static float ApplyDamage(Scene& scene, const DamageEvent& event);
		static float Heal(HealthComponent& health, float amount);
		static void Update(Scene& scene, float dt);
		static void SetDeathCallback(std::function<void(const DeathEvent&)> callback);
		static float CalculateMitigatedDamage(float rawDamage, float armor, DamageType type);
	};
}
