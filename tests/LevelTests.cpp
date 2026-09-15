// P2a W2:关卡清单(levels.welevel)与关卡服务(加载状态机/场景栈)。
#include "World/Core/WorldContext.h"
#include "World/Gameplay/LevelList.h"
#include "World/Gameplay/LevelService.h"
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
}

int main()
{
	try
	{
		using namespace World;
		using namespace World::Gameplay;

		const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "worldengine-level-tests";
		std::filesystem::remove_all(tempDir);
		std::filesystem::create_directories(tempDir);

		// 1. 关卡清单:保存 → 读回 → 查询。
		{
			LevelList list;
			list.Entries().push_back({ "arena", "Arena", "scenes/arena.wd", { "packages/Base.wpak" } });
			list.Entries().push_back({ "menu", "Main Menu", "scenes/menu.wd", {} });

			const std::filesystem::path path = tempDir / "levels.welevel";
			std::string error;
			CHECK(LevelList::Save(path, list, &error));
			CHECK(error.empty());

			LevelList loaded;
			CHECK(LevelList::Load(path, &loaded, &error));
			CHECK(loaded.Entries().size() == 2);
			const LevelEntry* arena = loaded.Find("arena");
			CHECK(arena != nullptr);
			CHECK(arena->DisplayName == "Arena");
			CHECK(arena->ScenePath == "scenes/arena.wd");
			CHECK(arena->Packages.size() == 1 && arena->Packages[0] == "packages/Base.wpak");
			CHECK(loaded.Find("menu") != nullptr);
			CHECK(loaded.Find("missing") == nullptr);

			// 缺字段与重复 id 必须加载失败(不能静默生成坏清单)。
			const std::filesystem::path broken = tempDir / "broken.welevel";
			{
				std::ofstream file(broken);
				file << "levels:\n  - id: dup\n    scene: scenes/a.wd\n  - id: dup\n    scene: scenes/b.wd\n";
			}
			LevelList rejected;
			CHECK(!LevelList::Load(broken, &rejected, &error));
			CHECK(error.find("duplicate") != std::string::npos);

			const std::filesystem::path missingField = tempDir / "missing.welevel";
			{
				std::ofstream file(missingField);
				file << "levels:\n  - id: no_scene\n    name: Broken\n";
			}
			CHECK(!LevelList::Load(missingField, &rejected, &error));
		}

		WorldContext context;

		// 2. 关卡服务:状态机、进度回调、场景栈。
		{
			LevelList list;
			list.Entries().push_back({ "alpha", "Alpha", "scenes/alpha.wd", {} });
			list.Entries().push_back({ "beta", "Beta", "scenes/beta.wd", {} });

			LevelService service;
			service.SetLevelList(list);
			CHECK(service.GetLevelList().Entries().size() == 2);

			std::vector<std::string> states;
			std::vector<std::string> loadedPaths;
			service.SetProgressCallback([&states](const LevelLoadProgress& report)
			{
				states.push_back(std::string(LevelLoadStateName(report.State)) + ":" + report.LevelId +
					":" + std::to_string(report.Progress));
			});

			// 未设置 loader:拒绝请求且状态为 Failed。
			CHECK(!service.RequestLoad("alpha"));
			CHECK(service.GetState() == LevelLoadState::Failed);
			CHECK(states.size() == 1 && states[0].rfind("Failed:alpha:", 0) == 0);
			states.clear();

			service.SetSceneLoader([&context, &loadedPaths](const std::string& scenePath, std::string* error)
			{
				loadedPaths.push_back(scenePath);
				(void)error;
				return CreateRef<Scene>(context);
			});

			// 未知关卡:拒绝。
			CHECK(!service.RequestLoad("ghost"));
			CHECK(states.size() == 1);
			states.clear();

			// 正常加载:请求只登记,下一次 Pump 才真正加载。
			CHECK(service.RequestLoad("alpha"));
			CHECK(service.IsLoading());
			CHECK(service.GetState() == LevelLoadState::Reading);
			CHECK(loadedPaths.empty());
			CHECK(states.size() == 1 && states[0].rfind("Reading:alpha:0", 0) == 0);

			service.Pump();
			CHECK(!service.IsLoading());
			CHECK(service.GetState() == LevelLoadState::Idle);
			CHECK(loadedPaths.size() == 1 && loadedPaths[0] == "scenes/alpha.wd");
			CHECK(service.GetActiveLevels().size() == 1 && service.GetPrimaryLevel() == "alpha");
			CHECK(service.GetPrimaryScene() != nullptr);
			// 进度序列:Reading → Deserializing → Activating → Idle(1.0)。
			CHECK(states.size() == 4);
			CHECK(states[0].rfind("Reading:alpha:0", 0) == 0);
			CHECK(states[1].rfind("Deserializing:alpha:0.4", 0) == 0);
			CHECK(states[2].rfind("Activating:alpha:0.8", 0) == 0);
			CHECK(states[3].rfind("Idle:alpha:1", 0) == 0);
			states.clear();

			// 叠加加载:主关卡不变,栈顶压入 beta。
			CHECK(service.RequestLoad("beta", /*additive=*/true));
			service.Pump();
			CHECK(service.GetActiveLevels().size() == 2);
			CHECK(service.GetPrimaryLevel() == "alpha");
			CHECK(service.FindScene("beta") != nullptr);
			CHECK(service.FindScene("ghost") == nullptr);

			// 卸载单关 + 全部卸载。
			service.Unload("beta");
			CHECK(service.GetActiveLevels().size() == 1);
			service.Unload("beta");   // 幂等
			CHECK(service.GetActiveLevels().size() == 1);

			// 替换加载:非叠加请求清空旧栈。
			CHECK(service.RequestLoad("beta"));
			service.Pump();
			CHECK(service.GetActiveLevels().size() == 1);
			CHECK(service.GetPrimaryLevel() == "beta");

			// loader 失败:状态 Failed、错误可见、原场景栈保持不变。
			service.SetSceneLoader([](const std::string&, std::string* error)
			{
				if (error) *error = "scene file missing";
				return Ref<Scene>(nullptr);
			});
			CHECK(service.RequestLoad("alpha"));
			service.Pump();
			CHECK(service.GetState() == LevelLoadState::Failed);
			CHECK(service.GetLastError() == "scene file missing");
			CHECK(service.GetActiveLevels().size() == 1);
			CHECK(service.GetPrimaryLevel() == "beta");

			service.UnloadAll();
			CHECK(service.GetActiveLevels().empty());
			CHECK(service.GetPrimaryScene() == nullptr);
			CHECK(service.GetState() == LevelLoadState::Idle);
		}

		std::filesystem::remove_all(tempDir);
		std::printf("World.Level: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Level FAILED: %s\n", error.what());
		return 1;
	}
}
