#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Physics/PhysicsSettings.h"
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
		)lua", "ServiceMethodExistenceCheck");

		// 2. Physics.GetChannelName(0) == "Default" and other preset names
		RUN_OK(R"lua(
			assert(Physics.GetChannelName(0) == "Default", "Channel 0 should be Default")
			assert(Physics.GetChannelName(1) == "Static", "Channel 1 should be Static")
			assert(Physics.GetChannelName(2) == "Pawn", "Channel 2 should be Pawn")
			assert(Physics.GetChannelName(3) == "Visibility", "Channel 3 should be Visibility")
			assert(Physics.GetChannelName(4) == "Projectile", "Channel 4 should be Projectile")
			assert(Physics.GetChannelName(5) == "Trigger", "Channel 5 should be Trigger")
			assert(Physics.GetChannelName(6) == "Water", "Channel 6 should be Water")
			assert(Physics.GetChannelName(7) == "Vehicle", "Channel 7 should be Vehicle")
			assert(Physics.GetChannelName(8) == "", "Channel 8 should be empty by default")
			assert(Physics.GetChannelName(99) == "", "Out-of-bounds channel index should return empty string")
		)lua", "GetChannelNameTest");

		// 3. Physics.FindChannel("Static") == 1 and other lookups
		RUN_OK(R"lua(
			assert(Physics.FindChannel("Default") == 0, "FindChannel Default should be 0")
			assert(Physics.FindChannel("Static") == 1, "FindChannel Static should be 1")
			assert(Physics.FindChannel("Pawn") == 2, "FindChannel Pawn should be 2")
			assert(Physics.FindChannel("Visibility") == 3, "FindChannel Visibility should be 3")
			assert(Physics.FindChannel("Projectile") == 4, "FindChannel Projectile should be 4")
			assert(Physics.FindChannel("Trigger") == 5, "FindChannel Trigger should be 5")
			assert(Physics.FindChannel("Water") == 6, "FindChannel Water should be 6")
			assert(Physics.FindChannel("Vehicle") == 7, "FindChannel Vehicle should be 7")
			assert(Physics.FindChannel("NonExistentChannel") == -1, "FindChannel unknown should return -1")
		)lua", "FindChannelTest");

		// 4. Physics.GetChannelMask("Pawn")
		RUN_OK(R"lua(
			local pawnMask = Physics.GetChannelMask("Pawn")
			assert(pawnMask == 0xFFFFFFFF, "Default Pawn mask should be 0xFFFFFFFF")

			local defaultMask = Physics.GetChannelMask("Default")
			assert(defaultMask == 0xFFFFFFFF, "Default channel mask should be 0xFFFFFFFF")

			-- Trigger (channel 5) does not collide with Static (channel 1) by default
			local triggerMask = Physics.GetChannelMask("Trigger")
			assert(triggerMask ~= 0xFFFFFFFF, "Trigger mask should have bit 1 cleared")

			-- Unknown channel name returns 0xFFFFFFFF
			local unknownMask = Physics.GetChannelMask("UnknownChannelName")
			assert(unknownMask == 0xFFFFFFFF, "Unknown channel should return 0xFFFFFFFF")
		)lua", "GetChannelMaskTest");

		// 5. Physics.SetChannelCollision(2, 4, false) and assert GetChannelCollision(2, 4) == false and (4, 2) == false
		RUN_OK(R"lua(
			-- Precondition: channel 2 (Pawn) and channel 4 (Projectile) collide by default
			assert(Physics.GetChannelCollision(2, 4) == true, "Pawn and Projectile should collide by default")
			assert(Physics.GetChannelCollision(4, 2) == true, "Symmetric collision check initially true")

			-- Set collision to false
			Physics.SetChannelCollision(2, 4, false)
			assert(Physics.GetChannelCollision(2, 4) == false, "Physics.GetChannelCollision(2, 4) must be false")
			assert(Physics.GetChannelCollision(4, 2) == false, "Physics.GetChannelCollision(4, 2) must be false")

			-- Verify Pawn mask now has bit 4 cleared (2^4 = 16)
			local pawnMaskDisabled = Physics.GetChannelMask("Pawn")
			assert(pawnMaskDisabled == (0xFFFFFFFF - 16), "Pawn mask must have bit 4 cleared")

			-- Re-enable collision and verify symmetry restored
			Physics.SetChannelCollision(2, 4, true)
			assert(Physics.GetChannelCollision(2, 4) == true, "Collision re-enabled (2, 4)")
			assert(Physics.GetChannelCollision(4, 2) == true, "Collision re-enabled (4, 2)")
			assert(Physics.GetChannelMask("Pawn") == 0xFFFFFFFF, "Pawn mask restored")
		)lua", "SetAndGetChannelCollisionTest");

		// 6. Error handling and argument validation
		RUN_FAIL("Physics.GetChannelMask()", "GetChannelMaskMissingArgs");
		RUN_FAIL("Physics.GetChannelMask(123)", "GetChannelMaskInvalidType");
		RUN_FAIL("Physics.SetChannelCollision(2, 4)", "SetChannelCollisionInsufficientArgs");
		RUN_FAIL("Physics.GetChannelCollision(2)", "GetChannelCollisionInsufficientArgs");
		RUN_FAIL("Physics.GetChannelName()", "GetChannelNameMissingArgs");
		RUN_FAIL("Physics.FindChannel()", "FindChannelMissingArgs");

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
