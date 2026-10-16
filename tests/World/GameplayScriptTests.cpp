#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
	using namespace World;

	void RunOk(const std::string& source, const char* chunk)
	{
		std::string error;
		if (!ScriptEngine::GetState().RunString(source, chunk, &error))
			throw std::runtime_error(std::string(chunk) + " failed: " + error);
	}
}

int main()
{
	try
	{
		WorldContext context;
		ScriptEngine::Init();
		Scene scene(context);
		ScriptEngine::SetActiveScene(&scene);

		Entity hero = Entity::CreateEntity(&scene, "Hero");
		auto& h = hero.AddComponent<HealthComponent>();
		h.CurrentHealth = 100.0f;
		h.MaxHealth = 100.0f;
		h.Armor = 50.0f;

		Entity bench = Entity::CreateEntity(&scene, "Bench");
		auto& so = bench.AddComponent<SmartObjectComponent>();
		so.SlotOffset = glm::vec3(1.0f, 0.0f, 2.0f);

		uint32_t heroId = static_cast<uint32_t>(static_cast<entt::entity>(hero));
		uint32_t benchId = static_cast<uint32_t>(static_cast<entt::entity>(bench));

		std::string setup = "HERO_ID = " + std::to_string(heroId) + "\n" +
		                    "BENCH_ID = " + std::to_string(benchId) + "\n";
		RunOk(setup, "SetupIds");

		const char* script = R"luau(
			-- Health API tests
			local info = Health.Get(HERO_ID)
			assert(info ~= nil, "Health.Get must return table")
			assert(info.CurrentHealth == 100, "Initial health should be 100")
			assert(info.IsDead == false, "Hero should not be dead")

			-- Damage test
			local dealt = Health.Damage(HERO_ID, 30.0, 0)
			assert(dealt > 0, "Damage dealt should be positive")
			local afterDmg = Health.Get(HERO_ID)
			assert(afterDmg.CurrentHealth < 100, "Health should decrease")

			-- Heal test
			local healed = Health.Heal(HERO_ID, 10.0)
			assert(healed == 10.0, "Healed amount mismatch")

			-- SmartObject API tests
			local state = SmartObject.GetState(BENCH_ID)
			assert(state == 0, "Initial slot state must be Available (0)")

			local claimed = SmartObject.Claim(BENCH_ID, HERO_ID)
			assert(claimed == true, "Claim must succeed")
			assert(SmartObject.GetState(BENCH_ID) == 1, "Slot state must be Claimed (1)")

			local used = SmartObject.Use(BENCH_ID, HERO_ID)
			assert(used == true, "Use must succeed")
			assert(SmartObject.GetState(BENCH_ID) == 2, "Slot state must be InUse (2)")

			local released = SmartObject.Release(BENCH_ID)
			assert(released == true, "Release must succeed")
			assert(SmartObject.GetState(BENCH_ID) == 3, "Slot state must be Cooldown (3)")
		)luau";
		RunOk(script, "GameplayScriptTest");

		std::cout << "World.GameplayScript: all checks passed\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "Fatal: " << e.what() << "\n";
		return 1;
	}
}
