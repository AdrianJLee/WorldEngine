#include "World/Level/Core/ChunkTypes.h"
#include "World/Level/Core/ChunkHeader.h"
#include "World/Level/Core/DeltaState.h"
#include "World/Level/Core/WorldSpace.h"
#include "World/Level/Authoring/EntityDescriptor.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Level/Cooking/LevelCooker.h"
#include "World/Level/Runtime/StreamableChunk.h"
#include "World/Level/Runtime/ChunkStreamingCoordinator.h"
#include "World/Level/Streaming/SpatialGridDriver.h"
#include "World/Level/AI/NavMeshTileTypes.h"
#include "World/Level/AI/NavMeshChunkStitcher.h"
#include "World/Level/AI/AiVirtualizationTypes.h"
#include "World/Level/AI/AiVirtualizationManager.h"
#include "World/Level/Script/LevelScriptTypes.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
		}
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)
}

int main()
{
	try
	{
		using namespace World;
		using namespace World::Level;
		using namespace World::Level::AI;

		// 1. ChunkHeader layout & constraints (M0)
		{
			ChunkHeader header;
			header.ChunkID = 0x123456789ABCDEF0ULL;
			header.SubspaceID = 42;
			header.BoundsMin = { -50.0f, -10.0f, -50.0f };
			header.BoundsMax = { 50.0f, 100.0f, 50.0f };
			header.StaticEntityCount = 100;
			header.DynamicSpawnerCount = 5;

			CHECK(sizeof(ChunkHeader) == 128);
			CHECK(alignof(ChunkHeader) == 16);
			CHECK(header.Magic == ChunkMagic);
			CHECK(header.Version == 1);
			CHECK(header.ChunkID == 0x123456789ABCDEF0ULL);
			CHECK(header.SubspaceID == 42);

			CHECK(sizeof(StaticEntityRecord) == 80);
			CHECK(alignof(StaticEntityRecord) == 16);
		}

		// 2. DeltaStateRegistry (M0)
		{
			DeltaStateRegistry registry;
			CHECK(registry.DestroyedCount() == 0);
			CHECK(registry.PropertyCount() == 0);

			registry.MarkDestroyed(1001);
			registry.MarkDestroyed(1002);
			CHECK(registry.IsDestroyed(1001));
			CHECK(registry.IsDestroyed(1002));
			CHECK(!registry.IsDestroyed(1003));
			CHECK(registry.DestroyedCount() == 2);

			registry.SetProperty(1003, "Health", "50.5");
			registry.SetProperty(1003, "State", "Stunned");
			CHECK(registry.HasProperty(1003, "Health"));
			CHECK(registry.GetProperty(1003, "Health") == "50.5");
			CHECK(registry.GetProperty(1003, "State") == "Stunned");
			CHECK(registry.GetProperty(1003, "Missing", "Default") == "Default");

			registry.Clear();
			CHECK(registry.DestroyedCount() == 0);
			CHECK(registry.PropertyCount() == 0);
			CHECK(!registry.IsDestroyed(1001));
		}

		// 3. EntityDescriptor (OFPA single entity serialization)
		const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "worldengine-level2-m2-tests";
		std::filesystem::remove_all(tempDir);
		std::filesystem::create_directories(tempDir);

		{
			EntityDescriptor original;
			original.EntityUUID = 987654321ULL;
			original.Name = "TestEnemy_Orc";
			original.ChunkID = 101;
			original.DataLayerMask = 0x3;
			original.Position = { 10.5f, 0.0f, -25.25f };
			original.RotationEuler = { 0.0f, 45.0f, 0.0f };
			original.Scale = { 1.5f, 1.5f, 1.5f };
			original.SetProperty("Faction", "Horde");
			original.SetProperty("AI_Behavior", "Patrol");

			const std::filesystem::path entityFile = tempDir / "orc.entity";
			std::string error;
			CHECK(original.Save(entityFile, &error));
			CHECK(error.empty());
			CHECK(std::filesystem::exists(entityFile));

			EntityDescriptor loaded;
			CHECK(loaded.Load(entityFile, &error));
			CHECK(error.empty());
			CHECK(loaded.EntityUUID == 987654321ULL);
			CHECK(loaded.Name == "TestEnemy_Orc");
			CHECK(loaded.ChunkID == 101);
			CHECK(loaded.DataLayerMask == 0x3);
			CHECK(loaded.Position.x == 10.5f);
			CHECK(loaded.Position.z == -25.25f);
			CHECK(loaded.RotationEuler.y == 45.0f);
			CHECK(loaded.Scale.x == 1.5f);
			CHECK(loaded.GetProperty("Faction") == "Horde");
			CHECK(loaded.GetProperty("AI_Behavior") == "Patrol");
		}

		// 4. LevelCooker: Cook Package to Binary .wchunk (M1)
		const std::filesystem::path cookedDir = tempDir / "CookedChunks";
		{
			LevelPackage package;
			package.SetName("Barrens");
			package.SetGridCellSize(64);

			EntityDescriptor e1;
			e1.EntityUUID = 2001;
			e1.Name = "GuardTower";
			e1.Position = { 10.0f, 0.0f, 10.0f };
			e1.ChunkID = 999;

			EntityDescriptor e2;
			e2.EntityUUID = 2002;
			e2.Name = "SupplyCrate";
			e2.Position = { 12.0f, 0.0f, 15.0f };
			e2.ChunkID = 999;

			EntityDescriptor e3;
			e3.EntityUUID = 2003;
			e3.Name = "DistantTree";
			e3.Position = { 500.0f, 0.0f, 500.0f };
			e3.ChunkID = 888;

			package.AddEntity(e1);
			package.AddEntity(e2);
			package.AddEntity(e3);

			CookSettings settings;
			settings.GridCellSize = 64;
			settings.SubspaceID = 0;

			CookResult result = LevelCooker::CookPackage(package, cookedDir, settings);
			CHECK(result.Success);
			CHECK(result.ChunksCooked == 2);
			CHECK(result.TotalEntitiesCooked == 3);
			CHECK(result.OutputChunkFiles.size() == 2);
			CHECK(std::filesystem::exists(cookedDir / "999.wchunk"));
			CHECK(std::filesystem::exists(cookedDir / "888.wchunk"));
		}

		// 5. StreamableChunk Loading, Activation in Scene & DeltaState (M1)
		{
			WorldContext context;
			Scene scene(context);

			StreamableChunk chunk;
			std::string loadErr;
			CHECK(chunk.LoadFromFile(cookedDir / "999.wchunk", &loadErr));
			CHECK(loadErr.empty());
			CHECK(chunk.GetChunkID() == 999);
			CHECK(chunk.GetState() == ChunkState::MemoryReady);
			CHECK(chunk.GetRecordCount() == 2);

			DeltaStateRegistry deltaState;
			deltaState.MarkDestroyed(2002); // SupplyCrate destroyed

			chunk.OnActivateWithDelta(scene, &deltaState);
			CHECK(chunk.GetState() == ChunkState::Active);
			CHECK(chunk.GetSpawnedCount() == 1);

			chunk.OnDeactivate(scene);
			CHECK(chunk.GetState() == ChunkState::MemoryReady);
			CHECK(chunk.GetSpawnedCount() == 0);
		}

		// 6. SpatialGridDriver & Hysteresis Buffer Streaming (M2)
		{
			SpatialGridDriver driver(100.0f, 150.0f);
			driver.AddChunk(10, ChunkBounds{ { 0.0f, 0.0f, 0.0f }, { 50.0f, 10.0f, 50.0f } });
			driver.AddChunk(20, ChunkBounds{ { 300.0f, 0.0f, 300.0f }, { 350.0f, 10.0f, 350.0f } });
			CHECK(driver.GetTrackedChunkCount() == 2);

			std::vector<uint64_t> toLoad;
			std::vector<uint64_t> toUnload;

			// Player at (25, 0, 25): Inside Chunk 10 (< 100m)
			driver.EvaluateStreaming({ 25.0f, 0.0f, 25.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.size() == 1 && toLoad[0] == 10);
			CHECK(toUnload.empty());

			// Player at (120, 0, 25): Distance ~70m to Chunk 10 (< 100m, already loaded), distance to Chunk 20 is > 200m
			toLoad.clear();
			toUnload.clear();
			driver.EvaluateStreaming({ 120.0f, 0.0f, 25.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.empty());
			CHECK(toUnload.empty()); // In safe buffer, zero thrashing!

			// Player moves to (180, 0, 25): Distance to Chunk 10 is 130m (inside hysteresis band 100~150m)
			toLoad.clear();
			toUnload.clear();
			driver.EvaluateStreaming({ 180.0f, 0.0f, 25.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.empty());
			CHECK(toUnload.empty()); // Still no unload!

			// Player moves far away to (250, 0, 25): Distance to Chunk 10 is 200m (> 150m unload radius)
			toLoad.clear();
			toUnload.clear();
			driver.EvaluateStreaming({ 250.0f, 0.0f, 25.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.empty());
			CHECK(toUnload.size() == 1 && toUnload[0] == 10);
		}

		// 7. WorldSpace & Origin Rebasing (M2)
		{
			WorldContext context;
			Scene scene(context);

			Entity e = Entity::CreateEntity(&scene, "Rock");
			auto& tc = e.AddComponent<TransformComponent>(glm::vec3(1050.0f, 10.0f, 1050.0f));

			WorldSpace space(1000.0f);
			CHECK(space.NeedsRebase({ 1050.0f, 0.0f, 1050.0f }));
			CHECK(!space.NeedsRebase({ 500.0f, 0.0f, 500.0f }));

			glm::dvec3 worldPosBefore = space.ToWorldCoordinates(tc.Location);

			// Rebase to (1000, 0, 1000)
			space.Rebase(scene, { 1000.0f, 0.0f, 1000.0f });

			CHECK(space.GetOriginOffset().x == 1000.0);
			CHECK(space.GetOriginOffset().z == 1000.0);
			CHECK(tc.Location.x == 50.0f);
			CHECK(tc.Location.y == 10.0f);
			CHECK(tc.Location.z == 50.0f);

			glm::dvec3 worldPosAfter = space.ToWorldCoordinates(tc.Location);
			CHECK(worldPosBefore.x == worldPosAfter.x);
			CHECK(worldPosBefore.y == worldPosAfter.y);
			CHECK(worldPosBefore.z == worldPosAfter.z);
		}

		// 8. NavMeshChunkStitcher (M2)
		{
			NavMeshChunkStitcher stitcher;
			CHECK(stitcher.GetActiveTileCount() == 0);
			CHECK(stitcher.GetStitchedBorderCount() == 0);

			NavMeshTileHeader th;
			NavMeshTileKey k1{ 101, 0, 0, 0 };
			NavMeshTileKey k2{ 102, 1, 0, 0 }; // Adjacent horizontally
			NavMeshTileKey k3{ 103, 5, 5, 0 }; // Far away

			stitcher.RegisterTile(101, k1, th);
			CHECK(stitcher.GetActiveTileCount() == 1);
			CHECK(stitcher.GetTileState(k1) == TileStitchState::Loaded);
			CHECK(stitcher.GetStitchedBorderCount() == 0);

			stitcher.RegisterTile(102, k2, th);
			CHECK(stitcher.GetActiveTileCount() == 2);
			CHECK(stitcher.GetTileState(k1) == TileStitchState::Stitched);
			CHECK(stitcher.GetTileState(k2) == TileStitchState::Stitched);
			CHECK(stitcher.GetStitchedBorderCount() == 1);

			stitcher.RegisterTile(103, k3, th);
			CHECK(stitcher.GetTileState(k3) == TileStitchState::Loaded);
			CHECK(stitcher.GetStitchedBorderCount() == 1);

			// Unregister tile 102: border detaches
			stitcher.UnregisterTile(102);
			CHECK(stitcher.GetActiveTileCount() == 2);
			CHECK(stitcher.GetTileState(k1) == TileStitchState::Loaded);
			CHECK(stitcher.GetStitchedBorderCount() == 0);
		}

		// 9. AiVirtualizationManager & Offline Delta Simulation (M2)
		{
			AiVirtualizationManager aiManager;
			aiManager.RegisterAi(5001, 101, { 10.0f, 0.0f, 10.0f }, 100.0f, 1);
			aiManager.RegisterAi(5002, 101, { 80.0f, 0.0f, 80.0f }, 100.0f, 2);
			CHECK(aiManager.GetTrackedAiCount() == 2);

			// Player at (0, 0, 0)
			aiManager.UpdateLOD({ 0.0f, 0.0f, 0.0f }, 50.0f, 120.0f);
			CHECK(aiManager.GetAiTier(5001) == AiLodTier::Tier1_Active);    // dist ~14m (< 50m)
			CHECK(aiManager.GetAiTier(5002) == AiLodTier::Tier2_Hibernated); // dist ~113m (50~120m)

			// Chunk 101 unloaded: snapshots stored in DeltaState
			DeltaStateRegistry deltaState;
			aiManager.OnChunkUnloaded(101, deltaState);
			CHECK(aiManager.GetTrackedAiCount() == 0);
			CHECK(deltaState.HasProperty(5001, "AI_Health"));
			CHECK(deltaState.GetProperty(5001, "AI_Health") == "100.000000");

			// Simulate offline damage to virtualized AI
			deltaState.SetProperty(5001, "AI_Health", "45.000000");

			// Chunk 101 loaded again: AI restored with updated health
			std::vector<AiVirtualSnapshot> restored;
			aiManager.OnChunkLoaded(101, deltaState, restored);
			CHECK(restored.size() == 2);
			CHECK(aiManager.GetTrackedAiCount() == 2);

			auto it = std::find_if(restored.begin(), restored.end(), [](const AiVirtualSnapshot& s) { return s.EntityUUID == 5001; });
			CHECK(it != restored.end());
			CHECK(it->Health == 45.0f);
		}

		std::filesystem::remove_all(tempDir);
		std::printf("[LevelStreamingTests] All Level 2.0 M0 + M1 + M2 tests passed successfully!\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[LevelStreamingTests] FAILED: %s\n", e.what());
		return 1;
	}
}
