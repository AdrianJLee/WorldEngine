#include "wldpch.h"
#include "World/Agent/WeAgentCore.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Ai/SelfValidation.h"
#include "World/WUI/WuiJson.h"
#include "World/Utils/Paths.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace World::Agent
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	namespace
	{
		std::string ToLowerAscii(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		std::string FormatTimestampNow()
		{
			const auto now = std::chrono::system_clock::now();
			const std::time_t t = std::chrono::system_clock::to_time_t(now);
			std::tm tmBuf{};
#if defined(_WIN32)
			localtime_s(&tmBuf, &t);
#else
			localtime_r(&t, &tmBuf);
#endif
			std::ostringstream ss;
			ss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
			return ss.str();
		}
	}

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
		const std::string s = ToLowerAscii(str);
		if (s == "0" || s == "concept" || s == "interview" || s == "brief")
			return WorkflowStage::Concept;
		if (s == "1" || s == "feasibility" || s == "pre_production" || s == "preproduction")
			return WorkflowStage::Feasibility;
		if (s == "2" || s == "prototype")
			return WorkflowStage::Prototype;
		if (s == "3" || s == "vertical_slice" || s == "verticalslice")
			return WorkflowStage::VerticalSlice;
		if (s == "4" || s == "production")
			return WorkflowStage::Production;
		if (s == "5" || s == "beta" || s == "polish")
			return WorkflowStage::Beta;
		if (s == "6" || s == "launch" || s == "release")
			return WorkflowStage::Launch;
		if (s == "7" || s == "liveops" || s == "live_ops")
			return WorkflowStage::LiveOps;
		return WorkflowStage::Concept;
	}

	const char* RoleRuntimeStateToString(RoleRuntimeState state)
	{
		switch (state)
		{
		case RoleRuntimeState::Active:   return "ACTIVE";
		case RoleRuntimeState::Dormant:  return "DORMANT";
		case RoleRuntimeState::Disabled: return "DISABLED";
		default:                         return "ACTIVE";
		}
	}

	RoleRuntimeState StringToRoleRuntimeState(const std::string& str)
	{
		const std::string s = ToLowerAscii(str);
		if (s == "dormant")  return RoleRuntimeState::Dormant;
		if (s == "disabled") return RoleRuntimeState::Disabled;
		return RoleRuntimeState::Active;
	}

	std::string SessionContext::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;

		root.Object.push_back({ "token", JsonValue::MakeString(token) });
		root.Object.push_back({ "role", JsonValue::MakeString(roleId) });
		root.Object.push_back({ "task", JsonValue::MakeString(currentTaskId) });
		root.Object.push_back({ "project_root", JsonValue::MakeString(projectRoot) });
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

	bool MinimalWorkflowPolicy::CanRoleExecuteCommand(const std::string& /*roleId*/, WorkflowStage /*stage*/, const std::string& /*cmd*/)
	{
		return true;
	}

	bool MinimalWorkflowPolicy::CanRoleWritePath(const std::string& /*roleId*/, WorkflowStage /*stage*/, const std::string& /*relPath*/)
	{
		return true;
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
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		m_Policy = policy ? policy : std::make_shared<MinimalWorkflowPolicy>();
	}

	std::shared_ptr<IWorkflowPolicy> WeAgentCore::GetPolicy() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return m_Policy;
	}

	void WeAgentCore::ResetToMinimalPolicy()
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		m_Policy = std::make_shared<MinimalWorkflowPolicy>();
	}

	void WeAgentCore::SetProjectRoot(const std::string& root)
	{
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			m_ProjectRoot = root;
		}
		if (!root.empty())
		{
			const fs::path stateFile = fs::u8path(root) / ".we" / "ai" / "state.json";
			if (fs::exists(stateFile))
			{
				LoadState(root);
			}
		}
	}

	bool WeAgentCore::SaveState(const std::string& projectRoot) const
	{
		WorkflowStage stageSnap;
		std::string activeTokSnap;
		std::map<std::string, RoleRuntimeState> roleStatesSnap;
		std::vector<std::string> blockersSnap;
		std::map<std::string, SessionContext> sessionsSnap;
		std::string rootStr;

		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			rootStr = !projectRoot.empty() ? projectRoot : m_ProjectRoot;
			if (rootStr.empty())
				return false;

			stageSnap = m_CurrentStage;
			activeTokSnap = m_ActiveSessionToken;
			roleStatesSnap = m_RoleStates;
			blockersSnap = m_OverriddenBlockers;
			sessionsSnap = m_Sessions;
		}

		const fs::path stateDir = fs::u8path(rootStr) / ".we" / "ai";
		std::error_code ec;
		fs::create_directories(stateDir, ec);

		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "current_stage", JsonValue::MakeString(WorkflowStageToString(stageSnap)) });
		root.Object.push_back({ "stage_index", JsonValue::MakeNumber(static_cast<double>(stageSnap)) });
		root.Object.push_back({ "active_session_token", JsonValue::MakeString(activeTokSnap) });

		JsonValue rolesObj;
		rolesObj.type = JsonValue::Type::Object;
		for (const auto& kv : roleStatesSnap)
		{
			rolesObj.Object.push_back({ kv.first, JsonValue::MakeString(RoleRuntimeStateToString(kv.second)) });
		}
		root.Object.push_back({ "role_states", std::move(rolesObj) });

		JsonValue ovArr;
		ovArr.type = JsonValue::Type::Array;
		for (const auto& b : blockersSnap)
		{
			ovArr.Array.push_back(JsonValue::MakeString(b));
		}
		root.Object.push_back({ "overridden_blockers", std::move(ovArr) });

		JsonValue sessArr;
		sessArr.type = JsonValue::Type::Array;
		for (const auto& kv : sessionsSnap)
		{
			auto parsed = JsonValue::Parse(kv.second.ToJson(), nullptr);
			if (parsed)
				sessArr.Array.push_back(std::move(*parsed));
		}
		root.Object.push_back({ "sessions", std::move(sessArr) });

		const fs::path stateFile = stateDir / "state.json";
		std::ofstream out(stateFile, std::ios::binary);
		if (!out)
			return false;

		const std::string dumped = root.Dump();
		out.write(dumped.data(), dumped.size());
		return true;
	}

	bool WeAgentCore::LoadState(const std::string& projectRoot)
	{
		std::string rootStr;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			rootStr = !projectRoot.empty() ? projectRoot : m_ProjectRoot;
		}
		if (rootStr.empty())
			return false;

		const fs::path stateFile = fs::u8path(rootStr) / ".we" / "ai" / "state.json";
		if (!fs::exists(stateFile))
			return false;

		std::ifstream in(stateFile, std::ios::binary);
		if (!in)
			return false;

		std::ostringstream ss;
		ss << in.rdbuf();
		auto parsed = JsonValue::Parse(ss.str(), nullptr);
		if (!parsed || parsed->type != JsonValue::Type::Object)
			return false;

		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		if (!projectRoot.empty() && m_ProjectRoot.empty())
			m_ProjectRoot = projectRoot;

		if (const auto* stVal = parsed->Find("current_stage"))
		{
			m_CurrentStage = StringToWorkflowStage(stVal->AsString());
		}

		if (const auto* tokVal = parsed->Find("active_session_token"))
		{
			m_ActiveSessionToken = tokVal->AsString();
		}

		if (const auto* rolesVal = parsed->Find("role_states"); rolesVal && rolesVal->type == JsonValue::Type::Object)
		{
			m_RoleStates.clear();
			for (const auto& kv : rolesVal->Object)
			{
				m_RoleStates[kv.first] = StringToRoleRuntimeState(kv.second.AsString());
			}
		}

		if (const auto* ovVal = parsed->Find("overridden_blockers"); ovVal && ovVal->type == JsonValue::Type::Array)
		{
			m_OverriddenBlockers.clear();
			for (const auto& item : ovVal->Array)
			{
				if (!item.AsString().empty())
					m_OverriddenBlockers.push_back(item.AsString());
			}
		}

		if (const auto* sessVal = parsed->Find("sessions"); sessVal && sessVal->type == JsonValue::Type::Array)
		{
			m_Sessions.clear();
			for (const auto& item : sessVal->Array)
			{
				if (item.type != JsonValue::Type::Object)
					continue;
				SessionContext ctx;
				if (const auto* t = item.Find("token")) ctx.token = t->AsString();
				if (const auto* r = item.Find("role")) ctx.roleId = r->AsString();
				if (const auto* k = item.Find("task")) ctx.currentTaskId = k->AsString();
				if (const auto* p = item.Find("project_root")) ctx.projectRoot = p->AsString();
				else ctx.projectRoot = rootStr;
				if (const auto* s = item.Find("stage")) ctx.activeStage = StringToWorkflowStage(s->AsString());
				else ctx.activeStage = m_CurrentStage;
				ctx.startTime = std::chrono::system_clock::now();
				if (!ctx.token.empty())
					m_Sessions[ctx.token] = std::move(ctx);
			}
		}

		return true;
	}

	void WeAgentCore::AppendJournal(const std::string& sessionToken,
		const std::string& roleId,
		const std::string& action,
		const std::string& details,
		const std::string& projectRoot) const
	{
		std::string rootStr;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			rootStr = !projectRoot.empty() ? projectRoot : m_ProjectRoot;
		}
		if (rootStr.empty())
			return;

		const fs::path journalDir = fs::u8path(rootStr) / ".we" / "ai" / "journal";
		std::error_code ec;
		fs::create_directories(journalDir, ec);

		const fs::path journalFile = journalDir / "journal.md";
		std::ofstream out(journalFile, std::ios::app);
		if (!out)
			return;

		out << "- [" << FormatTimestampNow() << "] (" << roleId << " | " << sessionToken << ") **"
		    << action << "**: " << details << "\n";
	}

	void WeAgentCore::AppendDecision(const std::string& actor,
		const std::string& decisionTitle,
		const std::string& rationale,
		const std::string& projectRoot) const
	{
		std::string rootStr;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			rootStr = !projectRoot.empty() ? projectRoot : m_ProjectRoot;
		}
		if (rootStr.empty())
			return;

		const fs::path studioDir = fs::u8path(rootStr) / ".we" / "studio";
		std::error_code ec;
		fs::create_directories(studioDir, ec);

		const fs::path decisionsFile = studioDir / "decisions.md";
		std::ofstream out(decisionsFile, std::ios::app);
		if (!out)
			return;

		out << "\n### [" << FormatTimestampNow() << "] " << decisionTitle << "\n"
		    << "- **Actor**: `" << actor << "`\n"
		    << "- **Rationale**: " << rationale << "\n";
	}

	void WeAgentCore::SetRoleState(const std::string& roleId,
		RoleRuntimeState state,
		const std::string& reason,
		const std::string& projectRoot)
	{
		std::string rootStr;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			m_RoleStates[roleId] = state;
			rootStr = !projectRoot.empty() ? projectRoot : m_ProjectRoot;
		}
		AppendDecision("producer", "Role State -> " + roleId + " : " + RoleRuntimeStateToString(state),
			reason.empty() ? "State updated" : reason, rootStr);
		SaveState(rootStr);
	}

	RoleRuntimeState WeAgentCore::GetRoleState(const std::string& roleId) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		auto it = m_RoleStates.find(roleId);
		if (it != m_RoleStates.end())
			return it->second;
		return RoleRuntimeState::Active;
	}

	std::map<std::string, RoleRuntimeState> WeAgentCore::GetAllRoleStates() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return m_RoleStates;
	}

	std::string WeAgentCore::GenerateRoleActivationPlan(const std::string& genreArchetype,
		const std::string& projectRoot)
	{
		const std::string genre = ToLowerAscii(genreArchetype);

		std::map<std::string, RoleRuntimeState> states;
		// 基础常驻角色
		states["producer"] = RoleRuntimeState::Active;
		states["creative_director"] = RoleRuntimeState::Active;
		states["art_director"] = RoleRuntimeState::Active;
		states["experience_director"] = RoleRuntimeState::Active;
		states["technical_director"] = RoleRuntimeState::Active;
		states["qa_lead"] = RoleRuntimeState::Active;
		states["automation_qa"] = RoleRuntimeState::Active;
		states["system_designer"] = RoleRuntimeState::Active;
		states["gameplay_programmer"] = RoleRuntimeState::Active;

		if (genre.find("2d") != std::string::npos || genre.find("pixel") != std::string::npos)
		{
			states["2d_artist"] = RoleRuntimeState::Active;
			states["2d_animator"] = RoleRuntimeState::Active;
			states["audio_designer"] = RoleRuntimeState::Active;
			states["3d_character_artist"] = RoleRuntimeState::Disabled;
			states["3d_env_artist"] = RoleRuntimeState::Disabled;
			states["3d_animator"] = RoleRuntimeState::Disabled;
			states["streaming_programmer"] = RoleRuntimeState::Disabled;
			states["network_programmer"] = RoleRuntimeState::Disabled;
		}
		else if (genre.find("multiplayer") != std::string::npos || genre.find("net") != std::string::npos)
		{
			states["network_programmer"] = RoleRuntimeState::Active;
			states["netcode_security_qa"] = RoleRuntimeState::Active;
		}
		else
		{
			states["3d_prop_artist"] = RoleRuntimeState::Active;
			states["3d_env_artist"] = RoleRuntimeState::Active;
			states["technical_artist"] = RoleRuntimeState::Active;
		}

		std::string rootStr;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			for (const auto& kv : states)
				m_RoleStates[kv.first] = kv.second;
			rootStr = !projectRoot.empty() ? projectRoot : m_ProjectRoot;
		}

		std::ostringstream ss;
		ss << "# Role Activation Plan: " << genreArchetype << "\n"
		   << "Generated on " << FormatTimestampNow() << "\n\n"
		   << "| Role | Initial State |\n| --- | --- |\n";
		for (const auto& kv : states)
		{
			ss << "| `" << kv.first << "` | **" << RoleRuntimeStateToString(kv.second) << "** |\n";
		}

		const std::string planMd = ss.str();
		if (!rootStr.empty())
		{
			const fs::path planPath = fs::u8path(rootStr) / ".we" / "studio" / "role-activation-plan.md";
			std::error_code ec;
			fs::create_directories(planPath.parent_path(), ec);
			std::ofstream f(planPath);
			if (f) f << planMd;
			AppendDecision("producer", "Generate Role Activation Plan: " + genreArchetype,
				"Automated archetype role switchboard setup", rootStr);
			SaveState(rootStr);
		}

		return planMd;
	}

	std::string WeAgentCore::OpenSession(const std::string& roleId,
		const std::string& taskId,
		const std::string& projectRoot)
	{
		std::string token;
		std::string projRoot;
		std::string role;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			const std::string effectiveRole = roleId.empty() ? "maker" : roleId;

			auto itRole = m_RoleStates.find(effectiveRole);
			if (itRole != m_RoleStates.end() && itRole->second == RoleRuntimeState::Disabled)
			{
				return ""; // 禁用角色严禁建立工作会话
			}

			const auto now = std::chrono::system_clock::now();
			const auto nowEpoch = now.time_since_epoch().count();
			token = "sess_" + std::to_string(nowEpoch) + "_" + std::to_string(++m_SessionCounter);

			SessionContext ctx;
			ctx.token = token;
			ctx.roleId = effectiveRole;
			ctx.currentTaskId = taskId;
			ctx.projectRoot = !projectRoot.empty() ? projectRoot : m_ProjectRoot;
			ctx.activeStage = m_CurrentStage;
			ctx.startTime = now;

			projRoot = ctx.projectRoot;
			role = ctx.roleId;
			m_Sessions[token] = ctx;
			m_ActiveSessionToken = token;
		}

		if (!projRoot.empty())
		{
			SaveState(projRoot);
			AppendJournal(token, role, "session.open", "Opened session for task: " + taskId, projRoot);
		}

		return token;
	}

	bool WeAgentCore::CloseSession(const std::string& token)
	{
		std::string roleId;
		std::string projRoot;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			auto it = m_Sessions.find(token);
			if (it == m_Sessions.end())
				return false;

			roleId = it->second.roleId;
			projRoot = it->second.projectRoot;
			m_Sessions.erase(it);

			if (m_ActiveSessionToken == token)
				m_ActiveSessionToken.clear();
		}

		if (!projRoot.empty())
		{
			SaveState(projRoot);
			AppendJournal(token, roleId, "session.close", "Closed working session", projRoot);
		}
		return true;
	}

	const SessionContext* WeAgentCore::GetSession(const std::string& token) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		auto it = m_Sessions.find(token);
		return it != m_Sessions.end() ? &it->second : nullptr;
	}

	std::vector<SessionContext> WeAgentCore::ListSessions() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		std::vector<SessionContext> list;
		list.reserve(m_Sessions.size());
		for (const auto& kv : m_Sessions)
		{
			list.push_back(kv.second);
		}
		return list;
	}

	void WeAgentCore::SetActiveSessionToken(const std::string& token)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		m_ActiveSessionToken = token;
	}

	std::string WeAgentCore::GetActiveSessionToken() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return m_ActiveSessionToken;
	}

	void WeAgentCore::SetCurrentStageDirect(WorkflowStage stage)
	{
		std::string rootStr;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			m_CurrentStage = stage;
			rootStr = m_ProjectRoot;
		}
		SaveState(rootStr);
	}

	bool WeAgentCore::AdvanceStage(const std::string& sessionToken, std::string& outError)
	{
		std::string projRoot;
		std::string roleId = "producer";
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			if (!sessionToken.empty())
			{
				auto it = m_Sessions.find(sessionToken);
				if (it == m_Sessions.end())
				{
					outError = "Invalid session token: " + sessionToken;
					return false;
				}
				roleId = it->second.roleId;
				projRoot = it->second.projectRoot;
			}
			if (projRoot.empty())
				projRoot = m_ProjectRoot;
		}

		GateEvaluationResult gate = CheckGate(m_CurrentStage, projRoot);
		if (!gate.pass)
		{
			outError = "Gate check failed: " + std::to_string(gate.blockers.size()) + " blocker(s) present.";
			return false;
		}

		if (m_CurrentStage < WorkflowStage::LiveOps)
		{
			{
				std::lock_guard<std::recursive_mutex> lock(m_Mutex);
				m_CurrentStage = static_cast<WorkflowStage>(static_cast<int>(m_CurrentStage) + 1);
				for (auto& kv : m_Sessions)
					kv.second.activeStage = m_CurrentStage;
			}
			SaveState(projRoot);
			AppendJournal(sessionToken, roleId, "workflow.advance",
				"Advanced to stage " + std::string(WorkflowStageToString(m_CurrentStage)), projRoot);
			return true;
		}

		outError = "Workflow already reached final stage: LiveOps.";
		return false;
	}

	bool WeAgentCore::RollbackToStage(const std::string& sessionToken, WorkflowStage targetStage, std::string& outError)
	{
		std::string projRoot;
		std::string roleId = "producer";
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			if (!sessionToken.empty())
			{
				auto it = m_Sessions.find(sessionToken);
				if (it == m_Sessions.end())
				{
					outError = "Invalid session token: " + sessionToken;
					return false;
				}
				roleId = it->second.roleId;
				projRoot = it->second.projectRoot;
			}
			if (projRoot.empty())
				projRoot = m_ProjectRoot;

			if (static_cast<int>(targetStage) >= static_cast<int>(m_CurrentStage))
			{
				outError = "Target rollback stage must be earlier than current stage.";
				return false;
			}

			m_CurrentStage = targetStage;
			for (auto& kv : m_Sessions)
				kv.second.activeStage = m_CurrentStage;
		}

		SaveState(projRoot);
		AppendJournal(sessionToken, roleId, "workflow.rollback",
			"Rolled back to stage " + std::string(WorkflowStageToString(targetStage)), projRoot);
		AppendDecision(roleId, "Workflow Rollback",
			"Controlled rollback to stage " + std::string(WorkflowStageToString(targetStage)), projRoot);
		return true;
	}

	GateEvaluationResult WeAgentCore::CheckGate(WorkflowStage stage, const std::string& projectRoot) const
	{
		std::shared_ptr<IWorkflowPolicy> policy;
		std::string projRoot;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			policy = m_Policy;
			projRoot = !projectRoot.empty() ? projectRoot : m_ProjectRoot;
		}

		GateEvaluationResult result = policy->EvaluateGate(stage, projRoot);

		// L0 出品人特权覆盖 Blocker 过滤
		std::vector<std::string> remainingBlockers;
		for (const auto& b : result.blockers)
		{
			if (IsBlockerOverridden(b))
			{
				result.warnings.push_back("[Overridden by L0 Publisher] " + b);
			}
			else
			{
				remainingBlockers.push_back(b);
			}
		}
		result.blockers = std::move(remainingBlockers);
		result.pass = result.blockers.empty();

		return result;
	}

	void WeAgentCore::OverrideGateBlocker(const std::string& blockerPattern,
		const std::string& rationale,
		const std::string& projectRoot)
	{
		std::string rootStr;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			m_OverriddenBlockers.push_back(blockerPattern);
			rootStr = !projectRoot.empty() ? projectRoot : m_ProjectRoot;
		}
		AppendDecision("publisher_l0", "Override Gate Blocker: " + blockerPattern,
			rationale.empty() ? "Explicit producer/publisher waiver" : rationale, rootStr);
		SaveState(rootStr);
	}

	bool WeAgentCore::IsBlockerOverridden(const std::string& blockerText) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		for (const auto& pattern : m_OverriddenBlockers)
		{
			if (pattern == "*" || blockerText.find(pattern) != std::string::npos)
				return true;
		}
		return false;
	}

	std::vector<std::string> WeAgentCore::GetOverriddenBlockers() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return m_OverriddenBlockers;
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
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		m_Policy = std::make_shared<MinimalWorkflowPolicy>();
		m_CurrentStage = WorkflowStage::Concept;
		m_Sessions.clear();
		m_ActiveSessionToken.clear();
		m_RoleStates.clear();
		m_OverriddenBlockers.clear();
		m_ProjectRoot.clear();
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
		openCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"role\":{\"type\":\"string\"},\"task\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}},\"required\":[\"role\"]}";
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
			const std::string root = args.count("root") ? args.at("root") : "";
			const std::string token = Get().OpenSession(role, task, root);
			if (token.empty())
			{
				error = "Failed to open session: role '" + role + "' is disabled or rejected by policy.";
				return false;
			}
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
			if (token.empty())
			{
				error = "Missing required argument 'token'";
				return false;
			}
			const bool closed = Get().CloseSession(token);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "closed", JsonValue::MakeBool(closed) });
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
		listCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{}}";
		listCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"sessions\":{\"type\":\"array\"}}}";
		listCmd.handler = [](const std::map<std::string, std::string>& /*args*/, std::string& result, std::string& /*error*/)
		{
			auto sessions = Get().ListSessions();
			JsonValue r; r.type = JsonValue::Type::Object;
			JsonValue arr; arr.type = JsonValue::Type::Array;
			for (const auto& s : sessions)
			{
				auto parsed = JsonValue::Parse(s.ToJson(), nullptr);
				if (parsed) arr.Array.push_back(std::move(*parsed));
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
		statusCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{}}";
		statusCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"current_stage\":{\"type\":\"string\"},\"policy\":{\"type\":\"string\"}}}";
		statusCmd.handler = [](const std::map<std::string, std::string>& /*args*/, std::string& result, std::string& /*error*/)
		{
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "current_stage", JsonValue::MakeString(WorkflowStageToString(Get().GetCurrentStage())) });
			r.Object.push_back({ "policy", JsonValue::MakeString(Get().GetPolicy() ? Get().GetPolicy()->GetPolicyName() : "none") });
			r.Object.push_back({ "active_session", JsonValue::MakeString(Get().GetActiveSessionToken()) });
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
			const std::string token = args.count("session") ? args.at("session") : Get().GetActiveSessionToken();
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
			const std::string token = args.count("session") ? args.at("session") : Get().GetActiveSessionToken();
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
		reqCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\"},\"problem\":{\"type\":\"string\"},\"repro\":{\"type\":\"string\"},\"proposed_api\":{\"type\":\"string\"},\"workaround\":{\"type\":\"string\"},\"blocking\":{\"type\":\"boolean\"},\"root\":{\"type\":\"string\"}},\"required\":[\"title\",\"problem\"]}";
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

		// 9. role.set_state
		CommandDescriptor roleSetCmd;
		roleSetCmd.name = "role.set_state";
		roleSetCmd.description = "Set role runtime state (ACTIVE, DORMANT, DISABLED).";
		roleSetCmd.effect = CommandEffect::Write;
		roleSetCmd.scope = CommandScope::Both;
		roleSetCmd.owner = "engine";
		roleSetCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"role\":{\"type\":\"string\"},\"state\":{\"type\":\"string\"},\"reason\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}},\"required\":[\"role\",\"state\"]}";
		roleSetCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"role\":{\"type\":\"string\"},\"state\":{\"type\":\"string\"}}}";
		roleSetCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string role = args.count("role") ? args.at("role") : "";
			const std::string st = args.count("state") ? args.at("state") : "";
			if (role.empty() || st.empty())
			{
				error = "Missing required 'role' or 'state'";
				return false;
			}
			const std::string reason = args.count("reason") ? args.at("reason") : "";
			const std::string root = args.count("root") ? args.at("root") : "";
			const auto stateEnum = StringToRoleRuntimeState(st);
			Get().SetRoleState(role, stateEnum, reason, root);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "role", JsonValue::MakeString(role) });
			r.Object.push_back({ "state", JsonValue::MakeString(RoleRuntimeStateToString(stateEnum)) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(roleSetCmd));

		// 10. role.get_states
		CommandDescriptor roleGetCmd;
		roleGetCmd.name = "role.get_states";
		roleGetCmd.description = "List runtime states of all roles.";
		roleGetCmd.effect = CommandEffect::Read;
		roleGetCmd.scope = CommandScope::Both;
		roleGetCmd.owner = "engine";
		roleGetCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{}}";
		roleGetCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"roles\":{\"type\":\"object\"}}}";
		roleGetCmd.handler = [](const std::map<std::string, std::string>& /*args*/, std::string& result, std::string& /*error*/)
		{
			auto states = Get().GetAllRoleStates();
			JsonValue r; r.type = JsonValue::Type::Object;
			JsonValue rolesObj; rolesObj.type = JsonValue::Type::Object;
			for (const auto& kv : states)
				rolesObj.Object.push_back({ kv.first, JsonValue::MakeString(RoleRuntimeStateToString(kv.second)) });
			r.Object.push_back({ "roles", std::move(rolesObj) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(roleGetCmd));

		// 11. role.generate_plan
		CommandDescriptor rolePlanCmd;
		rolePlanCmd.name = "role.generate_plan";
		rolePlanCmd.description = "Generate role activation plan for a genre archetype.";
		rolePlanCmd.effect = CommandEffect::Write;
		rolePlanCmd.scope = CommandScope::Both;
		rolePlanCmd.owner = "engine";
		rolePlanCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"genre\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}},\"required\":[\"genre\"]}";
		rolePlanCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"plan_markdown\":{\"type\":\"string\"}}}";
		rolePlanCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string genre = args.count("genre") ? args.at("genre") : "";
			if (genre.empty())
			{
				error = "Missing required 'genre'";
				return false;
			}
			const std::string root = args.count("root") ? args.at("root") : "";
			const std::string md = Get().GenerateRoleActivationPlan(genre, root);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "plan_markdown", JsonValue::MakeString(md) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(rolePlanCmd));

		// 12. gate.override_blocker
		CommandDescriptor overrideCmd;
		overrideCmd.name = "gate.override_blocker";
		overrideCmd.description = "L0 publisher override of gate blocker with audit trail.";
		overrideCmd.effect = CommandEffect::Write;
		overrideCmd.scope = CommandScope::Both;
		overrideCmd.owner = "engine";
		overrideCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"blocker\":{\"type\":\"string\"},\"rationale\":{\"type\":\"string\"},\"root\":{\"type\":\"string\"}},\"required\":[\"blocker\"]}";
		overrideCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"overridden\":{\"type\":\"string\"}}}";
		overrideCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string blocker = args.count("blocker") ? args.at("blocker") : "";
			if (blocker.empty())
			{
				error = "Missing required 'blocker'";
				return false;
			}
			const std::string rationale = args.count("rationale") ? args.at("rationale") : "";
			const std::string root = args.count("root") ? args.at("root") : "";
			Get().OverrideGateBlocker(blocker, rationale, root);
			JsonValue r; r.type = JsonValue::Type::Object;
			r.Object.push_back({ "overridden", JsonValue::MakeString(blocker) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(overrideCmd));

		// 13. engine.list_requests
		CommandDescriptor listReqCmd;
		listReqCmd.name = "engine.list_requests";
		listReqCmd.description = "List all tracked engine gap requests.";
		listReqCmd.effect = CommandEffect::Read;
		listReqCmd.scope = CommandScope::Both;
		listReqCmd.owner = "engine";
		listReqCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"root\":{\"type\":\"string\"}}}";
		listReqCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"requests\":{\"type\":\"array\"}}}";
		listReqCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			const std::string root = args.count("root") ? args.at("root") : "";
			auto reqs = Get().ListEngineRequests(root);
			JsonValue r; r.type = JsonValue::Type::Object;
			JsonValue arr; arr.type = JsonValue::Type::Array;
			for (const auto& item : reqs)
			{
				auto parsed = JsonValue::Parse(item.ToJson(), nullptr);
				if (parsed) arr.Array.push_back(std::move(*parsed));
			}
			r.Object.push_back({ "requests", std::move(arr) });
			result = r.Dump();
			return true;
		};
		reg.RegisterCommand(std::move(listReqCmd));
	}
}


