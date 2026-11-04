#include "World/Agent/WeAgentCore.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Ai/SelfValidation.h"
#include "World/Ai/PlaytestRunner.h"
#include "World/Ai/ChangesetTracker.h"
#include "World/Ai/WriteGuard.h"
#include "World/WUI/WuiJson.h"

#include "../../plugins/we-studio/src/StudioPolicy.h"
#include "../../plugins/we-studio/src/BacklogManager.h"
#include "../../plugins/we-studio/src/InterviewSystem.h"
#include "../../plugins/we-studio/src/StudioValidator.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
			std::exit(1); \
		} \
	} while (false)

namespace fs = std::filesystem;

namespace
{
	uint32_t PngCrc32(const uint8_t* data, size_t length)
	{
		uint32_t crc = 0xFFFFFFFF;
		for (size_t i = 0; i < length; ++i)
		{
			crc ^= data[i];
			for (int k = 0; k < 8; ++k)
				crc = (crc >> 1) ^ (0xEDB88320 & (-(int)(crc & 1)));
		}
		return ~crc;
	}

	uint32_t PngAdler32(const uint8_t* data, size_t length)
	{
		uint32_t a = 1, b = 0;
		for (size_t i = 0; i < length; ++i)
		{
			a = (a + data[i]) % 65521;
			b = (b + a) % 65521;
		}
		return (b << 16) | a;
	}

	void WriteBigEndian32(std::vector<uint8_t>& buf, uint32_t val)
	{
		buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
		buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
		buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
		buf.push_back(static_cast<uint8_t>(val & 0xFF));
	}

	std::vector<uint8_t> CreateTestPng32x32()
	{
		std::vector<uint8_t> png;
		const uint8_t sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
		png.insert(png.end(), sig, sig + 8);

		std::vector<uint8_t> ihdrData;
		ihdrData.push_back('I'); ihdrData.push_back('H'); ihdrData.push_back('D'); ihdrData.push_back('R');
		WriteBigEndian32(ihdrData, 32);
		WriteBigEndian32(ihdrData, 32);
		ihdrData.push_back(8);
		ihdrData.push_back(6);
		ihdrData.push_back(0);
		ihdrData.push_back(0);
		ihdrData.push_back(0);

		WriteBigEndian32(png, 13);
		png.insert(png.end(), ihdrData.begin(), ihdrData.end());
		WriteBigEndian32(png, PngCrc32(ihdrData.data(), ihdrData.size()));

		std::vector<uint8_t> rawPixels;
		rawPixels.reserve(32 * 129);
		for (int y = 0; y < 32; ++y)
		{
			rawPixels.push_back(0);
			for (int x = 0; x < 32; ++x)
			{
				rawPixels.push_back(0x20);
				rawPixels.push_back(0xA0);
				rawPixels.push_back(0x50);
				rawPixels.push_back(0xFF);
			}
		}

		std::vector<uint8_t> zlibStream;
		zlibStream.push_back(0x78);
		zlibStream.push_back(0x01);
		const uint16_t rawLen = static_cast<uint16_t>(rawPixels.size());
		const uint16_t rawNlen = ~rawLen;
		zlibStream.push_back(0x01);
		zlibStream.push_back(static_cast<uint8_t>(rawLen & 0xFF));
		zlibStream.push_back(static_cast<uint8_t>((rawLen >> 8) & 0xFF));
		zlibStream.push_back(static_cast<uint8_t>(rawNlen & 0xFF));
		zlibStream.push_back(static_cast<uint8_t>((rawNlen >> 8) & 0xFF));
		zlibStream.insert(zlibStream.end(), rawPixels.begin(), rawPixels.end());
		WriteBigEndian32(zlibStream, PngAdler32(rawPixels.data(), rawPixels.size()));

		std::vector<uint8_t> idatChunk;
		idatChunk.push_back('I'); idatChunk.push_back('D'); idatChunk.push_back('A'); idatChunk.push_back('T');
		idatChunk.insert(idatChunk.end(), zlibStream.begin(), zlibStream.end());

		WriteBigEndian32(png, static_cast<uint32_t>(zlibStream.size()));
		png.insert(png.end(), idatChunk.begin(), idatChunk.end());
		WriteBigEndian32(png, PngCrc32(idatChunk.data(), idatChunk.size()));

		const uint8_t iendChunk[12] = {
			0x00, 0x00, 0x00, 0x00,
			'I', 'E', 'N', 'D',
			0xAE, 0x42, 0x60, 0x82
		};
		png.insert(png.end(), iendChunk, iendChunk + 12);
		return png;
	}
}

int main()
{
	using namespace World::Agent;
	using namespace World::Ai;
	using namespace World::Studio;
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
	InterviewSystem::RegisterCommands(reg);
	StudioValidator::RegisterCommands(reg);

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

	// 3. 规范写入 2D 精灵资产产物
	const std::string targetAssetRel = "assets/textures/hero.png";
	const fs::path pngFile = tempProj / targetAssetRel;
	{
		auto pngBytes = CreateTestPng32x32();
		std::ofstream out(pngFile, std::ios::binary);
		CHECK(out.is_open());
		out.write(reinterpret_cast<const char*>(pngBytes.data()), pngBytes.size());
	}
	CHECK(fs::exists(pngFile));

	// 4. 自证命令族校验资产
	auto assetReport = SelfValidation::ValidateAsset(targetAssetRel, tempProj.string());
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

	// 7. 执行与验收分离校验
	{
		auto& bm = BacklogManager::Get();
		StudioTask t;
		t.title = "Implement Double Jump";
		t.role = "gameplay_programmer";
		t.stage = "0";
		const std::string tId = bm.CreateTask(tempProj.string(), t);

		std::string res, err;
		CHECK(!reg.Dispatch("review.submit", {
			{ "task_id", tId },
			{ "reviewer", "gameplay_programmer" },
			{ "verdict", "approved" },
			{ "root", tempProj.string()}
		}, res, err));
		CHECK(err.find("Self-review is forbidden") != std::string::npos);

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

		CHECK(fs::exists(tempProj / ".we" / "ai" / "state.json"));
		CHECK(fs::exists(tempProj / ".we" / "ai" / "journal" / "journal.md"));
	}

	fs::remove_all(tempProj, ec);

	std::printf("WorldAiEndToEndWorkflowTests PASSED (Headless Workflow Verified).\n");
	return 0;
}
