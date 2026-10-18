#include "wldpch.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Ai/VfxCommands.h"
#include "World/Renderer/ParticleSystem.h"
#include "World/Core/Log.h"

#include <iostream>
#include <cassert>
#include <map>

using namespace World;
using namespace World::Ai;

static void TestVfxAiCommandsRegistration()
{
	auto& reg = CommandRegistry::Get();
	assert(reg.HasCommand("vfx.list"));
	assert(reg.HasCommand("vfx.spawn"));
	assert(reg.HasCommand("vfx.stats"));
	assert(reg.HasCommand("vfx.preview"));
	assert(reg.HasCommand("vfx.validate"));

	std::cout << "[PASS] TestVfxAiCommandsRegistration\n";
}

static void TestVfxAiCommandsDispatch()
{
	auto& reg = CommandRegistry::Get();

	// 1. vfx.stats
	std::string resStats, errStats;
	bool okStats = reg.Dispatch("vfx.stats", {}, resStats, errStats);
	assert(okStats);
	assert(resStats.find("active_particles") != std::string::npos);

	// 2. vfx.spawn
	std::string resSpawn, errSpawn;
	std::map<std::string, std::string> spawnArgs = {
		{ "x", "10.0" },
		{ "y", "20.0" },
		{ "z", "30.0" },
		{ "count", "50" }
	};
	bool okSpawn = reg.Dispatch("vfx.spawn", spawnArgs, resSpawn, errSpawn);
	assert(okSpawn);
	assert(resSpawn.find("success") != std::string::npos);
	assert(GetGlobalParticleSystem().GetOneShots().size() > 0);

	// 3. vfx.preview
	std::string resPrev, errPrev;
	std::map<std::string, std::string> prevArgs = {
		{ "frames", "40" },
		{ "dt", "0.016" },
		{ "seed", "20261018" }
	};
	bool okPrev = reg.Dispatch("vfx.preview", prevArgs, resPrev, errPrev);
	assert(okPrev);
	assert(resPrev.find("deterministic") != std::string::npos);

	std::cout << "[PASS] TestVfxAiCommandsDispatch\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldVfxAiCommandsTests...\n";

	TestVfxAiCommandsRegistration();
	TestVfxAiCommandsDispatch();

	std::cout << "All WorldVfxAiCommandsTests passed successfully.\n";
	return 0;
}
