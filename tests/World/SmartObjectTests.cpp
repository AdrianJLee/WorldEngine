#include "World/Gameplay/Framework/SmartObjectSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"

#include <cassert>
#include <iostream>

using namespace World;
using namespace World::Gameplay;

static void TestSmartObjectClaimUseRelease()
{
	WorldContext context;
	Scene scene(context);

	Entity bench = Entity::CreateEntity(&scene, "ParkBench");
	auto& so = bench.AddComponent<SmartObjectComponent>();
	so.SlotOffset = glm::vec3(0.0f, 0.5f, 0.0f);
	so.CooldownDuration = 2.0f;

	Entity npc1 = Entity::CreateEntity(&scene, "NPC1");
	Entity npc2 = Entity::CreateEntity(&scene, "NPC2");

	bool claimed1 = SmartObjectSystem::ClaimSlot(scene, (entt::entity)bench, (entt::entity)npc1);
	assert(claimed1);
	assert(so.SlotState == SmartObjectSlotState::Claimed);
	assert(so.ClaimedByEntity == (uint32_t)(entt::entity)npc1);

	bool claimed2 = SmartObjectSystem::ClaimSlot(scene, (entt::entity)bench, (entt::entity)npc2);
	assert(!claimed2);

	bool used1 = SmartObjectSystem::UseSlot(scene, (entt::entity)bench, (entt::entity)npc1);
	assert(used1);
	assert(so.SlotState == SmartObjectSlotState::InUse);

	bool released = SmartObjectSystem::ReleaseSlot(scene, (entt::entity)bench);
	assert(released);
	assert(so.SlotState == SmartObjectSlotState::Cooldown);
	assert(so.CooldownTimer == 2.0f);

	SmartObjectSystem::Update(scene, 2.1f);
	assert(so.SlotState == SmartObjectSlotState::Available);
	assert(so.CooldownTimer == 0.0f);

	std::cout << "[PASS] TestSmartObjectClaimUseRelease\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldSmartObjectTests...\n";

	TestSmartObjectClaimUseRelease();

	std::cout << "All WorldSmartObjectTests passed successfully.\n";
	return 0;
}
