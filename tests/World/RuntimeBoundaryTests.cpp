#include "World/Core/EngineHostMode.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Level/Cooking/LevelCooker.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
			std::cerr << "[FAIL] Line " << line << ": " << expression << std::endl;
			throw std::runtime_error(std::string("Assertion failed at line ") + std::to_string(line) + ": " + expression);
		}
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void TestHostEnvironmentContract()
	{
		std::cout << "[Test] 1. Host Environment Mode Contract..." << std::endl;

		EngineEnvironment::SetHostMode(EngineHostMode::Editor);
		CHECK(EngineEnvironment::IsEditor());
		CHECK(!EngineEnvironment::IsRuntime());
		CHECK(!EngineEnvironment::IsPackaged());
		CHECK(!EngineEnvironment::IsPureGameRuntime());
		CHECK(EngineEnvironment::GetHostModeName() == "Editor");

		EngineEnvironment::SetHostMode(EngineHostMode::Runtime);
		CHECK(!EngineEnvironment::IsEditor());
		CHECK(EngineEnvironment::IsRuntime());
		CHECK(!EngineEnvironment::IsPackaged());
		CHECK(EngineEnvironment::IsPureGameRuntime());
		CHECK(EngineEnvironment::GetHostModeName() == "Runtime");

		EngineEnvironment::SetHostMode(EngineHostMode::Packaged);
		CHECK(!EngineEnvironment::IsEditor());
		CHECK(!EngineEnvironment::IsRuntime());
		CHECK(EngineEnvironment::IsPackaged());
		CHECK(EngineEnvironment::IsPureGameRuntime());
		CHECK(EngineEnvironment::GetHostModeName() == "Packaged");

		EngineEnvironment::SetHostMode(EngineHostMode::Headless);
		CHECK(EngineEnvironment::IsHeadless());
		CHECK(!EngineEnvironment::IsPureGameRuntime());
		CHECK(EngineEnvironment::GetHostModeName() == "Headless");

		EngineEnvironment::SetHostMode(EngineHostMode::Tool);
		CHECK(EngineEnvironment::IsTool());
		CHECK(EngineEnvironment::GetHostModeName() == "Tool");
	}

	void TestEditorOnlyComponentHierarchy()
	{
		std::cout << "[Test] 2. EditorOnlyComponent and Hierarchy Cascade..." << std::endl;

		WorldContext context;
		Ref<Scene> scene = CreateRef<Scene>(context);

		Entity parent = Entity::CreateEntity(scene.get(), "DebugMarkerParent");
		Entity child = Entity::CreateEntity(scene.get(), "DebugMarkerChild");

		Hierarchy::SetParent(scene->GetRegistry(), child, parent);

		CHECK(!parent.IsEditorOnly());
		CHECK(!child.IsEditorOnly());

		auto& comp = parent.AddComponent<EditorOnlyComponent>();
		comp.IncludeChildren = 1;

		CHECK(parent.IsEditorOnly());
		CHECK(child.IsEditorOnly()); // 子实体自动级联继承

		comp.IncludeChildren = 0;
		CHECK(parent.IsEditorOnly());
		CHECK(!child.IsEditorOnly()); // 关闭 IncludeChildren 后子实体不再继承

		parent.RemoveComponent<EditorOnlyComponent>();
		CHECK(!parent.IsEditorOnly());
		CHECK(!child.IsEditorOnly());
	}

	void TestSceneRendererOptionsDefaults()
	{
		std::cout << "[Test] 3. SceneRendererOptions Safe Defaults..." << std::endl;

		SceneRendererOptions options;
		CHECK(!options.ShowGrid);               // 确保非编辑态地面网格默认关闭
		CHECK(!options.ShowDebugColliders);     // 确保 2D 碰撞体调试线框默认关闭
		CHECK(!options.ShowDebugLights);        // 确保 2D 光照调试线框默认关闭
		CHECK(!options.ShowSelectionOutline);   // 确保选区描边默认关闭
		CHECK(!options.ShowNavMesh);            // 确保导航网格叠加层默认关闭
	}

	void TestLevelCookerEditorOnlyStripping()
	{
		std::cout << "[Test] 4. LevelCooker Stripping Gate..." << std::endl;

		WorldContext context;
		Ref<Scene> scene = CreateRef<Scene>(context);

		Entity gameplayEnt = Entity::CreateEntity(scene.get(), "NormalGameplayEntity");
		gameplayEnt.AddComponent<TransformComponent>().Location = { 5.0f, 0.0f, 5.0f };

		Entity editorOnlyEnt = Entity::CreateEntity(scene.get(), "EditorHelperEntity");
		editorOnlyEnt.AddComponent<TransformComponent>().Location = { 10.0f, 0.0f, 10.0f };
		editorOnlyEnt.AddComponent<EditorOnlyComponent>();

		Level::LevelPackage package;
		package.CaptureFromScene(*scene, "TestLevel", 64);
		CHECK(package.GetEntityCount() == 2);

		const std::filesystem::path tempOutputDir = std::filesystem::temp_directory_path() / "we_test_cooked_boundary";
		std::error_code ec;
		std::filesystem::remove_all(tempOutputDir, ec);

		Level::CookSettings settings;
		settings.GridCellSize = 64;
		const Level::CookResult cookResult = Level::LevelCooker::CookPackage(package, tempOutputDir, settings);

		CHECK(cookResult.Success);
		// 关键断言: 2 个实体中只有 1 个正常实体进入烘焙块，EditorOnly 实体被 100% 物理剥离！
		CHECK(cookResult.TotalEntitiesCooked == 1);

		std::filesystem::remove_all(tempOutputDir, ec);
	}

	void TestSceneSerializerEditorOnlyDefense()
	{
		std::cout << "[Test] 5. SceneSerializer Deserialization Defense in Pure Runtime..." << std::endl;

		WorldContext context;
		Ref<Scene> scene = CreateRef<Scene>(context);

		const std::string docWithEditorOnly = R"(
Entities:
  - World::TagComponent:
      Tag: "EditorOnlyNPC"
    World::UUIDComponent:
      ID: { m_UUID: 1234567890 }
    World::TransformComponent:
      Location: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    World::EditorOnlyComponent:
      IncludeChildren: 1
)";

		// 处于 Editor 模式时正常导入
		EngineEnvironment::SetHostMode(EngineHostMode::Editor);
		std::string err;
		size_t appliedInEditor = SceneSerializer::AppendEntityYaml(*scene, docWithEditorOnly, &err);
		CHECK(appliedInEditor == 1);
		CHECK(scene->GetRegistry().view<TagComponent>().size() == 1);

		// 处于 Packaged 纯游戏运行环境时，必须防御过滤丢弃
		EngineEnvironment::SetHostMode(EngineHostMode::Packaged);
		Ref<Scene> runtimeScene = CreateRef<Scene>(context);
		size_t appliedInRuntime = SceneSerializer::AppendEntityYaml(*runtimeScene, docWithEditorOnly, &err);
		CHECK(appliedInRuntime == 0);
		CHECK(runtimeScene->GetRegistry().view<TagComponent>().size() == 0); // 绝对零残留！

		EngineEnvironment::SetHostMode(EngineHostMode::Editor);
	}
}

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	try
	{
		TestHostEnvironmentContract();
		TestEditorOnlyComponentHierarchy();
		TestSceneRendererOptionsDefaults();
		TestLevelCookerEditorOnlyStripping();
		TestSceneSerializerEditorOnlyDefense();

		std::cout << "[SUCCESS] All RuntimeBoundaryTests passed successfully!" << std::endl;
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[FATAL] Test exception: " << e.what() << std::endl;
		return 1;
	}
}
