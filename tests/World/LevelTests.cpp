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
#include "World/Scene/SceneSerializer.h"

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
