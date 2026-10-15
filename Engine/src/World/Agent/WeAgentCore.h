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
	// 8 阶段通用游戏研发推进阶段 (0..7)
	enum class WorkflowStage : uint8_t
	{
		Concept = 0,     // 0: 概念与立项
		Feasibility,     // 1: 可行性论证与风格锁
		Prototype,       // 2: 核心玩法找乐原型
		VerticalSlice,   // 3: 垂直切片 (高质量完整样板)
		Production,      // 4: 资产与内容量产
		Beta,            // 5: 优化、Bug修复与体验打磨
		Launch,          // 6: 发行与打包上架
		LiveOps,         // 7: 持续运营与更新
		StageCount
	};

	WLD_API const char* WorkflowStageToString(WorkflowStage stage);
	WLD_API WorkflowStage StringToWorkflowStage(const std::string& str);

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
		void SetProjectRoot(const std::string& root) { m_ProjectRoot = root; }
		const std::string& GetProjectRoot() const { return m_ProjectRoot; }

		// 会话管理
		std::string OpenSession(const std::string& roleId, const std::string& taskId = "", const std::string& projectRoot = "");
		bool CloseSession(const std::string& token);
		const SessionContext* GetSession(const std::string& token) const;
		std::vector<SessionContext> ListSessions() const;

		// 状态机阶段推进与受控回退
		WorkflowStage GetCurrentStage() const { return m_CurrentStage; }
		void SetCurrentStageDirect(WorkflowStage stage) { m_CurrentStage = stage; }
		bool AdvanceStage(const std::string& sessionToken, std::string& outError);
		bool RollbackToStage(const std::string& sessionToken, WorkflowStage targetStage, std::string& outError);

		// 门禁检查
		GateEvaluationResult CheckGate(WorkflowStage stage, const std::string& projectRoot = "") const;

		// 引擎缺口上报
		std::string RequestEngineChange(const EngineChangeRequest& req, const std::string& projectRoot = "");
		std::vector<EngineChangeRequest> ListEngineRequests(const std::string& projectRoot = "") const;

		// 状态复位 (测试用)
		void ResetAll();

		// 注册 M4 命令到 CommandRegistry
		static void RegisterCommands(World::Ai::CommandRegistry& reg);

	private:
		mutable std::mutex m_Mutex;
		std::string m_ProjectRoot;
		std::shared_ptr<IWorkflowPolicy> m_Policy;
		WorkflowStage m_CurrentStage = WorkflowStage::Concept;
		std::map<std::string, SessionContext> m_Sessions;
		uint64_t m_SessionCounter = 0;
		uint64_t m_RequestCounter = 0;
	};
}
