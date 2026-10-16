#include "wldpch.h"
#include "World/Gameplay/Framework/DamageSystem.h"
#include "World/Renderer/PostProcess/PostProcessPasses.h"

#include <algorithm>

namespace World::Gameplay
{
	static std::function<void(const DeathEvent&)> s_DeathCallback = nullptr;

	void DamageSystem::SetDeathCallback(std::function<void(const DeathEvent&)> callback)
	{
		s_DeathCallback = std::move(callback);
	}

	float DamageSystem::CalculateMitigatedDamage(float rawDamage, float armor, DamageType type)
	{
		if (rawDamage <= 0.0f) return 0.0f;
		if (type == DamageType::TrueDamage) return rawDamage;
		float safeArmor = std::max(0.0f, armor);
		return rawDamage * (100.0f / (100.0f + safeArmor));
	}

	float DamageSystem::Heal(HealthComponent& health, float amount)
	{
		if (amount <= 0.0f || health.IsDead != 0) return 0.0f;
		float prev = health.CurrentHealth;
		health.CurrentHealth = std::min(health.MaxHealth, health.CurrentHealth + amount);
		return health.CurrentHealth - prev;
	}

	void DamageSystem::Update(Scene& scene, float dt)
	{
		auto& reg = scene.GetRegistry();
		for (auto entity : reg.view<HealthComponent>())
		{
			auto& health = reg.get<HealthComponent>(entity);
			if (health.InvulnerableTimer > 0.0f)
			{
				health.InvulnerableTimer = std::max(0.0f, health.InvulnerableTimer - dt);
			}
		}
	}

	float DamageSystem::ApplyDamage(Scene& scene, const DamageEvent& event)
	{
		auto& reg = scene.GetRegistry();
		if (!reg.valid(event.Target) || event.Amount <= 0.0f)
			return 0.0f;

		auto* health = reg.try_get<HealthComponent>(event.Target);
		if (!health || health->IsDead != 0)
			return 0.0f;

		// I-Frame check
		if (health->InvulnerableTimer > 0.0f)
			return 0.0f;

		float effectiveDamage = CalculateMitigatedDamage(event.Amount, health->Armor, event.Type);
		float damageRemaining = effectiveDamage;

		// Shield absorbs first
		if (health->Shield > 0.0f)
		{
			if (health->Shield >= damageRemaining)
			{
				health->Shield -= damageRemaining;
				damageRemaining = 0.0f;
			}
			else
			{
				damageRemaining -= health->Shield;
				health->Shield = 0.0f;
			}
		}

		health->CurrentHealth = std::max(0.0f, health->CurrentHealth - damageRemaining);

		// Trigger I-Frames
		health->InvulnerableTimer = health->InvulnerableDuration;

		// Trigger visual hit reaction
		PostProcessPipeline::FlashWhite(scene, 0.8f, 6.0f);

		if (health->CurrentHealth <= 0.0f)
		{
			health->IsDead = 1;
			if (s_DeathCallback)
			{
				s_DeathCallback(DeathEvent{ event.Target, event.Source });
			}
		}

		return effectiveDamage;
	}
}
