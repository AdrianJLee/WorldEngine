#include "World/Asset/ProjectManifest.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Physics/PhysicsQuery.h"
#include "World/Physics/PhysicsSettings.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	namespace fs = std::filesystem;
	using namespace World;
	using namespace World::Asset;
	using namespace World::Physics;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
		}
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void TestDefaultPresets()
	{
		CollisionChannelConfig cfg = CollisionChannelConfig::Default();
		CHECK(cfg.IsDefault());

		// Presets: 0=Default, 1=Static, 2=Pawn, 3=Visibility, 4=Projectile, 5=Trigger, 6=Water, 7=Vehicle
		CHECK(cfg.GetChannelName(0) == "Default");
		CHECK(cfg.GetChannelName(1) == "Static");
		CHECK(cfg.GetChannelName(2) == "Pawn");
		CHECK(cfg.GetChannelName(3) == "Visibility");
		CHECK(cfg.GetChannelName(4) == "Projectile");
		CHECK(cfg.GetChannelName(5) == "Trigger");
		CHECK(cfg.GetChannelName(6) == "Water");
		CHECK(cfg.GetChannelName(7) == "Vehicle");

		for (size_t i = 8; i < CollisionChannelConfig::kMaxChannels; ++i)
		{
			CHECK(cfg.GetChannelName(static_cast<uint8_t>(i)).empty());
		}

		CHECK(cfg.FindChannelIndex("Default") == 0);
		CHECK(cfg.FindChannelIndex("Static") == 1);
		CHECK(cfg.FindChannelIndex("Pawn") == 2);
		CHECK(cfg.FindChannelIndex("Visibility") == 3);
		CHECK(cfg.FindChannelIndex("Projectile") == 4);
		CHECK(cfg.FindChannelIndex("Trigger") == 5);
		CHECK(cfg.FindChannelIndex("Water") == 6);
		CHECK(cfg.FindChannelIndex("Vehicle") == 7);

		// Trigger (5) does not collide with Static (1) by default
		CHECK(!cfg.GetCollision(5, 1));
		CHECK(!cfg.GetCollision(1, 5));

		// Other standard channels collide with each other by default
		CHECK(cfg.GetCollision(0, 1));
		CHECK(cfg.GetCollision(1, 0));
		CHECK(cfg.GetCollision(2, 4));
		CHECK(cfg.GetCollision(4, 2));
		CHECK(cfg.GetCollision(2, 2));

		std::cout << "[PASS] TestDefaultPresets\n";
	}

	void TestSymmetryInvariant()
	{
		CollisionChannelConfig cfg = CollisionChannelConfig::Default();

		// Explicit pair testing
		const std::pair<uint8_t, uint8_t> testPairs[] = {
			{ 0, 7 }, { 2, 3 }, { 4, 6 }, { 1, 5 }, { 7, 7 }, { 10, 20 }
		};

		for (const auto& pair : testPairs)
		{
			cfg.SetCollision(pair.first, pair.second, false);
			CHECK(!cfg.GetCollision(pair.first, pair.second));
			CHECK(!cfg.GetCollision(pair.second, pair.first));

			cfg.SetCollision(pair.first, pair.second, true);
			CHECK(cfg.GetCollision(pair.first, pair.second));
			CHECK(cfg.GetCollision(pair.second, pair.first));
		}

		// Complex pattern test to guarantee symmetry holds across all pairs
		for (uint8_t i = 0; i < static_cast<uint8_t>(CollisionChannelConfig::kMaxChannels); ++i)
		{
			for (uint8_t j = 0; j < static_cast<uint8_t>(CollisionChannelConfig::kMaxChannels); ++j)
			{
				bool shouldCollide = ((i + j * 3) % 2) == 0;
				cfg.SetCollision(i, j, shouldCollide);
			}
		}

		for (uint8_t i = 0; i < static_cast<uint8_t>(CollisionChannelConfig::kMaxChannels); ++i)
		{
			for (uint8_t j = 0; j < static_cast<uint8_t>(CollisionChannelConfig::kMaxChannels); ++j)
			{
				CHECK(cfg.GetCollision(i, j) == cfg.GetCollision(j, i));
			}
		}

		std::cout << "[PASS] TestSymmetryInvariant\n";
	}

	void TestChannelRenamingAndLookup()
	{
		CollisionChannelConfig cfg;

		// Add new channel
		CHECK(cfg.SetChannelName(8, "Sensors"));
		CHECK(cfg.GetChannelName(8) == "Sensors");
		CHECK(cfg.FindChannelIndex("Sensors") == 8);

		// Rename existing channel
		CHECK(cfg.SetChannelName(2, "PlayerCharacter"));
		CHECK(cfg.GetChannelName(2) == "PlayerCharacter");
		CHECK(cfg.FindChannelIndex("PlayerCharacter") == 2);
		CHECK(cfg.FindChannelIndex("Pawn") == -1);

		// Edge cases: lookup non-existent and empty
		CHECK(cfg.FindChannelIndex("NonExistent") == -1);
		CHECK(cfg.FindChannelIndex("") == -1);

		// Out of bounds
		CHECK(!cfg.SetChannelName(32, "Invalid"));
		CHECK(cfg.GetChannelName(32).empty());
		CHECK(cfg.GetChannelName(99).empty());

		std::cout << "[PASS] TestChannelRenamingAndLookup\n";
	}

	void TestMaskComputation()
	{
		CollisionChannelConfig cfg = CollisionChannelConfig::Default();

		// Default trigger mask (channel 5) should have bit 1 cleared
		uint32_t triggerMask = cfg.GetMask(5);
		CHECK((triggerMask & (1u << 1)) == 0);

		// Channel 1 (Static) mask should have bit 5 cleared
		uint32_t staticMask = cfg.GetMask(1);
		CHECK((staticMask & (1u << 5)) == 0);

		// Channel 0 (Default) mask has all bits set
		CHECK(cfg.GetMask(0) == 0xFFFFFFFF);

		// Modify collision and check masks
		cfg.SetCollision(2, 4, false);
		CHECK((cfg.GetMask(2) & (1u << 4)) == 0);
		CHECK((cfg.GetMask(4) & (1u << 2)) == 0);

		cfg.SetCollision(2, 4, true);
		CHECK((cfg.GetMask(2) & (1u << 4)) != 0);
		CHECK((cfg.GetMask(4) & (1u << 2)) != 0);

		// Out of bounds mask returns 0xFFFFFFFF
		CHECK(cfg.GetMask(32) == 0xFFFFFFFF);
		CHECK(cfg.GetMask(100) == 0xFFFFFFFF);

		// Out of bounds collision setting/getting does not crash
		cfg.SetCollision(32, 1, false);
		CHECK(!cfg.GetCollision(32, 1));
		CHECK(!cfg.GetCollision(1, 32));

		std::cout << "[PASS] TestMaskComputation\n";
	}

	void TestPhysicsSettingsIntegration()
	{
		PhysicsSettings::ResetToDefaults();
		CHECK(PhysicsSettings::GetChannels().IsDefault());

		CHECK(PhysicsSettings::GetChannelName(0) == "Default");
		CHECK(PhysicsSettings::GetChannelName(1) == "Static");
		CHECK(PhysicsSettings::FindChannelIndex("Trigger") == 5);
		CHECK(!PhysicsSettings::GetChannelCollision(5, 1));
		CHECK(PhysicsSettings::GetChannelCollision(0, 1));

		// Mutate through PhysicsSettings
		CHECK(PhysicsSettings::SetChannelName(10, "NPC"));
		CHECK(PhysicsSettings::GetChannelName(10) == "NPC");
		CHECK(PhysicsSettings::FindChannelIndex("NPC") == 10);

		PhysicsSettings::SetChannelCollision(10, 1, false);
		CHECK(!PhysicsSettings::GetChannelCollision(10, 1));
		CHECK(!PhysicsSettings::GetChannelCollision(1, 10));
		CHECK((PhysicsSettings::GetMaskForChannel(10) & (1u << 1)) == 0);
		CHECK((PhysicsSettings::GetMaskForChannel(1) & (1u << 10)) == 0);

		// PhysicsQuerySystem::GetChannelMask
		CHECK(PhysicsQuerySystem::GetChannelMask("NPC") == PhysicsSettings::GetMaskForChannel(10));
		CHECK(PhysicsQuerySystem::GetChannelMask("Default") == PhysicsSettings::GetMaskForChannel(0));
		CHECK(PhysicsQuerySystem::GetChannelMask("NonExistentChannelName") == 0xFFFFFFFF);

		PhysicsSettings::ResetToDefaults();
		CHECK(PhysicsSettings::FindChannelIndex("NPC") == -1);

		std::cout << "[PASS] TestPhysicsSettingsIntegration\n";
	}

	void TestPhysicsQueryRaycastChannel()
	{
		PhysicsSettings::ResetToDefaults();
		WorldContext context;
		Scene scene(context);

		// Create entity at z = 5.0 with Static collision layer (bit 1)
		Entity box = Entity::CreateEntity(&scene, "TargetBox");
		auto& tr = box.AddComponent<TransformComponent>();
		tr.Location = glm::vec3(0.0f, 0.0f, 5.0f);
		auto& collider = box.AddComponent<BoxCollider3DComponent>();
		collider.HalfExtents = glm::vec3(1.0f);
		auto& rb = box.AddComponent<RigidBody3DComponent>();
		rb.Layer = (1u << 1); // Static channel (index 1)

		// Raycast along +Z with "Default" channel (index 0). Default collides with Static.
		RaycastHit hitDefault = PhysicsQuerySystem::RaycastChannel(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "Default", 10.0f);
		CHECK(hitDefault.Hit);
		CHECK(hitDefault.Entity == (entt::entity)box);
		CHECK(std::abs(hitDefault.Distance - 4.0f) < 1e-4f);

		// Raycast with "Trigger" channel (index 5). Trigger does NOT collide with Static by default.
		RaycastHit hitTrigger = PhysicsQuerySystem::RaycastChannel(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "Trigger", 10.0f);
		CHECK(!hitTrigger.Hit);

		// Dynamically enable collision between "Trigger" and "Static"
		PhysicsSettings::SetChannelCollision(5, 1, true);
		hitTrigger = PhysicsQuerySystem::RaycastChannel(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "Trigger", 10.0f);
		CHECK(hitTrigger.Hit);
		CHECK(hitTrigger.Entity == (entt::entity)box);

		// Dynamically disable collision between "Default" and "Static"
		PhysicsSettings::SetChannelCollision(0, 1, false);
		hitDefault = PhysicsQuerySystem::RaycastChannel(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "Default", 10.0f);
		CHECK(!hitDefault.Hit);

		// Query with unknown channel name: fallback mask 0xFFFFFFFF hits Static layer
		RaycastHit hitUnknown = PhysicsQuerySystem::RaycastChannel(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "NonExistentChannel", 10.0f);
		CHECK(hitUnknown.Hit);
		CHECK(hitUnknown.Entity == (entt::entity)box);

		PhysicsSettings::ResetToDefaults();
		std::cout << "[PASS] TestPhysicsQueryRaycastChannel\n";
	}

	void TestYamlRoundtrip()
	{
		const fs::path tempDir = fs::temp_directory_path() / "we-collision-matrix-test";
		std::error_code ec;
		fs::remove_all(tempDir, ec);
		fs::create_directories(tempDir, ec);

		std::string error;

		// 1. Default manifest roundtrip
		{
			ProjectManifest defaultManifest;
			defaultManifest.Id = "com.test.collision.default";
			defaultManifest.StartScene = "scenes/main.wlevel";
			const fs::path pathA = tempDir / "project_default.we.yaml";

			CHECK(ProjectManifest::Save(pathA, defaultManifest, &error));
			ProjectManifest loadedA;
			CHECK(ProjectManifest::Load(pathA, &loadedA, &error));
			CHECK(loadedA.Physics.Channels.IsDefault());

			for (uint8_t i = 0; i < 8; ++i)
			{
				CHECK(loadedA.Physics.Channels.GetChannelName(i) == defaultManifest.Physics.Channels.GetChannelName(i));
			}
			for (size_t i = 0; i < CollisionChannelConfig::kMaxChannels; ++i)
			{
				CHECK(loadedA.Physics.Channels.ChannelMasks[i] == defaultManifest.Physics.Channels.ChannelMasks[i]);
			}
		}

		// 2. Custom channels & matrix roundtrip
		{
			ProjectManifest customManifest;
			customManifest.Id = "com.test.collision.custom";
			customManifest.StartScene = "scenes/main.wlevel";
			CHECK(customManifest.Physics.Channels.SetChannelName(8, "Sensors"));
			CHECK(customManifest.Physics.Channels.SetChannelName(9, "Debris"));
			customManifest.Physics.Channels.SetCollision(8, 9, false);
			customManifest.Physics.Channels.SetCollision(8, 2, true);
			customManifest.Physics.Channels.SetCollision(9, 1, false);
			CHECK(!customManifest.Physics.Channels.IsDefault());

			const fs::path pathB = tempDir / "project.we.yaml";
			CHECK(ProjectManifest::Save(pathB, customManifest, &error));

			ProjectManifest loadedB;
			CHECK(ProjectManifest::Load(pathB, &loadedB, &error));
			CHECK(!loadedB.Physics.Channels.IsDefault());
			CHECK(loadedB.Physics.Channels.GetChannelName(8) == "Sensors");
			CHECK(loadedB.Physics.Channels.GetChannelName(9) == "Debris");
			CHECK(loadedB.Physics.Channels.FindChannelIndex("Sensors") == 8);
			CHECK(loadedB.Physics.Channels.FindChannelIndex("Debris") == 9);
			CHECK(!loadedB.Physics.Channels.GetCollision(8, 9));
			CHECK(!loadedB.Physics.Channels.GetCollision(9, 8));
			CHECK(!loadedB.Physics.Channels.GetCollision(9, 1));
			CHECK(!loadedB.Physics.Channels.GetCollision(1, 9));

			for (size_t i = 0; i < CollisionChannelConfig::kMaxChannels; ++i)
			{
				CHECK(loadedB.Physics.Channels.ChannelNames[i] == customManifest.Physics.Channels.ChannelNames[i]);
				CHECK(loadedB.Physics.Channels.ChannelMasks[i] == customManifest.Physics.Channels.ChannelMasks[i]);
			}

			// 3. Test PhysicsSettings::LoadFromProject loading the saved project.we.yaml
			CHECK(PhysicsSettings::LoadFromProject(tempDir));
			CHECK(PhysicsSettings::GetChannelName(8) == "Sensors");
			CHECK(PhysicsSettings::GetChannelName(9) == "Debris");
			CHECK(!PhysicsSettings::GetChannelCollision(8, 9));
			CHECK(!PhysicsSettings::GetChannelCollision(9, 1));

			PhysicsSettings::ResetToDefaults();
		}

		fs::remove_all(tempDir, ec);
		std::cout << "[PASS] TestYamlRoundtrip\n";
	}
}

int main()
{
	World::Log::Init();
	std::cout << "Running WorldCollisionMatrixTests...\n";
	try
	{
		TestDefaultPresets();
		TestSymmetryInvariant();
		TestChannelRenamingAndLookup();
		TestMaskComputation();
		TestPhysicsSettingsIntegration();
		TestPhysicsQueryRaycastChannel();
		TestYamlRoundtrip();
	}
	catch (const std::exception& e)
	{
		std::cerr << "[FAIL] " << e.what() << "\n";
		return 1;
	}
	std::cout << "All WorldCollisionMatrixTests passed successfully.\n";
	return 0;
}
