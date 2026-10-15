#include "World/Agent/WeAgentCore.h"
#include "World/Plugins/PluginManifest.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

// 直接引用插件源码以便完整测试规则逻辑
#include "../../plugins/we-studio/src/StudioPolicy.h"
#include "../../plugins/we-studio/src/BacklogManager.h"
#include "../../plugins/we-studio/src/StudioValidator.h"
#include "../../plugins/we-studio/src/AgentsDocGenerator.h"

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

		// 验证其余 5 大品类预设均能正常加载阶段
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

		// 权限检查
		CHECK(policy.CanRoleWritePath("gameplay_programmer", WorkflowStage::Prototype, "assets/scripts/hero.luau"));
		CHECK(!policy.CanRoleWritePath("game_designer", WorkflowStage::Prototype, "assets/scripts/hero.luau"));

		// 动态休眠与唤醒阶段
		auto conceptRoles = policy.GetActiveRolesForStage(WorkflowStage::Concept);
		bool foundProgInConcept = false;
		for (const auto& r : conceptRoles)
		{
			if (r == "gameplay_programmer") foundProgInConcept = true;
		}
		CHECK(!foundProgInConcept); // 玩法程序在阶段 0 (Concept) 休眠

		auto protoRoles = policy.GetActiveRolesForStage(WorkflowStage::Prototype);
		bool foundProgInProto = false;
		for (const auto& r : protoRoles)
		{
			if (r == "gameplay_programmer") foundProgInProto = true;
		}
		CHECK(foundProgInProto); // 玩法程序在阶段 2 (Prototype) 唤醒
	}

	// 4. 验证纯本地文件 Backlog & Bug 追踪
	const fs::path tempProj = fs::current_path() / "build" / "tmp_test_studio";
	std::error_code ec;
	fs::remove_all(tempProj, ec);
	fs::create_directories(tempProj, ec);

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
		CHECK(fs::exists(tempProj / ".we" / "studio" / "backlog" / (taskId + ".md")));

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
		const std::string bugId = backlog.CreateBug(tempProj.string(), bug);
		CHECK(!bugId.empty());
		CHECK(fs::exists(tempProj / ".we" / "studio" / "bugs" / (bugId + ".md")));

		auto blockers = backlog.ListBugs(tempProj.string(), "blocker");
		CHECK(blockers.size() == 1);
		CHECK(blockers[0].id == bugId);
	}

	// 5. 验证 StudioValidator 规则校验与自审自过检测
	{
		StudioPolicy cleanPolicy("default");
		auto rClean = StudioValidator::Validate(cleanPolicy);
		CHECK(rClean.valid);
		CHECK(rClean.errors.empty());

		// 构造循环自审错误
		StudioPolicy circularPolicy("lite");
		// 强制让 tester 审核 tester
		auto& rolesMut = const_cast<std::map<std::string, RoleDefinition>&>(circularPolicy.GetRoles());
		rolesMut["tester"].reviews.push_back("tester");

		auto rBad = StudioValidator::Validate(circularPolicy);
		CHECK(!rBad.valid);
		CHECK(!rBad.errors.empty());
		CHECK(rBad.errors[0].find("Circular review error") != std::string::npos);
	}

	// 6. 验证 AgentsDocGenerator 生成 AGENTS.md
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

	// 7. 验证三大 AI 插件清单 plugin.we.yaml 合法性 (杜绝 contributes 未知 face 报错)
	{
		using namespace World::Plugins;
		const fs::path manifests[] = {
			fs::current_path() / "plugins" / "we-studio" / "plugin.we.yaml",
			fs::current_path() / "plugins" / "we-gen" / "plugin.we.yaml",
			fs::current_path() / "plugins" / "we-ai-assistant" / "plugin.we.yaml"
		};

		for (const auto& mPath : manifests)
		{
			CHECK(fs::exists(mPath));
			PluginManifest manifest;
			std::string err;
			const bool ok = PluginManifest::Load(mPath, PluginScope::Engine, &manifest, &err);
			if (!ok)
			{
				std::fprintf(stderr, "Manifest validation failed for %s: %s\n", mPath.string().c_str(), err.c_str());
			}
			CHECK(ok);
			CHECK(!manifest.Id.empty());
			CHECK(manifest.Scope == PluginScope::Engine);
		}
	}

	// 8. 验证 CommandRegistry 集成
	{
		auto& reg = CommandRegistry::Get();
		BacklogManager::RegisterCommands(reg);
		StudioValidator::RegisterCommands(reg);

		CHECK(reg.HasCommand("task.create"));
		CHECK(reg.HasCommand("task.update"));
		CHECK(reg.HasCommand("task.list"));
		CHECK(reg.HasCommand("bug.create"));
		CHECK(reg.HasCommand("bug.update"));
		CHECK(reg.HasCommand("bug.list"));
		CHECK(reg.HasCommand("review.submit"));
		CHECK(reg.HasCommand("studio.generate_agents_doc"));
		CHECK(reg.HasCommand("studio.validate"));

		// 验证 review.submit 与 task.update/bug.update 派发
		std::string taskRes, bugRes, reviewRes, docRes, err;
		CHECK(reg.Dispatch("task.create", { {"title", "TestTask"}, {"role", "programmer"}, {"root", tempProj.string()} }, taskRes, err));
		auto tJson = JsonValue::Parse(taskRes, nullptr);
		CHECK(tJson.has_value());
		std::string tId = tJson->Find("task_id")->AsString();

		CHECK(reg.Dispatch("task.update", { {"task_id", tId}, {"status", "in_progress"}, {"root", tempProj.string()} }, taskRes, err));
		CHECK(reg.Dispatch("review.submit", { {"task_id", tId}, {"reviewer", "qa"}, {"verdict", "blocker"}, {"comments", "Bug found"}, {"root", tempProj.string()} }, reviewRes, err));

		CHECK(reg.Dispatch("studio.generate_agents_doc", { {"root", tempProj.string()} }, docRes, err));
		CHECK(fs::exists(tempProj / "AGENTS.md"));

		std::string result;
		std::string error;
		CHECK(reg.Dispatch("studio.validate", {}, result, error));
		auto json = JsonValue::Parse(result, nullptr);
		CHECK(json.has_value());
		CHECK(json->Find("valid") != nullptr);
	}

	fs::remove_all(tempProj, ec);

	std::printf("WorldAiStudioTests PASSED.\n");
	return 0;
}
