#pragma once

#include "World/Core/Export.h"
#include <entt.hpp>
#include <cstdint>

namespace World
{
	struct CameraComponent;

	// 纯 ECS 相机系统:
	// 负责视口大小同步、投影矩阵计算以及主相机解析。
	class WLD_API CameraSystem
	{
	public:
		static void SetViewportSize(CameraComponent& camera, uint32_t width, uint32_t height);
		static void UpdateProjection(CameraComponent& camera);
		static void UpdateAllCameras(entt::registry& registry, uint32_t viewportWidth, uint32_t viewportHeight);
		static entt::entity FindPrimaryCameraEntity(const entt::registry& registry);
	};
}
