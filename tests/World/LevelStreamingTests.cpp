#include "World/Level/Core/ChunkTypes.h"
#include "World/Level/Core/ChunkHeader.h"
#include "World/Level/Core/DeltaState.h"
#include "World/Level/Core/WorldSpace.h"
#include "World/Level/Authoring/EntityDescriptor.h"
#include "World/Level/Authoring/LevelCatalog.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Level/Cooking/LevelCooker.h"
#include "World/Level/Runtime/StreamableChunk.h"
#include "World/Level/Core/ChunkIndex.h"
#include "World/Level/Runtime/ChunkStreamingCoordinator.h"
#include "World/Level/Runtime/LevelStreamingRuntime.h"
#include "World/Level/Runtime/DataLayerManager.h"
#include "World/Level/Streaming/SpatialGridDriver.h"
#include "World/Level/Streaming/SectorPortalDriver.h"
#include "World/Level/AI/NavMeshTileTypes.h"
#include "World/Level/AI/NavMeshChunkStitcher.h"
#include "World/Level/AI/AiVirtualizationTypes.h"
#include "World/Level/AI/AiVirtualizationManager.h"
#include "World/Level/AI/AiPerceptionGate.h"
#include "World/Level/Script/LevelScriptTypes.h"
#include "World/Core/StringPool.h"
#include "World/Asset/ProjectManifest.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/SceneSerializer.h"
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

		// 1.ChunkHeader layout & constraints (M0)
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
			CHECK(header.Version == ChunkVersion);
			CHECK(ChunkVersion == 2);   // v2 = 带实体文档段
			CHECK(header.ChunkID == 0x123456789ABCDEF0ULL);
			CHECK(header.SubspaceID == 42);

			CHECK(sizeof(StaticEntityRecord) == 80);
			CHECK(alignof(StaticEntityRecord) == 16);
		}

		// 2.DeltaStateRegistry (M0)
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

		const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "worldengine-level2-m4-tests";
		// 14.数据层剔除:SceneRenderer 用的就是这条谓词(Scene::IsEntityInActiveDataLayers)。
		//     昼夜开关如果没有它,就只是个"翻状态但场景毫无变化"的摆设。
		{
			WorldContext context;
			Scene scene(context);

			Entity always = Entity::CreateEntity(&scene, "Floor", UUID(4001));
			always.AddComponent<TransformComponent>(glm::vec3(0.0f));
			always.AddComponent<LevelEntityComponent>(1u);        // Base_World

			Entity dayOnly = Entity::CreateEntity(&scene, "DayStatue", UUID(4002));
			dayOnly.AddComponent<TransformComponent>(glm::vec3(0.0f));
			dayOnly.AddComponent<LevelEntityComponent>(2u);       // Day

			Entity nightOnly = Entity::CreateEntity(&scene, "NightLantern", UUID(4003));
			nightOnly.AddComponent<TransformComponent>(glm::vec3(0.0f));
			nightOnly.AddComponent<LevelEntityComponent>(4u);     // Night

			// 未标注层归属的实体永远可见(旧内容不因引入数据层而消失)。
			Entity untagged = Entity::CreateEntity(&scene, "Untagged", UUID(4004));
			untagged.AddComponent<TransformComponent>(glm::vec3(0.0f));

			const entt::entity alwaysHandle = static_cast<entt::entity>(always);
			const entt::entity dayHandle = static_cast<entt::entity>(dayOnly);
			const entt::entity nightHandle = static_cast<entt::entity>(nightOnly);
			const entt::entity untaggedHandle = static_cast<entt::entity>(untagged);

			// 默认全层可见
			CHECK(scene.IsEntityInActiveDataLayers(alwaysHandle));
			CHECK(scene.IsEntityInActiveDataLayers(dayHandle));
			CHECK(scene.IsEntityInActiveDataLayers(nightHandle));
			CHECK(scene.IsEntityInActiveDataLayers(untaggedHandle));

			// 只看白天(Base + Day):夜灯被剔除
			scene.SetActiveDataLayers(1u | 2u);
			CHECK(scene.IsEntityInActiveDataLayers(alwaysHandle));
			CHECK(scene.IsEntityInActiveDataLayers(dayHandle));
			CHECK(!scene.IsEntityInActiveDataLayers(nightHandle));
			CHECK(scene.IsEntityInActiveDataLayers(untaggedHandle));

			// 只看夜晚(Base + Night):白天雕像被剔除
			scene.SetActiveDataLayers(1u | 4u);
			CHECK(scene.IsEntityInActiveDataLayers(alwaysHandle));
			CHECK(!scene.IsEntityInActiveDataLayers(dayHandle));
			CHECK(scene.IsEntityInActiveDataLayers(nightHandle));

			// 全关:只有常驻与未标注的留下
			scene.SetActiveDataLayers(0u);
			CHECK(!scene.IsEntityInActiveDataLayers(alwaysHandle));
			CHECK(!scene.IsEntityInActiveDataLayers(dayHandle));
			CHECK(!scene.IsEntityInActiveDataLayers(nightHandle));
			CHECK(scene.IsEntityInActiveDataLayers(untaggedHandle));
		}

		std::filesystem::remove_all(tempDir);
		std::filesystem::create_directories(tempDir);

		// 3.EntityDescriptor:OFPA 单实体文件必须携带**全部组件**,不只是 Transform。
		//    这里用真实场景实体走一遍 SceneSerializer 的实体级读写 —— 与编辑器/.wd 同一条口径。
		std::string doc;
		std::string docError;
		{
			WorldContext context;
			Scene source(context);

			Entity cube = Entity::CreateEntity(&source, "Mesh:Cube Orange", UUID(987654321ULL));
			cube.AddComponent<TransformComponent>(glm::vec3 { 10.5f, 0.5f, -25.25f },
				glm::vec3 { 0.0f, 45.0f, 0.0f }, glm::vec3 { 1.5f, 1.5f, 1.5f });
			MeshRendererComponent renderer;
			renderer.Primitive = MeshRendererComponent::PrimitiveShape::Cube;
			renderer.Color = glm::vec4 { 1.0f, 0.55f, 0.12f, 1.0f };
			cube.AddComponent<MeshRendererComponent>(renderer);
			// 数据层归属是**实体内嵌组件**(唯一事实源),不是文件级元数据。
			cube.AddComponent<LevelEntityComponent>(0x3u);

			CHECK(SceneSerializer::SerializeEntityDocumentToYaml(cube, &doc, &docError));
			CHECK(docError.empty());
			// 组件随文档走:MeshRenderer 必须在文档里(这正是旧实现丢掉的载荷)。
			CHECK(doc.find("World::MeshRendererComponent") != std::string::npos);
			CHECK(doc.find("World::TransformComponent") != std::string::npos);
			CHECK(doc.find("World::LevelEntityComponent") != std::string::npos);

			EntityDescriptor descriptor;
			descriptor.DocumentYaml = doc;
			descriptor.ChunkID = 101;
			descriptor.DataLayerMask = 0x3;
			descriptor.RefreshDerivedFromDocument();

			CHECK(descriptor.EntityUUID == 987654321ULL);
			CHECK(descriptor.Name == "Mesh:Cube Orange");
			CHECK(descriptor.Position.x == 10.5f);
			CHECK(descriptor.Position.z == -25.25f);
			CHECK(descriptor.Scale.x == 1.5f);

			const std::filesystem::path entityFile = tempDir / "cube.entity";
			std::string error;
			CHECK(descriptor.Save(entityFile, &error));
			CHECK(error.empty());
			CHECK(std::filesystem::exists(entityFile));

			EntityDescriptor loaded;
			CHECK(loaded.Load(entityFile, &error));
			CHECK(error.empty());
			CHECK(loaded.EntityUUID == 987654321ULL);
			CHECK(loaded.ChunkID == 101);
			CHECK(loaded.DataLayerMask == 0x3);
			CHECK(loaded.Name == "Mesh:Cube Orange");
			CHECK(loaded.Position.x == 10.5f);
			CHECK(loaded.Scale.x == 1.5f);

			// 并入一个**空场景**:组件必须真的回来(名字 + Transform + MeshRenderer)。
			Scene target(context);
			CHECK(SceneSerializer::AppendEntityYaml(target, loaded.DocumentYaml, &error) == 1);
			SceneSerializer::FinalizeEntityImport(target);

			auto& registry = target.GetRegistry();
			auto view = registry.view<UUIDComponent, TagComponent, TransformComponent, MeshRendererComponent>();
			size_t restored = 0;
			for (const entt::entity handle : view)
			{
				++restored;
				CHECK(registry.get<UUIDComponent>(handle).ID == UUID(987654321ULL));
				CHECK(StringPool::Get().NameOf(registry.get<TagComponent>(handle).Tag) == "Mesh:Cube Orange");
				CHECK(registry.get<TransformComponent>(handle).Location.x == 10.5f);
				CHECK(registry.get<MeshRendererComponent>(handle).Primitive
					== MeshRendererComponent::PrimitiveShape::Cube);
			}
			CHECK(restored == 1);
		}

		// 4.LevelCooker:由真实场景迁移出的关卡包 → 二进制 .wchunk
		const std::filesystem::path cookedDir = tempDir / "CookedChunks";
		{
			WorldContext context;
			Scene source(context);

			// 带组件(不只是 Transform)的实体:块里必须把 MeshRenderer 一起带上。
			MeshRendererComponent mesh;
			mesh.Primitive = MeshRendererComponent::PrimitiveShape::Cube;

			Entity tower = Entity::CreateEntity(&source, "GuardTower", UUID(2001));
			tower.AddComponent<TransformComponent>(glm::vec3 { 10.0f, 0.0f, 10.0f });
			tower.AddComponent<MeshRendererComponent>(mesh);

			Entity crate = Entity::CreateEntity(&source, "SupplyCrate", UUID(2002));
			crate.AddComponent<TransformComponent>(glm::vec3 { 12.0f, 0.0f, 15.0f });
			crate.AddComponent<MeshRendererComponent>(mesh);

			// 无渲染组件的实体(纯逻辑/占位)也要能正确往返。
			Entity tree = Entity::CreateEntity(&source, "DistantTree", UUID(2003));
			tree.AddComponent<TransformComponent>(glm::vec3 { 500.0f, 0.0f, 500.0f });

			LevelPackage package;
			package.CaptureFromScene(source, "Barrens", 64);
			CHECK(package.GetEntityCount() == 3);

			// 显式分块:tower/crate 同块,tree 单独一块。
			for (size_t i = 0; i < package.GetEntityCount(); ++i)
			{
				EntityDescriptor& d = package.GetMutableEntity(i);
				d.ChunkID = (d.EntityUUID == 2003ULL) ? 888ULL : 999ULL;
			}

			CookSettings settings;
			settings.GridCellSize = 64;
			settings.SubspaceID = 0;

			CookResult result = LevelCooker::CookPackage(package, cookedDir, settings);
			CHECK(result.Success);
			CHECK(result.ChunksCooked == 2);
			CHECK(result.TotalEntitiesCooked == 3);
			CHECK(std::filesystem::exists(cookedDir / "999.wchunk"));
			CHECK(std::filesystem::exists(cookedDir / "888.wchunk"));
		}

		// 5.流送块载入/激活 + DeltaState 过滤 + **组件载荷**(与编辑器路径等价)
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
			// v2 块必须带组件载荷:没有它,载入结果就是一堆命名空壳。
			CHECK(chunk.HasComponentPayload());
			CHECK(chunk.GetDocumentCount() == 2);

			DeltaStateRegistry deltaState;
			deltaState.MarkDestroyed(2002);   // SupplyCrate 已被玩家打掉

			chunk.OnActivateWithDelta(scene, &deltaState);
			CHECK(chunk.GetState() == ChunkState::Active);
			CHECK(chunk.GetSpawnedCount() == 1);   // 被摧毁的那个不生成

			// 组件真的回来了:不只是名字 + Transform。
			{
				entt::registry& registry = scene.GetRegistry();
				CHECK(registry.view<UUIDComponent>().size() == 1);

				auto view = registry.view<UUIDComponent, MeshRendererComponent>();
				size_t meshes = 0;
				for (const entt::entity handle : view)
				{
					++meshes;
					CHECK(registry.get<UUIDComponent>(handle).ID == UUID(2001));
					CHECK(registry.get<MeshRendererComponent>(handle).Primitive
						== MeshRendererComponent::PrimitiveShape::Cube);
				}
				CHECK(meshes == 1);
			}

			chunk.OnDeactivate(scene);
			CHECK(chunk.GetState() == ChunkState::MemoryReady);
			CHECK(chunk.GetSpawnedCount() == 0);
			// 卸载后场景里不留残骸。
			CHECK(scene.GetRegistry().view<UUIDComponent>().size() == 0);
		}

		// 6.SpatialGridDriver & Hysteresis Buffer Streaming (M2)
		{
			SpatialGridDriver driver(100.0f, 150.0f);
			driver.AddChunk(10, ChunkBounds{ { 0.0f, 0.0f, 0.0f }, { 50.0f, 10.0f, 50.0f } });
			driver.AddChunk(20, ChunkBounds{ { 300.0f, 0.0f, 300.0f }, { 350.0f, 10.0f, 350.0f } });
			CHECK(driver.GetTrackedChunkCount() == 2);

			std::vector<uint64_t> toLoad;
			std::vector<uint64_t> toUnload;

			driver.EvaluateStreaming({ 25.0f, 0.0f, 25.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.size() == 1 && toLoad[0] == 10);
			CHECK(toUnload.empty());

			toLoad.clear();
			toUnload.clear();
			driver.EvaluateStreaming({ 120.0f, 0.0f, 25.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.empty());
			CHECK(toUnload.empty());

			toLoad.clear();
			toUnload.clear();
			driver.EvaluateStreaming({ 180.0f, 0.0f, 25.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.empty());
			CHECK(toUnload.empty());

			toLoad.clear();
			toUnload.clear();
			driver.EvaluateStreaming({ 250.0f, 0.0f, 25.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.empty());
			CHECK(toUnload.size() == 1 && toUnload[0] == 10);
		}

		// 7.WorldSpace & Origin Rebasing (M2)
		{
			WorldContext context;
			Scene scene(context);

			Entity e = Entity::CreateEntity(&scene, "Rock");
			auto& tc = e.AddComponent<TransformComponent>(glm::vec3(1050.0f, 10.0f, 1050.0f));

			WorldSpace space(1000.0f);
			CHECK(space.NeedsRebase({ 1050.0f, 0.0f, 1050.0f }));
			CHECK(!space.NeedsRebase({ 500.0f, 0.0f, 500.0f }));

			glm::dvec3 worldPosBefore = space.ToWorldCoordinates(tc.Location);

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

		// 8.NavMeshChunkStitcher (M2)
		{
			NavMeshChunkStitcher stitcher;
			CHECK(stitcher.GetActiveTileCount() == 0);
			CHECK(stitcher.GetStitchedBorderCount() == 0);

			NavMeshTileHeader th;
			NavMeshTileKey k1{ 101, 0, 0, 0 };
			NavMeshTileKey k2{ 102, 1, 0, 0 };
			NavMeshTileKey k3{ 103, 5, 5, 0 };

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

			stitcher.UnregisterTile(102);
			CHECK(stitcher.GetActiveTileCount() == 2);
			CHECK(stitcher.GetTileState(k1) == TileStitchState::Loaded);
			CHECK(stitcher.GetStitchedBorderCount() == 0);
		}

		// 9.AiVirtualizationManager & Offline Delta Simulation (M2)
		{
			AiVirtualizationManager aiManager;
			aiManager.RegisterAi(5001, 101, { 10.0f, 0.0f, 10.0f }, 100.0f, 1);
			aiManager.RegisterAi(5002, 101, { 80.0f, 0.0f, 80.0f }, 100.0f, 2);
			CHECK(aiManager.GetTrackedAiCount() == 2);

			aiManager.UpdateLOD({ 0.0f, 0.0f, 0.0f }, 50.0f, 120.0f);
			CHECK(aiManager.GetAiTier(5001) == AiLodTier::Tier1_Active);
			CHECK(aiManager.GetAiTier(5002) == AiLodTier::Tier2_Hibernated);

			DeltaStateRegistry deltaState;
			aiManager.OnChunkUnloaded(101, deltaState);
			CHECK(aiManager.GetTrackedAiCount() == 0);
			CHECK(deltaState.HasProperty(5001, "AI_Health"));
			CHECK(deltaState.GetProperty(5001, "AI_Health") == "100.000000");

			deltaState.SetProperty(5001, "AI_Health", "45.000000");

			std::vector<AiVirtualSnapshot> restored;
			aiManager.OnChunkLoaded(101, deltaState, restored);
			CHECK(restored.size() == 2);
			CHECK(aiManager.GetTrackedAiCount() == 2);

			auto it = std::find_if(restored.begin(), restored.end(), [](const AiVirtualSnapshot& s) { return s.EntityUUID == 5001; });
			CHECK(it != restored.end());
			CHECK(it->Health == 45.0f);
		}

		// 10.SectorPortalDriver (M3)
		{
			SectorPortalDriver portalDriver;
			portalDriver.AddSector(1, ChunkBounds{ { 0.0f, 0.0f, 0.0f }, { 10.0f, 5.0f, 10.0f } });
			portalDriver.AddSector(2, ChunkBounds{ { 10.0f, 0.0f, 0.0f }, { 20.0f, 5.0f, 10.0f } });
			portalDriver.AddSector(3, ChunkBounds{ { 20.0f, 0.0f, 0.0f }, { 30.0f, 5.0f, 10.0f } });

			portalDriver.AddPortal(1, 2, { 10.0f, 2.5f, 5.0f }, { 0.1f, 2.0f, 1.0f }, true);
			portalDriver.AddPortal(2, 3, { 20.0f, 2.5f, 5.0f }, { 0.1f, 2.0f, 1.0f }, false);

			CHECK(portalDriver.GetSectorCount() == 3);
			CHECK(portalDriver.GetPortalCount() == 2);

			std::vector<uint64_t> toLoad;
			std::vector<uint64_t> toUnload;

			portalDriver.EvaluateStreaming({ 5.0f, 2.5f, 5.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.size() == 2);
			CHECK(std::find(toLoad.begin(), toLoad.end(), 1) != toLoad.end());
			CHECK(std::find(toLoad.begin(), toLoad.end(), 2) != toLoad.end());
			CHECK(std::find(toLoad.begin(), toLoad.end(), 3) == toLoad.end());

			toLoad.clear();
			toUnload.clear();
			portalDriver.SetPortalOpen(2, 3, true);
			portalDriver.EvaluateStreaming({ 5.0f, 2.5f, 5.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toLoad.size() == 1 && toLoad[0] == 3);

			toLoad.clear();
			toUnload.clear();
			portalDriver.SetPortalOpen(1, 2, false);
			portalDriver.EvaluateStreaming({ 5.0f, 2.5f, 5.0f }, glm::mat4(1.0f), toLoad, toUnload);
			CHECK(toUnload.size() == 2);
			CHECK(std::find(toUnload.begin(), toUnload.end(), 2) != toUnload.end());
			CHECK(std::find(toUnload.begin(), toUnload.end(), 3) != toUnload.end());
		}

		// 11.DataLayerManager (M3)
		{
			DataLayerManager layerMgr;
			layerMgr.RegisterLayer(1, "Base_World", true);
			layerMgr.RegisterLayer(2, "Day", true);
			layerMgr.RegisterLayer(4, "Night", false);
			CHECK(layerMgr.GetRegisteredLayerCount() == 3);

			CHECK(layerMgr.GetActiveMask() == (1 | 2));
			CHECK(layerMgr.IsEntityVisible(1));
			CHECK(layerMgr.IsEntityVisible(2));
			CHECK(!layerMgr.IsEntityVisible(4));
			CHECK(layerMgr.IsEntityVisible(1 | 4));

			layerMgr.SetLayerActive("Day", false);
			layerMgr.SetLayerActive("Night", true);
			CHECK(layerMgr.GetActiveMask() == (1 | 4));
			CHECK(!layerMgr.IsEntityVisible(2));
			CHECK(layerMgr.IsEntityVisible(4));
		}

		// 12.AiPerceptionGate: Raycast Obstruction via Unloaded Space (M3)
		{
			AiPerceptionGate gate;
			gate.RegisterChunk(10, ChunkBounds{ { 0.0f, 0.0f, 0.0f }, { 50.0f, 10.0f, 50.0f } }, true);
			gate.RegisterChunk(20, ChunkBounds{ { 60.0f, 0.0f, 0.0f }, { 100.0f, 10.0f, 50.0f } }, false);

			uint64_t blockedChunk = 0;
			PerceptionStatus res1 = gate.TestRay({ 10.0f, 5.0f, 25.0f }, { 40.0f, 5.0f, 25.0f }, &blockedChunk);
			CHECK(res1 == PerceptionStatus::Clear);

			PerceptionStatus res2 = gate.TestRay({ 10.0f, 5.0f, 25.0f }, { 80.0f, 5.0f, 25.0f }, &blockedChunk);
			CHECK(res2 == PerceptionStatus::BlockedByUnloadedChunk);
			CHECK(blockedChunk == 20);

			gate.SetChunkLoaded(20, true);
			PerceptionStatus res3 = gate.TestRay({ 10.0f, 5.0f, 25.0f }, { 80.0f, 5.0f, 25.0f }, &blockedChunk);
			CHECK(res3 == PerceptionStatus::Clear);
		}

		// 13.关卡包往返:场景 → 包(逐实体文件)→ 二进制烘焙 → 流送块载入
		{
			WorldContext context;
			Scene sourceScene(context);

			Entity e1 = Entity::CreateEntity(&sourceScene, "PlayerStart", UUID(3001));
			e1.AddComponent<TransformComponent>(glm::vec3(0.0f, 1.0f, 0.0f));

			Entity e2 = Entity::CreateEntity(&sourceScene, "BossMonster", UUID(3002));
			e2.AddComponent<TransformComponent>(glm::vec3(250.0f, 0.0f, 250.0f));

			LevelPackage package;
			package.CaptureFromScene(sourceScene, "Dungeon", 128);
			CHECK(package.GetName() == "Dungeon");
			CHECK(package.GetGridCellSize() == 128);
			CHECK(package.GetEntityCount() == 2);

			// 关卡 = 入口文件(`scenes/<Id>.wlevel`)+ 内容目录(`levels/<Id>/`)。
			const std::filesystem::path pkgEntry = tempDir / "scenes" / "Dungeon.wlevel";
			const std::filesystem::path pkgDir = tempDir / "levels" / "Dungeon";
			std::string expError;
			CHECK(package.Save(pkgEntry, &expError));
			CHECK(expError.empty());
			CHECK(std::filesystem::exists(pkgEntry));
			CHECK(std::filesystem::exists(pkgDir / "entities" / "3001.entity"));
			CHECK(std::filesystem::exists(pkgDir / "entities" / "3002.entity"));

			// 剪枝:删掉一个实体再存,磁盘上的旧 `.entity` 必须消失(否则重载会"复活"它)。
			LevelPackage pruned;
			CHECK(pruned.Load(pkgEntry, &expError));
			CHECK(pruned.GetEntityCount() == 2);
			EntityDescriptor kept = pruned.GetEntity(0);
			LevelPackage slim;
			slim.SetName("Dungeon");
			slim.SetGridCellSize(128);
			slim.AddEntity(kept);
			CHECK(slim.Save(pkgEntry, &expError));
			{
				size_t entityFiles = 0;
				for (const auto& entry : std::filesystem::directory_iterator(pkgDir / "entities"))
					if (entry.path().extension() == ".entity")
						++entityFiles;
				CHECK(entityFiles == 1);
			}

			// 烘焙 → 流送块
			CookResult cookRes = LevelCooker::CookPackage(slim, pkgDir / "cooked", { 128, 0 });
			CHECK(cookRes.Success);
			CHECK(cookRes.ChunksCooked >= 1);
			CHECK(cookRes.TotalEntitiesCooked == 1);

			StreamableChunk migChunk;
			CHECK(migChunk.LoadFromFile(cookRes.OutputChunkFiles[0]));
			CHECK(migChunk.GetState() == ChunkState::MemoryReady);
		}

		// 15.流送索引 + LevelStreamingRuntime:相机推进 → 按空间装/卸、块真的长出实体。
		{
			// 复用第 4 步烘出的块(999 两块 + 888 一块),补一份索引。
			ChunkIndex index;
			{
				StreamableChunk probe;
				CHECK(probe.LoadFromFile(cookedDir / "999.wchunk"));
				ChunkIndexEntry entry;
				entry.ChunkID = probe.GetChunkID();
				entry.File = "999.wchunk";
				entry.Bounds = probe.GetBounds();
				entry.EntityCount = static_cast<uint32_t>(probe.GetRecordCount());
				entry.DataLayerMask = 1u;
				index.Entries().push_back(entry);
			}
			CHECK(index.Entries().size() == 1);
			CHECK(index.Entries()[0].File == "999.wchunk");

			const std::filesystem::path levelDir = tempDir / "StreamLevel";
			std::filesystem::create_directories(levelDir / "cooked");
			std::string indexError;
			CHECK(ChunkIndex::Save(levelDir / "cooked" / ChunkIndex::FileName, index, &indexError));
			CHECK(indexError.empty());

			// 往返:存了再读,条目与包围盒必须一致(发行包只靠这份索引,读错就等于流送错地方)。
			ChunkIndex reloaded;
			CHECK(ChunkIndex::LoadFromFile(levelDir / "cooked" / ChunkIndex::FileName, &reloaded, &indexError));
			CHECK(reloaded.Entries().size() == 1);
			CHECK(reloaded.Entries()[0].ChunkID == index.Entries()[0].ChunkID);
			CHECK(reloaded.Entries()[0].Bounds.Min.x == index.Entries()[0].Bounds.Min.x);
			CHECK(reloaded.Entries()[0].Bounds.Max.z == index.Entries()[0].Bounds.Max.z);

			// 块文件放到索引同目录(ResolveFile 的口径)。
			std::filesystem::copy_file(cookedDir / "999.wchunk", levelDir / "cooked" / "999.wchunk",
				std::filesystem::copy_options::overwrite_existing);

			WorldContext context;
			LevelStreamingRuntime streaming;
			std::string initError;
			CHECK(streaming.Initialize(context, "StreamLevel", levelDir, &initError));
			// 本组验证的是**空间裁决与状态机**,用同步读让"一帧内装上"可断言;
			// 异步跨帧的收敛由后面的异步组单独覆盖。
			streaming.SetAsyncIO(false);
			CHECK(initError.empty());
			CHECK(streaming.IsReady());
			CHECK(streaming.GetLevelId() == "StreamLevel");
			CHECK(streaming.TotalChunks() == 1);
			CHECK(streaming.ActiveChunks() == 0);

			Scene scene(context);
			streaming.SetScene(&scene);
			DeltaStateRegistry deltaState;
			streaming.SetDeltaState(&deltaState);

			// ① 远视距:相机离块很远 ⇒ 不加载、不激活。
			streaming.Update(Timestep(0.016f), glm::vec3(10000.0f, 0.0f, 10000.0f), glm::mat4(1.0f));
			CHECK(streaming.LoadedChunks() == 0);
			CHECK(streaming.ActiveChunks() == 0);

			// ② 进入视距:块被读进内存并激活,实体出现在场景里。
			streaming.Update(Timestep(0.016f), glm::vec3(10.0f, 0.0f, 10.0f), glm::mat4(1.0f));
			CHECK(streaming.LoadedChunks() == 1);
			CHECK(streaming.ActiveChunks() == 1);
			CHECK(streaming.ActiveEntityCount() == 2);
			CHECK(scene.GetRegistry().view<UUIDComponent>().size() == 2);

			// ③ 走出去再回来:离开视距卸载,回到近处重新激活(且不重复累加实体)。
			streaming.Update(Timestep(0.016f), glm::vec3(10000.0f, 0.0f, 10000.0f), glm::mat4(1.0f));
			CHECK(streaming.ActiveChunks() == 0);
			CHECK(streaming.ActiveEntityCount() == 0);

			streaming.Update(Timestep(0.016f), glm::vec3(10.0f, 0.0f, 10.0f), glm::mat4(1.0f));
			CHECK(streaming.ActiveChunks() == 1);
			CHECK(scene.GetRegistry().view<UUIDComponent>().size() == 2);

			// ④ 索引缺失 = 未烘焙:必须**拒绝**而不是静默当成空关卡。
			LevelStreamingRuntime uninitialized;
			std::string missingError;
			CHECK(!uninitialized.Initialize(context, "NoSuchLevel", tempDir / "NoSuchLevel", &missingError));
			CHECK(!missingError.empty());
			CHECK(!uninitialized.IsReady());
		}

		// 16.流送参数进项目清单:往返 + 非法配置必须在**加载期**被拒(不是运行期抖动)。
		{
			const std::filesystem::path manifestPath = tempDir / "project.we.yaml";
			Asset::ProjectManifest manifest;
			manifest.Id = "com.test.streaming";
			manifest.StartScene = "levels/Arena";
			manifest.Packages.push_back("content.wpak");
			manifest.Streaming.ViewDistance = 96.0f;
			manifest.Streaming.UnloadDistance = 128.0f;
			manifest.Streaming.TimeBudgetMs = 1.5f;
			manifest.Streaming.AsyncIO = false;

			std::string manifestError;
			CHECK(Asset::ProjectManifest::Save(manifestPath, manifest, &manifestError));
			CHECK(manifestError.empty());

			Asset::ProjectManifest reloaded;
			CHECK(Asset::ProjectManifest::Load(manifestPath, &reloaded, &manifestError));
			CHECK(manifestError.empty());
			CHECK(reloaded.Streaming.ViewDistance == 96.0f);
			CHECK(reloaded.Streaming.UnloadDistance == 128.0f);
			CHECK(reloaded.Streaming.TimeBudgetMs == 1.5f);
			CHECK(reloaded.Streaming.AsyncIO == false);

			// ① 卸载视距 ≤ 视距 = 滞后缓冲带不存在 ⇒ 必然在边界反复装/卸。加载期必须拒。
			{
				const std::filesystem::path badPath = tempDir / "bad-streaming.we.yaml";
				std::ofstream(badPath) <<
					"id: com.test.bad\nversion: 1.0.0\ncontent_root: assets\n"
					"start_scene: levels/Arena\n"
					"streaming:\n  view_distance: 200\n  unload_distance: 100\n";
				Asset::ProjectManifest rejected;
				std::string rejectedError;
				CHECK(!Asset::ProjectManifest::Load(badPath, &rejected, &rejectedError));
				CHECK(!rejectedError.empty());
			}

			// ② 视距 ≤ 0 同样拒绝。
			{
				const std::filesystem::path badPath = tempDir / "bad-view.we.yaml";
				std::ofstream(badPath) <<
					"id: com.test.bad2\nversion: 1.0.0\ncontent_root: assets\n"
					"start_scene: levels/Arena\n"
					"streaming:\n  view_distance: 0\n  unload_distance: 64\n";
				Asset::ProjectManifest rejected;
				std::string rejectedError;
				CHECK(!Asset::ProjectManifest::Load(badPath, &rejected, &rejectedError));
			}

			// ③ 缺省清单(无 streaming 块)保持默认 —— 旧项目不因新增区块而改变行为。
			{
				const std::filesystem::path plainPath = tempDir / "plain.we.yaml";
				std::ofstream(plainPath) <<
					"id: com.test.plain\nversion: 1.0.0\ncontent_root: assets\n"
					"start_scene: levels/Arena\n";
				Asset::ProjectManifest loadedPlain;
				std::string plainError;
				CHECK(Asset::ProjectManifest::Load(plainPath, &loadedPlain, &plainError));
				CHECK(loadedPlain.Streaming.IsDefault());
			}
		}

		// 17.异步流送:后台预读 + 主线程实例化,必须跨帧收敛且不重复实例化。
		{
			JobSystem::Init(2);

			ChunkIndex index;
			{
				StreamableChunk probe;
				CHECK(probe.LoadFromFile(cookedDir / "888.wchunk"));
				ChunkIndexEntry entry;
				entry.ChunkID = probe.GetChunkID();
				entry.File = "888.wchunk";
				entry.Bounds = probe.GetBounds();
				entry.EntityCount = static_cast<uint32_t>(probe.GetRecordCount());
				index.Entries().push_back(entry);
			}

			const std::filesystem::path levelDir = tempDir / "AsyncLevel";
			std::filesystem::create_directories(levelDir / "cooked");
			std::string indexError;
			CHECK(ChunkIndex::Save(levelDir / "cooked" / ChunkIndex::FileName, index, &indexError));
			std::filesystem::copy_file(cookedDir / "888.wchunk", levelDir / "cooked" / "888.wchunk",
				std::filesystem::copy_options::overwrite_existing);

			WorldContext context;
			Scene scene(context);

			LevelStreamingRuntime streaming;
			std::string initError;
			CHECK(streaming.Initialize(context, "AsyncLevel", levelDir, &initError));
			streaming.SetAsyncIO(true);
			streaming.SetScene(&scene);

			// 上界放宽:读盘 + 线程调度在负载机器上可能占若干帧。一旦"待预读归零"就该已激活 ——
			// 那才是收敛的定义;固定 1~2 帧的断言在慢机器上是假失败。
			for (int frame = 0;
				frame < 200 && !(streaming.ActiveChunks() == 1 && streaming.PendingPrefetches() == 0);
				++frame)
			{
				streaming.Update(Timestep(0.016f), glm::vec3(500.0f, 0.0f, 500.0f), glm::mat4(1.0f));
			}

			CHECK(streaming.ActiveChunks() == 1);
			CHECK(streaming.PendingPrefetches() == 0);
			const size_t asyncEntities = streaming.ActiveEntityCount();
			CHECK(asyncEntities == 1);

			// 稳定后再跑若干帧:不得重复实例化(异步最典型的 bug 是同一块被装两次)。
			for (int frame = 0; frame < 16; ++frame)
				streaming.Update(Timestep(0.016f), glm::vec3(500.0f, 0.0f, 500.0f), glm::mat4(1.0f));
			CHECK(streaming.ActiveChunks() == 1);
			CHECK(streaming.ActiveEntityCount() == asyncEntities);
			CHECK(scene.GetRegistry().view<UUIDComponent>().size() == asyncEntities);

			JobSystem::WaitAll();
			JobSystem::Shutdown();
		}

		// 18.新建关卡(唯一创建入口 `LevelPackage::CreateEmpty`):落盘 → 被目录发现 →
		//     实例化后**带起手实体**(相机/灯/方块),并可再次保存。
		{
			WorldContext context;
			const std::filesystem::path contentRoot = tempDir / "FreshProject";
			std::filesystem::create_directories(contentRoot / "scenes");

			// 入口路径是**唯一**入参:内容目录由它推导(`levels/<Id>/`)。
			const std::filesystem::path packageEntry = contentRoot / "scenes" / "Brand New Level.wlevel";
			const std::filesystem::path packageDir = contentRoot / "levels" / "Brand New Level";
			std::string createError;
			CHECK(LevelPackage::CreateEmpty(context, packageEntry, "Brand New Level", &createError));
			CHECK(createError.empty());
			CHECK(std::filesystem::exists(packageEntry));
			CHECK(std::filesystem::is_directory(packageDir / "entities"));

			// 起手实体真的落了盘(每实体一个文件)。
			{
				size_t entityFiles = 0;
				for (const auto& entry : std::filesystem::directory_iterator(packageDir / "entities"))
					if (entry.path().extension() == ".entity")
						++entityFiles;
				CHECK(entityFiles == 3);
			}

			// 目录结构即注册表:新建出来立刻能被发现,不需要改任何清单。
			const std::vector<LevelInfo> discovered = LevelCatalog::Discover(contentRoot);
			CHECK(discovered.size() == 1);
			CHECK(discovered[0].Id == "Brand New Level");
			CHECK(discovered[0].DisplayName == "Brand New Level");

			// 实例化:起手实体带**组件**回来(相机 / 方向光 / 网格),不是空壳。
			LevelPackage loaded;
			std::string loadError;
			CHECK(loaded.Load(packageEntry, &loadError));
			Ref<Scene> scene = loaded.Instantiate(context);
			CHECK(scene != nullptr);
			{
				entt::registry& registry = scene->GetRegistry();
				CHECK(registry.view<CameraComponent>().size() == 1);
				CHECK(registry.view<DirectionalLightComponent>().size() == 1);
				CHECK(registry.view<MeshRendererComponent>().size() == 1);
			}

			// 幂等保护:已存在且非空的目录必须拒绝,不能覆盖别人的关卡。
			std::string clobberError;
			CHECK(!LevelPackage::CreateEmpty(context, packageEntry, "Brand New Level", &clobberError));
			CHECK(!clobberError.empty());

			// 保存往返:改了场景再存回同一个包,仍然是 3 个实体(不重复、不丢)。
			{
				Entity extra = Entity::CreateEntity(scene.get(), "ExtraProp");
				extra.AddComponent<TransformComponent>(glm::vec3 { 5.0f, 0.0f, 5.0f });
				LevelPackage repacked;
				repacked.CaptureFromScene(*scene, "Brand New Level", 64);
				CHECK(repacked.GetEntityCount() == 4);
				CHECK(repacked.Save(packageEntry, &loadError));

				LevelPackage reopened;
				CHECK(reopened.Load(packageEntry, &loadError));
				CHECK(reopened.GetEntityCount() == 4);
			}
		}

		std::filesystem::remove_all(tempDir);
		std::printf("[LevelStreamingTests] All Level 2.0 M0 + M1 + M2 + M3 + M4 tests passed successfully!\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[LevelStreamingTests] FAILED: %s\n", e.what());
		return 1;
	}
}
