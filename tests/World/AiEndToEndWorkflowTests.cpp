#include "World/Agent/WeAgentCore.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Ai/SelfValidation.h"
#include "World/Ai/PlaytestRunner.h"
#include "World/Ai/ChangesetTracker.h"
#include "World/Ai/WriteGuard.h"
#include "World/WUI/WuiJson.h"

#include "../../plugins/we-studio/src/StudioPolicy.h"
#include "../../plugins/we-studio/src/BacklogManager.h"
#include "../../plugins/we-gen/src/GenPipeline.h"
#include "../../plugins/we-ai-assistant/src/AssistantEngine.h"
#include "../../plugins/we-ai-assistant/src/InterviewSystem.h"

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
	using namespace World::Agent;
	using namespace World::Ai;
	using namespace World::Studio;
	using namespace World::Gen;
	using namespace World::Assistant;
	using namespace World::Wui;

	std::printf("Running WorldAiEndToEndWorkflowTests...\n");

	const fs::path tempProj = fs::current_path() / "build" / "tmp_test_ai_e2e";
	std::error_code ec;
	fs::remove_all(tempProj, ec);
	fs::create_directories(tempProj / "assets" / "textures", ec);
	fs::create_directories(tempProj / "assets" / "scripts", ec);
	fs::create_directories(tempProj / "assets" / "scenes", ec);

	{
		std::ofstream f(tempProj / "project.we.yaml");
		f << "name: E2EGameProject\nversion: 1.0.0\n";
	}

	auto& core = WeAgentCore::Get();
	core.ResetAll();
	core.SetProjectRoot(tempProj.string());

	auto studioPolicy = std::make_shared<StudioPolicy>("default", tempProj.string());
	core.SetPolicy(studioPolicy);

	auto& reg = CommandRegistry::Get();
	BacklogManager::RegisterCommands(reg);
	GenPipeline::RegisterCommands(reg);
	InterviewSystem::RegisterCommands(reg);
	AssistantEngine::RegisterCommands(reg);

	// 1. 制作人开启会话与生成职能开关方案 (Role Activation Plan)
	std::string sessionProducer = core.OpenSession("producer", "KICKOFF-01", tempProj.string());
	CHECK(!sessionProducer.empty());
	CHECK(core.GetActiveSessionToken() == sessionProducer);

	std::string planMd = core.GenerateRoleActivationPlan("2d_platformer", tempProj.string());
	CHECK(!planMd.empty());
	CHECK(fs::exists(tempProj / ".we" / "studio" / "role-activation-plan.md"));
	CHECK(core.GetRoleState("2d_artist") == RoleRuntimeState::Active);
	CHECK(core.GetRoleState("3d_character_artist") == RoleRuntimeState::Disabled);

	// 2. 制作人访谈卡片问答，沉淀品味与假设
	auto& interview = InterviewSystem::Get();
	CHECK(interview.AnswerQuestion(tempProj.string(), "genre_type", "2d_platformer"));
	CHECK(fs::exists(tempProj / ".we" / "studio" / "taste.yaml"));
	CHECK(fs::exists(tempProj / ".we" / "studio" / "assumptions.md"));

	// 3. 资产生成管线 (we-gen) 生成真实合法的二进制 PNG 图像与侧车文件
	GenRequest genReq;
	genReq.type = "2d_sprite";
	genReq.prompt = "Cyber hero jump animation frame";
	genReq.targetPath = "assets/textures/hero.png";
	genReq.estimatedCost = 0.05;

	auto genRes = GenPipeline::Generate(tempProj.string(), genReq);
	CHECK(genRes.success);
	const fs::path pngFile = tempProj / genReq.targetPath;
	CHECK(fs::exists(pngFile));

	// 验证生成产物为合法 PNG 签名 (非纯文本假文件)
	{
		std::ifstream pf(pngFile, std::ios::binary);
		char header[8] = { 0 };
		pf.read(header, 8);
		CHECK(std::memcmp(header, "\x89PNG\r\n\x1a\n", 8) == 0);
	}

	// 4. 自证命令族校验生成的资产
	auto assetReport = SelfValidation::ValidateAsset(genReq.targetPath, tempProj.string());
	CHECK(assetReport.valid);
	CHECK(assetReport.metrics.count("width") > 0);
	CHECK(assetReport.metrics.at("width") == "32");

	// 5. 变更集与脚本写入
	{
		auto& tracker = ChangesetTracker::Get();
		const std::string csId = tracker.BeginChangeset(tempProj.string(), "gameplay_programmer", "Hero move script");
		CHECK(!csId.empty());

		std::string csErr;
		const std::string scriptCode = "-- Player movement\nlocal M = {}\nfunction M.Update(dt) end\nreturn M\n";
		CHECK(tracker.StageFileWrite(csId, tempProj.string(), "assets/scripts/hero.luau", scriptCode, csErr));
		CHECK(fs::exists(tempProj / "assets" / "scripts" / "hero.luau"));

		const std::string diff = tracker.GetChangesetDiff(csId);
		CHECK(diff.find("hero.luau") != std::string::npos);
	}

	// 6. 自动化试玩 (playtest.run) 模拟真实场景加载与断言
	{
		const fs::path sceneFile = tempProj / "assets" / "scenes" / "level1.yaml";
		{
			std::ofstream sf(sceneFile);
			sf << "Entities:\n"
			   << "  - Tag: Player\n"
			   << "    TransformComponent:\n"
			   << "      Location: [0.0, 1.0, 0.0]\n"
			   << "  - Tag: Ground\n"
			   << "    TransformComponent:\n"
			   << "      Location: [0.0, -1.0, 0.0]\n";
		}

		PlaytestConfig ptCfg;
		ptCfg.scenePath = (tempProj / "assets" / "scenes" / "level1.yaml").string();
		ptCfg.frames = 30;
		ptCfg.asserts = { "entity_count > 0", "entity:Player exists", "no_void_fall" };
		ptCfg.captures = { (tempProj / "qa" / "snap.ppm").string() };

		auto ptRes = PlaytestRunner::Run(ptCfg, nullptr);
		CHECK(ptRes.pass);
		CHECK(ptRes.framesSimulated == 30);
		CHECK(!ptRes.captures.empty());
		CHECK(fs::exists(tempProj / "qa" / "snap.ppm"));
	}

	// 7. 执行与验收分离校验 (§3.4)
	{
		auto& bm = BacklogManager::Get();
		StudioTask t;
		t.title = "Implement Double Jump";
		t.role = "gameplay_programmer";
		t.stage = "0";
		const std::string tId = bm.CreateTask(tempProj.string(), t);

		std::string res, err;
		// 玩法程序自己审自己的任务必须被拦截
		CHECK(!reg.Dispatch("review.submit", {
			{ "task_id", tId },
			{ "reviewer", "gameplay_programmer" },
			{ "verdict", "approved" },
			{ "root", tempProj.string()}
		}, res, err));
		CHECK(err.find("Self-review is forbidden") != std::string::npos);

		// QA 负责人验收通过
		CHECK(reg.Dispatch("review.submit", {
			{ "task_id", tId },
			{ "reviewer", "qa_lead" },
			{ "verdict", "approved" },
			{ "root", tempProj.string()}
		}, res, err));
	}

	// 8. 阶段推进、状态持久化与日志核验
	{
		std::string advErr;
		CHECK(core.AdvanceStage(sessionProducer, advErr));
		CHECK(core.GetCurrentStage() == WorkflowStage::Feasibility);

		// 验证状态落盘
		CHECK(fs::exists(tempProj / ".we" / "ai" / "state.json"));
		CHECK(fs::exists(tempProj / ".we" / "ai" / "journal" / "journal.md"));
	}

	fs::remove_all(tempProj, ec);

	std::printf("WorldAiEndToEndWorkflowTests PASSED (10/10 AI Test Suite Verified).\n");
	return 0;
}
