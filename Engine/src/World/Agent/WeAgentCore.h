#pragma once

#include "World/Core/Export.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace World::Ai
{
	class CommandRegistry;
}

namespace World::Agent
{
	// 8 阶段通用游戏研发推进阶段 (0..7，兼容 §6.5 访谈立项..发行上线 与旧枚举命名)
	enum class WorkflowStage : uint8_t
	{
		Concept = 0,     // 0: 访谈立项 / 概念 (Interview / Brief / Concept)
		Feasibility,     // 1: 概念设计 / 可行性与风格锁 (Concept / Feasibility)
		Prototype,       // 2: 预制作规格 / 原型找乐 (PreProduction / Prototype)
		VerticalSlice,   // 3: 垂直切片 (VerticalSlice)
		Production,      // 4: 规模量产 (Production)
		Beta,            // 5: 打磨与优化 (Polish / Beta)
		Launch,          // 6: 发行与打包上架 (Launch)
		LiveOps,         // 7: 持续运营与更新 (LiveOps)
		StageCount
	};

	WLD_API const char* WorkflowStageToString(WorkflowStage stage);
	WLD_API WorkflowStage StringToWorkflowStage(const std::string& str);

	// 职能运行期状态机 (D13 / §6.4.1.2)
	enum class RoleRuntimeState : uint8_t
	{
		Active = 0,   // 激活态：分配会话与工具，参与门禁与评审
		Dormant,      // 休眠态：不分配工具，不参与通信流，零 Token 开销
		Disabled      // 禁用态：从当前品类彻底裁撤屏蔽
	};

	WLD_API const char* RoleRuntimeStateToString(RoleRuntimeState state);
	WLD_API RoleRuntimeState StringToRoleRuntimeState(const std::string& str);

	struct WLD_API SessionContext
	{
		std::string token;
		std::string roleId;
		std::string currentTaskId;
		std::string projectRoot;
		WorkflowStage activeStage = WorkflowStage::Concept;
		std::chrono::system_clock::time_point startTime;

		std::string ToJson() const;
	};

	struct WLD_API GateEvaluationResult
	{
		bool pass = true;
		uint32_t stage = 0;
		std::vector<std::string> blockers;
		std::vector<std::string> warnings;
		std::vector<std::string> satisfiedRequirements;

		std::string ToJson() const;
	};

	struct WLD_API EngineChangeRequest
	{
		std::string id;
		std::string title;
		std::string problem;
		std::string repro;
		std::string proposedApi;
		std::string workaround;
		bool blocking = false;

		std::string ToYaml() const;
		std::string ToJson() const;
	};

	// 规则策略抽象接口 (由 we-studio 或外部规则注入，未注入时退化为内置最小策略)
	class WLD_API IWorkflowPolicy
	{
	public:
		virtual ~IWorkflowPolicy() = default;

		virtual std::string GetPolicyName() const = 0;
		virtual bool CanRoleExecuteCommand(const std::string& roleId, WorkflowStage stage, const std::string& cmd) = 0;
		virtual bool CanRoleWritePath(const std::string& roleId, WorkflowStage stage, const std::string& relPath) = 0;
		virtual GateEvaluationResult EvaluateGate(WorkflowStage stage, const std::string& projectRoot) = 0;
		virtual std::vector<std::string> GetActiveRolesForStage(WorkflowStage stage) = 0;
	};

	// 引擎内置最小策略 (无插件时生效，保持单阶段、不阻断基础命令，保证引擎自愈基础可用)
	class WLD_API MinimalWorkflowPolicy : public IWorkflowPolicy
	{
	public:
		std::string GetPolicyName() const override { return "MinimalFallbackPolicy"; }
		bool CanRoleExecuteCommand(const std::string& roleId, WorkflowStage stage, const std::string& cmd) override;
		bool CanRoleWritePath(const std::string& roleId, WorkflowStage stage, const std::string& relPath) override;
		GateEvaluationResult EvaluateGate(WorkflowStage stage, const std::string& projectRoot) override;
		std::vector<std::string> GetActiveRolesForStage(WorkflowStage stage) override;
	};

	// 引擎编排内核中心 WeAgentCore (纯 C++ 单例，无 UI，无 LLM 依赖)
	class WLD_API WeAgentCore
	{
	public:
		static WeAgentCore& Get();

		WeAgentCore();
		~WeAgentCore() = default;

		// 策略管理
		void SetPolicy(std::shared_ptr<IWorkflowPolicy> policy);
		std::shared_ptr<IWorkflowPolicy> GetPolicy() const;
		void ResetToMinimalPolicy();

		// 项目与工程上下文
		void SetProjectRoot(const std::string& root);
		const std::string& GetProjectRoot() const { return m_ProjectRoot; }

		// 状态与日志持久化 (.we/ai/state.json, .we/ai/journal/*.md, .we/studio/decisions.md)
		bool SaveState(const std::string& projectRoot = "") const;
		bool LoadState(const std::string& projectRoot = "");
		void AppendJournal(const std::string& sessionToken, const std::string& roleId, const std::string& action, const std::string& details, const std::string& projectRoot = "") const;
		void AppendDecision(const std::string& actor, const std::string& decisionTitle, const std::string& rationale, const std::string& projectRoot = "") const;

		// 会话管理
		std::string OpenSession(const std::string& roleId, const std::string& taskId = "", const std::string& projectRoot = "");
		bool CloseSession(const std::string& token);
		const SessionContext* GetSession(const std::string& token) const;
		std::vector<SessionContext> ListSessions() const;
		void SetActiveSessionToken(const std::string& token);
		std::string GetActiveSessionToken() const;

		// 职能开关决断与运行期状态机 (D13 / §6.4.1.2)
		void SetRoleState(const std::string& roleId, RoleRuntimeState state, const std::string& reason = "", const std::string& projectRoot = "");
		RoleRuntimeState GetRoleState(const std::string& roleId) const;
		std::map<std::string, RoleRuntimeState> GetAllRoleStates() const;
		std::string GenerateRoleActivationPlan(const std::string& genreArchetype, const std::string& projectRoot = "");

		// 状态机阶段推进与受控回退
		WorkflowStage GetCurrentStage() const { return m_CurrentStage; }
		void SetCurrentStageDirect(WorkflowStage stage);
		bool AdvanceStage(const std::string& sessionToken, std::string& outError);
		bool RollbackToStage(const std::string& sessionToken, WorkflowStage targetStage, std::string& outError);

		// 门禁检查与 L0 出品人特权覆盖 Blocker (D10)
		GateEvaluationResult CheckGate(WorkflowStage stage, const std::string& projectRoot = "") const;
		void OverrideGateBlocker(const std::string& blockerPattern, const std::string& rationale, const std::string& projectRoot = "");
		bool IsBlockerOverridden(const std::string& blockerText) const;
		std::vector<std::string> GetOverriddenBlockers() const;

		// 引擎缺口上报
		std::string RequestEngineChange(const EngineChangeRequest& req, const std::string& projectRoot = "");
		std::vector<EngineChangeRequest> ListEngineRequests(const std::string& projectRoot = "") const;

		// 状态复位 (测试用)
		void ResetAll();

		// 注册 M4 命令到 CommandRegistry
		static void RegisterCommands(World::Ai::CommandRegistry& reg);

	private:
		mutable std::recursive_mutex m_Mutex;
		std::string m_ProjectRoot;
		std::shared_ptr<IWorkflowPolicy> m_Policy;
		WorkflowStage m_CurrentStage = WorkflowStage::Concept;
		std::map<std::string, SessionContext> m_Sessions;
		std::string m_ActiveSessionToken;
		std::map<std::string, RoleRuntimeState> m_RoleStates;
		std::vector<std::string> m_OverriddenBlockers;
		uint64_t m_SessionCounter = 0;
		uint64_t m_RequestCounter = 0;
	};
}

