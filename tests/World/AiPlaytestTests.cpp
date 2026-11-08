#include "World/Ai/PlaytestRunner.h"
#include "World/Scene/Scene.h"
#include "World/Core/WorldContext.h"
#include "World/Core/WorldContext.h"
#include "World/Ai/ProjectPackager.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Scene/Components.h"
#include "World/Core/Input.h"
#include "World/Core/KeyCodes.h"
#include "World/Script/Runtime/ScriptEngine.h" 

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

	// 3.1 验证 playtest.run 真实输入注入与边沿检测
	{
		WorldContext context;
		Scene testScene(context);

		PlaytestConfig cfg;
		cfg.frames = 4;
		cfg.inputs = {
			{ 1, "key_down", "Space", 0.0f, 0.0f },
			{ 3, "key_up", "Space", 0.0f, 0.0f }
		};

		int pressedCount = 0;
		int justPressedCount = 0;
		int justReleasedCount = 0;

		testScene.RegisterFrameSystem({
			"InputTestSystem",
			false,
			[&](Timestep)
			{
				if (Input::IsKeyPressed(KeyCodes::Space))
					pressedCount++;
				if (Input::IsKeyJustPressed(KeyCodes::Space))
					justPressedCount++;
				if (Input::IsKeyJustReleased(KeyCodes::Space))
					justReleasedCount++;
			}
		});

		PlaytestResult r = PlaytestRunner::Run(cfg, &testScene);
		CHECK(r.pass);
		// Frame 0: Space 未按下
		// Frame 1: Space 注入按下 -> IsKeyPressed=true, IsKeyJustPressed=true
		// Frame 2: Space 保持按下 -> IsKeyPressed=true, IsKeyJustPressed=false
		// Frame 3: Space 注入抬起 -> IsKeyPressed=false, IsKeyJustReleased=true
		CHECK(pressedCount == 2);
		CHECK(justPressedCount == 1);
		CHECK(justReleasedCount == 1);
	}

	// 3.2 验证 playtest.run 脚本异常硬性拦截
	{
		WorldContext context;
		Scene testScene(context);

		PlaytestConfig cfg;
		cfg.frames = 5;
		cfg.asserts = { "no_script_errors" };

		// 模拟推帧期间产生脚本异常
		testScene.RegisterFrameSystem({
			"FailingScriptSystem",
			false,
			[&](Timestep)
			{
				ScriptEngine::RecordScriptError("Simulated syntax error in test script");
			}
		});

		PlaytestResult r = PlaytestRunner::Run(cfg, &testScene);
		CHECK(!r.pass);
		CHECK(!r.failures.empty());
		CHECK(r.failures[0].find("Simulated syntax error") != std::string::npos);
	}

	// 3.3 验证 playtest.run 数值不变量断言求值
	{
		WorldContext context;
		Scene testScene(context);
		const entt::entity e = testScene.GetRegistry().create();
		testScene.GetRegistry().emplace<TagComponent>(e, "Hero");
		auto& hp = testScene.GetRegistry().emplace<HealthComponent>(e);
		hp.CurrentHealth = 60.0f;
		hp.MaxHealth = 80.0f;

		PlaytestConfig cfg;
		cfg.frames = 2;
		cfg.asserts = {
			"assert:HealthComponent.CurrentHealth < 80",
			"assert:Hero.HealthComponent.CurrentHealth >= 60",
			"assert:Hero.HealthComponent.CurrentHealth == 60"
		};

		PlaytestResult rPass = PlaytestRunner::Run(cfg, &testScene);
		CHECK(rPass.pass);
		CHECK(rPass.failures.empty());

		// 增加一条不成立的数值断言
		cfg.asserts.push_back("assert:Hero.HealthComponent.CurrentHealth < 50");
		PlaytestResult rFail = PlaytestRunner::Run(cfg, &testScene);
		CHECK(!rFail.pass);
		CHECK(!rFail.failures.empty());
		CHECK(rFail.failures[0].find("HealthComponent.CurrentHealth < 50") != std::string::npos);
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

	// 5. 验证物理硬门禁：退化零尺寸拦截与输入位移因果因变硬断言
	{
		WorldContext context;
		Scene scene(context);

		// 5.1 退化零尺寸拦截 (Degenerate Scale)
		const entt::entity dummy = scene.GetRegistry().create();
		scene.GetRegistry().emplace<TagComponent>(dummy, "FakeBoss");
		scene.GetRegistry().emplace<SpriteComponent>(dummy);
		auto& tc = scene.GetRegistry().emplace<TransformComponent>(dummy);
		tc.Scale = glm::vec3(0.0f, 0.0f, 0.0f); // 零尺寸

		PlaytestConfig cfgScale;
		cfgScale.frames = 10;
		auto resScale = PlaytestRunner::Run(cfgScale, &scene);
		CHECK(!resScale.pass);
		bool foundScaleError = false;
		for (const auto& f : resScale.failures) {
			if (f.find("degenerate zero scale") != std::string::npos) foundScaleError = true;
		}
		CHECK(foundScaleError);

		// 修正尺寸为正常
		tc.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

		// 5.2 输入位移因果硬断言：有移动输入但角色原地不动，强制判负
		const entt::entity pEnt = scene.GetRegistry().create();
		scene.GetRegistry().emplace<TagComponent>(pEnt, "char_hero");
		scene.GetRegistry().emplace<SpriteComponent>(pEnt);
		scene.GetRegistry().emplace<AudioListenerComponent>(pEnt);
		auto& pTc = scene.GetRegistry().emplace<TransformComponent>(pEnt);
		pTc.Location = glm::vec3(0.0f, 0.0f, 0.0f);
		pTc.Scale = glm::vec3(1.0f, 1.0f, 1.0f);

		PlaytestConfig cfgMove;
		cfgMove.frames = 30;
		// 注入 15 帧的移动按键
		for (int f = 0; f < 15; ++f) {
			PlaytestInputEvent inp;
			inp.frame = f;
			inp.type = "key_down";
			inp.key = "D";
			cfgMove.inputs.push_back(inp);
		}

		// 此时没有挂载移动系统，角色原地不动 (delta = 0)，物理硬断言必须判负！
		auto resMoveFail = PlaytestRunner::Run(cfgMove, &scene);
		CHECK(!resMoveFail.pass);
		bool foundStationaryError = false;
		for (const auto& f : resMoveFail.failures) {
			if (f.find("remained stationary") != std::string::npos) foundStationaryError = true;
		}
		CHECK(foundStationaryError);

		// 5.3 显式断言 player_moved 拦截
		PlaytestConfig cfgExplicit;
		cfgExplicit.frames = 10;
		cfgExplicit.asserts.push_back("player_moved");
		auto resExplicit = PlaytestRunner::Run(cfgExplicit, &scene);
		CHECK(!resExplicit.pass);
	}

	std::printf("WorldAiPlaytestTests PASSED.\n");
	return 0;
}
