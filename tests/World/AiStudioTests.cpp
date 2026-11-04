#include "World/Agent/WeAgentCore.h"
#include "World/Plugins/PluginManifest.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

// 直接引用插件源码以便完整测试规则逻辑
#include "../../plugins/we-studio/src/StudioPolicy.h"
#include "../../plugins/we-studio/src/BacklogManager.h"
#include "../../plugins/we-studio/src/StudioValidator.h"
#include "../../plugins/we-studio/src/AgentsDocGenerator.h"
#include "../../plugins/we-studio/src/InterviewSystem.h"

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
	using namespace World::Studio;
	using namespace World::Agent;
	using namespace World::Ai;
	using namespace World::Wui;

	std::printf("Running WorldAiStudioTests...\n");

	// 1. 验证默认预设包 default 加载
	{
		StudioPolicy defaultPolicy("default");
		CHECK(defaultPolicy.GetStages().size() == 8);
		CHECK(!defaultPolicy.GetRoles().empty());
		CHECK(defaultPolicy.GetRoles().count("producer") == 1);
		CHECK(defaultPolicy.GetRoles().count("gameplay_programmer") == 1);
		CHECK(defaultPolicy.GetRoles().count("qa_director") == 1);
	}

	// 2. 验证极速预设包 lite (Game Jam 5 角色 / 4 阶段) 与其余 5 大品类预设包
	{
		StudioPolicy litePolicy("lite");
		CHECK(litePolicy.GetStages().size() == 4);
		CHECK(litePolicy.GetRoles().size() == 5);
		CHECK(litePolicy.GetRoles().count("producer") == 1);
		CHECK(litePolicy.GetRoles().count("designer") == 1);
		CHECK(litePolicy.GetRoles().count("artist") == 1);
		CHECK(litePolicy.GetRoles().count("programmer") == 1);
		CHECK(litePolicy.GetRoles().count("tester") == 1);

		StudioPolicy p2d("2d-standard");
		CHECK(p2d.GetStages().size() == 4);

		StudioPolicy p3d("3d-nextgen");
		CHECK(p3d.GetStages().size() == 4);

		StudioPolicy pStyle("3d-stylized");
		CHECK(pStyle.GetStages().size() == 4);

		StudioPolicy pNet("multiplayer");
		CHECK(pNet.GetStages().size() == 4);

		StudioPolicy pStrat("strategy");
		CHECK(pStrat.GetStages().size() == 4);
	}

	// 3. 验证基于角色与阶段的权限判定与动态休眠
	{
		StudioPolicy policy("default");

		CHECK(policy.CanRoleWritePath("gameplay_programmer", WorkflowStage::Prototype, "assets/scripts/hero.luau"));
		CHECK(!policy.CanRoleWritePath("game_designer", WorkflowStage::Prototype, "assets/scripts/hero.luau"));

		auto conceptRoles = policy.GetActiveRolesForStage(WorkflowStage::Concept);
		bool foundProgInConcept = false;
		for (const auto& r : conceptRoles)
		{
			if (r == "gameplay_programmer") foundProgInConcept = true;
		}
		CHECK(!foundProgInConcept);

		auto protoRoles = policy.GetActiveRolesForStage(WorkflowStage::Prototype);
		bool foundProgInProto = false;
		for (const auto& r : protoRoles)
		{
			if (r == "gameplay_programmer") foundProgInProto = true;
		}
		CHECK(foundProgInProto);
	}

	// 4. 验证纯本地文件 Backlog & Bug 追踪
	const fs::path tempProj = fs::current_path() / "build" / "tmp_test_studio";
	std::error_code ec;
	fs::remove_all(tempProj, ec);
	fs::create_directories(tempProj, ec);

	std::string createdBugId;
	{
		auto& backlog = BacklogManager::Get();

		// 4.1 创建并更新任务
		StudioTask task;
		task.title = "Implement Boss Attack Pattern";
		task.role = "gameplay_programmer";
		task.stage = "prototype";
		task.description = "Boss performs 3-hit combo and laser beam.";
		const std::string taskId = backlog.CreateTask(tempProj.string(), task);
		CHECK(!taskId.empty());
		CHECK(fs::exists(tempProj / ".we" / "studio" / "backlog" / "tech" / (taskId + ".md")));

		auto tasks = backlog.ListTasks(tempProj.string(), "open");
		CHECK(tasks.size() == 1);
		CHECK(tasks[0].title == task.title);

		CHECK(backlog.UpdateTask(tempProj.string(), taskId, "done", "Implemented and tested clean."));
		auto tasksDone = backlog.ListTasks(tempProj.string(), "done");
		CHECK(tasksDone.size() == 1);

		// 4.2 创建并列出缺陷
		StudioBug bug;
		bug.title = "Audio drops out during explosion";
		bug.severity = "blocker";
		bug.role = "gameplay_tester";
		bug.repro = "Spawn 50 bombs simultaneously.";
		createdBugId = backlog.CreateBug(tempProj.string(), bug);
		CHECK(!createdBugId.empty());
		CHECK(fs::exists(tempProj / ".we" / "studio" / "bugs" / (createdBugId + ".md")));

		auto blockers = backlog.ListBugs(tempProj.string(), "blocker");
		CHECK(blockers.size() == 1);
		CHECK(blockers[0].id == createdBugId);
	}

	// 5. 验证 StudioValidator 规则校验与项目真实 Blocker 门禁阻断
	{
		StudioPolicy cleanPolicy("default");

		// 5.1 存在未关闭的 blocker 时，项目校验必须真实阻断！(杜绝假门禁)
		auto rBlocked = StudioValidator::Validate(cleanPolicy, tempProj.string());
		CHECK(!rBlocked.valid);
		CHECK(!rBlocked.errors.empty());
		CHECK(rBlocked.errors[0].find("Active Blocker Bug detected") != std::string::npos);

		// 5.2 修复/关闭 Blocker 缺陷后，项目校验通过
		CHECK(BacklogManager::Get().UpdateBug(tempProj.string(), createdBugId, "closed", "Fixed audio voice limit."));
		auto rPassed = StudioValidator::Validate(cleanPolicy, tempProj.string());
		CHECK(rPassed.valid);
		CHECK(rPassed.errors.empty());

		// 5.3 循环自审策略错误检测
		StudioPolicy circularPolicy("lite");
		auto& rolesMut = const_cast<std::map<std::string, RoleDefinition>&>(circularPolicy.GetRoles());
		rolesMut["tester"].reviews.push_back("tester");

		auto rBad = StudioValidator::Validate(circularPolicy);
		CHECK(!rBad.valid);
		CHECK(!rBad.errors.empty());
		CHECK(rBad.errors[0].find("Circular review error") != std::string::npos);
	}

	// 6. 验证合并进 we-studio 的 InterviewSystem 与 taste.yaml 防重复键记录
	{
		auto& interview = InterviewSystem::Get();
		auto round = interview.GetCurrentRound(tempProj.string(), 0);
		CHECK(round.round == 1);
		CHECK(round.questions.size() <= 3);

		CHECK(interview.AnswerQuestion(tempProj.string(), "genre_type", "2d_platformer"));
		CHECK(interview.AnswerQuestion(tempProj.string(), "genre_type", "3d_action")); // 再次切换选项

		// 验证 taste.yaml 不重复堆叠相同 key
		std::string taste = interview.GetTasteProfile(tempProj.string());
		CHECK(taste.find("3d_action") != std::string::npos);

		// 检查 assumptions.md 沉淀
		CHECK(fs::exists(tempProj / ".we" / "studio" / "assumptions.md"));
	}

	// 7. 验证 AgentsDocGenerator 生成 AGENTS.md
	{
		StudioPolicy policy("default");
		std::string doc = AgentsDocGenerator::Generate(policy);
		CHECK(!doc.empty());
		CHECK(doc.find("Project Agent Guidelines") != std::string::npos);
		CHECK(doc.find("we-mcp") != std::string::npos);
		CHECK(doc.find("gameplay_programmer") != std::string::npos);

		CHECK(AgentsDocGenerator::WriteToProject(tempProj.string(), policy));
		CHECK(fs::exists(tempProj / "AGENTS.md"));
		CHECK(fs::exists(tempProj / "CLAUDE.md"));
	}

	// 8. 验证保留的核心 Headless 插件清单 plugin.we.yaml 合法性
	{
		using namespace World::Plugins;
		const fs::path manifestPath = fs::current_path() / "plugins" / "we-studio" / "plugin.we.yaml";
		CHECK(fs::exists(manifestPath));
		PluginManifest manifest;
		std::string err;
		const bool ok = PluginManifest::Load(manifestPath, PluginScope::Engine, &manifest, &err);
		CHECK(ok);
		CHECK(manifest.Id == "engine.we-studio");
		CHECK(manifest.Scope == PluginScope::Engine);
	}

	// 9. 验证 CommandRegistry 集成与兼容别名
	{
		auto& reg = CommandRegistry::Get();
		BacklogManager::RegisterCommands(reg);
		StudioValidator::RegisterCommands(reg);
		InterviewSystem::RegisterCommands(reg);

		CHECK(reg.HasCommand("task.create"));
		CHECK(reg.HasCommand("task.update"));
		CHECK(reg.HasCommand("task.list"));
		CHECK(reg.HasCommand("bug.create"));
		CHECK(reg.HasCommand("bug.update"));
		CHECK(reg.HasCommand("bug.list"));
		CHECK(reg.HasCommand("review.submit"));
		CHECK(reg.HasCommand("studio.validate"));
		CHECK(reg.HasCommand("studio.generate_agents_doc"));
		CHECK(reg.HasCommand("studio.interview.get"));
		CHECK(reg.HasCommand("assistant.interview.get")); // 兼容别名
		CHECK(reg.HasCommand("studio.interview.answer"));
		CHECK(reg.HasCommand("assistant.interview.answer")); // 兼容别名
		CHECK(reg.HasCommand("studio.taste.get"));
		CHECK(reg.HasCommand("assistant.taste.get")); // 兼容别名
	}

	fs::remove_all(tempProj, ec);

	std::printf("WorldAiStudioTests PASSED (Studio Headless & Real Gate Verified).\n");
	return 0;
}
