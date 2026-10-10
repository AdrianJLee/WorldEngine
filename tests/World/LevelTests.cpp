// Level 2.0 关卡:入口文件(`scenes/<Id>.wlevel`,存 manifest)+ 内容目录
// (`levels/<Id>/entities/<uuid>.entity`,一实体一文件)。
//
//   1. LevelCatalog 的发现与**成对校验**(入口文件名 == manifest.Id、内容目录必须存在)
//   2. LevelService 的加载状态机与场景栈
//   3. 场景级往返 —— 旧关卡路径三样全丢的地方:World 块 / prefab 实例块 / 未注册组件片段
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Framework/LevelService.h"
#include "World/Level/Authoring/EntityDescriptor.h"
#include "World/Level/Authoring/LevelCatalog.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Gameplay/Prefab/Prefab.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void WriteRawFile(const std::filesystem::path& path, const std::string& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
			throw std::runtime_error("cannot write " + path.string());
		out << text;
	}

	// 造一个最小关卡:入口 `.wlevel` + `levels/<id>/entities/<uuid>.entity`(带 MeshRenderer)。
	void WriteLevel(const std::filesystem::path& contentRoot, const std::string& id,
		const std::string& displayName, uint64_t entityUuid)
	{
		World::Level::LevelPackage package;
		package.SetId(id);
		package.SetName(displayName);
		package.SetGridCellSize(64);
		package.GetDataLayers().push_back({ 1u, "Base_World", true });

		World::Level::EntityDescriptor descriptor;
		descriptor.DocumentYaml =
			"Entities:\n"
			"  - World::UUIDComponent:\n"
			"      ID:\n"
			"        m_UUID: " + std::to_string(entityUuid) + "\n"
			"    World::TagComponent:\n"
			"      Tag: Prop\n"
			"    World::TransformComponent:\n"
			"      Location: [1, 2, 3]\n"
			"      Rotation: [0, 0, 0]\n"
			"      Scale: [1, 1, 1]\n"
			"    World::MeshRendererComponent:\n"
			"      Primitive: 0\n"
			"      Color: [1, 1, 1, 1]\n"
			"      Mesh: \"\"\n"
			"    World::LevelEntityComponent:\n"
			"      DataLayerMask: 1\n";
		package.AddEntity(descriptor);

		std::string error;
		if (!package.Save(World::Level::LevelCatalog::EntryPathFor(contentRoot, id), &error))
			throw std::runtime_error("WriteLevel failed: " + error);
	}
}

int main()
{
	try
	{
		using namespace World;
		using namespace World::Level;


		const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "worldengine-level-tests";
		std::filesystem::remove_all(tempDir);
		std::filesystem::create_directories(tempDir);

		// 1. 发现:`scenes/*.wlevel` 即关卡集合(按 Id 排序),内容目录必须成对存在。
		{
			WriteLevel(tempDir, "arena", "Arena", 5001);
			WriteLevel(tempDir, "menu", "Main Menu", 5002);

			const std::vector<LevelInfo> levels = LevelCatalog::Discover(tempDir);
			CHECK(levels.size() == 2);
			CHECK(levels[0].Id == "arena");
			CHECK(levels[0].DisplayName == "Arena");
			CHECK(levels[0].EntryPath == tempDir / "scenes" / "arena.wlevel");
			CHECK(levels[0].ContentDir == tempDir / "levels" / "arena");
			CHECK(levels[0].DataLayers.size() == 1);
			CHECK(levels[0].DataLayers[0].Name == "Base_World");
			CHECK(levels[1].Id == "menu");
			CHECK(levels[1].DisplayName == "Main Menu");

			// 无 scenes/ 目录 = 空集合,不是错误。
			CHECK(LevelCatalog::Discover(tempDir / "nowhere").empty());

			// 坏关卡被跳过,不影响其它关卡:① 入口说自己是别的 Id;② 内容目录缺失。
			WriteRawFile(tempDir / "scenes" / "mismatch.wlevel", "Id: something-else\nName: Mismatch\n");
			WriteRawFile(tempDir / "scenes" / "orphan.wlevel", "Id: orphan\nName: Orphan\n");
			CHECK(LevelCatalog::Discover(tempDir).size() == 2);

			// 成对校验也挡住单点解析。
			LevelInfo info;
			std::string resolveError;
			CHECK(!LevelCatalog::Resolve(tempDir, "orphan", &info, &resolveError));
			CHECK(!resolveError.empty());
			CHECK(!LevelCatalog::Resolve(tempDir, "mismatch", &info, &resolveError));
		}

		// 2. 定位:合法 id 命中,越界/未知 id 拒绝。
		{
			LevelInfo info;
			std::string error;
			CHECK(LevelCatalog::Resolve(tempDir, "arena", &info, &error));
			CHECK(info.Id == "arena");
			CHECK(info.EntryPath == tempDir / "scenes" / "arena.wlevel");
			CHECK(info.ContentDir == tempDir / "levels" / "arena");

			CHECK(!LevelCatalog::Resolve(tempDir, "missing", &info, &error));
			CHECK(!error.empty());
			CHECK(!LevelCatalog::Resolve(tempDir, "../arena", &info, &error));
			CHECK(!LevelCatalog::Resolve(tempDir, "", &info, &error));
		}

		// 3. 服务:发现 + 加载状态机 + 场景栈(场景真的带组件回来)。
		{
			WorldContext context;
			Gameplay::LevelService service;
			service.SetContentRoot(tempDir);
			CHECK(service.GetLevels().size() == 2);

			service.SetSceneLoader([&context](const std::filesystem::path& entryPath,
				std::string* error) -> Ref<Scene>
			{
				// 与 GameHost 同一条:关卡入口 → Scene 走 LevelPackage::Instantiate。
				LevelPackage package;
				if (!package.Load(entryPath, error))
					return nullptr;
				return package.Instantiate(context);
			});

			CHECK(!service.RequestLoad("missing"));
			CHECK(service.GetState() == Gameplay::LevelLoadState::Failed);
			CHECK(!service.GetLastError().empty());

			CHECK(service.RequestLoad("arena"));
			CHECK(service.IsLoading());
			service.Pump();
			CHECK(service.GetState() == Gameplay::LevelLoadState::Idle);
			CHECK(service.GetPrimaryLevel() == "arena");
			CHECK(service.GetPrimaryScene() != nullptr);

			// 场景真的带组件回来:实体数 + MeshRenderer 都在。
			{
				entt::registry& registry = service.GetPrimaryScene()->GetRegistry();
				CHECK(registry.view<UUIDComponent>().size() == 1);
				CHECK(registry.view<MeshRendererComponent>().size() == 1);
			}

			// 重复请求同一主关卡 = 无操作。
			CHECK(!service.RequestLoad("arena"));

			// 换关卡:非叠加加载替换整个栈。
			CHECK(service.RequestLoad("menu"));
			service.Pump();
			CHECK(service.GetActiveLevels().size() == 1);
			CHECK(service.GetPrimaryLevel() == "menu");

			service.UnloadAll();
			CHECK(service.GetPrimaryLevel().empty());
			CHECK(service.GetPrimaryScene() == nullptr);
		}

		// 4. 场景级往返:World 块(关卡级模拟设置,进 manifest 不进实体文件)。
		{
			WorldContext context;
			Scene source(context);
			Scene::WorldSettings& settings = source.GetWorldSettings();
			settings.Gravity = -3.5f;
			settings.PhysicsDebug = true;
			Entity prop = Entity::CreateEntity(&source, "Prop");
			prop.AddComponent<TransformComponent>(glm::vec3 { 1.0f, 2.0f, 3.0f });

			LevelPackage package;
			package.CaptureFromScene(source, "GravityLevel", 64);
			CHECK(package.GetWorld().HasGravityOverride);
			CHECK(std::fabs(package.GetWorld().Gravity + 3.5f) < 1e-5f);
			CHECK(package.GetWorld().PhysicsDebug);

			const std::filesystem::path entry = LevelCatalog::EntryPathFor(tempDir / "WorldSettings", "GravityLevel");
			std::string error;
			CHECK(package.Save(entry, &error));
			CHECK(error.empty());

			LevelPackage reloaded;
			CHECK(reloaded.Load(entry, &error));
			CHECK(reloaded.GetWorld().HasGravityOverride);
			// 存盘再开**不能**静默回到项目默认重力 —— 这正是旧关卡路径丢掉的那一条。
			Ref<Scene> restored = reloaded.Instantiate(context);
			CHECK(std::fabs(restored->GetWorldSettings().Gravity + 3.5f) < 1e-5f);
			CHECK(restored->GetWorldSettings().PhysicsDebug);
		}

		// 5. 场景级往返:未注册组件片段(注册表里没有的组件键,读入按实体 UUID 保留,
		//    再写出必须原样还在 —— 旧路径只遍历已注册 schema,未知键**静默消失**)。
		{
			const std::filesystem::path root = tempDir / "UnknownRoot";
			std::filesystem::create_directories(root / "levels" / "Ghost" / "entities");
			WriteRawFile(LevelCatalog::EntryPathFor(root, "Ghost"),
				"Id: Ghost\nName: Ghost\nGridCellSize: 64\n");
			WriteRawFile(root / "levels" / "Ghost" / "entities" / "777.entity",
				"ChunkID: 0\n"
				"Entity:\n"
				"  World::UUIDComponent:\n"
				"    ID:\n"
				"      m_UUID: 777\n"
				"  World::TagComponent:\n"
				"    Tag: Ghost\n"
				"  Plugin::NotRegisteredComponent:\n"
				"    Health: 42\n");

			WorldContext context;
			LevelPackage package;
			std::string error;
			CHECK(package.Load(LevelCatalog::EntryPathFor(root, "Ghost"), &error));
			CHECK(error.empty());
			Ref<Scene> scene = package.Instantiate(context);
			CHECK(scene->GetRegistry().view<UUIDComponent>().size() == 1);

			entt::registry& registry = scene->GetRegistry();
			const entt::entity handle = *registry.view<UUIDComponent>().begin();
			Entity entity(scene.get(), handle);
			std::string document;
			CHECK(SceneSerializer::SerializeEntityDocumentToYaml(entity, &document, &error));
			CHECK(document.find("Plugin::NotRegisteredComponent") != std::string::npos);
			CHECK(document.find("Health") != std::string::npos);
		}

		// 6. 场景级往返:prefab 实例块(实例根那份 `.entity` 带模板引用 + 覆盖字段)。
		{
			WorldContext context;
			Scene source(context);
			Entity root = Entity::CreateEntity(&source, "CrateA");
			root.AddComponent<TransformComponent>(glm::vec3 { 3.0f, 0.0f, 0.0f });
			Entity child = Entity::CreateEntity(&source, "CrateLid");
			child.AddComponent<TransformComponent>(glm::vec3 { 0.0f, 1.0f, 0.0f });

			const entt::entity rootHandle = static_cast<entt::entity>(root);
			Gameplay::PrefabInstanceRecord& record =
				source.AddPrefabInstance("assets/prefabs/Crate.wprefab", rootHandle);
			record.Overrides[static_cast<uint32_t>(static_cast<entt::entity>(child))] = { "Location" };

			LevelPackage package;
			package.CaptureFromScene(source, "CrateLevel", 64);
			const std::filesystem::path entry = LevelCatalog::EntryPathFor(tempDir / "PrefabRoot", "CrateLevel");
			std::string error;
			CHECK(package.Save(entry, &error));
			CHECK(error.empty());

			LevelPackage reloaded;
			CHECK(reloaded.Load(entry, &error));
			Ref<Scene> restored = reloaded.Instantiate(context);
			// 重开之后实例**不断链**:来源路径与被覆盖字段都还在(否则不能 Revert / Unpack)。
			CHECK(restored->PrefabInstances().size() == 1);
			const Gameplay::PrefabInstanceRecord& restoredRecord = restored->PrefabInstances()[0];
			CHECK(restoredRecord.PrefabPath == "assets/prefabs/Crate.wprefab");
			CHECK(restoredRecord.Overrides.size() == 1);
			CHECK(restoredRecord.Overrides.begin()->second.size() == 1);
			CHECK(restoredRecord.Overrides.begin()->second[0] == "Location");
		}

		// 7. 稀疏放置(§16.10):未被改动的实例只落"实例根 + 模板引用",加载端实例化模板补齐。
		//    这正是"摆 200 个补给箱 ⇒ 200 份完整副本 + 失去传播"要消灭的形态。
		{
			const std::filesystem::path root = tempDir / "SparseRoot";
			std::filesystem::create_directories(root / "prefabs");
			WorldContext context;

			// 模板:补给箱 = 根 + 箱盖 + 箱底(3 个实体)。
			const std::filesystem::path prefabPath = root / "prefabs" / "Crate.wprefab";
			{
				Scene templateScene(context);
				Entity crateRoot = Entity::CreateEntity(&templateScene, "Crate");
				crateRoot.AddComponent<TransformComponent>(glm::vec3 { 0.0f, 0.0f, 0.0f });
				Entity lid = Entity::CreateEntity(&templateScene, "Lid");
				lid.AddComponent<TransformComponent>(glm::vec3 { 0.0f, 0.5f, 0.0f });
				Hierarchy::SetParent(templateScene.GetRegistry(), static_cast<entt::entity>(lid),
					static_cast<entt::entity>(crateRoot));
				Entity base = Entity::CreateEntity(&templateScene, "Base");
				base.AddComponent<TransformComponent>(glm::vec3 { 0.0f, -0.5f, 0.0f });
				Hierarchy::SetParent(templateScene.GetRegistry(), static_cast<entt::entity>(base),
					static_cast<entt::entity>(crateRoot));

				std::string prefabError;
				CHECK(Gameplay::SaveFromScene(templateScene, crateRoot, prefabPath, &prefabError));
				CHECK(prefabError.empty());
			}

			// 关卡:同一个模板摆 3 次(全部未改动,只是根的位置不同)。
			const std::filesystem::path entry = LevelCatalog::EntryPathFor(root, "Sparse");
			Scene levelScene(context);
			std::vector<entt::entity> placedRoots;
			for (int index = 0; index < 3; ++index)
			{
				const Gameplay::PrefabInstanceResult placed =
					Gameplay::InstantiateFromFile(prefabPath, levelScene, entt::null);
				CHECK(placed.IsValid());
				CHECK(placed.EntityCount == 3);
				Entity placedRoot = placed.Root;
				placedRoot.GetComponent<TransformComponent>().Location =
					glm::vec3 { static_cast<float>(index) * 4.0f, 0.0f, 0.0f };
				levelScene.AddPrefabInstance("prefabs/Crate.wprefab", static_cast<entt::entity>(placed.Root));
				placedRoots.push_back(static_cast<entt::entity>(placed.Root));
			}
			CHECK(levelScene.GetRegistry().view<UUIDComponent>().size() == 9);

			LevelPackage package;
			package.SetEntryPath(entry);
			package.CaptureFromScene(levelScene, "Sparse", 64);
			std::printf("[sparse] sparse=%d captured=%zu (expect 3)\n",
				static_cast<int>(levelScene.PrefabInstances().empty() ? -1 : levelScene.PrefabInstances()[0].SparsePlacement),
				package.GetEntityCount());
			CHECK(package.GetEntityCount() == 3);   // 只有 3 个实例根落盘(9 → 3)
			std::string error;
			CHECK(package.Save(entry, &error));
			CHECK(error.empty());
			{
				size_t entityFiles = 0;
				for (const auto& file : std::filesystem::directory_iterator(
					root / "levels" / "Sparse" / "entities"))
					if (file.path().extension() == ".entity")
						++entityFiles;
				CHECK(entityFiles == 3);
			}

			// 重开:9 个实体一个不少(其余 6 个由模板实例化补齐),3 条实例记录都在。
			LevelPackage reloaded;
			CHECK(reloaded.Load(entry, &error));
			Ref<Scene> restored = reloaded.Instantiate(context);
			CHECK(restored->GetRegistry().view<UUIDComponent>().size() == 9);
			CHECK(restored->PrefabInstances().size() == 3);
			// 根的位置是"用户摆的",必须原样回来(模板给的是 0,0,0)。
			std::vector<float> rootXs;
			for (const Gameplay::PrefabInstanceRecord& record : restored->PrefabInstances())
			{
				CHECK(record.Root != entt::null);
				const auto* transform = restored->GetRegistry().try_get<TransformComponent>(record.Root);
				CHECK(transform != nullptr);
				rootXs.push_back(transform->Location.x);
			}
			std::sort(rootXs.begin(), rootXs.end());
			CHECK(rootXs.size() == 3);
			CHECK(std::fabs(rootXs[0] - 0.0f) < 1e-5f);
			CHECK(std::fabs(rootXs[1] - 4.0f) < 1e-5f);
			CHECK(std::fabs(rootXs[2] - 8.0f) < 1e-5f);

			// 改一个实例的**子实体**并登记覆盖 ⇒ 那个子实体必须落盘(它是"差异实体"),
			// 重启后改动不能丢。其余未改动的实例仍是稀疏形态。
			{
				std::vector<entt::entity> subtree;
				Gameplay::FlattenInstanceSubtree(levelScene.GetRegistry(), placedRoots[1], subtree);
				CHECK(subtree.size() == 3);
				const entt::entity lid = subtree[1];
				levelScene.GetRegistry().get<TransformComponent>(lid).Location = glm::vec3 { 0.0f, 2.5f, 0.0f };
				Gameplay::PrefabInstanceRecord* record = levelScene.FindPrefabInstance(placedRoots[1]);
				CHECK(record != nullptr);
				Gameplay::MarkOverride(*record, lid, "TransformComponent.Location");

				LevelPackage repacked;
				repacked.SetEntryPath(entry);
				repacked.CaptureFromScene(levelScene, "Sparse", 64);
				CHECK(repacked.GetEntityCount() == 4);   // 3 个根 + 1 个差异子实体
				CHECK(repacked.Save(entry, &error));
				CHECK(error.empty());

				LevelPackage reopened;
				CHECK(reopened.Load(entry, &error));
				Ref<Scene> after = reopened.Instantiate(context);
				CHECK(after->GetRegistry().view<UUIDComponent>().size() == 9);
				bool overrideSurvived = false;
				for (const entt::entity handle : after->GetRegistry().view<TransformComponent>())
				{
					const TransformComponent& transform = after->GetRegistry().get<TransformComponent>(handle);
					if (std::fabs(transform.Location.y - 2.5f) < 1e-5f)
						overrideSurvived = true;
				}
				CHECK(overrideSurvived);
			}
		}
		// 8. 数据层定义规则(数据层编辑 UI 的引擎侧契约):
		//    归一化(每层一位/去重/排序/补名/至少一层)、下一个空闲位、掩码覆盖判定、manifest 往返。
		{
			std::vector<DataLayerDefinition> messy;
			messy.push_back({ 4u, "", false });        // 空名 ⇒ Layer3
			messy.push_back({ 0u, "Zero", true });     // 0 掩码 ⇒ 丢
			messy.push_back({ 3u, "MultiBit", true }); // 多位掩码 ⇒ 丢(一层恰一位)
			messy.push_back({ 4u, "Dup", true });      // 与首个同一位 ⇒ 丢后来的
			messy.push_back({ 1u, "Base_World", true });
			CHECK(NormalizeDataLayers(messy));
			CHECK(messy.size() == 2);
			CHECK(messy[0].LayerMask == 1u);           // 排序:掩码升序
			CHECK(messy[0].Name == "Base_World");
			CHECK(messy[1].LayerMask == 4u);
			CHECK(messy[1].Name == "Layer3");          // 位序 3(1 起)⇒ Layer3
			CHECK(messy[1].DefaultActive == false);
			CHECK(!NormalizeDataLayers(messy));        // 幂等:已规范 ⇒ 无改动

			// 全部无效 ⇒ 回填常驻层(关卡必须至少有一个可归属的层)。
			std::vector<DataLayerDefinition> fallback;
			fallback.push_back({ 0u, "OnlyZero", true });
			CHECK(NormalizeDataLayers(fallback));
			CHECK(fallback.size() == 1);
			CHECK(fallback[0].LayerMask == 1u);
			CHECK(fallback[0].Name == "Base_World");

			// 下一个空闲位(面板"添加层"的分配依据)。
			std::vector<DataLayerDefinition> layers { { 1u, "Base_World", true }, { 2u, "Day", true } };
			CHECK(NextFreeLayerMask(layers) == 4u);
			layers.push_back({ 0x80000000u, "Last", false });
			CHECK(NextFreeLayerMask(layers) == 4u);    // 高位被占不影响低位分配
			std::vector<DataLayerDefinition> everyBit;
			for (uint32_t bit = 0; bit < 32; ++bit)
				everyBit.push_back({ 1u << bit, "L" + std::to_string(bit), true });
			CHECK(NextFreeLayerMask(everyBit) == 0u);  // 32 位用尽 ⇒ 0(调用方禁用"添加层")

			// 覆盖判定(删除层守卫的依据)。
			CHECK(IsMaskCoveredByLayers(0u, layers));                  // 0 = 常驻可见,恒覆盖
			CHECK(IsMaskCoveredByLayers(1u | 2u, layers));
			CHECK(!IsMaskCoveredByLayers(1u | 8u, layers));            // 8 位没被任何层声明

			// manifest 往返:层定义随入口文件走,且写盘前被归一化。
			const std::filesystem::path root = tempDir / "LayerRoot";
			const std::filesystem::path entry = LevelCatalog::EntryPathFor(root, "Layered");
			LevelPackage package;
			package.SetId("Layered");
			package.SetName("Layered");
			std::vector<DataLayerDefinition>& savedLayers = package.GetDataLayers();
			savedLayers.push_back({ 2u, "Day", true });
			savedLayers.push_back({ 1u, "Base_World", true });
			savedLayers.push_back({ 2u, "DayDup", true });   // 写盘时被归一化丢掉
			std::string error;
			CHECK(package.Save(entry, &error));
			CHECK(error.empty());

			LevelPackage reloaded;
			CHECK(reloaded.Load(entry, &error));
			CHECK(reloaded.GetDataLayers().size() == 2);
			CHECK(reloaded.GetDataLayers()[0].Name == "Base_World");
			CHECK(reloaded.GetDataLayers()[1].Name == "Day");

			// 目录发现带出同一份层定义(视口工具栏 / 属性面板据此渲染)。
			const std::vector<LevelInfo> found = LevelCatalog::Discover(root);
			CHECK(found.size() == 1);
			CHECK(found[0].DataLayers.size() == 2);
			CHECK(found[0].DataLayers[1].Name == "Day");
		}

		std::filesystem::remove_all(tempDir);
		std::printf("[LevelTests] Level 2.0 .wlevel catalog + service + scene-level round trip tests passed.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[LevelTests] FAILED: %s\n", e.what());
		return 1;
	}
}
