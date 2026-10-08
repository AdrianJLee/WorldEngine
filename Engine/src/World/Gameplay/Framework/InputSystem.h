#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputMap.h"
#include "World/Scene/Scene.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>

namespace World::Gameplay
{
	// WP5:输入采样服务(帧首采样一次 -> 快照 -> PreFixed/Fixed 只消费)。
	class WLD_API InputSystem
	{
	public:
		// 帧首采样:把 InputService 的动作/轴 + 平台鼠标位置折算成 Scene::InputSnapshot。
		static const Scene::InputSnapshot& Sample(Scene& scene, const InputService& service,
			glm::vec2 mousePosition = glm::vec2(0.0f), glm::vec2 scrollDelta = glm::vec2(0.0f));

		// 丢弃某场景的边沿/鼠标缓存并清空快照
		static void Forget(Scene& scene);

		// ---- 模拟侧只读查询 (同时支持 NameId 与 string) ----
		static bool IsDown(const Scene& scene, NameId action);
		static bool IsDown(const Scene& scene, const std::string& action);
		static bool WasPressed(const Scene& scene, NameId action);
		static bool WasPressed(const Scene& scene, const std::string& action);
		static bool WasReleased(const Scene& scene, NameId action);
		static bool WasReleased(const Scene& scene, const std::string& action);
		static float GetAxis(const Scene& scene, NameId axis);
		static float GetAxis(const Scene& scene, const std::string& axis);

		// 可观测性
		static uint64_t GetLastSampledFrame(const Scene& scene);
		static uint64_t GetSampleCount(const Scene& scene);
	};
}
