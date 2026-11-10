#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <glm/glm.hpp>

namespace World
{
	namespace VirtualCameraFlags
	{
		constexpr uint16_t Active  = 1 << 0;
		constexpr uint16_t Confine = 1 << 1;
		constexpr uint16_t LockZ   = 1 << 2;
	}

	class WLD_API VirtualCameraSystem
	{
	public:
		// 每帧更新: 遍历 VirtualCameraComponent, 寻找最高优先级活动虚拟相机
		// 计算阻尼平滑跟踪、屏幕死区限制、速度前瞻、Perlin 震屏偏移及边界限制盒, 并驱动主摄像机 Transform
		static void Update(Scene& scene, float dt);

		// 向虚拟相机施加屏幕震动 Trauma [0, 1]
		static void AddTrauma(VirtualCameraComponent& vcam, float trauma);

		// 阻尼平滑算法 (Critically Damped Spring)
		static glm::vec3 SmoothDamp(const glm::vec3& current, const glm::vec3& target,
		                            glm::vec3& currentVelocity, float smoothTime, float dt);

		// 计算震屏偏移 (基于时间和外伤值 Trauma)
		static glm::vec3 EvaluateShakeOffset(const VirtualCameraComponent& vcam, float time);

		// 视口死区判定: 若目标相对相机中心的屏幕归一化偏移在 deadZone 范围内, 保持相机静止
		static glm::vec2 ApplyDeadZone(const glm::vec2& currentOffset, const glm::vec2& deadZoneHalfExtents);
	};
}
