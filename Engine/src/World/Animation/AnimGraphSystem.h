#pragma once
#include "World/Core/Export.h"
#include <entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <utility>
#include <vector>

namespace World
{
	class Scene;

	struct WLD_API AnimGraphRuntimeInfo
	{
		bool Loaded = false;
		std::string AssetPath;
		std::string CurrentState;
		std::string TargetState;
		std::string Error;
		float StateTime = 0.0f;
		float TransitionProgress = 0.0f;
		bool InTransition = false;
		float Speed = 0.0f;
		float Direction = 0.0f;
		bool Grounded = true;
		std::vector<std::pair<std::string, float>> Parameters;
	};

	class WLD_API AnimGraphSystem
	{
	public:
		static void Update(Scene& scene, float deltaSeconds);
		static const std::vector<glm::mat4>* GetPalette(entt::entity entity);
		static bool SetParameter(entt::entity entity, const std::string& name, float value);
		static float GetParameter(entt::entity entity, const std::string& name, float fallback = 0.0f);
		static bool GetRuntimeInfo(entt::entity entity, AnimGraphRuntimeInfo& out);
		static bool ReloadAsset(entt::entity entity);
		static void ClearCache();
	};
}
