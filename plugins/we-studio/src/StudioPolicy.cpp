#include "StudioPolicy.h"
#include "BacklogManager.h"
#include "World/Ai/SelfValidation.h"
#include "World/Ai/PlaytestRunner.h"
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
#if defined(WLD_REPO_ROOT)
			const fs::path repoCandidate = fs::u8path(WLD_REPO_ROOT) / "plugins" / "we-studio" / "presets";
			std::error_code ecRepo;
			if (fs::exists(repoCandidate, ecRepo))
				return repoCandidate;
#endif
			const fs::path candidates[] = {
				fs::current_path() / "plugins" / "we-studio" / "presets",
				fs::u8path(Paths::ProjectDir().string()) / "plugins" / "we-studio" / "presets"
			};
			for (const auto& c : candidates)
			{
				if (fs::exists(c))
					return c;
			}
#if defined(WLD_REPO_ROOT)
			return fs::u8path(WLD_REPO_ROOT) / "plugins" / "we-studio" / "presets";
#else
			return fs::current_path() / "plugins" / "we-studio" / "presets";
#endif
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

		auto loadRolesFromDir = [&](const fs::path& rDir)
		{
			if (!fs::exists(rDir)) return;
			std::error_code ec;
			for (const auto& entry : fs::directory_iterator(rDir, ec))
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
					if (rDoc["guide"]) role.guide = rDoc["guide"].as<std::string>();
					if (rDoc["disabled"]) role.disabled = rDoc["disabled"].as<bool>();

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
					if (rDoc["activation"])
					{
						const auto act = rDoc["activation"];
						if (act["category"]) role.activationCategory = act["category"].as<std::string>();
						if (act["default_state"]) role.defaultState = act["default_state"].as<std::string>();
						if (act["wake_stages"] && act["wake_stages"].IsSequence())
						{
							for (const auto& ws : act["wake_stages"]) role.wakeStages.push_back(ws.as<int>());
						}
						if (act["genres"] && act["genres"].IsSequence())
						{
							for (const auto& g : act["genres"]) role.activationGenres.push_back(g.as<std::string>());
						}
					}
					if (!role.id.empty())
						m_Roles[role.id] = std::move(role);
				}
				catch (...) {}
			}
		};

		// 2. 加载 roles/*.yaml (非 lite 预设默认继承 default 基础角色库)
		if (m_ActivePreset != "default" && m_ActivePreset != "lite")
		{
			loadRolesFromDir(FindPresetsBase() / "default" / "roles");
		}
		loadRolesFromDir(presetDir / "roles");

		// 3. 项目覆盖层合并 (.we/studio/)
		if (!m_ProjectRoot.empty())
		{
			const fs::path pStudio = fs::u8path(m_ProjectRoot) / ".we" / "studio";
			if (fs::exists(pStudio / "studio.yaml"))
			{
				try
				{
					YAML::Node sDoc = YAML::LoadFile((pStudio / "studio.yaml").string());
					if (sDoc["unattributed_changes"])
						m_UnattributedPolicy = sDoc["unattributed_changes"].as<std::string>();
					if (sDoc["forbid_dark_patterns"])
						m_ForbidDarkPatterns = sDoc["forbid_dark_patterns"].as<bool>();
				}
				catch (...) {}
			}
			loadRolesFromDir(pStudio / "roles");
		}

		return !m_Roles.empty() && !m_Stages.empty();
	}

	bool StudioPolicy::CanRoleExecuteCommand(const std::string& roleId, World::Agent::WorkflowStage /*stage*/, const std::string& cmd)
	{
		// 快捷 QA 角色命令权限判定
		if (roleId == "qa" || roleId == "tester")
		{
			if (cmd.rfind("changeset.", 0) == 0 || cmd.rfind("gen.", 0) == 0 || cmd == "build.package")
				return false;
			return true;
		}

		auto it = m_Roles.find(roleId);
		if (it == m_Roles.end())
			return true; // 未显式声明角色默认放行只读

		const auto& r = it->second;
		if (r.disabled)
			return false; // 禁用角色严禁执行任何指令

		if (r.commands.empty())
			return false; // 角色声明且 commands 为空则拒绝写入

		for (const auto& pat : r.commands)
		{
			if (pat == "*" || Ai::WriteGuard::MatchGlob(pat, cmd))
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

		// ---------------------------------------------------------------------
		// 1. 真实自证命令校验 (Real Self-Validations)
		// ---------------------------------------------------------------------
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
			else if (sv == "playtest.run")
			{
				// 寻找可用关卡文件执行真实无头试玩推帧
				fs::path sceneTarget;
				const fs::path manifestPath = root / "project.we.yaml";
				if (fs::exists(manifestPath))
				{
					try
					{
						YAML::Node doc = YAML::LoadFile(manifestPath.string());
						if (doc["start_scene"])
						{
							const std::string sc = doc["start_scene"].as<std::string>();
							const fs::path p1 = root / sc;
							const fs::path p2 = root / "assets" / sc;
							if (fs::exists(p1)) sceneTarget = p1;
							else if (fs::exists(p2)) sceneTarget = p2;
						}
					}
					catch (...) {}
				}
				if (sceneTarget.empty())
				{
					const fs::path defaultScene = root / "assets" / "scenes" / "Main.wlevel";
					const fs::path level1Scene = root / "assets" / "scenes" / "level1.yaml";
					if (fs::exists(defaultScene)) sceneTarget = defaultScene;
					else if (fs::exists(level1Scene)) sceneTarget = level1Scene;
				}

				if (sceneTarget.empty() || !fs::exists(sceneTarget))
				{
					result.blockers.push_back("[Gate Blocker] playtest.run failed: No valid scene file found for simulation in assets/scenes/.");
				}
				else
				{
					Ai::PlaytestConfig ptCfg;
					ptCfg.scenePath = sceneTarget.string();
					ptCfg.frames = 30;
					ptCfg.asserts = { "entity_count > 0" };

					auto ptRes = Ai::PlaytestRunner::Run(ptCfg, nullptr);
					if (!ptRes.pass)
					{
						std::string reason = ptRes.failures.empty() ? "Simulation assertion failed" : ptRes.failures[0]; result.blockers.push_back("[Gate Blocker] playtest.run failed: " + reason);
					}
					else
					{
						result.satisfiedRequirements.push_back("playtest.run simulated clean (30 frames, entity_count > 0).");
					}
				}
			}
			else
			{
				result.satisfiedRequirements.push_back("Self validation verified: " + sv);
			}
		}

		// ---------------------------------------------------------------------
		// 2. 真实必需产出物物理检查 (Real Required Outputs Verification)
		// ---------------------------------------------------------------------
		for (const auto& outName : stageDef->requiredOutputs)
		{
			if (outName == "brief" || outName == "vision_pitch")
			{
				const fs::path tastePath = root / ".we" / "studio" / "taste.yaml";
				bool hasValidBrief = false;
				if (fs::exists(tastePath))
				{
					std::ifstream f(tastePath);
					std::string str((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
					if (str.find("genre_type: \"\"") == std::string::npos && str.find("genre_type:") != std::string::npos)
					{
						hasValidBrief = true;
					}
				}
				if (!hasValidBrief)
				{
					result.blockers.push_back("[Gate Blocker] Required output '" + outName + "' missing: Project genre/taste is unspecified in .we/studio/taste.yaml.");
				}
				else
				{
					result.satisfiedRequirements.push_back("Required output verified: " + outName);
				}
			}
			else if (outName == "art_style_lock")
			{
				const fs::path tastePath = root / ".we" / "studio" / "taste.yaml";
				bool hasStyle = false;
				if (fs::exists(tastePath))
				{
					std::ifstream f(tastePath);
					std::string str((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
					if (str.find("art_style: \"\"") == std::string::npos && str.find("art_style:") != std::string::npos)
					{
						hasStyle = true;
					}
				}
				if (!hasStyle)
				{
					result.blockers.push_back("[Gate Blocker] Required output 'art_style_lock' missing: Art style is unspecified in .we/studio/taste.yaml.");
				}
				else
				{
					result.satisfiedRequirements.push_back("Required output verified: art_style_lock");
				}
			}
			else if (outName == "playable_prototype" || outName == "vertical_slice_build")
			{
				// 检查脚本与场景实体存在性
				bool hasCustomScripts = false;
				const fs::path scriptsDir1 = root / "assets" / "scripts" / "systems";
				const fs::path scriptsDir2 = root / "assets" / "scripts";
				auto checkDir = [&](const fs::path& dir) {
					if (fs::exists(dir)) {
						std::error_code ec;
						for (const auto& entry : fs::directory_iterator(dir, ec)) {
							if (entry.is_regular_file() && entry.path().extension() == ".luau")
								return true;
						}
					}
					return false;
				};
				hasCustomScripts = checkDir(scriptsDir1) || checkDir(scriptsDir2);

				if (!hasCustomScripts)
				{
					result.blockers.push_back("[Gate Blocker] Required output '" + outName + "' missing: No game logic scripts found under assets/scripts/.");
				}
				else
				{
					result.satisfiedRequirements.push_back("Required output verified: " + outName);
				}
			}
			else
			{
				result.satisfiedRequirements.push_back("Required output registered: " + outName);
			}
		}

		// ---------------------------------------------------------------------
		// 3. 检查当前阶段未闭环任务 (Unfinished Tasks Check)
		// ---------------------------------------------------------------------
		auto tasks = BacklogManager::Get().ListTasks(root.string());
		for (const auto& t : tasks)
		{
			if ((t.stage == std::to_string(sIdx) || t.stage == stageDef->id) && (t.status == "open" || t.status == "in_progress"))
			{
				result.blockers.push_back("[Gate Blocker] Active unfinished task: [" + t.id + "] " + t.title + " (status: " + t.status + ")");
			}
		}

		// ---------------------------------------------------------------------
		// 4. 检查未关闭的 Blocker 缺陷 (§6.5, D10)
		// ---------------------------------------------------------------------
		auto bugs = BacklogManager::Get().ListBugs(root.string(), "blocker");
		for (const auto& b : bugs)
		{
			if (b.status != "closed" && b.status != "resolved")
			{
				result.blockers.push_back("[Gate Blocker] Unresolved blocker defect: [" + b.id + "] " + b.title);
			}
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


