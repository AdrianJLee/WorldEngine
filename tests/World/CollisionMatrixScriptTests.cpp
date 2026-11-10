#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Physics/PhysicsSettings.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptValue.h"

#include <cassert>
#undef assert
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
	using namespace World;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
		}
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void RUN_OK(const std::string& source, const char* chunk)
	{
		std::string error;
		if (!ScriptEngine::GetState().RunString(source, chunk, &error))
		{
			throw std::runtime_error(std::string(chunk) + " failed: " + error);
		}
	}

	void RUN_FAIL(const std::string& source, const char* chunk)
	{
		std::string error;
		if (ScriptEngine::GetState().RunString(source, chunk, &error))
		{
			throw std::runtime_error(std::string(chunk) + " unexpectedly succeeded");
		}
	}
}

int main()
{
	try
	{
		Log::Init();
		WorldContext context;
		ScriptEngine::Init();
		PhysicsSettings::ResetToDefaults();

		// 1. Table and method existence checks
		RUN_OK(R"lua(
			assert(type(Physics) == "table", "Physics table must exist")
			assert(type(Physics.GetChannelMask) == "function", "Physics.GetChannelMask must be a function")
			assert(type(Physics.SetChannelCollision) == "function", "Physics.SetChannelCollision must be a function")
			assert(type(Physics.GetChannelCollision) == "function", "Physics.GetChannelCollision must be a function")
			assert(type(Physics.GetChannelName) == "function", "Physics.GetChannelName must be a function")
			assert(type(Physics.FindChannel) == "function", "Physics.FindChannel must be a function")
			assert(type(Physics.GetCollisionResponse) == "function", "Physics.GetCollisionResponse must be a function")
			assert(type(Physics.SetCollisionResponse) == "function", "Physics.SetCollisionResponse must be a function")
			assert(type(Physics.EvaluateInteraction) == "function", "Physics.EvaluateInteraction must be a function")
			assert(type(Physics.GetProfile) == "function", "Physics.GetProfile must be a function")
			assert(type(Physics.RaycastTrace) == "function", "Physics.RaycastTrace must be a function")
		)lua", "ServiceMethodExistenceCheck");

		// 2. Physics.GetChannelName with 64 channels
		RUN_OK(R"lua(
			assert(Physics.GetChannelName(0) == "Default", "Channel 0 should be Default")
			assert(Physics.GetChannelName(1) == "Static", "Channel 1 should be Static")
			assert(Physics.GetChannelName(2) == "Pawn", "Channel 2 should be Pawn")
			assert(Physics.GetChannelName(3) == "Dynamic", "Channel 3 should be Dynamic")
			assert(Physics.GetChannelName(4) == "TriggerZone", "Channel 4 should be TriggerZone")
			assert(Physics.GetChannelName(5) == "Water", "Channel 5 should be Water")
			assert(Physics.GetChannelName(6) == "Destructible", "Channel 6 should be Destructible")
			assert(Physics.GetChannelName(7) == "Vehicle", "Channel 7 should be Vehicle")
			assert(Physics.GetChannelName(8) == "Ragdoll", "Channel 8 should be Ragdoll")
			assert(Physics.GetChannelName(9) == "Foliage", "Channel 9 should be Foliage")
			assert(Physics.GetChannelName(10) == "Debris", "Channel 10 should be Debris")
			assert(Physics.GetChannelName(11) == "Shield", "Channel 11 should be Shield")
			assert(Physics.GetChannelName(48) == "Visibility", "Channel 48 should be Visibility")
			assert(Physics.GetChannelName(49) == "Camera", "Channel 49 should be Camera")
			assert(Physics.GetChannelName(50) == "WeaponBullet", "Channel 50 should be WeaponBullet")
			assert(Physics.GetChannelName(51) == "WeaponLaser", "Channel 51 should be WeaponLaser")
			assert(Physics.GetChannelName(52) == "Interact", "Channel 52 should be Interact")
			assert(Physics.GetChannelName(53) == "AiSight", "Channel 53 should be AiSight")
			assert(Physics.GetChannelName(54) == "AudioOcclusion", "Channel 54 should be AudioOcclusion")
			assert(Physics.GetChannelName(55) == "GroundSnap", "Channel 55 should be GroundSnap")
			assert(Physics.GetChannelName(56) == "", "Channel 56 should be empty by default")
			assert(Physics.GetChannelName(99) == "", "Out-of-bounds channel index should return empty string")
		)lua", "GetChannelNameTest");

		// 3. Physics.FindChannel
		RUN_OK(R"lua(
			assert(Physics.FindChannel("Default") == 0, "FindChannel Default should be 0")
			assert(Physics.FindChannel("Static") == 1, "FindChannel Static should be 1")
			assert(Physics.FindChannel("Pawn") == 2, "FindChannel Pawn should be 2")
			assert(Physics.FindChannel("Dynamic") == 3, "FindChannel Dynamic should be 3")
			assert(Physics.FindChannel("TriggerZone") == 4, "FindChannel TriggerZone should be 4")
			assert(Physics.FindChannel("Water") == 5, "FindChannel Water should be 5")
			assert(Physics.FindChannel("Destructible") == 6, "FindChannel Destructible should be 6")
			assert(Physics.FindChannel("Vehicle") == 7, "FindChannel Vehicle should be 7")
			assert(Physics.FindChannel("Visibility") == 48, "FindChannel Visibility should be 48")
			assert(Physics.FindChannel("Camera") == 49, "FindChannel Camera should be 49")
			assert(Physics.FindChannel("WeaponBullet") == 50, "FindChannel WeaponBullet should be 50")
			assert(Physics.FindChannel("NonExistentChannel") == -1, "FindChannel unknown should return -1")
		)lua", "FindChannelTest");

		// 4. Three-State Collision Response & Interaction tests (Required by dispatch)
		// - Physics.GetCollisionResponse(2, 4) == 1 (Pawn vs TriggerZone is Overlap)
		// - Physics.SetCollisionResponse(2, 4, 0) -> Ignore
		// - Physics.EvaluateInteraction(2, 1) == 2 (Pawn vs Static is Block)
		RUN_OK(R"lua(
			-- Pawn (2) vs TriggerZone (4) is Overlap (1)
			assert(Physics.GetCollisionResponse(2, 4) == 1, "Pawn vs TriggerZone response must be 1 (Overlap)")
			assert(Physics.GetCollisionResponse(4, 2) == 1, "TriggerZone vs Pawn response must be 1 (Overlap)")

			-- Pawn (2) vs Static (1) is Block (2)
			assert(Physics.EvaluateInteraction(2, 1) == 2, "Pawn vs Static interaction must be 2 (Block)")

			-- TriggerZone (4) vs Static (1) is Ignore (0)
			assert(Physics.GetCollisionResponse(4, 1) == 0, "TriggerZone vs Static response must be 0 (Ignore)")
			assert(Physics.EvaluateInteraction(4, 1) == 0, "TriggerZone vs Static interaction must be 0 (Ignore)")

			-- Modify response: Set Pawn vs TriggerZone to Ignore (0)
			Physics.SetCollisionResponse(2, 4, 0)
			assert(Physics.GetCollisionResponse(2, 4) == 0, "GetCollisionResponse(2, 4) should now be 0 (Ignore)")
			assert(Physics.GetCollisionResponse(4, 2) == 0, "GetCollisionResponse(4, 2) should be symmetric (0)")
			assert(Physics.EvaluateInteraction(2, 4) == 0, "EvaluateInteraction(2, 4) should be 0")

			-- Set back to Overlap (1)
			Physics.SetCollisionResponse(2, 4, 1)
			assert(Physics.GetCollisionResponse(2, 4) == 1, "Restored response to 1 (Overlap)")
			assert(Physics.GetCollisionResponse(4, 2) == 1, "Symmetric restored response to 1 (Overlap)")
			assert(Physics.EvaluateInteraction(2, 4) == 1, "EvaluateInteraction(2, 4) should be 1")

			-- Test SetCollisionResponse to Block (2)
			Physics.SetCollisionResponse(2, 4, 2)
			assert(Physics.GetCollisionResponse(2, 4) == 2, "Response set to 2 (Block)")
			assert(Physics.EvaluateInteraction(2, 4) == 2, "EvaluateInteraction(2, 4) should be 2")

			-- Restore default for subsequent tests
			Physics.SetCollisionResponse(2, 4, 1)
		)lua", "ThreeStateCollisionResponseTest");

		// 5. Collision Profile tests
		// - Physics.GetProfile("Glass") returns valid table
		RUN_OK(R"lua(
			local glass = Physics.GetProfile("Glass")
			assert(type(glass) == "table", "Glass profile table must exist")
			assert(glass.Name == "Glass", "Glass profile Name should be 'Glass'")
			assert(glass.ObjectType == 1, "Glass ObjectType should be 1 (Static)")
			assert(glass.ObjectTypeName == "Static", "Glass ObjectTypeName should be 'Static'")

			local pawnProf = Physics.GetProfile("Pawn")
			assert(type(pawnProf) == "table", "Pawn profile table must exist")
			assert(pawnProf.Name == "Pawn", "Pawn profile Name should be 'Pawn'")
			assert(pawnProf.ObjectType == 2, "Pawn ObjectType should be 2 (Pawn)")
			assert(pawnProf.ObjectTypeName == "Pawn", "Pawn ObjectTypeName should be 'Pawn'")

			local blockAll = Physics.GetProfile("BlockAll")
			assert(type(blockAll) == "table", "BlockAll profile must exist")
			assert(blockAll.Name == "BlockAll", "BlockAll profile Name should be 'BlockAll'")

			assert(Physics.GetProfile("NonExistentProfile") == nil, "Unknown profile should return nil")
		)lua", "CollisionProfileTest");

		// 6. RaycastTrace tests
		// - Physics.RaycastTrace returns valid table with Hit and Response
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity box = Entity::CreateEntity(&scene, "TraceTarget");
			auto& tr = box.AddComponent<TransformComponent>();
			tr.Location = glm::vec3(0.0f, 0.0f, 5.0f);
			auto& collider = box.AddComponent<BoxCollider3DComponent>();
			collider.HalfExtents = glm::vec3(1.0f, 1.0f, 1.0f);

			RUN_OK(R"lua(
				-- RaycastTrace hit test
				local hit = Physics.RaycastTrace(0, 0, 0, 0, 0, 1, "Visibility", 10.0)
				assert(type(hit) == "table", "RaycastTrace must return a table")
				assert(type(hit.Hit) == "boolean", "Hit must be a boolean")
				assert(type(hit.Distance) == "number", "Distance must be a number")
				assert(type(hit.Point) == "table", "Point must be a table")
				assert(type(hit.Normal) == "table", "Normal must be a table")
				assert(type(hit.Response) == "number", "Response must be a number")
				assert(hit.Hit == true, "RaycastTrace should hit collider")
				assert(math.abs(hit.Distance - 4.0) < 0.05, "RaycastTrace distance mismatch")
				assert(hit.Response == 2, "Default response against Visibility trace should be Block (2)")

				-- RaycastTrace miss test
				local miss = Physics.RaycastTrace(0, 100, 0, 0, 1, 0, "Visibility", 10.0)
				assert(type(miss) == "table", "Miss RaycastTrace must return a table")
				assert(miss.Hit == false, "Miss Hit should be false")
				assert(miss.Distance == 0, "Miss Distance should be 0")
				assert(miss.Response == 0, "Miss Response should be Ignore (0)")
			)lua", "RaycastTraceTest");

			ScriptEngine::SetActiveScene(nullptr);
		}

		// 7. Backward compatibility tests: Physics.GetChannelMask, SetChannelCollision, GetChannelCollision
		RUN_OK(R"lua(
			-- Static (1) vs Pawn (2) collides by default (Block)
			assert(Physics.GetChannelCollision(1, 2) == true, "Static and Pawn should collide by default")
			assert(Physics.GetChannelCollision(2, 1) == true, "Symmetric collision check initially true")

			-- TriggerZone (4) vs Static (1) is Ignore by default (does not collide)
			assert(Physics.GetChannelCollision(1, 4) == false, "Static and TriggerZone should not collide by default")

			-- SetChannelCollision(1, 2, false) sets response to Ignore
			Physics.SetChannelCollision(1, 2, false)
			assert(Physics.GetChannelCollision(1, 2) == false, "GetChannelCollision(1, 2) must be false after SetChannelCollision")
			assert(Physics.GetChannelCollision(2, 1) == false, "GetChannelCollision(2, 1) must be symmetric false")
			assert(Physics.GetCollisionResponse(1, 2) == 0, "GetCollisionResponse must be 0 (Ignore)")

			-- Re-enable collision
			Physics.SetChannelCollision(1, 2, true)
			assert(Physics.GetChannelCollision(1, 2) == true, "Collision re-enabled (1, 2)")
			assert(Physics.GetChannelCollision(2, 1) == true, "Collision re-enabled (2, 1)")
			assert(Physics.GetCollisionResponse(1, 2) == 2, "GetCollisionResponse must be 2 (Block)")
		)lua", "BackwardCompatibilityTest");

		// 8. Error handling and argument validation
		RUN_FAIL("Physics.GetChannelMask()", "GetChannelMaskMissingArgs");
		RUN_FAIL("Physics.GetChannelMask(123)", "GetChannelMaskInvalidType");
		RUN_FAIL("Physics.SetChannelCollision(2, 4)", "SetChannelCollisionInsufficientArgs");
		RUN_FAIL("Physics.GetChannelCollision(2)", "GetChannelCollisionInsufficientArgs");
		RUN_FAIL("Physics.GetChannelName()", "GetChannelNameMissingArgs");
		RUN_FAIL("Physics.FindChannel()", "FindChannelMissingArgs");
		RUN_FAIL("Physics.GetCollisionResponse(2)", "GetCollisionResponseInsufficientArgs");
		RUN_FAIL("Physics.SetCollisionResponse(2, 4)", "SetCollisionResponseInsufficientArgs");
		RUN_FAIL("Physics.EvaluateInteraction(2)", "EvaluateInteractionInsufficientArgs");
		RUN_FAIL("Physics.GetProfile()", "GetProfileMissingArgs");
		RUN_FAIL("Physics.RaycastTrace(0, 0, 0, 0, 0, 1)", "RaycastTraceInsufficientArgs");
		RUN_FAIL("Physics.RaycastTrace(0, 0, 0, 0, 0, 1, 123)", "RaycastTraceInvalidChannelType");

		PhysicsSettings::ResetToDefaults();
		ScriptEngine::Shutdown();

		std::printf("World.CollisionMatrixScript: all checks passed\n");
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::fprintf(stderr, "World.CollisionMatrixScript: fatal: %s\n", ex.what());
		return 1;
	}
}
