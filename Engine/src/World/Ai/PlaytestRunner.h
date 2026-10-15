#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Scene.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace World::Ai
{
	struct WLD_API PlaytestInputEvent
	{
		int frame = 0;
		std::string type; // "key_down", "key_up", "mouse_click", "action"
		std::string key;
		float x = 0.0f;
		float y = 0.0f;
	};

	struct WLD_API PlaytestConfig
	{
		std::string scenePath;
		int frames = 60;
		float fixedDeltaTime = 1.0f / 60.0f;
		std::vector<PlaytestInputEvent> inputs;
		std::vector<std::string> asserts; // 断言列表 (如 "entity_count > 0", "entity:Player exists")
	};

	struct WLD_API PlaytestResult
	{
		bool pass = true;
		int framesSimulated = 0;
		std::vector<std::string> failures;
		std::vector<std::string> logs;
		std::map<std::string, double> perf;

		std::string ToJson() const;
	};

	class WLD_API PlaytestRunner
	{
	public:
		static PlaytestResult Run(const PlaytestConfig& config, Scene* activeScene = nullptr);
		static PlaytestConfig ParseConfig(const std::map<std::string, std::string>& args);
		static void RegisterCommands(class CommandRegistry& reg);
	};
}
