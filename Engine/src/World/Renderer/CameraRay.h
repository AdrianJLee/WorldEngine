#pragma once

#include "World/Core/Export.h"

#include <glm/glm.hpp>

namespace World
{
	// 世界空间射线(Direction 已单位化)。
	struct Ray
	{
		glm::vec3 Origin { 0.0f };
		glm::vec3 Direction { 0.0f, 0.0f, -1.0f };

		glm::vec3 At(float t) const { return Origin + Direction * t; }
	};

	// 屏幕/NDC → 世界射线:invViewProjection = inverse(projection * view)。
	// NDC 约定为 OpenGL 风格:z ∈ [-1, 1],近平面 z=-1、远平面 z=+1。
	WLD_API Ray ScreenPointToRay(const glm::vec2& ndc, const glm::mat4& invViewProjection);

	// 世界点 → NDC。outVisible:点位于相机前方且在近/远裁剪面之间时为 true(w≈0 视为不可见)。
	WLD_API glm::vec2 WorldToNdc(const glm::vec3& world, const glm::mat4& viewProjection,
		bool* outVisible = nullptr);

	// 射线与平面求交(平面由点 + 法线定义)。命中返回 true 并写入 outT(射线参数,>0 为正方向)。
	WLD_API bool IntersectPlane(const Ray& ray, const glm::vec3& planePoint,
		const glm::vec3& planeNormal, float* outT);
}
