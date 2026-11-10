#include "World/Agent/WeAgentCore.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

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

namespace
{
	class MockBlockingPolicy : public World::Agent::IWorkflowPolicy
	{
	public:
		std::string GetPolicyName() const override { return "MockBlockingPolicy"; }
		bool CanRoleExecuteCommand(const std::string&, World::Agent::WorkflowStage, const std::string&) override { return true; }
		bool CanRoleWritePath(const std::string&, World::Agent::WorkflowStage, const std::string&) override { return true; }
		World::Agent::GateEvaluationResult EvaluateGate(World::Agent::WorkflowStage stage, const std::string&) override
		{
			World::Agent::GateEvaluationResult res;
			res.stage = static_cast<uint32_t>(stage);
			res.pass = false;
			res.blockers.push_back("Custom policy blocking transition unconditionally");
			return res;
		}
		std::vector<std::string> GetActiveRolesForStage(World::Agent::WorkflowStage) override { return { "all" }; }
	};
}

int main()
{
	using namespace World::Agent;
	using namespace World::Ai;
	using namespace World::Wui;

	std::printf("Running WorldAiAgentCoreTests...\n");

	auto& core = WeAgentCore::Get();
	core.ResetAll();

	// 1. 验证默认策略为内置最小策略，初始阶段为 Concept
	{
		CHECK(core.GetPolicy() != nullptr);
		CHECK(core.GetPolicy()->GetPolicyName() == "MinimalFallbackPolicy");
		CHECK(core.GetCurrentStage() == WorkflowStage::Concept);
	}

	// 2. 验证会话生命周期 (开启、绑定角色与任务、查询、列出、关闭)
	{
		const std::string token = core.OpenSession("programmer", "TASK-101");
		CHECK(!token.empty());
		CHECK(token.rfind("sess_", 0) == 0);

		const auto* ctx = core.GetSession(token);
		CHECK(ctx != nullptr);
		CHECK(ctx->token == token);
		CHECK(ctx->roleId == "programmer");
		CHECK(ctx->currentTaskId == "TASK-101");
		CHECK(ctx->activeStage == WorkflowStage::Concept);

		auto sessions = core.ListSessions();
		CHECK(sessions.size() == 1);

		CHECK(core.CloseSession(token));
		CHECK(core.GetSession(token) == nullptr);
		CHECK(core.ListSessions().empty());
	}

	// 3. 验证阶段状态机推进与门禁检查
	const fs::path tempProj = fs::current_path() / "build" / "tmp_test_agent_core";
	std::error_code ec;
	fs::remove_all(tempProj, ec);
	fs::create_directories(tempProj / "assets" / "scripts", ec);
	{
		std::ofstream f(tempProj / "project.we.yaml");
		f << "name: AgentCoreTest\nversion: 1.0.0\n";
	}

	{
		const std::string token = core.OpenSession("producer", "MAIN-FLOW", tempProj.string());

		// 3.1 当前阶段为 Concept，自证通过，推进至 Feasibility
		std::string err;
		CHECK(core.AdvanceStage(token, err));
		CHECK(core.GetCurrentStage() == WorkflowStage::Feasibility);

		// 3.2 推进至 Prototype
		CHECK(core.AdvanceStage(token, err));
		CHECK(core.GetCurrentStage() == WorkflowStage::Prototype);

		// 3.3 受控回退至 Concept
		CHECK(core.RollbackToStage(token, WorkflowStage::Concept, err));
		CHECK(core.GetCurrentStage() == WorkflowStage::Concept);

		// 3.4 非法回退 (回退至同阶段或后续阶段) 必须失败
		CHECK(!core.RollbackToStage(token, WorkflowStage::Concept, err));
		CHECK(!core.RollbackToStage(token, WorkflowStage::Beta, err));
	}

	// 4. 验证自定义策略注入与门禁阻断
	{
		core.SetPolicy(std::make_shared<MockBlockingPolicy>());
		CHECK(core.GetPolicy()->GetPolicyName() == "MockBlockingPolicy");

		const std::string token = core.OpenSession("producer");
		std::string err;
		// 必须被 MockBlockingPolicy 强行阻断
		CHECK(!core.AdvanceStage(token, err));
		CHECK(err.find("blocker(s) present") != std::string::npos);
		CHECK(core.GetCurrentStage() == WorkflowStage::Concept);

		core.ResetToMinimalPolicy();
		CHECK(core.GetPolicy()->GetPolicyName() == "MinimalFallbackPolicy");
	}

	// 5. 验证引擎需求缺口上报 (engine.request_change)
	{
		EngineChangeRequest req;
		req.title = "Audio SPSC Queue Overflow";
		req.problem = "Rapid-fire SFX drops events under heavy concurrency.";
		req.repro = "Play 500 sound events in one frame.";
		req.proposedApi = "AudioSystem::SetQueueCapacity(size_t)";
		req.workaround = "Throttle events manually in script.";
		req.blocking = true;

		const std::string reqId = core.RequestEngineChange(req, tempProj.string());
		CHECK(!reqId.empty());

		const fs::path reqFile = tempProj / ".we" / "ai" / "engine-requests" / (reqId + ".yaml");
		CHECK(fs::exists(reqFile));

		auto list = core.ListEngineRequests(tempProj.string());
		CHECK(!list.empty());
		CHECK(list[0].id == reqId);
		CHECK(list[0].title == req.title);
		CHECK(list[0].blocking == true);
	}

	// 6. 验证 CommandRegistry 注册与统一派发
	{
		auto& reg = CommandRegistry::Get();
		CHECK(reg.HasCommand("session.open"));
		CHECK(reg.HasCommand("session.close"));
		CHECK(reg.HasCommand("session.list"));
		CHECK(reg.HasCommand("workflow.status"));
		CHECK(reg.HasCommand("workflow.advance"));
		CHECK(reg.HasCommand("workflow.rollback"));
		CHECK(reg.HasCommand("gate.check"));
		CHECK(reg.HasCommand("engine.request_change"));

		std::string res;
		std::string err;
		std::map<std::string, std::string> args = { {"role", "designer"}, {"task", "UI-01"} };
		CHECK(reg.Dispatch("session.open", args, res, err));
		auto json = JsonValue::Parse(res, nullptr);
		CHECK(json.has_value());
		const auto* tok = json->Find("token");
		CHECK(tok != nullptr);

		// workflow.status
		CHECK(reg.Dispatch("workflow.status", {}, res, err));
		auto stJson = JsonValue::Parse(res, nullptr);
		CHECK(stJson.has_value());
		CHECK(stJson->Find("current_stage") != nullptr);
	}

	fs::remove_all(tempProj, ec);

	std::printf("WorldAiAgentCoreTests PASSED.\n");
	return 0;
}
