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

	void TestCollisionResponseContainer()
	{
		static_assert(sizeof(CollisionResponseContainer) == 16, "CollisionResponseContainer must be exactly 16 bytes (128 bits)");

		// 1. Default constructor
		CollisionResponseContainer container;
		CHECK(container.Low == 0);
		CHECK(container.High == 0);
		for (uint8_t i = 0; i < 64; ++i)
		{
			CHECK(container.GetResponse(i) == CollisionResponse::Ignore);
		}

		// 2. Value constructor
		CollisionResponseContainer blockContainer(CollisionResponse::Block);
		for (uint8_t i = 0; i < 64; ++i)
		{
			CHECK(blockContainer.GetResponse(i) == CollisionResponse::Block);
		}

		// 3. SetResponse & GetResponse across 64 channels
		container.SetResponse(0, CollisionResponse::Block);
		CHECK(container.GetResponse(0) == CollisionResponse::Block);
		CHECK(container.GetResponse(1) == CollisionResponse::Ignore);

		container.SetResponse(1, CollisionResponse::Overlap);
		CHECK(container.GetResponse(1) == CollisionResponse::Overlap);
		CHECK(container.GetResponse(0) == CollisionResponse::Block);

		container.SetResponse(31, CollisionResponse::Block);
		CHECK(container.GetResponse(31) == CollisionResponse::Block);
		CHECK(container.High == 0); // High should not be affected by low 32 channels

		container.SetResponse(32, CollisionResponse::Overlap);
		CHECK(container.GetResponse(32) == CollisionResponse::Overlap);
		CHECK(container.GetResponse(31) == CollisionResponse::Block);

		container.SetResponse(48, CollisionResponse::Block);
		CHECK(container.GetResponse(48) == CollisionResponse::Block);

		container.SetResponse(63, CollisionResponse::Overlap);
		CHECK(container.GetResponse(63) == CollisionResponse::Overlap);

		// Out of bounds
		CHECK(container.GetResponse(64) == CollisionResponse::Ignore);
		CHECK(container.GetResponse(100) == CollisionResponse::Ignore);
		container.SetResponse(64, CollisionResponse::Block);
		CHECK(container.GetResponse(64) == CollisionResponse::Ignore);

		// 4. SetAll
		container.SetAll(CollisionResponse::Overlap);
		for (uint8_t i = 0; i < 64; ++i)
		{
			CHECK(container.GetResponse(i) == CollisionResponse::Overlap);
		}

		container.SetAll(CollisionResponse::Ignore);
		CHECK(container.Low == 0);
		CHECK(container.High == 0);
		for (uint8_t i = 0; i < 64; ++i)
		{
			CHECK(container.GetResponse(i) == CollisionResponse::Ignore);
		}

		// 5. Equality operators
		CollisionResponseContainer c1, c2;
		CHECK(c1 == c2);
		c1.SetResponse(10, CollisionResponse::Block);
		CHECK(c1 != c2);
		c2.SetResponse(10, CollisionResponse::Block);
		CHECK(c1 == c2);
		c1.SetResponse(45, CollisionResponse::Overlap);
		CHECK(c1 != c2);
		c2.SetResponse(45, CollisionResponse::Overlap);
		CHECK(c1 == c2);

		// 6. Combine rule min(A, B)
		CHECK(CollisionResponseContainer::Combine(CollisionResponse::Ignore, CollisionResponse::Ignore) == CollisionResponse::Ignore);
		CHECK(CollisionResponseContainer::Combine(CollisionResponse::Ignore, CollisionResponse::Overlap) == CollisionResponse::Ignore);
		CHECK(CollisionResponseContainer::Combine(CollisionResponse::Ignore, CollisionResponse::Block) == CollisionResponse::Ignore);
		CHECK(CollisionResponseContainer::Combine(CollisionResponse::Overlap, CollisionResponse::Ignore) == CollisionResponse::Ignore);
		CHECK(CollisionResponseContainer::Combine(CollisionResponse::Overlap, CollisionResponse::Overlap) == CollisionResponse::Overlap);
		CHECK(CollisionResponseContainer::Combine(CollisionResponse::Overlap, CollisionResponse::Block) == CollisionResponse::Overlap);
		CHECK(CollisionResponseContainer::Combine(CollisionResponse::Block, CollisionResponse::Ignore) == CollisionResponse::Ignore);
		CHECK(CollisionResponseContainer::Combine(CollisionResponse::Block, CollisionResponse::Overlap) == CollisionResponse::Overlap);
		CHECK(CollisionResponseContainer::Combine(CollisionResponse::Block, CollisionResponse::Block) == CollisionResponse::Block);

		std::cout << "[PASS] TestCollisionResponseContainer\n";
	}

	void Test64ChannelsAndCapacity()
	{
		CollisionChannelConfig cfg = CollisionChannelConfig::Default();
		CHECK(cfg.IsDefault());

		CHECK(CollisionChannelConfig::kMaxChannels == 64);
		CHECK(CollisionChannelConfig::kTraceChannelStart == 48);

		// Object presets (0..11)
		const char* const objectPresets[] = {
			"Default", "Static", "Pawn", "Dynamic",
			"TriggerZone", "Water", "Destructible", "Vehicle",
			"Ragdoll", "Foliage", "Debris", "Shield"
		};
		for (uint8_t i = 0; i < 12; ++i)
		{
			CHECK(cfg.GetChannelName(i) == objectPresets[i]);
			CHECK(cfg.GetChannelType(i) == CollisionChannelType::Object);
			CHECK(cfg.FindChannelIndex(objectPresets[i]) == static_cast<int>(i));
		}

		// Channels 12..47 should be unassigned Object channels
		for (size_t i = 12; i < CollisionChannelConfig::kTraceChannelStart; ++i)
		{
			CHECK(cfg.GetChannelName(static_cast<uint8_t>(i)).empty());
			CHECK(cfg.GetChannelType(static_cast<uint8_t>(i)) == CollisionChannelType::Object);
		}

		// Trace presets (48..55)
		const char* const tracePresets[] = {
			"Visibility", "Camera", "WeaponBullet", "WeaponLaser",
			"Interact", "AiSight", "AudioOcclusion", "GroundSnap"
		};
		for (uint8_t i = 0; i < 8; ++i)
		{
			uint8_t idx = static_cast<uint8_t>(CollisionChannelConfig::kTraceChannelStart + i);
			CHECK(cfg.GetChannelName(idx) == tracePresets[i]);
			CHECK(cfg.GetChannelType(idx) == CollisionChannelType::Trace);
			CHECK(cfg.FindChannelIndex(tracePresets[i]) == static_cast<int>(idx));
		}

		// Channels 56..63 should be unassigned
		for (size_t i = 56; i < CollisionChannelConfig::kMaxChannels; ++i)
		{
			CHECK(cfg.GetChannelName(static_cast<uint8_t>(i)).empty());
		}

		std::cout << "[PASS] Test64ChannelsAndCapacity\n";
	}

	void TestThreeStateInteractionRule()
	{
		CollisionChannelConfig cfg = CollisionChannelConfig::Default();

		// 1. Core interaction rules specified in dispatch:
		// - TriggerZone (4) vs Static (1) is Ignore
		CHECK(cfg.EvaluateInteraction(4, 1) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(1, 4) == CollisionResponse::Ignore);

		// - TriggerZone (4) vs Pawn (2) is Overlap
		CHECK(cfg.EvaluateInteraction(4, 2) == CollisionResponse::Overlap);
		CHECK(cfg.EvaluateInteraction(2, 4) == CollisionResponse::Overlap);

		// - Pawn (2) vs Static (1) is Block
		CHECK(cfg.EvaluateInteraction(2, 1) == CollisionResponse::Block);
		CHECK(cfg.EvaluateInteraction(1, 2) == CollisionResponse::Block);

		// Additional preset interactions:
		// TriggerZone (4) vs Vehicle (7) is Overlap
		CHECK(cfg.EvaluateInteraction(4, 7) == CollisionResponse::Overlap);
		CHECK(cfg.EvaluateInteraction(7, 4) == CollisionResponse::Overlap);

		// Foliage (9) vs Pawn (2) is Overlap
		CHECK(cfg.EvaluateInteraction(9, 2) == CollisionResponse::Overlap);
		CHECK(cfg.EvaluateInteraction(2, 9) == CollisionResponse::Overlap);

		// Water (5) vs Pawn (2) is Overlap
		CHECK(cfg.EvaluateInteraction(5, 2) == CollisionResponse::Overlap);

		// Debris (10) vs Pawn (2) is Ignore
		CHECK(cfg.EvaluateInteraction(10, 2) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(2, 10) == CollisionResponse::Ignore);

		// Trace presets interactions:
		// Visibility (48) ignores TriggerZone (4), Water (5), Foliage (9), Debris (10)
		CHECK(cfg.EvaluateInteraction(48, 4) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(48, 5) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(48, 9) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(48, 10) == CollisionResponse::Ignore);
		// Visibility (48) blocks Static (1) and Pawn (2)
		CHECK(cfg.EvaluateInteraction(48, 1) == CollisionResponse::Block);
		CHECK(cfg.EvaluateInteraction(48, 2) == CollisionResponse::Block);

		// Camera (49) ignores Pawn (2), Vehicle (7), TriggerZone (4), Water (5), Foliage (9), Debris (10)
		CHECK(cfg.EvaluateInteraction(49, 2) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(49, 7) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(49, 4) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(49, 5) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(49, 9) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(49, 10) == CollisionResponse::Ignore);

		// WeaponBullet (50) ignores TriggerZone (4), Water (5), Debris (10)
		CHECK(cfg.EvaluateInteraction(50, 4) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(50, 5) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(50, 10) == CollisionResponse::Ignore);
		// WeaponBullet (50) blocks Static (1) and Pawn (2)
		CHECK(cfg.EvaluateInteraction(50, 1) == CollisionResponse::Block);
		CHECK(cfg.EvaluateInteraction(50, 2) == CollisionResponse::Block);

		// 2. Dynamic modification and symmetry
		cfg.SetCollisionResponse(4, 1, CollisionResponse::Block);
		CHECK(cfg.EvaluateInteraction(4, 1) == CollisionResponse::Block);
		CHECK(cfg.EvaluateInteraction(1, 4) == CollisionResponse::Block);
		CHECK(cfg.GetCollisionResponse(4, 1) == CollisionResponse::Block);
		CHECK(cfg.GetCollisionResponse(1, 4) == CollisionResponse::Block);

		cfg.SetCollisionResponse(4, 1, CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(4, 1) == CollisionResponse::Ignore);
		CHECK(cfg.EvaluateInteraction(1, 4) == CollisionResponse::Ignore);

		// Backward-compatible bool interface
		CHECK(!cfg.GetCollision(4, 1));
		CHECK(cfg.GetCollision(2, 1));
		cfg.SetCollision(4, 1, true);
		CHECK(cfg.GetCollision(4, 1));
		CHECK(cfg.EvaluateInteraction(4, 1) == CollisionResponse::Block);
		cfg.SetCollision(4, 1, false);
		CHECK(!cfg.GetCollision(4, 1));
		CHECK(cfg.EvaluateInteraction(4, 1) == CollisionResponse::Ignore);

		std::cout << "[PASS] TestThreeStateInteractionRule\n";
	}

	void TestCollisionProfiles()
	{
		CollisionChannelConfig cfg = CollisionChannelConfig::Default();

		// 1. Pawn profile: Camera is Ignore, TriggerZone is Overlap, Static is Block
		const CollisionProfile* pawn = cfg.FindProfile("Pawn");
		CHECK(pawn != nullptr);
		CHECK(pawn->ObjectType == 2); // Pawn
		CHECK(pawn->Responses.GetResponse(49) == CollisionResponse::Ignore);  // Camera
		CHECK(pawn->Responses.GetResponse(4) == CollisionResponse::Overlap);  // TriggerZone
		CHECK(pawn->Responses.GetResponse(9) == CollisionResponse::Overlap);  // Foliage
		CHECK(pawn->Responses.GetResponse(1) == CollisionResponse::Block);    // Static
		CHECK(pawn->Responses.GetResponse(2) == CollisionResponse::Block);    // Pawn
		CHECK(pawn->Responses.GetResponse(50) == CollisionResponse::Block);   // WeaponBullet

		// 2. Glass profile: Visibility is Ignore, Camera is Ignore, Pawn is Block
		const CollisionProfile* glass = cfg.FindProfile("Glass");
		CHECK(glass != nullptr);
		CHECK(glass->ObjectType == 1); // Static
		CHECK(glass->Responses.GetResponse(48) == CollisionResponse::Ignore); // Visibility
		CHECK(glass->Responses.GetResponse(49) == CollisionResponse::Ignore); // Camera
		CHECK(glass->Responses.GetResponse(2) == CollisionResponse::Block);   // Pawn
		CHECK(glass->Responses.GetResponse(50) == CollisionResponse::Block);  // WeaponBullet

		// 3. InvisibleWall profile: Pawn is Block, Visibility is Ignore
		const CollisionProfile* invisibleWall = cfg.FindProfile("InvisibleWall");
		CHECK(invisibleWall != nullptr);
		CHECK(invisibleWall->ObjectType == 1); // Static
		CHECK(invisibleWall->Responses.GetResponse(2) == CollisionResponse::Block);   // Pawn
		CHECK(invisibleWall->Responses.GetResponse(7) == CollisionResponse::Block);   // Vehicle
		CHECK(invisibleWall->Responses.GetResponse(48) == CollisionResponse::Ignore); // Visibility
		CHECK(invisibleWall->Responses.GetResponse(50) == CollisionResponse::Ignore); // WeaponBullet

		// 4. TriggerOnly profile
		const CollisionProfile* triggerOnly = cfg.FindProfile("TriggerOnly");
		CHECK(triggerOnly != nullptr);
		CHECK(triggerOnly->ObjectType == 4); // TriggerZone
		CHECK(triggerOnly->Responses.GetResponse(2) == CollisionResponse::Overlap); // Pawn
		CHECK(triggerOnly->Responses.GetResponse(7) == CollisionResponse::Overlap); // Vehicle
		CHECK(triggerOnly->Responses.GetResponse(1) == CollisionResponse::Ignore);  // Static

		// 5. BlockAll, OverlapAll, NoCollision
		const CollisionProfile* blockAll = cfg.FindProfile("BlockAll");
		CHECK(blockAll != nullptr);
		for (uint8_t i = 0; i < 64; ++i)
		{
			CHECK(blockAll->Responses.GetResponse(i) == CollisionResponse::Block);
		}

		const CollisionProfile* overlapAll = cfg.FindProfile("OverlapAll");
		CHECK(overlapAll != nullptr);
		for (uint8_t i = 0; i < 64; ++i)
		{
			CHECK(overlapAll->Responses.GetResponse(i) == CollisionResponse::Overlap);
		}

		const CollisionProfile* noCollision = cfg.FindProfile("NoCollision");
		CHECK(noCollision != nullptr);
		for (uint8_t i = 0; i < 64; ++i)
		{
			CHECK(noCollision->Responses.GetResponse(i) == CollisionResponse::Ignore);
		}

		// 6. Unknown and empty profile lookup
		CHECK(cfg.FindProfile("NonExistent") == nullptr);
		CHECK(cfg.FindProfile("") == nullptr);

		std::cout << "[PASS] TestCollisionProfiles\n";
	}

	void TestSymmetryInvariant()
	{
		CollisionChannelConfig cfg = CollisionChannelConfig::Default();

		const std::pair<uint8_t, uint8_t> testPairs[] = {
			{ 0, 7 }, { 2, 3 }, { 4, 6 }, { 1, 5 }, { 7, 7 }, { 10, 20 }, { 48, 50 }, { 1, 55 }
		};

		for (const auto& pair : testPairs)
		{
			cfg.SetCollisionResponse(pair.first, pair.second, CollisionResponse::Ignore);
			CHECK(cfg.EvaluateInteraction(pair.first, pair.second) == CollisionResponse::Ignore);
			CHECK(cfg.EvaluateInteraction(pair.second, pair.first) == CollisionResponse::Ignore);

			cfg.SetCollisionResponse(pair.first, pair.second, CollisionResponse::Overlap);
			CHECK(cfg.EvaluateInteraction(pair.first, pair.second) == CollisionResponse::Overlap);
			CHECK(cfg.EvaluateInteraction(pair.second, pair.first) == CollisionResponse::Overlap);

			cfg.SetCollisionResponse(pair.first, pair.second, CollisionResponse::Block);
			CHECK(cfg.EvaluateInteraction(pair.first, pair.second) == CollisionResponse::Block);
			CHECK(cfg.EvaluateInteraction(pair.second, pair.first) == CollisionResponse::Block);
		}

		// Complex pattern test to guarantee symmetry holds across all pairs
		for (uint8_t i = 0; i < static_cast<uint8_t>(CollisionChannelConfig::kMaxChannels); ++i)
		{
			for (uint8_t j = 0; j < static_cast<uint8_t>(CollisionChannelConfig::kMaxChannels); ++j)
			{
				CHECK(cfg.EvaluateInteraction(i, j) == cfg.EvaluateInteraction(j, i));
			}
		}

		std::cout << "[PASS] TestSymmetryInvariant\n";
	}

	void TestChannelRenamingAndLookup()
	{
		CollisionChannelConfig cfg;

		// Add new channels
		CHECK(cfg.SetChannelName(12, "Sensors"));
		CHECK(cfg.GetChannelName(12) == "Sensors");
		CHECK(cfg.FindChannelIndex("Sensors") == 12);

		CHECK(cfg.SetChannelName(56, "FootstepScan"));
		CHECK(cfg.GetChannelName(56) == "FootstepScan");
		CHECK(cfg.FindChannelIndex("FootstepScan") == 56);

		// Rename existing channel
		CHECK(cfg.SetChannelName(2, "PlayerCharacter"));
		CHECK(cfg.GetChannelName(2) == "PlayerCharacter");
		CHECK(cfg.FindChannelIndex("PlayerCharacter") == 2);
		CHECK(cfg.FindChannelIndex("Pawn") == -1);

		// Edge cases: lookup non-existent and empty
		CHECK(cfg.FindChannelIndex("NonExistent") == -1);
		CHECK(cfg.FindChannelIndex("") == -1);

		// Out of bounds
		CHECK(!cfg.SetChannelName(64, "Invalid"));
		CHECK(cfg.GetChannelName(64).empty());
		CHECK(cfg.GetChannelName(99).empty());

		std::cout << "[PASS] TestChannelRenamingAndLookup\n";
	}

	void TestMaskComputation()
	{
		CollisionChannelConfig cfg = CollisionChannelConfig::Default();

		// Default TriggerZone (channel 4) should have bit 1 (Static) cleared
		uint64_t triggerMask64 = cfg.GetMask64(4);
		CHECK((triggerMask64 & (1ull << 1)) == 0);

		// Static (channel 1) mask should have bit 4 (TriggerZone) cleared
		uint64_t staticMask64 = cfg.GetMask64(1);
		CHECK((staticMask64 & (1ull << 4)) == 0);

		// Pawn (channel 2) mask should have bit 1 (Static) set
		uint64_t pawnMask64 = cfg.GetMask64(2);
		CHECK((pawnMask64 & (1ull << 1)) != 0);

		// Compatibility 32-bit mask matches lower 32 bits of 64-bit mask
		CHECK(cfg.GetMask(4) == static_cast<uint32_t>(triggerMask64 & 0xFFFFFFFFull));
		CHECK(cfg.GetMask(1) == static_cast<uint32_t>(staticMask64 & 0xFFFFFFFFull));
		CHECK(cfg.GetMask(2) == static_cast<uint32_t>(pawnMask64 & 0xFFFFFFFFull));

		std::cout << "[PASS] TestMaskComputation\n";
	}

	void TestPhysicsSettingsIntegration()
	{
		PhysicsSettings::ResetToDefaults();
		CHECK(PhysicsSettings::GetChannels().IsDefault());

		CHECK(PhysicsSettings::GetChannelName(0) == "Default");
		CHECK(PhysicsSettings::GetChannelName(1) == "Static");
		CHECK(PhysicsSettings::FindChannelIndex("TriggerZone") == 4);
		CHECK(PhysicsSettings::FindChannelIndex("Visibility") == 48);

		// Three-state queries through PhysicsSettings
		CHECK(PhysicsSettings::EvaluateInteraction(4, 1) == CollisionResponse::Ignore);
		CHECK(PhysicsSettings::EvaluateInteraction(4, 2) == CollisionResponse::Overlap);
		CHECK(PhysicsSettings::EvaluateInteraction(0, 1) == CollisionResponse::Block);

		// Mutate through PhysicsSettings
		CHECK(PhysicsSettings::SetChannelName(12, "NPC"));
		CHECK(PhysicsSettings::GetChannelName(12) == "NPC");
		CHECK(PhysicsSettings::FindChannelIndex("NPC") == 12);

		PhysicsSettings::SetCollisionResponse(12, 1, CollisionResponse::Ignore);
		CHECK(PhysicsSettings::GetCollisionResponse(12, 1) == CollisionResponse::Ignore);
		CHECK(PhysicsSettings::GetCollisionResponse(1, 12) == CollisionResponse::Ignore);
		CHECK(PhysicsSettings::EvaluateInteraction(12, 1) == CollisionResponse::Ignore);
		CHECK((PhysicsSettings::GetMask64ForChannel(12) & (1ull << 1)) == 0);
		CHECK((PhysicsSettings::GetMask64ForChannel(1) & (1ull << 12)) == 0);

		// Profile query through PhysicsSettings
		const CollisionProfile* glass = PhysicsSettings::FindProfile("Glass");
		CHECK(glass != nullptr);
		CHECK(glass->ObjectType == 1);
		CHECK(glass->Responses.GetResponse(48) == CollisionResponse::Ignore);

		// PhysicsQuerySystem::GetChannelMask
		CHECK(PhysicsQuerySystem::GetChannelMask("NPC") == PhysicsSettings::GetMaskForChannel(12));
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

		// Raycast with "TriggerZone" channel (index 4). TriggerZone does NOT collide with Static by default.
		RaycastHit hitTrigger = PhysicsQuerySystem::RaycastChannel(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "TriggerZone", 10.0f);
		CHECK(!hitTrigger.Hit);

		// Dynamically enable collision between "TriggerZone" and "Static"
		PhysicsSettings::SetCollisionResponse(4, 1, CollisionResponse::Block);
		hitTrigger = PhysicsQuerySystem::RaycastChannel(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "TriggerZone", 10.0f);
		CHECK(hitTrigger.Hit);
		CHECK(hitTrigger.Entity == (entt::entity)box);

		// Dynamically disable collision between "Default" and "Static"
		PhysicsSettings::SetCollisionResponse(0, 1, CollisionResponse::Ignore);
		hitDefault = PhysicsQuerySystem::RaycastChannel(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "Default", 10.0f);
		CHECK(!hitDefault.Hit);

		// Query with unknown channel name: fallback mask 0xFFFFFFFF hits Static layer
		RaycastHit hitUnknown = PhysicsQuerySystem::RaycastChannel(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "NonExistentChannel", 10.0f);
		CHECK(hitUnknown.Hit);
		CHECK(hitUnknown.Entity == (entt::entity)box);

		PhysicsSettings::ResetToDefaults();
		std::cout << "[PASS] TestPhysicsQueryRaycastChannel\n";
	}

	void TestPhysicsQueryRaycastTrace()
	{
		PhysicsSettings::ResetToDefaults();
		WorldContext context;
		Scene scene(context);

		// Setup:
		// 1. Glass entity at z = 5.0 with profile "Glass"
		Entity glass = Entity::CreateEntity(&scene, "Glass");
		auto& trGlass = glass.AddComponent<TransformComponent>();
		trGlass.Location = glm::vec3(0.0f, 0.0f, 5.0f);
		auto& colGlass = glass.AddComponent<BoxCollider3DComponent>();
		colGlass.HalfExtents = glm::vec3(1.0f, 1.0f, 0.1f);
		auto& rbGlass = glass.AddComponent<RigidBody3DComponent>();
		rbGlass.Layer = (1u << 1); // Static

		// 2. Target Wall entity at z = 10.0 with Static channel
		Entity targetWall = Entity::CreateEntity(&scene, "TargetWall");
		auto& trWall = targetWall.AddComponent<TransformComponent>();
		trWall.Location = glm::vec3(0.0f, 0.0f, 10.0f);
		auto& colWall = targetWall.AddComponent<BoxCollider3DComponent>();
		colWall.HalfExtents = glm::vec3(2.0f, 2.0f, 0.1f);
		auto& rbWall = targetWall.AddComponent<RigidBody3DComponent>();
		rbWall.Layer = (1u << 1); // Static

		// 1. RaycastTrace with "Visibility" (channel 48) along +Z:
		// Glass profile ignores Visibility trace. Ray ignores Glass and hits TargetWall!
		TraceHit hitVis = PhysicsQuerySystem::RaycastTrace(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "Visibility", 20.0f);
		CHECK(hitVis.Hit);
		CHECK(hitVis.Entity == (entt::entity)targetWall);
		CHECK(hitVis.Response == CollisionResponse::Block);
		CHECK(std::abs(hitVis.Distance - 9.9f) < 0.1f);

		// 2. RaycastTrace with "WeaponBullet" (channel 50) along +Z:
		// Glass profile blocks WeaponBullet trace. Ray hits Glass!
		TraceHit hitBullet = PhysicsQuerySystem::RaycastTrace(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "WeaponBullet", 20.0f);
		CHECK(hitBullet.Hit);
		CHECK(hitBullet.Entity == (entt::entity)glass);
		CHECK(hitBullet.Response == CollisionResponse::Block);
		CHECK(std::abs(hitBullet.Distance - 4.9f) < 0.1f);

		// 3. RaycastTrace with "Camera" (channel 49) along +Z:
		// Glass ignores Camera. Ray hits TargetWall!
		TraceHit hitCamera = PhysicsQuerySystem::RaycastTrace(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "Camera", 20.0f);
		CHECK(hitCamera.Hit);
		CHECK(hitCamera.Entity == (entt::entity)targetWall);
		CHECK(hitCamera.Response == CollisionResponse::Block);

		// 4. Trace against entity without profile using raw Object channel (TriggerZone):
		Entity triggerActor = Entity::CreateEntity(&scene, "TriggerZoneEntity");
		auto& trTrig = triggerActor.AddComponent<TransformComponent>();
		trTrig.Location = glm::vec3(0.0f, 0.0f, 2.0f);
		auto& colTrig = triggerActor.AddComponent<BoxCollider3DComponent>();
		colTrig.HalfExtents = glm::vec3(1.0f, 1.0f, 0.1f);
		auto& rbTrig = triggerActor.AddComponent<RigidBody3DComponent>();
		rbTrig.Layer = (1u << 4); // TriggerZone channel (index 4)

		// Visibility trace ignores TriggerZone (via matrix rule), still hits TargetWall!
		hitVis = PhysicsQuerySystem::RaycastTrace(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "Visibility", 20.0f);
		CHECK(hitVis.Hit);
		CHECK(hitVis.Entity == (entt::entity)targetWall);

		// WeaponBullet trace also ignores TriggerZone (via matrix rule), still hits Glass!
		hitBullet = PhysicsQuerySystem::RaycastTrace(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "WeaponBullet", 20.0f);
		CHECK(hitBullet.Hit);
		CHECK(hitBullet.Entity == (entt::entity)glass);

		// 5. Query with unknown trace channel returns miss with Response::Ignore
		TraceHit hitUnknown = PhysicsQuerySystem::RaycastTrace(scene, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), "NonExistentTraceChannel", 20.0f);
		CHECK(!hitUnknown.Hit);
		CHECK(hitUnknown.Response == CollisionResponse::Ignore);

		// 6. Query with zero direction or distance returns miss
		TraceHit hitZero = PhysicsQuerySystem::RaycastTrace(scene, glm::vec3(0.0f), glm::vec3(0.0f), "Visibility", 20.0f);
		CHECK(!hitZero.Hit);

		PhysicsSettings::ResetToDefaults();
		std::cout << "[PASS] TestPhysicsQueryRaycastTrace\n";
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

			for (uint8_t i = 0; i < CollisionChannelConfig::kMaxChannels; ++i)
			{
				CHECK(loadedA.Physics.Channels.GetChannelName(i) == defaultManifest.Physics.Channels.GetChannelName(i));
				CHECK(loadedA.Physics.Channels.GetChannelType(i) == defaultManifest.Physics.Channels.GetChannelType(i));
				CHECK(loadedA.Physics.Channels.Matrix[i] == defaultManifest.Physics.Channels.Matrix[i]);
			}
		}

		// 2. Custom channels & 64-channel matrix roundtrip
		{
			ProjectManifest customManifest;
			customManifest.Id = "com.test.collision.custom";
			customManifest.StartScene = "scenes/main.wlevel";
			CHECK(customManifest.Physics.Channels.SetChannelName(12, "Sensors"));
			CHECK(customManifest.Physics.Channels.SetChannelType(12, CollisionChannelType::Object));
			CHECK(customManifest.Physics.Channels.SetChannelName(56, "FootstepScan"));
			CHECK(customManifest.Physics.Channels.SetChannelType(56, CollisionChannelType::Trace));

			customManifest.Physics.Channels.SetCollisionResponse(12, 56, CollisionResponse::Overlap);
			customManifest.Physics.Channels.SetCollisionResponse(12, 1, CollisionResponse::Ignore);
			CHECK(!customManifest.Physics.Channels.IsDefault());

			const fs::path pathB = tempDir / "project.we.yaml";
			CHECK(ProjectManifest::Save(pathB, customManifest, &error));

			ProjectManifest loadedB;
			CHECK(ProjectManifest::Load(pathB, &loadedB, &error));
			CHECK(!loadedB.Physics.Channels.IsDefault());
			CHECK(loadedB.Physics.Channels.GetChannelName(12) == "Sensors");
			CHECK(loadedB.Physics.Channels.GetChannelType(12) == CollisionChannelType::Object);
			CHECK(loadedB.Physics.Channels.GetChannelName(56) == "FootstepScan");
			CHECK(loadedB.Physics.Channels.GetChannelType(56) == CollisionChannelType::Trace);
			CHECK(loadedB.Physics.Channels.FindChannelIndex("Sensors") == 12);
			CHECK(loadedB.Physics.Channels.FindChannelIndex("FootstepScan") == 56);

			CHECK(loadedB.Physics.Channels.GetCollisionResponse(12, 56) == CollisionResponse::Overlap);
			CHECK(loadedB.Physics.Channels.GetCollisionResponse(56, 12) == CollisionResponse::Overlap);
			CHECK(loadedB.Physics.Channels.GetCollisionResponse(12, 1) == CollisionResponse::Ignore);
			CHECK(loadedB.Physics.Channels.GetCollisionResponse(1, 12) == CollisionResponse::Ignore);

			for (size_t i = 0; i < CollisionChannelConfig::kMaxChannels; ++i)
			{
				CHECK(loadedB.Physics.Channels.ChannelNames[i] == customManifest.Physics.Channels.ChannelNames[i]);
				CHECK(loadedB.Physics.Channels.ChannelTypes[i] == customManifest.Physics.Channels.ChannelTypes[i]);
				CHECK(loadedB.Physics.Channels.Matrix[i] == customManifest.Physics.Channels.Matrix[i]);
			}

			// 3. Test PhysicsSettings::LoadFromProject loading the saved project.we.yaml
			CHECK(PhysicsSettings::LoadFromProject(tempDir));
			CHECK(PhysicsSettings::GetChannelName(12) == "Sensors");
			CHECK(PhysicsSettings::GetChannelName(56) == "FootstepScan");
			CHECK(PhysicsSettings::EvaluateInteraction(12, 56) == CollisionResponse::Overlap);
			CHECK(PhysicsSettings::EvaluateInteraction(12, 1) == CollisionResponse::Ignore);

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
		TestCollisionResponseContainer();
		Test64ChannelsAndCapacity();
		TestThreeStateInteractionRule();
		TestCollisionProfiles();
		TestSymmetryInvariant();
		TestChannelRenamingAndLookup();
		TestMaskComputation();
		TestPhysicsSettingsIntegration();
		TestPhysicsQueryRaycastChannel();
		TestPhysicsQueryRaycastTrace();
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
