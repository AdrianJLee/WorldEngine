// P2a W8:存档服务(槽位/全局块/场景块往返、版本迁移、坏档保护)。
#include "World/Core/WorldContext.h"
#include "World/Gameplay/GameApp.h"
#include "World/Gameplay/SaveService.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)
}

int main()
{
	try
	{
		using namespace World;
		using namespace World::Gameplay;

		WorldContext context;
		Scene scene(context);

		entt::registry& registry = scene.GetRegistry();
		const entt::entity handle = registry.create();
		registry.emplace<UUIDComponent>(handle, UUID());
		registry.emplace<TransformComponent>(handle);
		const uint64_t entityUuid = static_cast<uint64_t>(registry.get<UUIDComponent>(handle).ID);
		registry.get<TransformComponent>(handle).Location = { 1.0f, 2.0f, 3.0f };

		const std::filesystem::path root = std::filesystem::temp_directory_path() / "we-save-tests";
		std::filesystem::remove_all(root);
		SaveService saves("test.project", [&scene] { return &scene; }, root);

		// 1. 场景块往返:保存 → 改值 → 读取 → 还原(Transform 是默认 SaveTrait)。
		CHECK(saves.Save(1, "level-a"));
		registry.get<TransformComponent>(handle).Location = { 9.0f, 9.0f, 9.0f };
		CHECK(saves.Load(1));
		const glm::vec3 restored = registry.get<TransformComponent>(handle).Location;
		CHECK(std::fabs(restored.x - 1.0f) < 1e-4f);
		CHECK(std::fabs(restored.y - 2.0f) < 1e-4f);
		CHECK(std::fabs(restored.z - 3.0f) < 1e-4f);

		// 2. ListSaves 只读头(槽位号 / 关卡 / 版本)。
		{
			const std::vector<SaveSlotInfo> slots = saves.ListSaves();
			CHECK(slots.size() == 1);
			CHECK(slots[0].Valid);
			CHECK(slots[0].Slot == 1);
			CHECK(slots[0].Header.LevelId == "level-a");
			CHECK(slots[0].Header.Version == SaveService::CurrentVersion);
			CHECK(slots[0].Header.ProjectId == "test.project");
		}

		// 3. 全局块往返。
		{
			SaveService::GlobalValue progress;
			progress.Type = SaveService::GlobalValue::Kind::Int;
			progress.Int = 42;
			saves.SetGlobal("progress", progress);

			SaveService::GlobalValue name;
			name.Type = SaveService::GlobalValue::Kind::String;
			name.String = "chapter-one";
			saves.SetGlobal("chapter", name);

			CHECK(saves.Save(2, "level-b"));
			saves.ClearGlobals();
			CHECK(saves.Load(2));

			SaveService::GlobalValue loaded;
			CHECK(saves.TryGetGlobal("progress", &loaded) && loaded.Type == SaveService::GlobalValue::Kind::Int && loaded.Int == 42);
			CHECK(saves.TryGetGlobal("chapter", &loaded) && loaded.Type == SaveService::GlobalValue::Kind::String && loaded.String == "chapter-one");
		}

		// 4. 坏档保护:解析失败 → 报错返回、原档保留、场景不变。
		{
			const std::filesystem::path broken = saves.GetSlotPath(3);
			{
				std::ofstream stream(broken, std::ios::binary | std::ios::trunc);
				stream << "SaveHeader: [this is not a valid save]\n\t- broken";
			}
			registry.get<TransformComponent>(handle).Location = { 5.0f, 5.0f, 5.0f };
			CHECK(!saves.Load(3));
			CHECK(!saves.GetLastError().empty());
			CHECK(std::filesystem::exists(broken));
			CHECK(std::fabs(registry.get<TransformComponent>(handle).Location.x - 5.0f) < 1e-4f);

			const std::vector<SaveSlotInfo> slots = saves.ListSaves();
			bool foundBroken = false;
			for (const SaveSlotInfo& slot : slots)
				if (slot.Slot == 3)
				{
					foundBroken = true;
					CHECK(!slot.Valid);        // 只读头也识别坏档,但不当成崩溃
					CHECK(!slot.Error.empty());
				}
			CHECK(foundBroken);
		}

		// 5. 版本迁移:v0 档 + 注册迁移钩子 → 迁移被调用且 Load 成功。
		{
			const std::filesystem::path legacy = saves.GetSlotPath(4);
			{
				std::ofstream stream(legacy, std::ios::binary | std::ios::trunc);
				stream << "SaveHeader:\n  ProjectId: test.project\n  Version: 0\n  LevelId: level-legacy\n  Timestamp: 0\n"
					<< "Globals: {}\n"
					<< "Entities:\n"
					<< "  - UUID: " << entityUuid << "\n"
					<< "    World::TransformComponent:\n"
					<< "      Location: [7, 8, 9]\n"
					<< "      Rotation: [0, 0, 0]\n"
					<< "      Scale: [1, 1, 1]\n";
			}

			bool migrationRan = false;
			saves.RegisterMigration(0, [&migrationRan](uint32_t fromVersion, void* root)
			{
				migrationRan = (fromVersion == 0) && (root != nullptr);
				return migrationRan;
			});
			CHECK(saves.Load(4));
			CHECK(migrationRan);
			CHECK(std::fabs(registry.get<TransformComponent>(handle).Location.x - 7.0f) < 1e-4f);
		}

		// 6. 未注册迁移的旧档必须拒绝(而不是半途应用)。
		{
			const std::filesystem::path legacy = saves.GetSlotPath(5);
			{
				std::ofstream stream(legacy, std::ios::binary | std::ios::trunc);
				stream << "SaveHeader:\n  ProjectId: test.project\n  Version: 0\n  LevelId: x\n  Timestamp: 0\n"
					<< "Globals: {}\nEntities: []\n";
			}
			SaveService other("test.project", [&scene] { return &scene; }, root);
			CHECK(!other.Load(5));
			CHECK(other.GetLastError().find("migration") != std::string::npos);
		}

		// 7. 删除槽位。
		CHECK(saves.Delete(1));
		CHECK(!std::filesystem::exists(saves.GetSlotPath(1)));

		// 8. W8-2:实体在当前场景缺失 → 读档时按 UUID 重建(只含 SaveTrait 组件),并给出读档报告。
		{
			CHECK(saves.Save(6, "level-rebuild"));
			Entity::DestroyEntity(&scene, Entity(&scene, handle));
			scene.FlushStructuralChanges();   // 销毁是延迟的:先落结构变更,再验证"实体已不在场景里"
			bool stillPresent = false;
			for (const entt::entity candidate : registry.storage<entt::entity>())
				if (registry.all_of<UUIDComponent>(candidate) &&
					static_cast<uint64_t>(registry.get<UUIDComponent>(candidate).ID) == entityUuid)
					stillPresent = true;
			CHECK(!stillPresent);

			CHECK(saves.Load(6));
			const SaveLoadReport& report = saves.GetLastLoadReport();
			CHECK(report.EntitiesCreated == 1);
			CHECK(report.EntitiesUpdated == 0);
			CHECK(report.ComponentsApplied >= 1);

			bool rebuilt = false;
			for (const entt::entity candidate : registry.storage<entt::entity>())
			{
				const UUIDComponent& id = registry.get<UUIDComponent>(candidate);
				if (static_cast<uint64_t>(id.ID) != entityUuid)
					continue;
				rebuilt = true;
				const TransformComponent& transform = registry.get<TransformComponent>(candidate);
				CHECK(std::fabs(transform.Location.x - 7.0f) < 1e-4f);   // 上一步迁移后写入的值
			}
			CHECK(rebuilt);
		}

		// 9. W8-3 接线:GameApp 持有 SaveService(宿主注入场景来源),未注入前为空。
		{
			_putenv_s("WLD_SAVE_DIR", (root / "app-wired").string().c_str());   // 别写进真实用户目录
			GameAppDesc desc;
			desc.ProjectId = "wire.project";
			GameApp::Create(desc);
			CHECK(GameApp::Get().Saves() == nullptr);
			GameApp::Get().CreateSaveService([&scene] { return &scene; });
			CHECK(GameApp::Get().Saves() != nullptr);
			CHECK(GameApp::Get().Saves()->Save(9, "wired"));
			CHECK(GameApp::Get().Saves()->Load(9));
			GameApp::Shutdown();
			_putenv_s("WLD_SAVE_DIR", "");
		}

		std::filesystem::remove_all(root);
		std::printf("World.Save: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Save FAILED: %s\n", error.what());
		return 1;
	}
}
