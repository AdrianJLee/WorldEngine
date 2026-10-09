// Level 2.0 关卡目录发现(LevelCatalog)与关卡服务(LevelService 加载状态机/场景栈)。
//
// 关卡集合来自内容根下的**目录结构** `levels/<Id>/`;没有 levels.welevel 清单文件。
#include "World/Core/WorldContext.h"
#include "World/Level/Authoring/LevelCatalog.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Gameplay/Framework/LevelService.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <cstdio>
#include <filesystem>
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

	// 造一个最小关卡包:`levels/<id>/manifest.yaml` + `entities/1.entity`(带 MeshRenderer)。
	void WriteLevel(const std::filesystem::path& contentRoot, const std::string& id,
		const std::string& displayName, uint64_t entityUuid)
	{
		World::Level::LevelPackage package;
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
		if (!package.Save(contentRoot / "levels" / id, &error))
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

		// 1. 发现:目录结构即关卡集合(按 Id 排序)。
		{
			WriteLevel(tempDir, "arena", "Arena", 5001);
			WriteLevel(tempDir, "menu", "Main Menu", 5002);

			const std::vector<LevelInfo> levels = LevelCatalog::Discover(tempDir);
			CHECK(levels.size() == 2);
			CHECK(levels[0].Id == "arena");
			CHECK(levels[0].DisplayName == "Arena");
			CHECK(levels[0].DataLayers.size() == 1);
			CHECK(levels[0].DataLayers[0].Name == "Base_World");
			CHECK(levels[1].Id == "menu");
			CHECK(levels[1].DisplayName == "Main Menu");

			// 无 levels/ 目录 = 空集合,不是错误。
			CHECK(LevelCatalog::Discover(tempDir / "nowhere").empty());

			// 坏目录被跳过,不影响其它关卡。
			std::filesystem::create_directories(tempDir / "levels" / "broken");
			CHECK(LevelCatalog::Discover(tempDir).size() == 2);
		}

		// 2. 定位:合法 id 命中,越界/未知 id 拒绝。
		{
			LevelInfo info;
			std::string error;
			CHECK(LevelCatalog::Resolve(tempDir, "arena", &info, &error));
			CHECK(info.Id == "arena");
			CHECK(info.PackageDir == tempDir / "levels" / "arena");

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

			service.SetSceneLoader([&context](const std::filesystem::path& packageDir,
				std::string* error) -> Ref<Scene>
			{
				// 与 GameHost 同一条:关卡包 → Scene 走 LevelPackage::Instantiate。
				LevelPackage package;
				if (!package.Load(packageDir, error))
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

		std::filesystem::remove_all(tempDir);
		std::printf("[LevelTests] Level 2.0 catalog + service tests passed.\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[LevelTests] FAILED: %s\n", e.what());
		return 1;
	}
}
