#pragma once

#include "World/Animation/AnimGraph.h"
#include "World/Asset/WModelIO.h"
#include "World/Core/Export.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>

namespace World
{
	class Scene;

	// 通用两骨 IK 请求。目标与极向都在**模型空间**（与 AnimPose 一致）；
	// ApplyToPose 负责把组件上的世界目标/世界实体位置换算进来。
	struct WLD_API IkChainRequest
	{
		int32_t Root = -1;
		int32_t Mid = -1;
		int32_t End = -1;
		glm::vec3 TargetModel { 0.0f };
		glm::vec3 PoleDirectionModel { 0.0f, 0.0f, 1.0f };
		float Weight = 1.0f;
	};

	// 通用两骨 IK（髋-膝-踝 / 肩-肘-腕 / 任意三节点链）。
	// 脚步 IK 只是它的一个示例驱动：FootIkComponent 采样地形后生成两条请求。
	class WLD_API IkSystem
	{
	public:
		// 每帧收集一次 TwoBoneIkComponent 与场景地形；只读 registry。
		static void BeginFrame(Scene& scene);

		// 姿态空间求解：把 Root→Mid→End 链的末端送到 TargetModel。
		// 目标超出可达范围时钳制到最大伸展；权重用 slerp 混合；无效链返回 false。
		static bool SolveTwoBone(const Asset::WModelData& model, AnimPose& pose, const IkChainRequest& request);

		// 应用 owner 上的通用链接口（TwoBoneIkComponent），随后执行脚步 IK 示例驱动。
		// 只读 registry，不做结构写；必须在 EvaluatePose 之后、BuildPalette 之前调用。
		static void ApplyToPose(Scene& scene, entt::entity owner, const Asset::WModelData& model,
			const glm::mat4& characterWorld, AnimPose& pose, bool grounded);

		static void EndFrame();
	};
}
