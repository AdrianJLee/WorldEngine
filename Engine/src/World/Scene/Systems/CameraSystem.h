#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/Camera.h"

#include <entt.hpp>
#include <cstdint>
#include <glm/glm.hpp>

namespace World
{
	struct CameraComponent;
	struct CameraSettings;

	// 纯 ECS 相机系统(PECS 数据导向化):
	//  - 参数集中钳制;
	//  - 视口同步 + 投影矩阵按指纹脏标记缓存(参数不变时每帧 0 次重算,修掉
	//    "FixedAspectRatio 相机读档后按默认投影渲染"的隐患);
	//  - 主相机解析。
	class WLD_API CameraSystem
	{
	public:
		// 集中钳制:投影类型合法、FOV ∈ (0,180)、透视 near>0 / far>near、正交 far>near、
		// zoom>0、aspect>0;NaN/退化值回退到安全默认(不改写入者意图之外的东西)。
		static void ClampSettings(CameraSettings& settings, float aspectRatio);

		// 取/算该实体的帧相机数据。**只读缓存**:CameraViewComponent 由结构提交点预建
		// (Scene::OnRuntimeStart / Scene::EnsureCameraView),本函数绝不 emplace。
		// 缺组件时返回静态单位投影并 WARN 一次,不崩。
		static const Camera& EnsureView(entt::registry& registry, entt::entity entity,
		                                uint32_t viewportWidth, uint32_t viewportHeight);

		// 视口尺寸变化 / 每帧刷新:对注册表内所有已有的相机视图调用 EnsureView。
		static void UpdateAllCameras(entt::registry& registry, uint32_t width, uint32_t height);

		static entt::entity FindPrimaryCameraEntity(const entt::registry& registry);

		// 与迁移前 SceneCamera::RecalculateProjection 逐元素一致:
		//   透视 glm::perspective(radians(FOV), aspect, near, far);
		//   正交 glm::ortho(-aspect*zoom, aspect*zoom, -zoom, zoom, near, far)。
		static glm::mat4 ComputeProjection(const CameraSettings& settings, float aspectRatio);

		// 测试/诊断:EnsureView 真正重建投影矩阵的次数(进程级、单调递增)。
		static uint64_t ProjectionRebuildCount();
	};
}
