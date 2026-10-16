#include "World/Gameplay/Framework/GlobalStateStore.h"
#include "World/Core/Log.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

using namespace World::Gameplay;

static void TestGlobalStateStoreAndWalRecovery()
{
	std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "we_wal_test";
	std::filesystem::remove_all(tempDir);

	{
		GlobalStateStore store(tempDir);
		store.SetString("PlayerName", "Hero_Adrian");
		store.SetNumber("PlayerGold", 1250.0);
		store.SetBool("QuestComplete_01", true);

		assert(store.GetString("PlayerName") == "Hero_Adrian");
		assert(store.GetNumber("PlayerGold") == 1250.0);
		assert(store.GetBool("QuestComplete_01") == true);

		bool committed = store.CommitSnapshot();
		assert(committed);
		assert(std::filesystem::exists(tempDir / "global_state.wstate"));
	}

	{
		GlobalStateStore store2(tempDir);
		store2.RecoverFromWal();
		assert(store2.GetString("PlayerName") == "Hero_Adrian");
		assert(store2.GetNumber("PlayerGold") == 1250.0);

		store2.SetNumber("PlayerGold", 2000.0);
		store2.SetString("CurrentZone", "Dungeon_Level_2");
	}

	{
		GlobalStateStore store3(tempDir);
		bool recovered = store3.RecoverFromWal();
		assert(recovered);
		assert(store3.GetNumber("PlayerGold") == 2000.0);
		assert(store3.GetString("CurrentZone") == "Dungeon_Level_2");
		assert(store3.GetBool("QuestComplete_01") == true);
	}

	std::filesystem::remove_all(tempDir);
	std::cout << "[PASS] TestGlobalStateStoreAndWalRecovery\n";
}

int main()
{
	World::Log::Init();
	std::cout << "Running WorldGlobalStateWalTests...\n";

	TestGlobalStateStoreAndWalRecovery();

	std::cout << "All WorldGlobalStateWalTests passed successfully.\n";
	return 0;
}
