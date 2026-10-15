#include "World/Ai/PlaytestRunner.h"
#include "World/Scene/Scene.h"
#include "World/Core/WorldContext.h"
#include "World/Core/WorldContext.h"
#include "World/Ai/ProjectPackager.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

	std::printf("Running WorldAiPlaytestTests...\n");

	auto& registry = CommandRegistry::Get();

	// 1. 验证命令已自动注册到 CommandRegistry
	{
		CHECK(registry.HasCommand("playtest.run"));
		CHECK(registry.HasCommand("build.package"));

		const auto* descPlay = registry.GetDescriptor("playtest.run");
		CHECK(descPlay != nullptr && descPlay->effect == CommandEffect::Exec);

		const auto* descPkg = registry.GetDescriptor("build.package");
		CHECK(descPkg != nullptr && descPkg->effect == CommandEffect::Exec);
	}

	// 2. 验证 playtest.run 模拟推进与性能指标
	{
		std::string result;
		std::string error;
		std::map<std::string, std::string> args = {
			{"frames", "15"},
			{"dt", "0.016666"}
		};
		CHECK(registry.Dispatch("playtest.run", args, result, error));
		CHECK(!result.empty());

		auto json = JsonValue::Parse(result, nullptr);
		CHECK(json.has_value());
		const auto* passVal = json->Find("pass");
		CHECK(passVal && passVal->AsBool());

		const auto* framesVal = json->Find("frames_simulated");
		CHECK(framesVal && framesVal->AsNumber() == 15.0);

		const auto* perfObj = json->Find("perf");
		CHECK(perfObj && perfObj->type == JsonValue::Type::Object);
		CHECK(perfObj->Find("simulated_fps") != nullptr);
	}

	// 3. 验证 playtest.run 实体断言求值 (通过与拦截)
	{
		WorldContext context;
		Scene testScene(context);
		const entt::entity e = testScene.GetRegistry().create(); testScene.GetRegistry().emplace<TagComponent>(e, "Player");
		(void)e;

		PlaytestConfig cfg;
		cfg.frames = 5;
		cfg.asserts = { "entity_count > 0", "entity:Player exists" };

		PlaytestResult rPass = PlaytestRunner::Run(cfg, &testScene);
		CHECK(rPass.pass);
		CHECK(rPass.failures.empty());

		// 增加一条不成立的断言
		cfg.asserts.push_back("entity:NonExistentBoss exists");
		PlaytestResult rFail = PlaytestRunner::Run(cfg, &testScene);
		CHECK(!rFail.pass);
		CHECK(!rFail.failures.empty());
		CHECK(rFail.failures[0].find("NonExistentBoss") != std::string::npos);
	}

	// 4. 验证 build.package 打包功能
	{
		const fs::path tempProj = fs::current_path() / "build" / "tmp_test_packaging";
		std::error_code ec;
		fs::remove_all(tempProj, ec);
		fs::create_directories(tempProj / "assets" / "materials", ec);

		// 4.1 缺少清单时返回失败
		PackageConfig cfgFail;
		cfgFail.projectRoot = tempProj.string();
		auto resFail = ProjectPackager::Package(cfgFail);
		CHECK(!resFail.success);
		CHECK(!resFail.errors.empty());

		// 4.2 建立标准清单与测试资产
		{
			std::ofstream f(tempProj / "project.we.yaml");
			f << "id: TestPackageProj\nname: TestPackageProj\nversion: 1.0.0\ncontent_root: assets\n";
		}
		{
			std::ofstream a(tempProj / "assets" / "materials" / "dummy.txt");
			a << "placeholder content for package test\n";
		}

		PackageConfig cfgOk;
		cfgOk.projectRoot = tempProj.string();
		cfgOk.profile = "debug";
		auto resOk = ProjectPackager::Package(cfgOk);
		CHECK(resOk.success);
		CHECK(resOk.errors.empty());
		CHECK(resOk.artifactsCount > 0);
		CHECK(resOk.totalSizeBytes > 0);
		CHECK(fs::exists(fs::u8path(resOk.outputDirectory) / "assets" / "materials" / "dummy.txt"));

		// 4.3 验证通过 CommandRegistry 调用 build.package
		std::string result;
		std::string error;
		std::map<std::string, std::string> args = {
			{"root", tempProj.string()},
			{"profile", "shipping"}
		};
		CHECK(registry.Dispatch("build.package", args, result, error));
		auto jsonPkg = JsonValue::Parse(result, nullptr);
		CHECK(jsonPkg.has_value());
		CHECK(jsonPkg->Find("success") && jsonPkg->Find("success")->AsBool());

		fs::remove_all(tempProj, ec);
	}

	std::printf("WorldAiPlaytestTests PASSED.\n");
	return 0;
}
