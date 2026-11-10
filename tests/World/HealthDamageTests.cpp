#include "World/Gameplay/Framework/DamageSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/Log.h"
#include "World/Core/StringPool.h"
#include "World/Core/WorldContext.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;
using namespace World::Gameplay;

static void TestArmorMitigationAndDamage()
{
	WorldContext context;
	Scene scene(context);

	Entity target = Entity::CreateEntity(&scene, "Enemy");
	auto& health = target.AddComponent<HealthComponent>();
	health.CurrentHealth = 100.0f;
	health.MaxHealth = 100.0f;
	health.Armor = 100.0f;

	DamageEvent dmg;
	dmg.Target = static_cast<entt::entity>(target);
	dmg.Amount = 50.0f;
	dmg.Type = DamageType::Physical;

	float dealt = DamageSystem::ApplyDamage(scene, dmg);
	assert(std::abs(dealt - 25.0f) < 1e-4f);
	assert(std::abs(health.CurrentHealth - 75.0f) < 1e-4f);

	DamageEvent rapid;
	rapid.Target = static_cast<entt::entity>(target);
	rapid.Amount = 50.0f;
	float secondDealt = DamageSystem::ApplyDamage(scene, rapid);
	assert(secondDealt == 0.0f);
	assert(std::abs(health.CurrentHealth - 75.0f) < 1e-4f);

	DamageSystem::Update(scene, 0.6f);
	assert(health.InvulnerableTimer == 0.0f);

	DamageEvent trueDmg;
	trueDmg.Target = static_cast<entt::entity>(target);
	trueDmg.Amount = 25.0f;
	trueDmg.Type = DamageType::TrueDamage;

	float trueDealt = DamageSystem::ApplyDamage(scene, trueDmg);
	assert(std::abs(trueDealt - 25.0f) < 1e-4f);
	assert(std::abs(health.CurrentHealth - 50.0f) < 1e-4f);

	float healed = DamageSystem::Heal(health, 30.0f);
	assert(std::abs(healed - 30.0f) < 1e-4f);
	assert(std::abs(health.CurrentHealth - 80.0f) < 1e-4f);

	std::cout << "[PASS] TestArmorMitigationAndDamage\n";
}

static void TestShieldAbsorptionAndDeath()
{
	WorldContext context;
	Scene scene(context);

	bool deathTriggered = false;
	DamageSystem::SetDeathCallback([&](const DeathEvent& ev) {
		deathTriggered = true;
	});

	Entity boss = Entity::CreateEntity(&scene, "Boss");
	auto& health = boss.AddComponent<HealthComponent>();
	health.CurrentHealth = 50.0f;
	health.MaxHealth = 50.0f;
	health.Shield = 30.0f;
	health.MaxShield = 30.0f;
	health.Armor = 0.0f;
	health.InvulnerableDuration = 0.0f;

	DamageEvent d1;
	d1.Target = static_cast<entt::entity>(boss);
	d1.Amount = 40.0f;
	DamageSystem::ApplyDamage(scene, d1);
	assert(health.Shield == 0.0f);
	assert(std::abs(health.CurrentHealth - 40.0f) < 1e-4f);

	DamageEvent d2;
	d2.Target = static_cast<entt::entity>(boss);
	d2.Amount = 50.0f;
	DamageSystem::ApplyDamage(scene, d2);
	assert(health.CurrentHealth == 0.0f);
	assert(health.IsDead == 1);
	assert(deathTriggered);

	// Reset death callback to avoid leaking
	DamageSystem::SetDeathCallback(nullptr);
	std::cout << "[PASS] TestShieldAbsorptionAndDeath\n";
}

static void TestMitigationModes()
{
	WorldContext context;
	Scene scene(context);

	// 1. Flat mode (RPG / D&D: max(1.0, Damage - Armor))
	{
		Entity e = Entity::CreateEntity(&scene, "FlatChar");
		auto& h = e.AddComponent<HealthComponent>();
		h.CurrentHealth = 100.0f;
		h.Armor = 20.0f;
		h.MitigationMode = static_cast<uint8_t>(MitigationMode::Flat);
		h.InvulnerableDuration = 0.0f;

		DamageEvent dmg;
		dmg.Target = static_cast<entt::entity>(e);
		dmg.Amount = 50.0f;
		float dealt = DamageSystem::ApplyDamage(scene, dmg);
		assert(std::abs(dealt - 30.0f) < 1e-4f);
		assert(std::abs(h.CurrentHealth - 70.0f) < 1e-4f);

		// High armor exceeds damage -> guaranteed 1 minimum damage
		dmg.Amount = 15.0f;
		float minDealt = DamageSystem::ApplyDamage(scene, dmg);
		assert(std::abs(minDealt - 1.0f) < 1e-4f);
		assert(std::abs(h.CurrentHealth - 69.0f) < 1e-4f);
	}

	// 2. Percentage mode (Armor as 0..100% reduction)
	{
		Entity e = Entity::CreateEntity(&scene, "PctChar");
		auto& h = e.AddComponent<HealthComponent>();
		h.CurrentHealth = 100.0f;
		h.Armor = 40.0f; // 40% reduction
		h.MitigationMode = static_cast<uint8_t>(MitigationMode::Percentage);
		h.InvulnerableDuration = 0.0f;

		DamageEvent dmg;
		dmg.Target = static_cast<entt::entity>(e);
		dmg.Amount = 100.0f;
		float dealt = DamageSystem::ApplyDamage(scene, dmg);
		assert(std::abs(dealt - 60.0f) < 1e-4f);
		assert(std::abs(h.CurrentHealth - 40.0f) < 1e-4f);
	}

	std::cout << "[PASS] TestMitigationModes\n";
}

static void TestCustomDamageCalculator()
{
	WorldContext context;
	Scene scene(context);

	Entity e = Entity::CreateEntity(&scene, "CustomCalcEntity");
	auto& h = e.AddComponent<HealthComponent>();
	h.CurrentHealth = 100.0f;
	h.Armor = 999.0f; // Normally blocks almost all
	h.InvulnerableDuration = 0.0f;

	// Injected custom formula: static 75% flat damage multiplier
	DamageSystem::SetDamageCalculator([](const DamageEvent& ev, const HealthComponent& target) {
		return ev.Amount * 0.75f;
	});

	DamageEvent dmg;
	dmg.Target = static_cast<entt::entity>(e);
	dmg.Amount = 40.0f;
	float dealt = DamageSystem::ApplyDamage(scene, dmg);
	assert(std::abs(dealt - 30.0f) < 1e-4f);
	assert(std::abs(h.CurrentHealth - 70.0f) < 1e-4f);

	DamageSystem::ResetDamageCalculator();

	// After reset, reverts to standard mitigation
	float normalDealt = DamageSystem::ApplyDamage(scene, dmg);
	assert(normalDealt < 5.0f); // 999 armor reduces 40 damage to ~3.6

	std::cout << "[PASS] TestCustomDamageCalculator\n";
}

static void TestDamageBypassFlagsAndTags()
{
	WorldContext context;
	Scene scene(context);

	Entity e = Entity::CreateEntity(&scene, "BypassEntity");
	auto& h = e.AddComponent<HealthComponent>();
	h.CurrentHealth = 100.0f;
	h.Shield = 50.0f;
	h.MaxShield = 50.0f;
	h.Armor = 100.0f;
	h.InvulnerableDuration = 1.0f;

	// 1. IgnoreShield flag: bypass shield and hit health directly
	DamageEvent d1;
	d1.Target = static_cast<entt::entity>(e);
	d1.Amount = 50.0f;
	d1.Flags = DamageFlags::IgnoreShield | DamageFlags::IgnoreArmor;
	d1.Tag = StringPool::Get().InternName("Damage.DirectPoison");

	float dealt1 = DamageSystem::ApplyDamage(scene, d1);
	assert(std::abs(dealt1 - 50.0f) < 1e-4f);
	assert(h.Shield == 50.0f); // Shield was completely bypassed
	assert(std::abs(h.CurrentHealth - 50.0f) < 1e-4f);
	assert(h.InvulnerableTimer > 0.0f); // I-Frames active

	// 2. IgnoreInvulnerability flag: hits even while invulnerable
	DamageEvent d2;
	d2.Target = static_cast<entt::entity>(e);
	d2.Amount = 20.0f;
	d2.Flags = DamageFlags::IgnoreInvulnerability | DamageFlags::IgnoreArmor;
	float dealt2 = DamageSystem::ApplyDamage(scene, d2);
	assert(std::abs(dealt2 - 20.0f) < 1e-4f);
	assert(h.Shield == 30.0f); // Shield absorbed this hit

	std::cout << "[PASS] TestDamageBypassFlagsAndTags\n";
}

static void TestDamageTakenCallback()
{
	WorldContext context;
	Scene scene(context);

	DamageTakenEvent receivedEvent{};
	bool callbackFired = false;

	DamageSystem::SetDamageCallback([&](const DamageTakenEvent& ev) {
		receivedEvent = ev;
		callbackFired = true;
	});

	Entity attacker = Entity::CreateEntity(&scene, "Attacker");
	Entity victim = Entity::CreateEntity(&scene, "Victim");
	auto& h = victim.AddComponent<HealthComponent>();
	h.CurrentHealth = 100.0f;
	h.Shield = 20.0f;
	h.Armor = 0.0f;
	h.InvulnerableDuration = 0.0f;

	DamageEvent dmg;
	dmg.Source = static_cast<entt::entity>(attacker);
	dmg.Target = static_cast<entt::entity>(victim);
	dmg.Amount = 50.0f;
	dmg.Tag = StringPool::Get().InternName("Damage.Slash");
	dmg.HitPoint = glm::vec3(1.0f, 2.0f, 3.0f);
	dmg.HitNormal = glm::vec3(0.0f, 1.0f, 0.0f);

	DamageSystem::ApplyDamage(scene, dmg);

	assert(callbackFired);
	assert(receivedEvent.Target == static_cast<entt::entity>(victim));
	assert(receivedEvent.Source == static_cast<entt::entity>(attacker));
	assert(receivedEvent.RawDamage == 50.0f);
	assert(receivedEvent.MitigatedDamage == 50.0f);
	assert(receivedEvent.ShieldAbsorbed == 20.0f);
	assert(receivedEvent.HealthDamage == 30.0f);
	assert(receivedEvent.Tag == StringPool::Get().InternName("Damage.Slash"));
	assert(receivedEvent.HitPoint.x == 1.0f);

	DamageSystem::SetDamageCallback(nullptr);
	std::cout << "[PASS] TestDamageTakenCallback\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldHealthDamageTests...\n";

	TestArmorMitigationAndDamage();
	TestShieldAbsorptionAndDeath();
	TestMitigationModes();
	TestCustomDamageCalculator();
	TestDamageBypassFlagsAndTags();
	TestDamageTakenCallback();

	std::cout << "All WorldHealthDamageTests passed successfully.\n";
	return 0;
}