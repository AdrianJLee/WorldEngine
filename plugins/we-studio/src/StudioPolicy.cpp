#include "StudioPolicy.h"
#include "BacklogManager.h"
#include "World/Ai/SelfValidation.h"
#include "World/Core/StringPool.h"
#include "World/Core/WorldContext.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Scene/Components.h"
#include "World/Ai/PlaytestRunner.h"
#include "World/Ai/WriteGuard.h"
#include "World/Utils/Paths.h"

#include <yaml-cpp/yaml.h>

#include <cctype>
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

		namespace
	{
		struct LevelFacts
		{
			bool ok = false;
			size_t levelCount = 0;
			size_t entities = 0;
			size_t cameras = 0;
			size_t lights = 0;
			size_t meshes = 0;
			size_t meshesWithAsset = 0;
			size_t whiteboxes = 0;
			size_t bodies = 0;
			size_t audioSources = 0;
			size_t audioResolved = 0;
			size_t health = 0;
			size_t behaviors = 0;
			size_t smartObjects = 0;
			size_t emptyMeshEntities = 0;
			std::vector<std::string> errors;
		};

		struct ScriptFacts
		{
			size_t files = 0;
			size_t wired = 0;
			size_t stateMachines = 0;
			std::string firstError;
		};

		std::string LowerCopy(std::string value)
		{
			for (char& c : value)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return value;
		}

		bool HasArtifact(const fs::path& root, const std::string& name)
		{
			const std::string wanted = LowerCopy(name);
			const fs::path dirs[] = { root / ".we" / "studio" / "artifacts", root / ".we" / "studio" / "reports" };
			std::error_code ec;
			for (const auto& dir : dirs)
			{
				if (!fs::exists(dir, ec))
					continue;
				for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
				{
					if (!entry.is_regular_file())
						continue;
					const std::string stem = LowerCopy(entry.path().stem().string());
					if (stem == wanted && entry.file_size(ec) >= 64)
						return true;
				}
			}
			return false;
		}

		std::string ReadSmallFile(const fs::path& path)
		{
			std::ifstream f(path, std::ios::binary);
			if (!f)
				return std::string();
			return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		}

		LevelFacts InspectLevels(const fs::path& root)
		{
			LevelFacts facts;
			std::error_code ec;
			const fs::path assets = root / "assets";
			if (!fs::exists(assets, ec))
			{
				facts.errors.push_back("assets/ directory is missing");
				return facts;
			}

			for (const auto& entry : fs::recursive_directory_iterator(assets, ec))
			{
				if (!entry.is_regular_file() || entry.path().extension() != ".wlevel")
					continue;
				const std::string pathText = entry.path().string();
				if (pathText.find("intermediate") != std::string::npos)
					continue;

				++facts.levelCount;
				WorldContext context;
				Level::LevelPackage package;
				std::string loadError;
				if (!package.Load(entry.path(), &loadError))
				{
					facts.errors.push_back("level load failed (" + entry.path().filename().string() + "): " + loadError);
					continue;
				}
				Ref<Scene> scene = package.Instantiate(context);
				if (!scene)
				{
					facts.errors.push_back("level instantiate failed: " + entry.path().filename().string());
					continue;
				}

				facts.ok = true;
				const auto& registry = scene->GetRegistry();
				for (auto handle : registry.view<TagComponent>())
				{
					++facts.entities;
					if (registry.all_of<CameraComponent>(handle))
						++facts.cameras;
					if (registry.all_of<DirectionalLightComponent>(handle) ||
						registry.all_of<PointLightComponent>(handle) ||
						registry.all_of<AmbientLightComponent>(handle))
						++facts.lights;
					if (registry.all_of<RigidBody2DComponent>(handle) || registry.all_of<RigidBody3DComponent>(handle) ||
						registry.all_of<BoxCollider2DComponent>(handle) || registry.all_of<BoxCollider3DComponent>(handle))
						++facts.bodies;
					if (registry.all_of<HealthComponent>(handle))
						++facts.health;
					if (registry.all_of<BehaviorTreeComponent>(handle))
						++facts.behaviors;
					if (registry.all_of<SmartObjectComponent>(handle))
						++facts.smartObjects;

					if (const auto* whitebox = registry.try_get<WhiteboxComponent>(handle))
					{
						(void)whitebox;
						++facts.whiteboxes;
					}

					if (const auto* mesh = registry.try_get<MeshRendererComponent>(handle))
					{
						++facts.meshes;
						const std::string meshPath = StringPool::Get().PathOf(mesh->Mesh.Path);
						const std::string materialPath = StringPool::Get().PathOf(mesh->Material.Path);
						if (!meshPath.empty() || !materialPath.empty())
							++facts.meshesWithAsset;
						else if (!registry.all_of<WhiteboxComponent>(handle))
							++facts.emptyMeshEntities;
					}

					if (const auto* audio = registry.try_get<AudioSourceComponent>(handle))
					{
						++facts.audioSources;
						const std::string soundPath = StringPool::Get().PathOf(audio->Sound.Path);
						if (!soundPath.empty())
						{
							const fs::path candidates[] = { assets / fs::u8path(soundPath), root / fs::u8path(soundPath) };
							for (const auto& candidate : candidates)
							{
								if (fs::exists(candidate))
								{
									++facts.audioResolved;
									break;
								}
							}
						}
					}
				}
			}
			return facts;
		}

		ScriptFacts InspectScripts(const fs::path& root)
		{
			ScriptFacts facts;
			std::error_code ec;
			const fs::path scriptsRoot = root / "assets" / "scripts";
			if (!fs::exists(scriptsRoot, ec))
				return facts;

			for (const auto& entry : fs::recursive_directory_iterator(scriptsRoot, ec))
			{
				if (!entry.is_regular_file())
					continue;
				const std::string ext = entry.path().extension().string();
				if (ext != ".luau" && ext != ".lua")
					continue;
				const std::string pathText = entry.path().string();
				if (pathText.find("intermediate") != std::string::npos)
					continue;
				const std::string source = ReadSmallFile(entry.path());
				if (source.size() < 64)
					continue;

				++facts.files;
				const bool wired = source.find("AddSystem") != std::string::npos ||
					source.find("OnUpdate") != std::string::npos ||
					source.find("OnBeginPlay") != std::string::npos ||
					source.find("function Update") != std::string::npos;
				if (wired)
					++facts.wired;

				const std::string lower = LowerCopy(source);
				int stateTokens = 0;
				for (const char* token : { "title", "menu", "playing", "gameover", "game_over", "victory", "defeat", "paused" })
				{
					if (lower.find(token) != std::string::npos)
						++stateTokens;
				}
				const bool restarts = lower.find("restart") != std::string::npos ||
					lower.find("retry") != std::string::npos ||
					lower.find("reset") != std::string::npos;
				if (stateTokens >= 3 && restarts)
					++facts.stateMachines;
			}
			return facts;
		}

		bool AnyFileWithExtension(const fs::path& dir, const std::vector<std::string>& extensions, size_t limit)
		{
			std::error_code ec;
			if (!fs::exists(dir, ec))
				return false;
			size_t seen = 0;
			for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
			{
				if (!entry.is_regular_file())
					continue;
				const std::string ext = entry.path().extension().string();
				for (const auto& wanted : extensions)
				{
					if (ext == wanted)
						return true;
				}
				if (++seen > limit)
					break;
			}
			return false;
		}
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
		bool playtestPassed = false;

		// ---- 1. 自证命令:没有实现的命令一律阻断,不再写 "verified" 混过去 ----
		for (const auto& sv : stageDef->selfValidations)
		{
			if (sv == "project.validate")
			{
				const auto report = Ai::SelfValidation::ValidateProject(root.string());
				if (!report.valid)
				{
					size_t shown = 0;
					for (const auto& e : report.errors)
					{
						if (shown++ >= 5)
							break;
						result.blockers.push_back("[Gate Blocker] project.validate failed: " + e.code + " " + e.file + " - " + e.message);
					}
					if (shown == 0)
						result.blockers.push_back("[Gate Blocker] project.validate failed without a report");
				}
				else
				{
					result.satisfiedRequirements.push_back("project.validate passed clean.");
				}
			}
			else if (sv == "asset.validate")
			{
				size_t checked = 0;
				size_t invalid = 0;
				std::string firstError;
				std::error_code ec;
				const fs::path assets = root / "assets";
				if (fs::exists(assets, ec))
				{
					for (const auto& entry : fs::recursive_directory_iterator(assets, ec))
					{
						if (!entry.is_regular_file())
							continue;
						const std::string ext = entry.path().extension().string();
						if (ext != ".wmat" && ext != ".wmodel" && ext != ".watlas" && ext != ".wanim" &&
							ext != ".wtilemap" && ext != ".wphysmat" && ext != ".wvfx")
							continue;
						++checked;
						const auto report = Ai::SelfValidation::ValidateAsset(entry.path().string(), root.string());
						if (!report.valid)
						{
							++invalid;
							if (firstError.empty() && !report.errors.empty())
								firstError = entry.path().filename().string() + ": " + report.errors.front().message;
						}
						if (checked >= 200)
							break;
					}
				}
				if (invalid > 0)
					result.blockers.push_back("[Gate Blocker] asset.validate failed: " + std::to_string(invalid) + "/" + std::to_string(checked) + " assets invalid (" + firstError + ")");
				else
					result.satisfiedRequirements.push_back("asset.validate checked " + std::to_string(checked) + " asset(s), all valid.");
			}
			else if (sv == "playtest.run")
			{
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
					std::error_code ec;
					for (const auto& entry : fs::recursive_directory_iterator(root / "assets", ec))
					{
						if (entry.is_regular_file() && entry.path().extension() == ".wlevel")
						{
							sceneTarget = entry.path();
							break;
						}
					}
				}

				if (sceneTarget.empty())
				{
					result.blockers.push_back("[Gate Blocker] playtest.run failed: no .wlevel scene found under assets/.");
				}
				else
				{
					Ai::PlaytestConfig ptCfg;
					ptCfg.scenePath = sceneTarget.string();
					ptCfg.frames = 60;
					ptCfg.asserts = { "entity_count > 0", "no_nan" };
					const auto ptRes = Ai::PlaytestRunner::Run(ptCfg, nullptr);
					if (!ptRes.pass)
					{
						const std::string reason = ptRes.failures.empty() ? "simulation failed" : ptRes.failures.front();
						result.blockers.push_back("[Gate Blocker] playtest.run failed: " + reason);
					}
					else
					{
						playtestPassed = true;
						result.satisfiedRequirements.push_back("playtest.run passed 60 frames on " + sceneTarget.filename().string() + ".");
					}
				}
			}
			else if (sv == "build.package")
			{
				bool runtimeFound = false;
				std::error_code ec;
				for (const fs::path& dir : { root / "dist", fs::u8path("build") })
				{
					if (!fs::exists(dir, ec))
						continue;
					for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
					{
						if (entry.is_regular_file() && entry.path().filename() == "Runtime.exe")
						{
							runtimeFound = true;
							break;
						}
					}
					if (runtimeFound)
						break;
				}
				if (!runtimeFound)
					result.blockers.push_back("[Gate Blocker] build.package failed: no Runtime.exe found in dist/ or build/.");
				else
					result.satisfiedRequirements.push_back("build.package prerequisites found (Runtime.exe).");
			}
			else
			{
				result.blockers.push_back("[Gate Blocker] Self validation '" + sv + "' has no implementation; refusing to auto-pass.");
			}
		}

		// ---- 2. 必需产出物:每一条都有可核对的物理证据 ----
		LevelFacts facts;
		ScriptFacts scripts;
		bool factsReady = false;
		bool scriptsReady = false;
		auto ensureFacts = [&]() { if (!factsReady) { facts = InspectLevels(root); factsReady = true; } };
		auto ensureScripts = [&]() { if (!scriptsReady) { scripts = InspectScripts(root); scriptsReady = true; } };
		auto block = [&](const std::string& message) { result.blockers.push_back("[Gate Blocker] " + message); };

		for (const auto& outName : stageDef->requiredOutputs)
		{
			if (outName == "brief" || outName == "vision_pitch")
			{
				bool hasGenre = false;
				const fs::path tastePath = root / ".we" / "studio" / "taste.yaml";
				if (fs::exists(tastePath))
				{
					const std::string str = ReadSmallFile(tastePath);
					hasGenre = str.find("genre_type: \"\"") == std::string::npos && str.find("genre_type:") != std::string::npos;
				}
				const fs::path assumptions = root / ".we" / "studio" / "assumptions.md";
				const bool hasAssumptions = fs::exists(assumptions) && fs::file_size(assumptions) >= 64;
				if (!hasGenre)
					block("Required output 'brief' missing: Project genre/taste is unspecified in .we/studio/taste.yaml.");
				else if (!hasAssumptions)
					block("Required output 'brief' missing: .we/studio/assumptions.md does not exist or is empty.");
				else
					result.satisfiedRequirements.push_back("Required output verified: " + outName);
			}
			else if (outName == "art_style_lock")
			{
				bool hasStyle = false;
				const fs::path tastePath = root / ".we" / "studio" / "taste.yaml";
				if (fs::exists(tastePath))
				{
					const std::string str = ReadSmallFile(tastePath);
					hasStyle = str.find("art_style: \"\"") == std::string::npos && str.find("art_style:") != std::string::npos;
				}
				ensureFacts();
				const bool hasArt = AnyFileWithExtension(root / "assets" / "textures", { ".wtex", ".png", ".watlas" }, 2000) ||
					AnyFileWithExtension(root / "assets" / "materials", { ".wmat" }, 2000) ||
					AnyFileWithExtension(root / "assets" / "models", { ".wmodel" }, 2000);
				if (!hasStyle)
					block("Required output 'art_style_lock' missing: Art style is unspecified in .we/studio/taste.yaml.");
				else if (!hasArt)
					block("Required output 'art_style_lock' missing: no texture/material/model assets found to prove the locked style.");
				else
					result.satisfiedRequirements.push_back("Required output verified: art_style_lock");
			}
			else if (outName == "playable_prototype" || outName == "vertical_slice_build")
			{
				ensureFacts();
				ensureScripts();

				if (!facts.ok || facts.levelCount == 0)
					block("Required output '" + outName + "' missing: no loadable .wlevel level package under assets/.");
				for (const auto& error : facts.errors)
					block("Required output '" + outName + "' level error: " + error);

				if (facts.entities == 0)
					block("Required output '" + outName + "' missing: scene has 0 entities.");
				if (facts.cameras == 0)
					block("Required output '" + outName + "' missing: no CameraComponent (viewport would render through nothing).");
				if (facts.meshes > 0 && facts.lights == 0)
					block("Black screen hazard: " + std::to_string(facts.meshes) + " mesh entities but no light source (DirectionalLight/PointLight/AmbientLight).");
				if (facts.meshes >= 15 && facts.meshesWithAsset == 0 && facts.whiteboxes == 0)
					block("Anti-Stub violation: " + std::to_string(facts.meshes) + " mesh entities reference no .wmodel/.wmat and carry no WhiteboxComponent.");
				if (facts.meshes > 0 && facts.meshesWithAsset == 0 && facts.whiteboxes == 0)
					block("Anti-Stub violation: every mesh entity is an untextured default primitive.");

				if (scripts.files == 0)
					block("Required output '" + outName + "' missing: no .luau script under assets/scripts/.");
				else if (scripts.wired == 0)
					block("Required output '" + outName + "' missing: no script registers a system or lifecycle function (AddSystem/OnUpdate/OnBeginPlay).");

				if (facts.bodies == 0)
					block("Required output '" + outName + "' missing: no rigid body or collider; movement would be scripted without physics.");
				if (facts.health + facts.behaviors + facts.smartObjects == 0)
					block("Game Challenge violation: no entity carries HealthComponent, BehaviorTreeComponent or SmartObjectComponent (a name is not a threat).");
				if (facts.audioResolved == 0)
					block("Audio Immersion violation: no AudioSourceComponent resolves to an existing sound asset under assets/.");

				if (!playtestPassed)
					block("Required output '" + outName + "' missing: playtest.run did not pass on the start scene.");

				if (outName == "vertical_slice_build")
				{
					if (facts.meshesWithAsset == 0)
						block("Required output 'vertical_slice_build' missing: no entity references a real .wmodel/.wmat.");
					if (facts.emptyMeshEntities > 0)
						block("Anti-Stub violation: " + std::to_string(facts.emptyMeshEntities) + " mesh entities still render default primitives in a vertical slice.");
					if (scripts.stateMachines == 0)
						block("Finished Game Loop violation: no script defines a state machine with 3+ states and a restart/retry path.");
				}

				if (result.blockers.empty())
					result.satisfiedRequirements.push_back("Required output verified: " + outName);
			}
			else if (outName == "all_levels")
			{
				ensureFacts();
				if (facts.levelCount < 2 || facts.entities < 10)
					block("Required output 'all_levels' missing: need at least 2 loadable levels with 10+ entities total (found " +
						std::to_string(facts.levelCount) + " levels / " + std::to_string(facts.entities) + " entities).");
				else
					result.satisfiedRequirements.push_back("Required output verified: all_levels");
			}
			else if (outName == "full_assets" || outName == "feature_complete")
			{
				ensureFacts();
				if (facts.meshesWithAsset == 0)
					block("Required output '" + outName + "' missing: no entity references a real model/material asset.");
				else
					result.satisfiedRequirements.push_back("Required output verified: " + outName);
			}
			else if (outName == "zero_blocker_report")
			{
				const auto bugs = BacklogManager::Get().ListBugs(root.string());
				for (const auto& b : bugs)
				{
					if ((b.severity == "blocker" || b.severity == "critical") && b.status != "closed" && b.status != "resolved")
						block("Unresolved blocker defect: [" + b.id + "] " + b.title);
				}
				if (!HasArtifact(root, "qa_report") && !HasArtifact(root, "zero_blocker_report"))
					block("Required output 'zero_blocker_report' missing: no .we/studio/reports/qa_report.* artifact.");
				else
					result.satisfiedRequirements.push_back("Required output verified: zero_blocker_report");
			}
			else if (outName == "shipping_package")
			{
				bool shipped = false;
				std::error_code ec;
				const fs::path dist = root / "dist";
				if (fs::exists(dist, ec))
				{
					for (const auto& entry : fs::recursive_directory_iterator(dist, ec))
					{
						if (entry.is_regular_file() && entry.path().filename() == "Runtime.exe")
						{
							shipped = true;
							break;
						}
					}
				}
				if (!shipped)
					block("Required output 'shipping_package' missing: no dist/**/Runtime.exe bundle.");
				else
					result.satisfiedRequirements.push_back("Required output verified: shipping_package");
			}
			else if (HasArtifact(root, outName))
			{
				result.satisfiedRequirements.push_back("Required output verified by artifact: " + outName);
			}
			else
			{
				block("Required output '" + outName + "' has no verifier and no artifact under .we/studio/{artifacts,reports}/.");
			}
		}

		// ---- 3. 当前阶段未闭环任务 ----
		auto tasks = BacklogManager::Get().ListTasks(root.string());
		for (const auto& t : tasks)
		{
			if ((t.stage == std::to_string(sIdx) || t.stage == stageDef->id) && (t.status == "open" || t.status == "in_progress"))
				block("Active unfinished task: [" + t.id + "] " + t.title + " (status: " + t.status + ")");
		}

		// ---- 4. 未关闭的 blocker 缺陷 ----
		auto bugs = BacklogManager::Get().ListBugs(root.string(), "blocker");
		for (const auto& b : bugs)
		{
			if (b.status != "closed" && b.status != "resolved")
				block("Unresolved blocker defect: [" + b.id + "] " + b.title);
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


