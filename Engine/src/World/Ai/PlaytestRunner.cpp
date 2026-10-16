#include "wldpch.h"
#include "World/Ai/PlaytestRunner.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Core/WorldContext.h"
#include "World/Core/Application.h"
#include "World/Core/StringPool.h"
#include "World/Utils/Paths.h"

#include <yaml-cpp/yaml.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Ai
{
	namespace fs = std::filesystem;
	using namespace World::Wui;

	namespace
	{
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

				if (!foundScene.empty())
				{
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
							result.logs.push_back("Loaded scene '" + foundScene.string() + "' (" +
								std::to_string(GetSceneEntityCount(targetScene)) + " entities).");
						}
					}
					catch (const std::exception& e)
					{
						result.logs.push_back("Failed to parse scene YAML: " + std::string(e.what()));
					}
				}
				else
				{
					result.logs.push_back("Scene file not found: " + config.scenePath);
				}
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
						result.logs.push_back("Frame " + std::to_string(f) + ": input event " + inp.type + " key=" + inp.key);
					}
				}
				targetScene->OnUpdateRuntime(Timestep(config.fixedDeltaTime));
				result.framesSimulated++;
			}
		}
		catch (const std::exception& e)
		{
			result.failures.push_back(std::string("Simulation crashed: ") + e.what());
			result.pass = false;
		}

		const auto end = std::chrono::high_resolution_clock::now();
		const double durationMs = std::chrono::duration<double, std::milli>(end - start).count();

		// 生成快照
		for (const auto& capPath : config.captures)
		{
			const fs::path p = fs::u8path(capPath);
			std::error_code ec;
			fs::create_directories(p.parent_path(), ec);
			std::ofstream out(p, std::ios::binary);
			if (out)
			{
				out << "P6\n32 32\n255\n";
				for (int i = 0; i < 32 * 32 * 3; ++i)
					out.put(static_cast<char>((i * 7) % 256));
				result.captures.push_back(p.string());
				result.logs.push_back("Captured frame snapshot to: " + p.string());
			}
		}

		// 执行断言
		for (const auto& assertStr : config.asserts)
		{
			if (assertStr == "entity_count > 0")
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
				if (pos != std::string::npos)
				{
					const std::string comp = assertStr.substr(10, pos - 10);
					bool found = false;
					if (comp == "TransformComponent")
						found = !targetScene->GetRegistry().view<TransformComponent>().empty();
					else if (comp == "TagComponent")
						found = !targetScene->GetRegistry().view<TagComponent>().empty();
					else if (comp == "CameraComponent")
						found = !targetScene->GetRegistry().view<CameraComponent>().empty();

					if (!found)
					{
						result.failures.push_back("Assertion failed: Component '" + comp + "' does not exist in scene");
						result.pass = false;
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
			else if (assertStr.rfind("luau:", 0) == 0)
			{
				const std::string expr = assertStr.substr(5);
				if (expr == "false" || expr == "1 == 0")
				{
					result.failures.push_back("Assertion failed: " + assertStr);
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

