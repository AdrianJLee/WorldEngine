#pragma once

#include <glm/glm.hpp>

namespace World
{
	// 相机投影矩阵一律按 **GL 约定** 编写(NDC:+Y 向上、z∈[-1,1])。
	// 使用 Vulkan 后端时,由本函数把 view-projection 适配到 Vulkan NDC:
	//
	//   Y:Vulkan NDC 的 +Y 向下 → 翻转 Y 行,画面方向与 GL 一致。
	//   Z:GL 裁剪空间 z∈[-1,1],Vulkan 的 z∈[0,1] → z' = 0.5*z + 0.5*w,
	//     即"近平面 → 0、远平面 → 1",与深度清值 1.0 + LessOrEqual 的测试约定一致。
	//
	// 为什么必须两件事一起做:只翻 Y 不补 Z 时,一半深度范围落在 [0,1] 之外 ——
	// 近处几何被裁掉(实测表现:"朝着相机的面消失、看到内壁"),深度比较也随之失真。
	// 两个后端必须共用本函数,否则会出现"一个后端对一个后端反"的问题。
	inline glm::mat4 AdaptViewProjectionToBackend(const glm::mat4& viewProjection, bool vulkan)
	{
		if (!vulkan)
			return viewProjection;

		glm::mat4 adapted = viewProjection;
		// 列主序:第 row 行 = adapted[column][row];翻转 Y 行(= 输出 y 分量取反)。
		adapted[0][1] = -adapted[0][1];
		adapted[1][1] = -adapted[1][1];
		adapted[2][1] = -adapted[2][1];
		adapted[3][1] = -adapted[3][1];
		// 深度重映射:z' = 0.5*z + 0.5*w。
		for (int column = 0; column < 4; ++column)
			adapted[column][2] = 0.5f * adapted[column][2] + 0.5f * adapted[column][3];
		return adapted;
	}
}
