#include "World/Gameplay/Framework/DamageSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/Log.h"
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

	std::cout << "[PASS] TestShieldAbsorptionAndDeath\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldHealthDamageTests...\n";

	TestArmorMitigationAndDamage();
	TestShieldAbsorptionAndDeath();

	std::cout << "All WorldHealthDamageTests passed successfully.\n";
	return 0;
}
