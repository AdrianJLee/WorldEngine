#include "wldpch.h"
#include "World/Ai/PlaytestRunner.h"
#include "World/Schema/Schema.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Core/WorldContext.h"
#include "World/Core/Application.h"
#include "World/Core/StringPool.h"
#include "World/Utils/Paths.h"
#include "World/Core/Input.h"
#include "World/Core/KeyCodes.h"
#include "World/Events/KeyEvent.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Schema/SchemaRegistry.h"
#include <algorithm>
#include <cctype>

#include <yaml-cpp/yaml.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Ai
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	namespace
	{
		bool ValueToDouble(const Schema::Value& val, double& outVal)
		{
			if (auto* f = std::get_if<float>(&val)) { outVal = static_cast<double>(*f); return true; }
			if (auto* d = std::get_if<double>(&val)) { outVal = *d; return true; }
			if (auto* i = std::get_if<int32_t>(&val)) { outVal = static_cast<double>(*i); return true; }
			if (auto* u = std::get_if<uint32_t>(&val)) { outVal = static_cast<double>(*u); return true; }
			if (auto* i64 = std::get_if<int64_t>(&val)) { outVal = static_cast<double>(*i64); return true; }
			if (auto* u64 = std::get_if<uint64_t>(&val)) { outVal = static_cast<double>(*u64); return true; }
			if (auto* i16 = std::get_if<int16_t>(&val)) { outVal = static_cast<double>(*i16); return true; }
			if (auto* u16 = std::get_if<uint16_t>(&val)) { outVal = static_cast<double>(*u16); return true; }
			if (auto* i8 = std::get_if<int8_t>(&val)) { outVal = static_cast<double>(*i8); return true; }
			if (auto* u8 = std::get_if<uint8_t>(&val)) { outVal = static_cast<double>(*u8); return true; }
			if (auto* b = std::get_if<bool>(&val)) { outVal = *b ? 1.0 : 0.0; return true; }
			return false;
		}

		bool CompareNumbers(double actual, const std::string& op, double expected)
		{
			if (op == "<") return actual < expected;
			if (op == "<=") return actual <= expected;
			if (op == ">") return actual > expected;
			if (op == ">=") return actual >= expected;
			if (op == "==") return std::abs(actual - expected) < 1e-4;
			if (op == "!=") return std::abs(actual - expected) >= 1e-4;
			return false;
		}

		bool EvaluateFieldAssertion(Scene* scene, const std::string& assertExpr, std::string& outFailure)
		{
			if (!scene)
			{
				outFailure = "Assertion failed: null scene";
				return true;
			}

			std::string str = assertExpr;
			if (str.rfind("assert:", 0) == 0)
				str = str.substr(7);

			static const std::vector<std::string> ops = { "<=", ">=", "==", "!=", "<", ">" };
			std::string foundOp;
			size_t opPos = std::string::npos;
			for (const auto& op : ops)
			{
				const size_t pos = str.find(op);
				if (pos != std::string::npos)
				{
					foundOp = op;
					opPos = pos;
					break;
				}
			}

			if (opPos == std::string::npos)
				return false;

			std::string left = str.substr(0, opPos);
			std::string right = str.substr(opPos + foundOp.size());
			auto trim = [](std::string& s) {
				while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
					s.erase(s.begin());
				while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
					s.pop_back();
			};
			trim(left);
			trim(right);

			double expectedVal = 0.0;
			try {
				expectedVal = std::stod(right);
			} catch (...) {
				outFailure = "Assertion syntax error in '" + assertExpr + "': right-hand side is not a valid number";
				return true;
			}

			std::vector<std::string> parts;
			{
				std::stringstream ss(left);
				std::string token;
				while (std::getline(ss, token, '.'))
				{
					trim(token);
					if (!token.empty())
						parts.push_back(token);
				}
			}

			if (parts.size() < 2)
			{
				outFailure = "Assertion syntax error in '" + assertExpr + "': expected '[entity.]Component.Field'";
				return true;
			}

			std::string entityName;
			std::string compName;
			std::string fieldName;
			std::string subFieldName;

			if (parts.size() == 2)
			{
				compName = parts[0];
				fieldName = parts[1];
			}
			else if (parts.size() == 3)
			{
				bool isEntity = false;
				for (auto [e, tag] : scene->GetRegistry().view<TagComponent>().each())
				{
					if (tag.Tag == StringPool::Get().InternName(parts[0]))
					{
						isEntity = true;
						break;
					}
				}
				if (isEntity)
				{
					entityName = parts[0];
					compName = parts[1];
					fieldName = parts[2];
				}
				else
				{
					compName = parts[0];
					fieldName = parts[1];
					subFieldName = parts[2];
				}
			}
			else if (parts.size() >= 4)
			{
				entityName = parts[0];
				compName = parts[1];
				fieldName = parts[2];
				subFieldName = parts[3];
			}

			const Schema::TypeSchema* schema = scene->GetContext().Schemas().Find(compName);
			if (!schema)
			{
				for (const auto* candidate : scene->GetContext().Schemas().List(Schema::TypeCategory::Component))
				{
					if (!candidate) continue;
					const size_t sep = candidate->Id.Name.rfind("::");
					const std::string shortName = (sep == std::string::npos) ? candidate->Id.Name : candidate->Id.Name.substr(sep + 2);
					if (shortName == compName || candidate->DisplayName == compName)
					{
						schema = candidate;
						break;
					}
				}
			}

			if (!schema || !schema->Storage)
			{
				outFailure = "Assertion failed: component schema '" + compName + "' not found in scene context";
				return true;
			}

			const Schema::FieldSchema* fieldSchema = nullptr;
			for (const auto& f : schema->Fields)
			{
				if (f.Name == fieldName)
				{
					fieldSchema = &f;
					break;
				}
			}
			if (!fieldSchema || !fieldSchema->Get)
			{
				outFailure = "Assertion failed: field '" + fieldName + "' not found on component '" + compName + "'";
				return true;
			}

			entt::entity targetHandle = entt::null;
			auto& registry = scene->GetRegistry();

			if (!entityName.empty())
			{
				for (auto [e, tag] : registry.view<TagComponent>().each())
				{
					if (tag.Tag == StringPool::Get().InternName(entityName))
					{
						targetHandle = e;
						break;
					}
				}
				if (targetHandle == entt::null)
				{
					outFailure = "Assertion failed: entity '" + entityName + "' not found for assertion '" + assertExpr + "'";
					return true;
				}
			}
			else
			{
				auto* storage = registry.storage(schema->Storage->ComponentId);
				if (storage && !storage->empty())
				{
					targetHandle = storage->operator[](0);
				}
				if (targetHandle == entt::null)
				{
					outFailure = "Assertion failed: no entity in scene carries component '" + compName + "'";
					return true;
				}
			}

			Entity ent(scene, targetHandle);
			if (!ent.HasComponent(schema->Storage->ComponentId))
			{
				outFailure = "Assertion failed: entity does not carry component '" + compName + "'";
				return true;
			}

			const void* compPtr = ent.GetComponent(schema->Storage->ComponentId);
			if (!compPtr)
			{
				outFailure = "Assertion failed: null component pointer for '" + compName + "'";
				return true;
			}

			Schema::Value val = fieldSchema->Get(compPtr);
			double actualVal = 0.0;
			bool valOk = false;

			if (!subFieldName.empty())
			{
				if (auto* v3 = std::get_if<glm::vec3>(&val))
				{
					if (subFieldName == "x") { actualVal = v3->x; valOk = true; }
					else if (subFieldName == "y") { actualVal = v3->y; valOk = true; }
					else if (subFieldName == "z") { actualVal = v3->z; valOk = true; }
				}
				else if (auto* v2 = std::get_if<glm::vec2>(&val))
				{
					if (subFieldName == "x") { actualVal = v2->x; valOk = true; }
					else if (subFieldName == "y") { actualVal = v2->y; valOk = true; }
				}
				else if (auto* v4 = std::get_if<glm::vec4>(&val))
				{
					if (subFieldName == "x") { actualVal = v4->x; valOk = true; }
					else if (subFieldName == "y") { actualVal = v4->y; valOk = true; }
					else if (subFieldName == "z") { actualVal = v4->z; valOk = true; }
					else if (subFieldName == "w") { actualVal = v4->w; valOk = true; }
				}
			}
			else
			{
				valOk = ValueToDouble(val, actualVal);
			}

			if (!valOk)
			{
				outFailure = "Assertion failed: could not extract numeric value for '" + left + "'";
				return true;
			}

			if (!CompareNumbers(actualVal, foundOp, expectedVal))
			{
				outFailure = "Assertion failed: " + assertExpr + " (actual: " + std::to_string(actualVal) + ")";
			}
			return true;
		}

		size_t GetSceneEntityCount(Scene* scene)
		{
			if (!scene)
				return 0;
			return scene->GetRegistry().view<TagComponent>().size();
		}
	}

	std::string PlaytestResult::ToJson() const
	{
		JsonValue root;
		root.type = JsonValue::Type::Object;

		root.Object.push_back({ "pass", JsonValue::MakeBool(pass) });
		root.Object.push_back({ "frames_simulated", JsonValue::MakeNumber(static_cast<double>(framesSimulated)) });

		// 失败项
		JsonValue failArr;
		failArr.type = JsonValue::Type::Array;
		for (const auto& f : failures)
		{
			failArr.Array.push_back(JsonValue::MakeString(f));
		}
		root.Object.push_back({ "failures", std::move(failArr) });

		// 日志
		JsonValue logArr;
		logArr.type = JsonValue::Type::Array;
		for (const auto& l : logs)
		{
			logArr.Array.push_back(JsonValue::MakeString(l));
		}
		root.Object.push_back({ "logs", std::move(logArr) });

		// 截图/快照
		JsonValue capArr;
		capArr.type = JsonValue::Type::Array;
		for (const auto& c : captures)
		{
			capArr.Array.push_back(JsonValue::MakeString(c));
		}
		root.Object.push_back({ "captures", std::move(capArr) });

		// 性能字典
		JsonValue perfObj;
		perfObj.type = JsonValue::Type::Object;
		for (const auto& kv : perf)
		{
			perfObj.Object.push_back({ kv.first, JsonValue::MakeNumber(kv.second) });
		}
		root.Object.push_back({ "perf", std::move(perfObj) });

		return root.Dump();
	}

	PlaytestConfig PlaytestRunner::ParseConfig(const std::map<std::string, std::string>& args)
	{
		PlaytestConfig config;
		if (args.count("scene"))
			config.scenePath = args.at("scene");

		if (args.count("frames"))
		{
			try { config.frames = std::stoi(args.at("frames")); }
			catch (...) { config.frames = 60; }
		}

		if (args.count("dt"))
		{
			try { config.fixedDeltaTime = std::stof(args.at("dt")); }
			catch (...) { config.fixedDeltaTime = 1.0f / 60.0f; }
		}

		if (args.count("captures"))
		{
			const std::string text = args.at("captures");
			std::string err;
			auto parsed = JsonValue::Parse(text, &err);
			if (parsed && parsed->type == JsonValue::Type::Array)
			{
				for (const auto& item : parsed->Array)
					if (item.type == JsonValue::Type::String)
						config.captures.push_back(item.AsString());
			}
			else
			{
				std::stringstream ss(text);
				std::string item;
				while (std::getline(ss, item, ';'))
					if (!item.empty()) config.captures.push_back(item);
			}
		}

		if (args.count("inputs"))
		{
			const std::string text = args.at("inputs");
			std::string err;
			auto parsed = JsonValue::Parse(text, &err);
			if (parsed && parsed->type == JsonValue::Type::Array)
			{
				for (const auto& item : parsed->Array)
				{
					if (item.type == JsonValue::Type::Object)
					{
						PlaytestInputEvent ev;
						if (const auto* f = item.Find("frame")) ev.frame = static_cast<int>(f->AsNumber());
						if (const auto* t = item.Find("type")) ev.type = t->AsString();
						if (const auto* k = item.Find("key")) ev.key = k->AsString();
						if (const auto* x = item.Find("x")) ev.x = static_cast<float>(x->AsNumber());
						if (const auto* y = item.Find("y")) ev.y = static_cast<float>(y->AsNumber());
						config.inputs.push_back(std::move(ev));
					}
				}
			}
			else
			{
				std::stringstream ss(text);
				std::string token;
				while (std::getline(ss, token, ';'))
				{
					const size_t col = token.find(':');
					if (col != std::string::npos)
					{
						PlaytestInputEvent ev;
						ev.key = token.substr(0, col);
						try { ev.frame = std::stoi(token.substr(col + 1)); } catch (...) { ev.frame = 0; }
						ev.type = "key_down";
						config.inputs.push_back(std::move(ev));
					}
				}
			}
		}

		if (args.count("asserts"))
		{
			const std::string text = args.at("asserts");
			std::string err;
			auto parsed = JsonValue::Parse(text, &err);
			if (parsed && parsed->type == JsonValue::Type::Array)
			{
				for (const auto& item : parsed->Array)
				{
					if (item.type == JsonValue::Type::String)
						config.asserts.push_back(item.AsString());
				}
			}
			else
			{
				std::stringstream ss(text);
				std::string item;
				while (std::getline(ss, item, ';'))
				{
					if (!item.empty())
						config.asserts.push_back(item);
				}
			}
		}

		return config;
	}

	PlaytestResult PlaytestRunner::Run(const PlaytestConfig& config, Scene* activeScene)
	{
		PlaytestResult result;
		result.framesSimulated = 0;

		std::unique_ptr<WorldContext> localContext;
		std::unique_ptr<Scene> localScene;
		Ref<Scene> packagedScene;
		Scene* targetScene = activeScene;

		if (!targetScene)
		{
			if (Application::HasInstance())
			{
				localScene = std::make_unique<Scene>(Application::Get().GetContext());
			}
			else
			{
				localContext = std::make_unique<WorldContext>();
				localScene = std::make_unique<Scene>(*localContext);
			}
			targetScene = localScene.get();

			if (!config.scenePath.empty())
			{
				std::vector<fs::path> candidates = {
					fs::u8path(config.scenePath),
					Paths::ProjectDir() / fs::u8path(config.scenePath),
					Paths::ProjectDir() / "assets" / fs::u8path(config.scenePath),
					fs::current_path() / fs::u8path(config.scenePath)
				};
				fs::path foundScene;
				for (const auto& p : candidates)
				{
					if (fs::exists(p) && fs::is_regular_file(p))
					{
						foundScene = p;
						break;
					}
				}

				if (foundScene.empty())
				{
					result.logs.push_back("Scene file not found: " + config.scenePath);
					result.failures.push_back("playtest: scene file not found: " + config.scenePath);
					result.pass = false;
				}
				else if (foundScene.extension() == ".wlevel")
				{
					// 真实关卡包:逐实体反序列化**全部组件**(MeshRenderer/Camera/Light/RigidBody/Script…),
					// 不是只读 Tag+Transform 的假加载。
					WorldContext& context = localContext ? *localContext : Application::Get().GetContext();
					Level::LevelPackage package;
					std::string loadError;
					if (!package.Load(foundScene, &loadError))
					{
						result.logs.push_back("Failed to load level package: " + loadError);
						result.failures.push_back("playtest: level package load failed: " + loadError);
						result.pass = false;
					}
					else
					{
						packagedScene = package.Instantiate(context);
						if (!packagedScene)
						{
							result.failures.push_back("playtest: level package instantiate failed");
							result.pass = false;
						}
						else
						{
							targetScene = packagedScene.get();
							result.logs.push_back("Loaded level package '" + foundScene.string() + "' (" +
								std::to_string(GetSceneEntityCount(targetScene)) + " entities, all components).");
						}
					}
				}
				else
				{
					// 兼容历史测试用的 Entities YAML(只有 Tag/Transform)。真实工程必须走 .wlevel。
					try
					{
						YAML::Node sDoc = YAML::LoadFile(foundScene.string());
						if (sDoc["Entities"] && sDoc["Entities"].IsSequence())
						{
							for (const auto& entityNode : sDoc["Entities"])
							{
								std::string name = "Entity";
								if (entityNode["TagComponent"] && entityNode["TagComponent"]["Tag"])
									name = entityNode["TagComponent"]["Tag"].as<std::string>();
								else if (entityNode["Tag"])
									name = entityNode["Tag"].as<std::string>();

								Entity ent = targetScene->CreateEntityShell(name);
								ent.AddComponent<TransformComponent>();
								if (entityNode["TransformComponent"])
								{
									auto& tc = ent.GetComponent<TransformComponent>();
									const auto& tNode = entityNode["TransformComponent"];
									if (tNode["Location"] && tNode["Location"].IsSequence())
									{
										tc.Location.x = tNode["Location"][0].as<float>();
										tc.Location.y = tNode["Location"][1].as<float>();
										tc.Location.z = tNode["Location"][2].as<float>();
									}
									else if (tNode["Translation"] && tNode["Translation"].IsSequence())
									{
										tc.Location.x = tNode["Translation"][0].as<float>();
										tc.Location.y = tNode["Translation"][1].as<float>();
										tc.Location.z = tNode["Translation"][2].as<float>();
									}
								}
							}
							result.logs.push_back("Loaded legacy Entities YAML '" + foundScene.string() + "' (" +
								std::to_string(GetSceneEntityCount(targetScene)) + " entities; Tag+Transform only).");
						}
					}
					catch (const std::exception& e)
					{
						result.logs.push_back("Failed to parse scene YAML: " + std::string(e.what()));
						result.failures.push_back("playtest: scene YAML parse failed: " + std::string(e.what()));
						result.pass = false;
					}
				}
			}
		}

		Input::ClearSimulatedKeys();
		ScriptEngine::ClearScriptErrors();

		// [物理硬门禁] 探测受控玩家实体与 2D/3D 模式，记录初始空间位置
		Entity playerEntity;
		glm::vec3 playerStartPos { 0.0f };
		bool playerFound = false;
		bool is2DScene = false;

		auto toLowerStr = [](std::string s) {
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		};

		// 检查正交相机或 2D 场景标志
		for (auto handle : targetScene->GetRegistry().view<CameraComponent>())
		{
			const auto& cam = targetScene->GetRegistry().get<CameraComponent>(handle);
			if (cam.Primary && cam.Camera.m_ProjectionType == CameraSettings::ProjectionType::Orthographic)
			{
				is2DScene = true;
				break;
			}
		}
		if (!is2DScene)
		{
			const auto& reg = targetScene->GetRegistry();
			if (!reg.view<SpriteComponent>().empty() && reg.view<MeshRendererComponent>().empty())
				is2DScene = true;
		}

		// 寻找玩家受控角色实体
		for (auto handle : targetScene->GetRegistry().view<TagComponent, TransformComponent>())
		{
			const auto& tagComp = targetScene->GetRegistry().get<TagComponent>(handle);
			const std::string tagName = toLowerStr(StringPool::Get().NameOf(tagComp.Tag));
			const bool hasListener = targetScene->GetRegistry().all_of<AudioListenerComponent>(handle);
			if (hasListener || tagName.find("player") != std::string::npos ||
				tagName.find("char_") != std::string::npos || tagName.find("hero") != std::string::npos)
			{
				playerEntity = Entity(targetScene, handle);
				playerStartPos = targetScene->GetRegistry().get<TransformComponent>(handle).Location;
				playerFound = true;
				break;
			}
		}

		// 统计移动按键帧数
		int movementInputFrames = 0;
		for (const auto& inp : config.inputs)
		{
			std::string k = toLowerStr(inp.key);
			if (k == "w" || k == "a" || k == "s" || k == "d" ||
				k == "up" || k == "down" || k == "left" || k == "right" ||
				k == "moveleft" || k == "moveright" || k == "moveup" || k == "movedown")
			{
				if (inp.type == "key_down" || inp.type == "press" || inp.type == "keydown")
					movementInputFrames++;
			}
		}

		const auto start = std::chrono::high_resolution_clock::now();

		try
		{
			for (int f = 0; f < config.frames; ++f)
			{
				for (const auto& inp : config.inputs)
				{
					if (inp.frame == f)
					{
						int keycode = -1;
						try {
							if (!inp.key.empty() && std::all_of(inp.key.begin(), inp.key.end(), ::isdigit))
								keycode = std::stoi(inp.key);
						} catch (...) {}
						if (keycode < 0 && !inp.key.empty())
						{
							std::string upperKey = inp.key;
							for (char& c : upperKey) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
							if (upperKey == "SPACE") keycode = KeyCodes::Space;
							else if (upperKey == "TAB") keycode = KeyCodes::Tab;
							else if (upperKey == "ENTER" || upperKey == "RETURN") keycode = KeyCodes::Enter;
							else if (upperKey == "ESCAPE" || upperKey == "ESC") keycode = KeyCodes::Escape;
							else if (upperKey == "LEFT") keycode = KeyCodes::Left;
							else if (upperKey == "RIGHT") keycode = KeyCodes::Right;
							else if (upperKey == "UP") keycode = KeyCodes::Up;
							else if (upperKey == "DOWN") keycode = KeyCodes::Down;
							else if (upperKey.size() == 1 && upperKey[0] >= 'A' && upperKey[0] <= 'Z')
								keycode = KeyCodes::A + (upperKey[0] - 'A');
							else if (upperKey.size() == 1 && upperKey[0] >= '0' && upperKey[0] <= '9')
								keycode = KeyCodes::D0 + (upperKey[0] - '0');
						}

						const bool isDown = (inp.type == "key_down" || inp.type == "press" || inp.type == "keydown");
						const bool isUp = (inp.type == "key_up" || inp.type == "release" || inp.type == "keyup");

						if (isDown && keycode >= 0)
						{
							Input::SetSimulatedKeyState(keycode, true);
							if (Gameplay::GameApp* app = Gameplay::GameApp::TryGet())
								app->Events().Emit(KeyPressedEvent(keycode, 0));
						}
						else if (isUp && keycode >= 0)
						{
							Input::SetSimulatedKeyState(keycode, false);
							if (Gameplay::GameApp* app = Gameplay::GameApp::TryGet())
								app->Events().Emit(KeyReleasedEvent(keycode));
						}
						result.logs.push_back("Frame " + std::to_string(f) + ": input event " + inp.type + " key=" + inp.key + " (keycode=" + std::to_string(keycode) + ")");
					}
				}
				targetScene->OnUpdateRuntime(Timestep(config.fixedDeltaTime));
				Input::UpdateFrameInputState();
				result.framesSimulated++;

				// 脚本异常硬性拦截：推帧期间若探测到任何脚本解析失败或未捕获异常，立即终止推帧并判负
				if (ScriptEngine::GetScriptErrorCount() > 0)
				{
					for (const auto& err : ScriptEngine::GetRecentScriptErrors())
					{
						result.failures.push_back("Script error at frame " + std::to_string(f) + ": " + err);
					}
					result.pass = false;
					break;
				}
			}
		}
		catch (const std::exception& e)
		{
			result.failures.push_back(std::string("Simulation crashed: ") + e.what());
			result.pass = false;
		}

		Input::ClearSimulatedKeys();

		// 全量推帧结束后再次确认脚本错误
		if (ScriptEngine::GetScriptErrorCount() > 0)
		{
			for (const auto& err : ScriptEngine::GetRecentScriptErrors())
			{
				bool alreadyReported = false;
				for (const auto& existing : result.failures)
				{
					if (existing.find(err) != std::string::npos)
					{
						alreadyReported = true;
						break;
					}
				}
				if (!alreadyReported)
					result.failures.push_back("Script error: " + err);
			}
			result.pass = false;
		}

		const auto end = std::chrono::high_resolution_clock::now();
		const double durationMs = std::chrono::duration<double, std::milli>(end - start).count();

		// 快照:headless 没有渲染面,绝不伪造像素。
		// 真实画面证据走编辑器 AI 通道的 capture.scene / capture.screen(WeMcp 会自动拉起 Editor)。
		for (const auto& capPath : config.captures)
			result.logs.push_back("capture skipped: '" + capPath + "' (headless playtest has no render surface; use capture.scene via Editor MCP)");

		// 真实场景报告:让模型与门禁都能核对"场景里到底有什么"。
		{
			const auto& registry = targetScene->GetRegistry();
			size_t cameras = 0, lights = 0, meshes = 0, meshWithAsset = 0, bodies = 0, audioSources = 0, healthEntities = 0;
			glm::vec3 bboxMin { 0.0f }, bboxMax { 0.0f };
			bool hasBounds = false;
			for (auto handle : registry.view<TagComponent>())
			{
				if (registry.all_of<CameraComponent>(handle)) ++cameras;
				if (registry.all_of<DirectionalLightComponent>(handle) || registry.all_of<PointLightComponent>(handle) ||
					registry.all_of<AmbientLightComponent>(handle)) ++lights;
				if (registry.all_of<RigidBody2DComponent>(handle) || registry.all_of<RigidBody3DComponent>(handle) ||
					registry.all_of<BoxCollider2DComponent>(handle) || registry.all_of<BoxCollider3DComponent>(handle)) ++bodies;
				if (registry.all_of<AudioSourceComponent>(handle)) ++audioSources;
				if (registry.all_of<HealthComponent>(handle)) ++healthEntities;
				if (const auto* mesh = registry.try_get<MeshRendererComponent>(handle))
				{
					++meshes;
					if (!StringPool::Get().PathOf(mesh->Mesh.Path).empty() || !StringPool::Get().PathOf(mesh->Material.Path).empty())
						++meshWithAsset;
				}
				if (const auto* transform = registry.try_get<TransformComponent>(handle))
				{
					const glm::vec3 p = transform->Location;
					if (!hasBounds) { bboxMin = p; bboxMax = p; hasBounds = true; }
					else { bboxMin = glm::min(bboxMin, p); bboxMax = glm::max(bboxMax, p); }
				}
			}

			JsonValue report;
			report.type = JsonValue::Type::Object;
			report.Object.push_back({ "entities", JsonValue::MakeNumber(static_cast<double>(GetSceneEntityCount(targetScene))) });
			report.Object.push_back({ "cameras", JsonValue::MakeNumber(static_cast<double>(cameras)) });
			report.Object.push_back({ "lights", JsonValue::MakeNumber(static_cast<double>(lights)) });
			report.Object.push_back({ "meshes", JsonValue::MakeNumber(static_cast<double>(meshes)) });
			report.Object.push_back({ "meshes_with_asset", JsonValue::MakeNumber(static_cast<double>(meshWithAsset)) });
			report.Object.push_back({ "physics_bodies", JsonValue::MakeNumber(static_cast<double>(bodies)) });
			report.Object.push_back({ "audio_sources", JsonValue::MakeNumber(static_cast<double>(audioSources)) });
			report.Object.push_back({ "health_entities", JsonValue::MakeNumber(static_cast<double>(healthEntities)) });
			if (hasBounds)
			{
				JsonValue minArr; minArr.type = JsonValue::Type::Array;
				minArr.Array.push_back(JsonValue::MakeNumber(bboxMin.x));
				minArr.Array.push_back(JsonValue::MakeNumber(bboxMin.y));
				minArr.Array.push_back(JsonValue::MakeNumber(bboxMin.z));
				JsonValue maxArr; maxArr.type = JsonValue::Type::Array;
				maxArr.Array.push_back(JsonValue::MakeNumber(bboxMax.x));
				maxArr.Array.push_back(JsonValue::MakeNumber(bboxMax.y));
				maxArr.Array.push_back(JsonValue::MakeNumber(bboxMax.z));
				report.Object.push_back({ "bbox_min", std::move(minArr) });
				report.Object.push_back({ "bbox_max", std::move(maxArr) });
			}
			result.logs.push_back("scene_report: " + report.Dump());
		}

		// [物理硬门禁 1] 渲染实体退化零尺寸硬检 (Degenerate zero scale check)
		for (auto handle : targetScene->GetRegistry().view<TagComponent, TransformComponent>())
		{
			const bool isRenderable = targetScene->GetRegistry().any_of<SpriteComponent, MeshRendererComponent>(handle);
			if (!isRenderable)
				continue;

			const auto& tagComp = targetScene->GetRegistry().get<TagComponent>(handle);
			const std::string tagName = toLowerStr(StringPool::Get().NameOf(tagComp.Tag));
			if (tagName.find("marker") != std::string::npos || tagName.find("dummy") != std::string::npos ||
				tagName.find("hidden") != std::string::npos || tagName.find("debug") != std::string::npos)
				continue;

			const auto& tc = targetScene->GetRegistry().get<TransformComponent>(handle);
			if (std::abs(tc.Scale.x) < 1e-4f || std::abs(tc.Scale.y) < 1e-4f || std::abs(tc.Scale.z) < 1e-4f)
			{
				result.failures.push_back("Assertion failed: Renderable entity '" + StringPool::Get().NameOf(tagComp.Tag) +
					"' has degenerate zero scale: [" + std::to_string(tc.Scale.x) + ", " +
					std::to_string(tc.Scale.y) + ", " + std::to_string(tc.Scale.z) + "]. Visual scale must be non-zero.");
				result.pass = false;
			}
		}

		// [物理硬门禁 2] 输入-位移因果因变硬断言 (Input-Kinematics Causality Assertion)
		if (playerFound && movementInputFrames >= 10 && playerEntity.IsValid())
		{
			const glm::vec3 playerEndPos = playerEntity.GetComponent<TransformComponent>().Location;
			const glm::vec3 delta = playerEndPos - playerStartPos;
			const float totalDist = glm::length(delta);
			const float planarDist = std::sqrt(delta.x * delta.x + delta.y * delta.y);
			const float zDist = std::abs(delta.z);

			if (totalDist < 0.05f)
			{
				result.failures.push_back("Assertion failed: Player entity '" + StringPool::Get().NameOf(playerEntity.GetComponent<TagComponent>().Tag) +
					"' remained stationary (delta = " + std::to_string(totalDist) + "m) despite " +
					std::to_string(movementInputFrames) + " active movement input frames. Kinematics or input mapping is broken. " +
					"(HINT: in Luau, assign full Vector3 to transform.Location = MakeVec3(x, y, z); direct assignment to transform.Location.x modifies a temporary value).");
				result.pass = false;
			}
			else if (is2DScene && zDist > 0.05f && planarDist < 0.01f)
			{
				result.failures.push_back("Assertion failed: 2D player entity '" + StringPool::Get().NameOf(playerEntity.GetComponent<TagComponent>().Tag) +
					"' moved along camera depth Z-axis (deltaZ = " + std::to_string(zDist) +
					"m) instead of 2D XY plane. Check movement coordinates in gameplay systems.");
				result.pass = false;
			}
		}

		for (const auto& assertStr : config.asserts)
		{
			if (assertStr == "no_script_errors" || assertStr == "assert:no_script_errors")
			{
				if (ScriptEngine::GetScriptErrorCount() > 0)
				{
					result.failures.push_back("Assertion failed: no_script_errors (" +
						std::to_string(ScriptEngine::GetScriptErrorCount()) + " error(s) detected)");
					result.pass = false;
				}
			}
			else if (assertStr == "entity_count > 0")
			{
				if (GetSceneEntityCount(targetScene) == 0)
				{
					result.failures.push_back("Assertion failed: entity_count > 0 (actual: 0)");
					result.pass = false;
				}
			}
			else if (assertStr.rfind("entity_count >=", 0) == 0)
			{
				int expected = 0;
				try { expected = std::stoi(assertStr.substr(16)); } catch (...) {}
				if (static_cast<int>(GetSceneEntityCount(targetScene)) < expected)
				{
					result.failures.push_back("Assertion failed: " + assertStr + " (actual: " + std::to_string(GetSceneEntityCount(targetScene)) + ")");
					result.pass = false;
				}
			}
			else if (assertStr.rfind("entity_count ==", 0) == 0)
			{
				int expected = 0;
				try { expected = std::stoi(assertStr.substr(16)); } catch (...) {}
				if (static_cast<int>(GetSceneEntityCount(targetScene)) != expected)
				{
					result.failures.push_back("Assertion failed: " + assertStr + " (actual: " + std::to_string(GetSceneEntityCount(targetScene)) + ")");
					result.pass = false;
				}
			}
			else if (assertStr.rfind("entity:", 0) == 0)
			{
				const size_t pos = assertStr.find(" exists");
				if (pos != std::string::npos)
				{
					const std::string name = assertStr.substr(7, pos - 7);
					bool found = false;
					for (auto [e, tag] : targetScene->GetRegistry().view<TagComponent>().each())
					{
						if (tag.Tag == StringPool::Get().InternName(name))
						{
							found = true;
							break;
						}
					}
					if (!found)
					{
						result.failures.push_back("Assertion failed: Entity '" + name + "' does not exist");
						result.pass = false;
					}
				}
			}
			else if (assertStr.rfind("component:", 0) == 0)
			{
				const size_t pos = assertStr.find(" exists");
				if (pos == std::string::npos)
				{
					result.failures.push_back("Assertion failed: " + assertStr + " (expected 'component:<Type> exists')");
					result.pass = false;
				}
				else
				{
					const std::string comp = assertStr.substr(10, pos - 10);
					const Schema::TypeSchema* schema = targetScene->GetContext().Schemas().Find(comp);
					if (!schema)
					{
						for (const auto* candidate : targetScene->GetContext().Schemas().List(Schema::TypeCategory::Component))
						{
							if (!candidate)
								continue;
							const size_t sep = candidate->Id.Name.rfind("::");
							const std::string shortName = sep == std::string::npos ? candidate->Id.Name : candidate->Id.Name.substr(sep + 2);
							if (shortName == comp || candidate->DisplayName == comp)
							{
								schema = candidate;
								break;
							}
						}
					}
					bool found = false;
					if (schema && schema->Storage)
					{
						auto& registry = targetScene->GetRegistry();
						auto* storage = registry.storage(schema->Storage->ComponentId);
						found = storage && !storage->empty();
					}
					if (!found)
					{
						result.failures.push_back("Assertion failed: Component '" + comp + "' does not exist in scene");
						result.pass = false;
					}
				}
			}
			else if (assertStr == "no_nan")
			{
				for (auto entityHandle : targetScene->GetRegistry().view<TransformComponent>())
				{
					const auto& trans = targetScene->GetRegistry().get<TransformComponent>(entityHandle);
					const glm::vec3 p = trans.Location;
					if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
					{
						result.failures.push_back("Assertion failed: no_nan (entity " + std::to_string(static_cast<uint32_t>(entityHandle)) + " has non-finite transform)");
						result.pass = false;
						break;
					}
				}
			}
			else if (assertStr == "no_void_fall")
			{
				for (auto [e, trans, tag] : targetScene->GetRegistry().view<TransformComponent, TagComponent>().each())
				{
					if (trans.Location.y < -35.0f)
					{
						result.failures.push_back("Assertion failed: no_void_fall (Entity '" + StringPool::Get().NameOf(tag.Tag) + "' fell to Y=" + std::to_string(trans.Location.y) + ")");
						result.pass = false;
						break;
					}
				}
			}
			else if (assertStr == "no_crashes")
			{
				// Handled by simulation try-catch
			}
			else if (assertStr == "player_moved" || assertStr == "assert:player_moved")
			{
				if (!playerFound || !playerEntity.IsValid())
				{
					result.failures.push_back("Assertion failed: player_moved (no player entity found in scene)");
					result.pass = false;
				}
				else
				{
					const glm::vec3 playerEndPos = playerEntity.GetComponent<TransformComponent>().Location;
					if (glm::length(playerEndPos - playerStartPos) < 0.05f)
					{
						result.failures.push_back("Assertion failed: player_moved (player entity remained stationary)");
						result.pass = false;
					}
				}
			}
			else if (assertStr == "2d_planar" || assertStr == "assert:2d_planar")
			{
				if (playerFound && playerEntity.IsValid())
				{
					const glm::vec3 playerEndPos = playerEntity.GetComponent<TransformComponent>().Location;
					const glm::vec3 delta = playerEndPos - playerStartPos;
					if (std::abs(delta.z) > 0.05f && std::sqrt(delta.x * delta.x + delta.y * delta.y) < 0.01f)
					{
						result.failures.push_back("Assertion failed: 2d_planar (motion deviated into depth Z-axis)");
						result.pass = false;
					}
				}
			}
			else if (assertStr == "valid_scales" || assertStr == "assert:valid_scales")
			{
				// Handled by default scale check
			}
			else if (assertStr.rfind("luau:", 0) == 0)
			{
				// 以前这里只把字面量 false 判失败，其余静默通过 —— 那是假自证。
				const std::string expr = assertStr.substr(5);
				if (expr != "true")
				{
					result.failures.push_back("Assertion failed: " + assertStr +
						" (luau expressions are not executed by playtest; use entity:/component:/no_void_fall or run Runtime)");
					result.pass = false;
				}
			}
			else
			{
				std::string fieldFailure;
				if (EvaluateFieldAssertion(targetScene, assertStr, fieldFailure))
				{
					if (!fieldFailure.empty())
					{
						result.failures.push_back(fieldFailure);
						result.pass = false;
					}
				}
				else
				{
					result.failures.push_back("Unknown assertion: '" + assertStr + "'");
					result.pass = false;
				}
			}

		}

		const double avgStepMs = result.framesSimulated > 0 ? (durationMs / result.framesSimulated) : 0.0;
		result.perf["duration_ms"] = durationMs;
		result.perf["avg_step_ms"] = avgStepMs;
		result.perf["simulated_fps"] = avgStepMs > 0.0001 ? (1000.0 / avgStepMs) : 60.0;
		result.perf["entity_count"] = static_cast<double>(GetSceneEntityCount(targetScene));

		result.pass = result.failures.empty();
		return result;
	}

	void PlaytestRunner::RegisterCommands(CommandRegistry& reg)
	{
		CommandDescriptor desc;
		desc.name = "playtest.run";
		desc.description = "Run headless/simulated playtest loop with input injection, frame stepping, and assertions evaluation.";
		desc.effect = CommandEffect::Exec;
		desc.scope = CommandScope::Both;
		desc.owner = "engine";
		desc.paramsSchema = "{\"type\":\"object\",\"properties\":{\"scene\":{\"type\":\"string\"},\"frames\":{\"type\":\"integer\"},\"dt\":{\"type\":\"number\"},\"asserts\":{\"type\":\"array\"},\"inputs\":{\"type\":\"array\"},\"captures\":{\"type\":\"array\"}}}";
		desc.resultSchema = "{\"type\":\"object\",\"properties\":{\"pass\":{\"type\":\"boolean\"},\"frames_simulated\":{\"type\":\"integer\"},\"failures\":{\"type\":\"array\"},\"captures\":{\"type\":\"array\"},\"perf\":{\"type\":\"object\"}}}";
		desc.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& /*error*/)
		{
			PlaytestConfig config = ParseConfig(args);
			result = Run(config, nullptr).ToJson();
			return true;
		};
		reg.RegisterCommand(std::move(desc));
	}
}

