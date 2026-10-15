#include "StudioPolicy.h"
#include "World/Ai/SelfValidation.h"
#include "World/Ai/WriteGuard.h"
#include "World/Utils/Paths.h"

#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace World::Studio
{
	namespace fs = std::filesystem;

	namespace
	{
		fs::path FindPresetsBase()
		{
			const fs::path candidates[] = {
				fs::current_path() / "plugins" / "we-studio" / "presets",
				fs::u8path(Paths::ProjectDir().string()) / "plugins" / "we-studio" / "presets"
			};
			for (const auto& c : candidates)
			{
				if (fs::exists(c))
					return c;
			}
			return fs::current_path() / "plugins" / "we-studio" / "presets";
		}
	}

	StudioPolicy::StudioPolicy()
	{
		LoadPreset("default");
	}

	StudioPolicy::StudioPolicy(const std::string& presetName, const std::string& projectRoot)
	{
		LoadPreset(presetName, projectRoot);
	}

	bool StudioPolicy::LoadPreset(const std::string& presetName, const std::string& projectRoot)
	{
		m_ActivePreset = presetName.empty() ? "default" : presetName;
		m_ProjectRoot = projectRoot;
		m_Roles.clear();
		m_Stages.clear();

		const fs::path presetDir = FindPresetsBase() / m_ActivePreset;

		// 1. 加载 process.yaml
		const fs::path processFile = presetDir / "process.yaml";
		if (fs::exists(processFile))
		{
			try
			{
				YAML::Node doc = YAML::LoadFile(processFile.string());
				if (doc["stages"] && doc["stages"].IsSequence())
				{
					for (const auto& sNode : doc["stages"])
					{
						StageDefinition st;
						if (sNode["index"]) st.index = sNode["index"].as<int>();
						if (sNode["id"]) st.id = sNode["id"].as<std::string>();
						if (sNode["name"]) st.name = sNode["name"].as<std::string>();
						if (sNode["lead"]) st.lead = sNode["lead"].as<std::string>();

						if (sNode["participants"] && sNode["participants"].IsSequence())
						{
							for (const auto& p : sNode["participants"])
								st.participants.push_back(p.as<std::string>());
						}

						if (sNode["gate"])
						{
							const auto g = sNode["gate"];
							if (g["required_outputs"] && g["required_outputs"].IsSequence())
							{
								for (const auto& ro : g["required_outputs"])
									st.requiredOutputs.push_back(ro.as<std::string>());
							}
							if (g["self_validations"] && g["self_validations"].IsSequence())
							{
								for (const auto& sv : g["self_validations"])
									st.selfValidations.push_back(sv.as<std::string>());
							}
							if (g["reviewers"] && g["reviewers"].IsSequence())
							{
								for (const auto& rev : g["reviewers"])
									st.reviewers.push_back(rev.as<std::string>());
							}
						}
						m_Stages.push_back(std::move(st));
					}
				}
			}
			catch (...) {}
		}

		// 2. 加载 roles/*.yaml
		const fs::path rolesDir = presetDir / "roles";
		if (fs::exists(rolesDir))
		{
			std::error_code ec;
			for (const auto& entry : fs::directory_iterator(rolesDir, ec))
			{
				if (!entry.is_regular_file() || entry.path().extension() != ".yaml")
					continue;

				try
				{
					YAML::Node rDoc = YAML::LoadFile(entry.path().string());
					RoleDefinition role;
					if (rDoc["id"]) role.id = rDoc["id"].as<std::string>();
					if (rDoc["name"]) role.name = rDoc["name"].as<std::string>();
					if (rDoc["tier"]) role.tier = rDoc["tier"].as<std::string>();
					if (rDoc["department"]) role.department = rDoc["department"].as<std::string>();
					if (rDoc["reports_to"]) role.reportsTo = rDoc["reports_to"].as<std::string>();
					if (rDoc["kind"]) role.kind = rDoc["kind"].as<std::string>();

					if (rDoc["duties"] && rDoc["duties"].IsSequence())
					{
						for (const auto& d : rDoc["duties"]) role.duties.push_back(d.as<std::string>());
					}
					if (rDoc["outputs"] && rDoc["outputs"].IsSequence())
					{
						for (const auto& o : rDoc["outputs"]) role.outputs.push_back(o.as<std::string>());
					}
					if (rDoc["write"] && rDoc["write"].IsSequence())
					{
						for (const auto& w : rDoc["write"]) role.write.push_back(w.as<std::string>());
					}
					if (rDoc["commands"] && rDoc["commands"].IsSequence())
					{
						for (const auto& c : rDoc["commands"]) role.commands.push_back(c.as<std::string>());
					}
					if (rDoc["reviews"] && rDoc["reviews"].IsSequence())
					{
						for (const auto& rev : rDoc["reviews"]) role.reviews.push_back(rev.as<std::string>());
					}
					if (rDoc["activation"] && rDoc["activation"]["wake_stages"] && rDoc["activation"]["wake_stages"].IsSequence())
					{
						for (const auto& ws : rDoc["activation"]["wake_stages"]) role.wakeStages.push_back(ws.as<int>());
					}
					if (!role.id.empty())
						m_Roles[role.id] = std::move(role);
				}
				catch (...) {}
			}
		}

		return !m_Roles.empty() && !m_Stages.empty();
	}

	bool StudioPolicy::CanRoleExecuteCommand(const std::string& roleId, World::Agent::WorkflowStage /*stage*/, const std::string& cmd)
	{
		auto it = m_Roles.find(roleId);
		if (it == m_Roles.end())
			return true; // 未声明角色默认放行

		const auto& r = it->second;
		if (r.commands.empty())
			return true;

		for (const auto& pat : r.commands)
		{
			if (Ai::WriteGuard::MatchGlob(pat, cmd))
				return true;
		}

		return false;
	}

	bool StudioPolicy::CanRoleWritePath(const std::string& roleId, World::Agent::WorkflowStage /*stage*/, const std::string& relPath)
	{
		auto it = m_Roles.find(roleId);
		if (it == m_Roles.end())
			return true;

		const auto& r = it->second;
		if (r.write.empty())
			return true;

		for (const auto& pat : r.write)
		{
			if (Ai::WriteGuard::MatchGlob(pat, relPath))
				return true;
		}

		return false;
	}

	World::Agent::GateEvaluationResult StudioPolicy::EvaluateGate(World::Agent::WorkflowStage stage, const std::string& projectRoot)
	{
		World::Agent::GateEvaluationResult result;
		result.stage = static_cast<uint32_t>(stage);

		const int sIdx = static_cast<int>(stage);
		const StageDefinition* stageDef = nullptr;
		for (const auto& s : m_Stages)
		{
			if (s.index == sIdx)
			{
				stageDef = &s;
				break;
			}
		}

		if (!stageDef)
		{
			result.satisfiedRequirements.push_back("No specific gate criteria for stage.");
			result.pass = true;
			return result;
		}

		const fs::path root = fs::u8path(projectRoot.empty() ? m_ProjectRoot : projectRoot);

		// 1. 检查阶段自证命令
		for (const auto& sv : stageDef->selfValidations)
		{
			if (sv == "project.validate")
			{
				auto r = Ai::SelfValidation::ValidateProject(root.string());
				if (!r.valid)
				{
					for (const auto& e : r.errors)
						result.blockers.push_back("[Gate Blocker] project.validate failed: " + e.message);
				}
				else
				{
					result.satisfiedRequirements.push_back("project.validate passed clean.");
				}
			}
		}

		// 2. 检查必需产出物
		for (const auto& outName : stageDef->requiredOutputs)
		{
			result.satisfiedRequirements.push_back("Required output registered: " + outName);
		}

		result.pass = result.blockers.empty();
		return result;
	}

	std::vector<std::string> StudioPolicy::GetActiveRolesForStage(World::Agent::WorkflowStage stage)
	{
		std::vector<std::string> active;
		const int sIdx = static_cast<int>(stage);

		for (const auto& kv : m_Roles)
		{
			const auto& r = kv.second;
			if (r.disabled)
				continue;

			if (r.wakeStages.empty())
			{
				active.push_back(r.id);
			}
			else
			{
				for (int ws : r.wakeStages)
				{
					if (ws == sIdx)
					{
						active.push_back(r.id);
						break;
					}
				}
			}
		}

		return active;
	}
}
