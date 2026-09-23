#pragma once

// D5c-3a:CPU 蒙皮/动画数学(纯函数:无 RHI 设备、无场景依赖,headless 可断言)。
// 运行时(D5c-3)只是把这里算出的调色板搬进 SSBO;数学口径以本文件为准。

#include "World/Core/Asset/WModelIO.h"
#include "World/Core/Export.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace World
{
	// WModelIO 定义在 World::Asset 内;下面的签名沿用 "WModelIO::" 的写法(D5c-3a 冻结接口)。
	namespace WModelIO = Asset::WModelIO;

	// 采样动画:返回每个通道的 TRS 值(线性;旋转 slerp)。time 会被 clamp 到 [0, duration]。
	struct AnimationSample { uint32_t Node = 0; WModelIO::WModelAnimationPath Path; glm::vec4 Value; };
	WLD_API std::vector<AnimationSample> SampleAnimation(
		const Asset::WModelAnimation& animation, float time);

	// 关节调色板:palette[j] = inverse(meshWorld) * jointWorld[JointNodes[j]] * inverseBind[j]。
	// jointWorld.size() 必须 >= skin.JointNodes.size();越界返回空 vector(调用方按"无蒙皮"处理)。
	WLD_API std::vector<glm::mat4> BuildJointMatrices(const Asset::WModelSkin& skin,
		const std::vector<glm::mat4>& jointWorld, const glm::mat4& meshWorld);
}
