#include "wldpch.h"
#include "World/Core/WorldContext.h"
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

		const char* script = R"luau(
			-- Streaming API tests
			local coord = Streaming.GetChunkCoord(130, 20, 200, 64)
			assert(coord ~= nil, "GetChunkCoord must return table")
			assert(coord.x == 2, "coord.x mismatch")
			assert(coord.y == 0, "coord.y mismatch")
			assert(coord.z == 3, "coord.z mismatch")

			-- GlobalState API tests
			GlobalState.Set("LevelProgress", 5)
			GlobalState.Set("PlayerName", "TestPlayer")
			GlobalState.Set("MusicEnabled", true)

			assert(GlobalState.Has("LevelProgress") == true, "Must have LevelProgress")
			assert(GlobalState.Get("LevelProgress") == 5, "LevelProgress value mismatch")
			assert(GlobalState.Get("PlayerName") == "TestPlayer", "PlayerName mismatch")
			assert(GlobalState.Get("MusicEnabled") == true, "MusicEnabled mismatch")
			assert(GlobalState.Get("NonExistent", "default") == "default", "Fallback mismatch")
		)luau";

		RunOk(script, "StreamingScriptTest");
		std::cout << "World.StreamingScript: all checks passed\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "Fatal: " << e.what() << "\n";
		return 1;
	}
}
