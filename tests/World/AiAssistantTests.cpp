#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

// 引入 we-ai-assistant 插件源码进行单元测试
#include "../../plugins/we-ai-assistant/src/InterviewSystem.h"
#include "../../plugins/we-ai-assistant/src/AssistantEngine.h"

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
	using namespace World::Assistant;
	using namespace World::Ai;
	using namespace World::Wui;

	std::printf("Running WorldAiAssistantTests...\n");

	const fs::path tempProj = fs::current_path() / "build" / "tmp_test_assistant";
	std::error_code ec;
	fs::remove_all(tempProj, ec);
	fs::create_directories(tempProj, ec);

	// 1. 验证 L1 制作人 3 题访谈卡片契约 (每轮 <= 3 题，且必含推荐项)
	{
		auto& interview = InterviewSystem::Get();
		InterviewRound round = interview.GetCurrentRound(tempProj.string(), 0);
		CHECK(round.round == 1);
		CHECK(!round.questions.empty());
		CHECK(round.questions.size() <= 3);

		for (const auto& q : round.questions)
		{
			CHECK(!q.id.empty());
			CHECK(!q.header.empty());
			CHECK(!q.prompt.empty());
			CHECK(!q.options.empty());

			bool hasRecommended = false;
			for (const auto& opt : q.options)
			{
				if (opt.isRecommended) hasRecommended = true;
			}
			CHECK(hasRecommended);
		}
	}

	// 2. 验证回答问题、假设沉淀 assumptions.md 与品味档案 taste.yaml 记录
	{
		auto& interview = InterviewSystem::Get();
		CHECK(interview.AnswerQuestion(tempProj.string(), "genre_type", "2d_platformer"));

		// 检查 assumptions.md
		const fs::path assumptionsFile = tempProj / "assumptions.md";
		CHECK(fs::exists(assumptionsFile));
		{
			std::ifstream af(assumptionsFile);
			std::string aText((std::istreambuf_iterator<char>(af)), std::istreambuf_iterator<char>());
			CHECK(aText.find("genre_type") != std::string::npos);
			CHECK(aText.find("2d_platformer") != std::string::npos);
		}

		// 检查 taste.yaml
		const fs::path tasteFile = tempProj / "taste.yaml";
		CHECK(fs::exists(tasteFile));
		{
			std::ifstream tf(tasteFile);
			std::string tText((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
			CHECK(tText.find("genre_type: \"2d_platformer\"") != std::string::npos);
		}

		const std::string tasteProfile = interview.GetTasteProfile(tempProj.string());
		CHECK(tasteProfile.find("2d_platformer") != std::string::npos);
	}

	// 3. 验证 AssistantEngine 对话交互
	{
		std::string chatResp = AssistantEngine::Chat("我想做一款类似蔚蓝的平台跳跃游戏", "producer");
		CHECK(!chatResp.empty());

		auto json = JsonValue::Parse(chatResp, nullptr);
		CHECK(json.has_value());
		CHECK(json->Find("role")->AsString() == "producer");
		CHECK(!json->Find("reply")->AsString().empty());
	}

	// 3b. 验证 "做个游戏" 意图一键立项全流程触发
	{
		std::string kickoffResp = AssistantEngine::Chat("做个游戏", "producer");
		CHECK(!kickoffResp.empty());
		auto json = JsonValue::Parse(kickoffResp, nullptr);
		CHECK(json.has_value());
		CHECK(json->Find("reply")->AsString().find("已为您一键启动游戏立项全流程") != std::string::npos);
	}

	// 4. 验证 CommandRegistry 集成与统一派发
	{
		auto& reg = CommandRegistry::Get();
		InterviewSystem::RegisterCommands(reg);
		AssistantEngine::RegisterCommands(reg);

		CHECK(reg.HasCommand("assistant.interview.get"));
		CHECK(reg.HasCommand("assistant.interview.answer"));
		CHECK(reg.HasCommand("assistant.taste.get"));
		CHECK(reg.HasCommand("assistant.chat"));

		std::string result;
		std::string error;
		std::map<std::string, std::string> args = {
			{"message", "请帮我检查当前阶段"},
			{"role", "qa"}
		};
		CHECK(reg.Dispatch("assistant.chat", args, result, error));
		auto json = JsonValue::Parse(result, nullptr);
		CHECK(json.has_value());
		CHECK(json->Find("role")->AsString() == "qa");
	}

	fs::remove_all(tempProj, ec);

	std::printf("WorldAiAssistantTests PASSED.\n");
	return 0;
}
