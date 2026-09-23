#include "wldpch.h"
#include "World/Renderer/CameraRay.h"

#include <cmath>

namespace World
{
	Ray ScreenPointToRay(const glm::vec2& ndc, const glm::mat4& invViewProjection)
	{
		Ray ray;
		// 近平面 (z=-1) 与远平面 (z=+1) 反投影成两点,连线即射线。
		glm::vec4 nearPoint = invViewProjection * glm::vec4(ndc.x, ndc.y, -1.0f, 1.0f);
		glm::vec4 farPoint = invViewProjection * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
		if (std::fabs(nearPoint.w) < 1e-8f || std::fabs(farPoint.w) < 1e-8f)
			return ray;   // 退化矩阵:返回默认射线,调用方需自检

		const glm::vec3 nearWorld = glm::vec3(nearPoint) / nearPoint.w;
		const glm::vec3 farWorld = glm::vec3(farPoint) / farPoint.w;
		const glm::vec3 direction = farWorld - nearWorld;
		const float length = glm::length(direction);
		if (length < 1e-8f)
			return ray;

		ray.Origin = nearWorld;
		ray.Direction = direction / length;
		return ray;
	}

	glm::vec2 WorldToNdc(const glm::vec3& world, const glm::mat4& viewProjection, bool* outVisible)
	{
		const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
		if (std::fabs(clip.w) < 1e-8f)
		{
			if (outVisible)
				*outVisible = false;
			return { 0.0f, 0.0f };
		}

		const glm::vec3 ndc = glm::vec3(clip) / clip.w;
		if (outVisible)
			*outVisible = clip.w > 0.0f && ndc.z >= -1.0f && ndc.z <= 1.0f;
		return { ndc.x, ndc.y };
	}

	bool IntersectPlane(const Ray& ray, const glm::vec3& planePoint,
		const glm::vec3& planeNormal, float* outT)
	{
		const float denominator = glm::dot(planeNormal, ray.Direction);
		if (std::fabs(denominator) < 1e-6f)
			return false;   // 射线与平面平行(含共面)

		const float t = glm::dot(planePoint - ray.Origin, planeNormal) / denominator;
		if (t < 0.0f)
			return false;   // 交点在射线反方向

		if (outT)
			*outT = t;
		return true;
	}
}
