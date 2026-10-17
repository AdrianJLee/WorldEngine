#include "wldpch.h"
#include "World/Gameplay/Framework/DamageSystem.h"

#include <algorithm>

namespace World::Gameplay
{
	static std::function<void(const DeathEvent&)> s_DeathCallback = nullptr;
	static std::function<void(const DamageTakenEvent&)> s_DamageCallback = nullptr;
	static DamageCalculatorFn s_DamageCalculator = nullptr;

	void DamageSystem::SetDeathCallback(std::function<void(const DeathEvent&)> callback)
	{
		s_DeathCallback = std::move(callback);
	}

	void DamageSystem::SetDamageCallback(std::function<void(const DamageTakenEvent&)> callback)
	{
		s_DamageCallback = std::move(callback);
	}

	void DamageSystem::SetDamageCalculator(DamageCalculatorFn calculator)
	{
		s_DamageCalculator = std::move(calculator);
	}

	void DamageSystem::ResetDamageCalculator()
	{
		s_DamageCalculator = nullptr;
	}

	float DamageSystem::CalculateMitigatedDamage(float rawDamage, float armor, MitigationMode mode)
	{
		if (rawDamage <= 0.0f) return 0.0f;
		float safeArmor = std::max(0.0f, armor);

		switch (mode)
		{
		case MitigationMode::Flat:
			// 线性保底公式: 保证至少造成 1 点基础伤害
			return std::max(1.0f, rawDamage - safeArmor);

		case MitigationMode::Percentage:
		{
			// 百分比减伤: armor 为 0~100 减免百分比
			float pct = std::clamp(safeArmor, 0.0f, 100.0f) / 100.0f;
			return rawDamage * (1.0f - pct);
		}

		case MitigationMode::Hyperbolic:
		default:
			// 经典双曲递减公式
			return rawDamage * (100.0f / (100.0f + safeArmor));
		}
	}

	float DamageSystem::CalculateMitigatedDamage(float rawDamage, float armor, DamageType type)
	{
		if (rawDamage <= 0.0f) return 0.0f;
		if (type == DamageType::TrueDamage) return rawDamage;
		return CalculateMitigatedDamage(rawDamage, armor, MitigationMode::Hyperbolic);
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
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
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
		auto& reg = const_cast<entt::registry&>(static_cast<const Scene&>(scene).GetRegistry());
		if (!reg.valid(event.Target) || event.Amount <= 0.0f)
			return 0.0f;

		auto* health = reg.try_get<HealthComponent>(event.Target);
		if (!health || health->IsDead != 0)
			return 0.0f;

		// 无敌帧检测 (除非指定了 IgnoreInvulnerability 标志)
		const bool bypassInvuln = (event.Flags & DamageFlags::IgnoreInvulnerability) != 0;
		if (!bypassInvuln && health->InvulnerableTimer > 0.0f)
			return 0.0f;

		// 减伤求解: 若注册了自定义求解器则优先调用; 否则按组件的 MitigationMode 求解
		float effectiveDamage = 0.0f;
		const bool bypassArmor = (event.Flags & DamageFlags::IgnoreArmor) != 0 || event.Type == DamageType::TrueDamage;

		if (bypassArmor)
		{
			effectiveDamage = event.Amount;
		}
		else if (s_DamageCalculator)
		{
			effectiveDamage = s_DamageCalculator(event, *health);
		}
		else
		{
			auto mode = static_cast<MitigationMode>(health->MitigationMode);
			effectiveDamage = CalculateMitigatedDamage(event.Amount, health->Armor, mode);
		}

		effectiveDamage = std::max(0.0f, effectiveDamage);
		float damageRemaining = effectiveDamage;
		float shieldAbsorbed = 0.0f;

		// 护盾吸收 (除非指定了 IgnoreShield 标志)
		const bool bypassShield = (event.Flags & DamageFlags::IgnoreShield) != 0;
		if (!bypassShield && health->Shield > 0.0f)
		{
			if (health->Shield >= damageRemaining)
			{
				health->Shield -= damageRemaining;
				shieldAbsorbed = damageRemaining;
				damageRemaining = 0.0f;
			}
			else
			{
				shieldAbsorbed = health->Shield;
				damageRemaining -= health->Shield;
				health->Shield = 0.0f;
			}
		}

		float healthDamage = damageRemaining;
		health->CurrentHealth = std::max(0.0f, health->CurrentHealth - healthDamage);

		// 参数化无敌时间: 仅当 InvulnerableDuration > 0 时才启动无敌帧
		if (health->InvulnerableDuration > 0.0f)
		{
			health->InvulnerableTimer = health->InvulnerableDuration;
		}

		// 广播纯净受击事件 (解耦 FlashWhite 等硬编码表现)
		if (s_DamageCallback)
		{
			DamageTakenEvent takenEv;
			takenEv.Target = event.Target;
			takenEv.Source = event.Source;
			takenEv.RawDamage = event.Amount;
			takenEv.MitigatedDamage = effectiveDamage;
			takenEv.ShieldAbsorbed = shieldAbsorbed;
			takenEv.HealthDamage = healthDamage;
			takenEv.Type = event.Type;
			takenEv.Tag = event.Tag;
			takenEv.HitPoint = event.HitPoint;
			takenEv.HitNormal = event.HitNormal;
			takenEv.Flags = event.Flags;
			s_DamageCallback(takenEv);
		}

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