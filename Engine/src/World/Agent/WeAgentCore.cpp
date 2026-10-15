#include "wldpch.h"
#include "World/Agent/WeAgentCore.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Ai/SelfValidation.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Agent
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	const char* WorkflowStageToString(WorkflowStage stage)
	{
		switch (stage)
		{
		case WorkflowStage::Concept:       return "concept";
		case WorkflowStage::Feasibility:   return "feasibility";
		case WorkflowStage::Prototype:     return "prototype";
		case WorkflowStage::VerticalSlice: return "vertical_slice";
		case WorkflowStage::Production:    return "production";
		case WorkflowStage::Beta:          return "beta";
		case WorkflowStage::Launch:        return "launch";
		case WorkflowStage::LiveOps:       return "liveops";
		default:                           return "concept";
		}
	}

	WorkflowStage StringToWorkflowStage(const std::string& str)
	{
		if (str == "feasibility")    return WorkflowStage::Feasibility;
		if (str == "prototype")      return WorkflowStage::Prototype;
		if (str == "vertical_slice") return WorkflowStage::VerticalSlice;
		if (str == "production")     return WorkflowStage::Production;
		if (str == "beta")           return WorkflowStage::Beta;
		if (str == "launch")         return WorkflowStage::Launch;
		if (str == "liveops")        return WorkflowStage::LiveOps;
		return WorkflowStage::Concept;
	}

	std::string SessionContext::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;

		root.Object.push_back({ "token", JsonValue::MakeString(token) });
		root.Object.push_back({ "role", JsonValue::MakeString(roleId) });
		root.Object.push_back({ "task", JsonValue::MakeString(currentTaskId) });
		root.Object.push_back({ "stage", JsonValue::MakeString(WorkflowStageToString(activeStage)) });

		const auto now = std::chrono::system_clock::now();
		const double durationSec = std::chrono::duration<double>(now - startTime).count();
		root.Object.push_back({ "duration_sec", JsonValue::MakeNumber(durationSec) });

		return root.Dump();
	}

	std::string GateEvaluationResult::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;

		root.Object.push_back({ "pass", JsonValue::MakeBool(pass) });
		root.Object.push_back({ "stage", JsonValue::MakeNumber(static_cast<double>(stage)) });
		root.Object.push_back({ "stage_name", JsonValue::MakeString(WorkflowStageToString(static_cast<WorkflowStage>(stage))) });

		JsonValue bArr; bArr.type = JsonValue::Type::Array;
		for (const auto& b : blockers) bArr.Array.push_back(JsonValue::MakeString(b));
		root.Object.push_back({ "blockers", std::move(bArr) });

		JsonValue wArr; wArr.type = JsonValue::Type::Array;
		for (const auto& w : warnings) wArr.Array.push_back(JsonValue::MakeString(w));
		root.Object.push_back({ "warnings", std::move(wArr) });

		JsonValue sArr; sArr.type = JsonValue::Type::Array;
		for (const auto& s : satisfiedRequirements) sArr.Array.push_back(JsonValue::MakeString(s));
		root.Object.push_back({ "satisfied", std::move(sArr) });

		return root.Dump();
	}

	std::string EngineChangeRequest::ToYaml() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "id" << YAML::Value << id;
		out << YAML::Key << "title" << YAML::Value << title;
		out << YAML::Key << "problem" << YAML::Value << problem;
		out << YAML::Key << "repro" << YAML::Value << repro;
		out << YAML::Key << "proposed_api" << YAML::Value << proposedApi;
		out << YAML::Key << "workaround" << YAML::Value << workaround;
		out << YAML::Key << "blocking" << YAML::Value << blocking;
		out << YAML::EndMap;
		return out.c_str();
	}

	std::string EngineChangeRequest::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "id", JsonValue::MakeString(id) });
		root.Object.push_back({ "title", JsonValue::MakeString(title) });
		root.Object.push_back({ "problem", JsonValue::MakeString(problem) });
		root.Object.push_back({ "repro", JsonValue::MakeString(repro) });
		root.Object.push_back({ "proposed_api", JsonValue::MakeString(proposedApi) });
		root.Object.push_back({ "workaround", JsonValue::MakeString(workaround) });
		root.Object.push_back({ "blocking", JsonValue::MakeBool(blocking) });
		return root.Dump();
	}

	// ---------------- MinimalWorkflowPolicy ----------------
	bool MinimalWorkflowPolicy::CanRoleExecuteCommand(const std::string& /*roleId*/, WorkflowStage /*stage*/, const std::string& /*cmd*/)
	{
		return true; // 最小后备策略不阻断任何命令
	}

	bool MinimalWorkflowPolicy::CanRoleWritePath(const std::string& /*roleId*/, WorkflowStage /*stage*/, const std::string& /*relPath*/)
	{
		return true; // 具体由 WriteGuard 兜底项目沙箱约束
	}

	GateEvaluationResult MinimalWorkflowPolicy::EvaluateGate(WorkflowStage stage, const std::string& projectRoot)
	{
		GateEvaluationResult result;
		result.stage = static_cast<uint32_t>(stage);

		std::string root = projectRoot;
		if (root.empty() && fs::exists(Paths::ProjectDir() / "project.we.yaml"))
			root = Paths::ProjectDir().string();

		if (!root.empty())
		{
			auto report = Ai::SelfValidation::ValidateProject(root);
			if (!report.valid)
			{
				for (const auto& err : report.errors)
				{
					result.blockers.push_back("[" + err.code + "] " + err.file + ": " + err.message);
				}
				result.pass = false;
			}
			else
			{
				result.satisfiedRequirements.push_back("Project validation passed clean.");
				result.pass = true;
			}

			for (const auto& w : report.warnings)
			{
				result.warnings.push_back(w);
			}
		}
		else
		{
			result.satisfiedRequirements.push_back("Minimal fallback policy active (no project specified).");
			result.pass = true;
		}

		return result;
	}

	std::vector<std::string> MinimalWorkflowPolicy::GetActiveRolesForStage(WorkflowStage /*stage*/)
	{
		return { "all", "maker", "reviewer", "producer" };
	}

	// ---------------- WeAgentCore ----------------
	WeAgentCore& WeAgentCore::Get()
	{
		static WeAgentCore instance;
		return instance;
	}

	WeAgentCore::WeAgentCore()
	{
		ResetToMinimalPolicy();
	}

	void WeAgentCore::SetPolicy(std::shared_ptr<IWorkflowPolicy> policy)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Policy = policy ? policy : std::make_shared<MinimalWorkflowPolicy>();
	}

	std::shared_ptr<IWorkflowPolicy> WeAgentCore::GetPolicy() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Policy;
	}

	void WeAgentCore::ResetToMinimalPolicy()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Policy = std::make_shared<MinimalWorkflowPolicy>();
	}

	std::string WeAgentCore::OpenSession(const std::string& roleId, const std::string& taskId, const std::string& projectRoot)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		const auto now = std::chrono::system_clock::now();
		const auto nowEpoch = now.time_since_epoch().count();
		const std::string token = "sess_" + std::to_string(nowEpoch) + "_" + std::to_string(++m_SessionCounter);

		SessionContext ctx;
		ctx.token = token;
		ctx.roleId = roleId.empty() ? "maker" : roleId;
		ctx.currentTaskId = taskId;
		ctx.projectRoot = projectRoot.empty() ? m_ProjectRoot : projectRoot;
		ctx.activeStage = m_CurrentStage;
		ctx.startTime = now;

		m_Sessions[token] = ctx;
		return token;
	}

	bool WeAgentCore::CloseSession(const std::string& token)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Sessions.find(token);
		if (it != m_Sessions.end())
		{
			m_Sessions.erase(it);
			return true;
		}
		return false;
	}

	const SessionContext* WeAgentCore::GetSession(const std::string& token) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Sessions.find(token);
		return it != m_Sessions.end() ? &it->second : nullptr;
	}

	std::vector<SessionContext> WeAgentCore::ListSessions() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::vector<SessionContext> list;
		list.reserve(m_Sessions.size());
		for (const auto& kv : m_Sessions)
		{
			list.push_back(kv.second);
		}
		return list;
	}

	GateEvaluationResult WeAgentCore::CheckGate(WorkflowStage stage, const std::string& projectRoot) const
	{
		std::shared_ptr<IWorkflowPolicy> policy;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			policy = m_Policy;
		}
		return policy->EvaluateGate(stage, projectRoot);
	}

	bool WeAgentCore::AdvanceStage(const std::string& sessionToken, std::string& outError)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!sessionToken.empty())
		{
			if (m_Sessions.find(sessionToken) == m_Sessions.end())
			{
				outError = "Invalid session token: " + sessionToken;
				return false;
			}
		}

		std::string projRoot = m_ProjectRoot;
		if (!sessionToken.empty())
		{
			auto it = m_Sessions.find(sessionToken);
			if (it != m_Sessions.end() && !it->second.projectRoot.empty())
				projRoot = it->second.projectRoot;
		}

		// 执行当前阶段门禁检查
		GateEvaluationResult gate = m_Policy->EvaluateGate(m_CurrentStage, projRoot);
		if (!gate.pass)
		{
			outError = "Gate check failed: " + std::to_string(gate.blockers.size()) + " blocker(s) present.";
			return false;
		}

		if (m_CurrentStage < WorkflowStage::LiveOps)
		{
			m_CurrentStage = static_cast<WorkflowStage>(static_cast<int>(m_CurrentStage) + 1);
			return true;
		}

		outError = "Workflow already reached final stage: LiveOps.";
		return false;
	}

	bool WeAgentCore::RollbackToStage(const std::string& sessionToken, WorkflowStage targetStage, std::string& outError)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!sessionToken.empty())
		{
			if (m_Sessions.find(sessionToken) == m_Sessions.end())
			{
				outError = "Invalid session token: " + sessionToken;
				return false;
			}
		}

		if (static_cast<int>(targetStage) >= static_cast<int>(m_CurrentStage))
		{
			outError = "Target rollback stage must be earlier than current stage.";
			return false;
		}

		m_CurrentStage = targetStage;
		return true;
	}

	std::string WeAgentCore::RequestEngineChange(const EngineChangeRequest& req, const std::string& projectRoot)
	{
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path dir = root / ".we" / "ai" / "engine-requests";

		std::error_code ec;
		fs::create_directories(dir, ec);

		EngineChangeRequest finalized = req;
		if (finalized.id.empty())
		{
			const auto now = std::chrono::system_clock::now().time_since_epoch().count();
			finalized.id = "req_" + std::to_string(now) + "_" + std::to_string(++m_RequestCounter);
		}

		const fs::path filePath = dir / (finalized.id + ".yaml");
		std::ofstream f(filePath);
		if (f)
		{
			f << finalized.ToYaml();
		}

		return finalized.id;
	}

	std::vector<EngineChangeRequest> WeAgentCore::ListEngineRequests(const std::string& projectRoot) const
	{
		std::vector<EngineChangeRequest> list;
		const fs::path root = fs::u8path(projectRoot.empty() ? Paths::ProjectDir().string() : projectRoot);
		const fs::path dir = root / ".we" / "ai" / "engine-requests";

		if (!fs::exists(dir))
			return list;

		std::error_code ec;
		for (const auto& entry : fs::directory_iterator(dir, ec))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".yaml")
				continue;

			try
			{
				YAML::Node doc = YAML::LoadFile(entry.path().string());
				EngineChangeRequest req;
				if (doc["id"]) req.id = doc["id"].as<std::string>();
				if (doc["title"]) req.title = doc["title"].as<std::string>();
				if (doc["problem"]) req.problem = doc["problem"].as<std::string>();
				if (doc["repro"]) req.repro = doc["repro"].as<std::string>();
				if (doc["proposed_api"]) req.proposedApi = doc["proposed_api"].as<std::string>();
				if (doc["workaround"]) req.workaround = doc["workaround"].as<std::string>();
				if (doc["blocking"]) req.blocking = doc["blocking"].as<bool>();
				list.push_back(std::move(req));
			}
			catch (...) {}
		}

		return list;
	}

	void WeAgentCore::ResetAll()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Policy = std::make_shared<MinimalWorkflowPolicy>();
		m_CurrentStage = WorkflowStage::Concept;
		m_Sessions.clear();
		m_SessionCounter = 0;
		m_RequestCounter = 0;
	}

	void WeAgentCore::RegisterCommands(World::Ai::CommandRegistry& reg)
	{
		using namespace World::Ai;

		// 1. session.open
		CommandDescriptor openCmd;
		openCmd.name = "session.open";
		openCmd.description = "Open an AI working session with designated role and optional task ID.";
		openCmd.effect = CommandEffect::Write;
		openCmd.scope = CommandScope::Both;
		openCmd.owner = "engine";
		openCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"role\":{\"type\":\"string\"},\"task\":{\"type\":\"string\"}},\"required\":[\"role\"]}";
		openCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"token\":{\"type\":\"string\"}}}";
		openCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string role = args.count("role") ? args.at("role") : "";
			if (role.empty())
			{
				error = "Missing required argument 'role'";
				return false;
			}
			const std::string task = args.count("task") ? args.at("task") : "";
			const std::string token = Get().OpenSession(role, task);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "token", JsonValue::MakeString(token) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(openCmd));

		// 2. session.close
		CommandDescriptor closeCmd;
		closeCmd.name = "session.close";
		closeCmd.description = "Close an active AI working session.";
		closeCmd.effect = CommandEffect::Write;
		closeCmd.scope = CommandScope::Both;
		closeCmd.owner = "engine";
		closeCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"token\":{\"type\":\"string\"}},\"required\":[\"token\"]}";
		closeCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"closed\":{\"type\":\"boolean\"}}}";
		closeCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string token = args.count("token") ? args.at("token") : "";
			if (!Get().CloseSession(token))
			{
				error = "Session not found: " + token;
				return false;
			}
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "closed", JsonValue::MakeBool(true) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(closeCmd));

		// 3. session.list
		CommandDescriptor listCmd;
		listCmd.name = "session.list";
		listCmd.description = "List all active AI sessions.";
		listCmd.effect = CommandEffect::Read;
		listCmd.scope = CommandScope::Both;
		listCmd.owner = "engine";
		listCmd.paramsSchema = "{\"type\":\"object\"}";
		listCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"sessions\":{\"type\":\"array\"}}}";
		listCmd.handler = [](const std::map<std::string, std::string>& /*args*/, std::string& result, std::string& /*error*/)
		{
			const auto list = Get().ListSessions();
			JsonValue r; r.type = JsonValue::Type::Object;
			JsonValue arr; arr.type = JsonValue::Type::Array;
			for (const auto& s : list)
			{
				auto opt = JsonValue::Parse(s.ToJson(), nullptr);
				if (opt) arr.Array.push_back(std::move(*opt));
			}
			r.Object.push_back({ "sessions", std::move(arr) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(listCmd));

		// 4. workflow.status
		CommandDescriptor statusCmd;
		statusCmd.name = "workflow.status";
		statusCmd.description = "Get current workflow stage, policy name, and gate readiness.";
		statusCmd.effect = CommandEffect::Read;
		statusCmd.scope = CommandScope::Both;
		statusCmd.owner = "engine";
		statusCmd.paramsSchema = "{\"type\":\"object\"}";
		statusCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"current_stage\":{\"type\":\"string\"},\"policy\":{\"type\":\"string\"}}}";
		statusCmd.handler = [](const std::map<std::string, std::string>& /*args*/, std::string& result, std::string& /*error*/)
		{
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "current_stage", JsonValue::MakeString(WorkflowStageToString(Get().GetCurrentStage())) });
			r.Object.push_back({ "stage_index", JsonValue::MakeNumber(static_cast<double>(Get().GetCurrentStage())) });
			r.Object.push_back({ "policy", JsonValue::MakeString(Get().GetPolicy()->GetPolicyName()) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(statusCmd));

		// 5. workflow.advance
		CommandDescriptor advanceCmd;
		advanceCmd.name = "workflow.advance";
		advanceCmd.description = "Advance workflow to the next stage if gate check passes.";
		advanceCmd.effect = CommandEffect::Write;
		advanceCmd.scope = CommandScope::Both;
		advanceCmd.owner = "engine";
		advanceCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\"}}}";
		advanceCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"new_stage\":{\"type\":\"string\"}}}";
		advanceCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string token = args.count("session") ? args.at("session") : "";
			if (!Get().AdvanceStage(token, error))
				return false;
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "new_stage", JsonValue::MakeString(WorkflowStageToString(Get().GetCurrentStage())) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(advanceCmd));

		// 6. workflow.rollback
		CommandDescriptor rollbackCmd;
		rollbackCmd.name = "workflow.rollback";
		rollbackCmd.description = "Controlled rollback to an earlier workflow stage.";
		rollbackCmd.effect = CommandEffect::Write;
		rollbackCmd.scope = CommandScope::Both;
		rollbackCmd.owner = "engine";
		rollbackCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"stage\":{\"type\":\"string\"},\"session\":{\"type\":\"string\"}},\"required\":[\"stage\"]}";
		rollbackCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"rolled_back_to\":{\"type\":\"string\"}}}";
		rollbackCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string stStr = args.count("stage") ? args.at("stage") : "";
			const std::string token = args.count("session") ? args.at("session") : "";
			WorkflowStage target = StringToWorkflowStage(stStr);
			if (!Get().RollbackToStage(token, target, error))
				return false;
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "rolled_back_to", JsonValue::MakeString(WorkflowStageToString(Get().GetCurrentStage())) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(rollbackCmd));

		// 7. gate.check
		CommandDescriptor gateCmd;
		gateCmd.name = "gate.check";
		gateCmd.description = "Evaluate stage gate requirements, blockers, and readiness.";
		gateCmd.effect = CommandEffect::Read;
		gateCmd.scope = CommandScope::Both;
		gateCmd.owner = "engine";
		gateCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"stage\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}}}";
		gateCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"pass\":{\"type\":\"boolean\"},\"blockers\":{\"type\":\"array\"}}}";
		gateCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string stStr = args.count("stage") ? args.at("stage") : "";
			WorkflowStage stage = stStr.empty() ? Get().GetCurrentStage() : StringToWorkflowStage(stStr);
			const std::string root = args.count("root") ? args.at("root") : "";
			result = Get().CheckGate(stage, root).ToJson();
			return true;
		};
		reg.RegisterCommand(std::move(gateCmd));

		// 8. engine.request_change
		CommandDescriptor reqCmd;
		reqCmd.name = "engine.request_change";
		reqCmd.description = "Report an engine functionality gap into structured project requests.";
		reqCmd.effect = CommandEffect::Write;
		reqCmd.scope = CommandScope::Both;
		reqCmd.owner = "engine";
		reqCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\"},\"problem\":{\"type\":\"string\"},\"repro\":{\"type\":\"string\"},\"proposed_api\":{\"type\":\"string\"},\"workaround\":{\"type\":\"string\"},\"blocking\":{\"type\":\"boolean\"}},\"required\":[\"title\",\"problem\"]}";
		reqCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}}}";
		reqCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			EngineChangeRequest req;
			req.title = args.count("title") ? args.at("title") : "";
			if (req.title.empty())
			{
				error = "Missing required argument 'title'";
				return false;
			}
			req.problem = args.count("problem") ? args.at("problem") : "";
			req.repro = args.count("repro") ? args.at("repro") : "";
			req.proposedApi = args.count("proposed_api") ? args.at("proposed_api") : "";
			req.workaround = args.count("workaround") ? args.at("workaround") : "";
			req.blocking = args.count("blocking") && args.at("blocking") == "true";

			const std::string root = args.count("root") ? args.at("root") : "";
			const std::string id = Get().RequestEngineChange(req, root);

			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "id", JsonValue::MakeString(id) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(reqCmd));
	}
}
