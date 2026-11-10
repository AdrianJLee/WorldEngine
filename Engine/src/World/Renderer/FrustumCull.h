#pragma once

#include <glm/glm.hpp>

#include <cfloat>

namespace World
{
	// P1b D8a:视锥剔除(纯数学,无设备依赖,headless 单测可断言;双后端共用一份)。
	//
	// 平面由 **view-projection 矩阵**按 Gribb–Hartmann 方法直接抽取,约定 `n·p + d >= 0`
	// 为视锥内侧(与 clip 空间 -w<=x<=w 同号)。
	//
	// 近平面用 GL 语义(row3 + row2):对 Vulkan 的 z∈[0,1] 裁剪空间它是**保守超集**
	// (近平面更靠后),只会少剔除、不会误剔除 —— 因此两个后端共用同一份代码,
	// 不需要像投影矩阵那样按后端分支。
	struct FrustumPlanes
	{
		// 顺序:左 / 右 / 下 / 上 / 近 / 远;xyz = 单位法线,w = 常数项。
		glm::vec4 Planes[6];
	};

	inline FrustumPlanes ExtractFrustumPlanes(const glm::mat4& viewProjection)
	{
		// glm 是列主序:m[column][row];第 row 行 = {m[0][row], m[1][row], m[2][row], m[3][row]}。
		const auto row = [&viewProjection](int index)
		{
			return glm::vec4(viewProjection[0][index], viewProjection[1][index],
				viewProjection[2][index], viewProjection[3][index]);
		};
		const glm::vec4 row0 = row(0);
		const glm::vec4 row1 = row(1);
		const glm::vec4 row2 = row(2);
		const glm::vec4 row3 = row(3);

		FrustumPlanes frustum;
		frustum.Planes[0] = row3 + row0;   // 左:x >= -w
		frustum.Planes[1] = row3 - row0;   // 右:x <= w
		frustum.Planes[2] = row3 + row1;   // 下:y >= -w
		frustum.Planes[3] = row3 - row1;   // 上:y <= w
		frustum.Planes[4] = row3 + row2;   // 近:z >= -w(GL 语义;Vulkan 下是保守超集)
		frustum.Planes[5] = row3 - row2;   // 远:z <= w

		for (glm::vec4& plane : frustum.Planes)
		{
			const float length = glm::length(glm::vec3(plane));
			if (length > 1e-8f)
				plane /= length;
		}
		return frustum;
	}

	// 局部 AABB → 世界 AABB(8 角点法;支持旋转/缩放/非均匀缩放)。
	inline void TransformAabb(const glm::mat4& transform, const glm::vec3& localMin, const glm::vec3& localMax,
		glm::vec3& worldMin, glm::vec3& worldMax)
	{
		worldMin = glm::vec3(FLT_MAX);
		worldMax = glm::vec3(-FLT_MAX);
		for (uint32_t corner = 0; corner < 8; ++corner)
		{
			const glm::vec3 local {
				(corner & 1u) ? localMax.x : localMin.x,
				(corner & 2u) ? localMax.y : localMin.y,
				(corner & 4u) ? localMax.z : localMin.z };
			const glm::vec3 world = glm::vec3(transform * glm::vec4(local, 1.0f));
			worldMin = glm::min(worldMin, world);
			worldMax = glm::max(worldMax, world);
		}
	}

	// AABB 与 6 个平面求交(保守:只要有一个平面把整个盒子判在外侧,即不可见)。
	inline bool AabbInFrustum(const FrustumPlanes& frustum, const glm::vec3& min, const glm::vec3& max)
	{
		for (const glm::vec4& plane : frustum.Planes)
		{
			// 取"沿平面法线最远"的角点(法线分量 >= 0 时取 max,否则取 min)。
			const glm::vec3 positive {
				plane.x >= 0.0f ? max.x : min.x,
				plane.y >= 0.0f ? max.y : min.y,
				plane.z >= 0.0f ? max.z : min.z };
			if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f)
				return false;
		}
		return true;
	}
}
