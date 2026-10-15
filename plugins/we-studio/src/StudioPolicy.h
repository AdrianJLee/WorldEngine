#pragma once

#include "World/Agent/WeAgentCore.h"
#include <map>
#include <string>
#include <vector>

namespace World::Studio
{
	struct RoleDefinition
	{
		std::string id;
		std::string name;
		std::string tier;
		std::string department;
		std::string reportsTo;
		std::string kind;
		std::vector<std::string> duties;
		std::vector<std::string> outputs;
		std::vector<std::string> write;
		std::vector<std::string> commands;
		std::vector<std::string> reviews;
		std::vector<int> wakeStages;
		bool disabled = false;
	};

	struct StageDefinition
	{
		int index = 0;
		std::string id;
		std::string name;
		std::string lead;
		std::vector<std::string> participants;
		std::vector<std::string> requiredOutputs;
		std::vector<std::string> selfValidations;
		std::vector<std::string> reviewers;
	};

	class StudioPolicy : public World::Agent::IWorkflowPolicy
	{
	public:
		StudioPolicy();
		explicit StudioPolicy(const std::string& presetName, const std::string& projectRoot = "");

		std::string GetPolicyName() const override { return "StudioWorkflowPolicy"; }
		bool CanRoleExecuteCommand(const std::string& roleId, World::Agent::WorkflowStage stage, const std::string& cmd) override;
		bool CanRoleWritePath(const std::string& roleId, World::Agent::WorkflowStage stage, const std::string& relPath) override;
		World::Agent::GateEvaluationResult EvaluateGate(World::Agent::WorkflowStage stage, const std::string& projectRoot) override;
		std::vector<std::string> GetActiveRolesForStage(World::Agent::WorkflowStage stage) override;

		bool LoadPreset(const std::string& presetName, const std::string& projectRoot = "");
		const std::map<std::string, RoleDefinition>& GetRoles() const { return m_Roles; }
		const std::vector<StageDefinition>& GetStages() const { return m_Stages; }

	private:
		std::string m_ActivePreset = "default";
		std::string m_ProjectRoot;
		std::map<std::string, RoleDefinition> m_Roles;
		std::vector<StageDefinition> m_Stages;
	};
}
