#include "World/Ai/SceneAuthoring.h"
#include "World/Ai/ChangesetTracker.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Scene/Components.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiJson.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
			std::exit(1); \
		} \
	} while (false)

namespace fs = std::filesystem;

int main()
{
	using namespace World;
	using namespace World::Ai;
	using namespace World::Wui;

	std::printf("Running WorldAiSceneAuthoringTests...\n");

	const fs::path tempProj = fs::current_path() / "build" / "tmp_test_ai_scene_authoring";
	std::error_code ec;
	fs::remove_all(tempProj, ec);
	fs::create_directories(tempProj / "assets" / "scenes", ec);
	fs::create_directories(tempProj / "assets" / "scripts" / "systems", ec);
	{
		std::ofstream f(tempProj / "project.we.yaml");
		f << "id: SceneAuthoringTest\nname: SceneAuthoringTest\nversion: 1.0.0\n"
		  << "content_root: assets\nstart_scene: scenes/AiAuthored.wlevel\n";
	}

	Paths::SetProjectDirOverride(tempProj);

	auto& registry = CommandRegistry::Get();
	CHECK(registry.HasCommand("scene.create_level"));
	CHECK(registry.HasCommand("scene.open"));
	CHECK(registry.HasCommand("scene.save"));
	CHECK(registry.HasCommand("scene.create_entity"));
	CHECK(registry.HasCommand("scene.get"));
	CHECK(registry.HasCommand("scene.set"));
	CHECK(registry.HasCommand("scene.add_component"));
	CHECK(registry.HasCommand("scene.delete_entity"));
	CHECK(registry.HasCommand("changeset.stage_write"));

	auto dispatch = [&](const std::string& cmd, std::map<std::string, std::string> args, std::string& out) -> bool
	{
		std::string error;
		args["root"] = tempProj.string();
		const bool ok = registry.Dispatch(cmd, args, out, error);
		if (!ok)
			std::fprintf(stderr, "[cmd] %s failed: %s\n", cmd.c_str(), error.c_str());
		return ok;
	};

	// 1. 真实建关:磁盘上必须出现 .wlevel 与 starter entities
	{
		std::string out;
		CHECK(dispatch("scene.create_level", { { "path", "AiAuthored" }, { "id", "AiAuthored" } }, out));
		CHECK(fs::exists(tempProj / "assets" / "scenes" / "AiAuthored.wlevel"));
		CHECK(fs::exists(tempProj / "assets" / "levels" / "AiAuthored" / "entities"));

		auto json = JsonValue::Parse(out, nullptr);
		CHECK(json.has_value());
		CHECK(json->Find("entity_count")->AsNumber() > 0);
	}

	// 2. 创建实体 + 加组件 + 写字段
	std::string heroHandle;
	{
		std::string out;
		CHECK(dispatch("scene.create_entity",
			{ { "name", "Hero" }, { "location", "1,2,3" }, { "scale", "1,1,1" } }, out));
		auto json = JsonValue::Parse(out, nullptr);
		CHECK(json.has_value());
		heroHandle = std::to_string(static_cast<int>(json->Find("handle")->AsNumber()));
		CHECK(!heroHandle.empty());

		CHECK(dispatch("scene.add_component", { { "handle", heroHandle }, { "component", "HealthComponent" } }, out));
		CHECK(dispatch("scene.set",
			{ { "handle", heroHandle }, { "component", "HealthComponent" }, { "property", "CurrentHealth" }, { "value", "42" } }, out));
		CHECK(dispatch("scene.set",
			{ { "handle", heroHandle }, { "component", "HealthComponent" }, { "property", "MaxHealth" }, { "value", "120" } }, out));

		CHECK(dispatch("scene.get", { { "handle", heroHandle } }, out));
		auto got = JsonValue::Parse(out, nullptr);
		CHECK(got.has_value());
		const auto* components = got->Find("components");
		CHECK(components != nullptr);
		const auto* health = components->Find("World::HealthComponent");
		CHECK(health != nullptr);
		CHECK(health->Find("CurrentHealth")->AsNumber() == 42.0);
		CHECK(health->Find("MaxHealth")->AsNumber() == 120.0);
		const auto* transform = components->Find("World::TransformComponent");
		CHECK(transform != nullptr);
		const auto* location = transform->Find("Location");
		CHECK(location != nullptr && location->type == JsonValue::Type::Array && location->Array.size() == 3);
		CHECK(location->Array[0].AsNumber() == 1.0);
		CHECK(location->Array[1].AsNumber() == 2.0);
		CHECK(location->Array[2].AsNumber() == 3.0);
	}

	// 3. 落盘:实体文件必须真实包含 Hero 与 42
	{
		std::string out;
		CHECK(dispatch("scene.save", {}, out));
		bool foundHeroFile = false;
		for (const auto& entry : fs::recursive_directory_iterator(tempProj / "assets" / "levels" / "AiAuthored"))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".entity")
				continue;
			std::ifstream f(entry.path());
			const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (text.find("Hero") != std::string::npos)
			{
				foundHeroFile = true;
				CHECK(text.find("HealthComponent") != std::string::npos);
				CHECK(text.find("42") != std::string::npos);
			}
		}
		CHECK(foundHeroFile);
	}

	// 4. 二次独立加载:关闭会话后重新 Open,逐字段一致
	{
		SceneAuthoring::Get().Close();
		std::string error;
		CHECK(SceneAuthoring::Get().Open("AiAuthored", &error));

		std::string out;
		CHECK(dispatch("scene.get", { { "name", "Hero" } }, out));
		auto got = JsonValue::Parse(out, nullptr);
		CHECK(got.has_value());
		const auto* components = got->Find("components");
		const auto* health = components->Find("World::HealthComponent");
		CHECK(health != nullptr);
		CHECK(health->Find("CurrentHealth")->AsNumber() == 42.0);
		CHECK(health->Find("MaxHealth")->AsNumber() == 120.0);
		const auto* location = components->Find("World::TransformComponent")->Find("Location");
		CHECK(location->Array[1].AsNumber() == 2.0);
	}

	// 5. 删除 + 再次落盘/加载:实体必须真的消失
	{
		std::string out;
		CHECK(dispatch("scene.delete_entity", { { "name", "Hero" } }, out));
		CHECK(dispatch("scene.save", {}, out));
		SceneAuthoring::Get().Close();
		std::string error;
		CHECK(SceneAuthoring::Get().Open("AiAuthored", &error));
		CHECK(dispatch("scene.list", {}, out));
		CHECK(out.find("\"name\":\"Hero\"") == std::string::npos);
	}

	// 6. changeset.stage_write:真实落盘 + diff + 原子回滚
	{
		auto& tracker = ChangesetTracker::Get();
		const std::string csId = tracker.BeginChangeset(tempProj.string(), "gameplay_programmer", "round-trip script");
		CHECK(!csId.empty());

		const std::string scriptPath = "assets/scripts/systems/RoundTrip.luau";
		const std::string scriptCode =
			"-- round trip\nlocal M = {}\nfunction M.Update(dt)\n  -- no-op\nend\nreturn M\n";

		std::string out;
		CHECK(dispatch("changeset.stage_write",
			{ { "id", csId }, { "path", scriptPath }, { "content", scriptCode } }, out));
		CHECK(fs::exists(tempProj / scriptPath));

		std::string diff;
		CHECK(dispatch("changeset.diff", { { "id", csId } }, diff));
		CHECK(diff.find("RoundTrip.luau") != std::string::npos);

		CHECK(dispatch("changeset.revert", { { "id", csId } }, out));
		CHECK(!fs::exists(tempProj / scriptPath));
	}

	// 7. 路径逃逸必须被拒绝
	{
		auto& tracker = ChangesetTracker::Get();
		const std::string csId = tracker.BeginChangeset(tempProj.string(), "gameplay_programmer", "escape attempt");
		std::map<std::string, std::string> args = {
			{ "id", csId }, { "path", "../../Windows/System32/evil.txt" }, { "content", "x" }
		};
		std::string out;
		CHECK(!dispatch("changeset.stage_write", args, out));
	}

	SceneAuthoring::Get().Close();
	fs::remove_all(tempProj, ec);

	std::printf("WorldAiSceneAuthoringTests PASSED.\n");
	return 0;
}
