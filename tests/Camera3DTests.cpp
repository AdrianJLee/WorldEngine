// P1b D1:3D 相机与射线数学基线(纯 glm,无窗口/无设备)。
#include "World/Renderer/CameraRay.h"
#include "World/Renderer/EditorCamera3D.h"
#include "World/Renderer/ProjectionConventions.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	// 点到射线的最短距离。
	float DistanceToRay(const World::Ray& ray, const glm::vec3& point)
	{
		const glm::vec3 toPoint = point - ray.Origin;
		const float t = glm::dot(toPoint, ray.Direction);
		return glm::length(toPoint - ray.Direction * t);
	}
}

int main()
{
	try
	{
		using namespace World;

		EditorCamera3D camera(60.0f, 16.0f / 9.0f, 0.1f, 1000.0f, 10.0f);
		camera.SetViewportSize(1280, 720);
		camera.SetTarget({ 0.0f, 0.0f, 0.0f });

		// 1. 初始姿态:目标在正前方,距离与 GetDistance 一致。
		{
			const glm::vec3 toTarget = camera.GetTarget() - camera.GetPosition();
			const float length = glm::length(toTarget);
			CHECK(std::fabs(length - camera.GetDistance()) < 1e-4f);
			CHECK(glm::length(toTarget / length - camera.GetForward()) < 1e-5f);
			CHECK(glm::dot(camera.GetForward(), glm::vec3(0.0f, 0.0f, 1.0f)) > 0.999f);
		}

		// 2. Orbit 不改变到目标点的距离;pitch 限幅 ±89°。
		{
			const float before = glm::length(camera.GetTarget() - camera.GetPosition());
			camera.Orbit(35.0f, 25.0f);
			const float after = glm::length(camera.GetTarget() - camera.GetPosition());
			CHECK(std::fabs(before - after) < 1e-3f);
			CHECK(std::fabs(after - camera.GetDistance()) < 1e-3f);

			camera.Orbit(0.0f, 1000.0f);
			CHECK(camera.GetPitch() <= 89.0f + 1e-3f);
			CHECK(camera.GetPitch() >= -89.0f - 1e-3f);
			camera.Orbit(0.0f, -5000.0f);
			CHECK(camera.GetPitch() >= -89.0f - 1e-3f);
		}

		// 3. Dolly 有上下限,Pan 保持朝向(只平移目标)。
		{
			camera.Dolly(-1000.0f);
			CHECK(camera.GetDistance() > 0.0f);
			camera.SetDistance(12.0f);
			const glm::vec3 forwardBefore = camera.GetForward();
			camera.Pan(1.0f, -0.5f);
			CHECK(glm::length(camera.GetForward() - forwardBefore) < 1e-5f);
		}

		// 4. NDC 往返:世界点 → NDC → 世界射线,点应落在射线上。
		{
			const glm::mat4 viewProjection = camera.GetViewProjectionMatrix(false);
			const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
			const glm::vec3 worldPoint(2.0f, 1.5f, 3.0f);

			bool visible = false;
			const glm::vec2 ndc = WorldToNdc(worldPoint, viewProjection, &visible);
			CHECK(visible);

			const Ray ray = ScreenPointToRay(ndc, inverseViewProjection);
			CHECK(std::fabs(glm::length(ray.Direction) - 1.0f) < 1e-4f);
			CHECK(DistanceToRay(ray, worldPoint) < 1e-3f);

			// 屏幕中心射线方向应与相机前向一致(视线中心)。
			const Ray centerRay = ScreenPointToRay({ 0.0f, 0.0f }, inverseViewProjection);
			CHECK(glm::length(centerRay.Direction - camera.GetForward()) < 1e-3f);
		}

		// 5. 后端 NDC 适配:GL 保持原样,Vulkan = 翻转 Y 行 + 深度重映射(z'=0.5z+0.5w)。
		{
			const glm::mat4 gl = camera.GetProjectionMatrix(false);
			const glm::mat4 vk = camera.GetProjectionMatrix(true);
			for (int column = 0; column < 4; ++column)
			{
				CHECK(std::fabs(vk[column][1] + gl[column][1]) < 1e-6f);
				CHECK(std::fabs(vk[column][2] - (0.5f * gl[column][2] + 0.5f * gl[column][3])) < 1e-6f);
				CHECK(std::fabs(vk[column][3] - gl[column][3]) < 1e-6f);
			}
		}

		// 5b. 深度端点:相机前方 near/far 处的点,裁剪空间 z/w 必须落在
		//     GL:[-1,1](近=-1,远=+1)与 Vulkan:[0,1](近=0,远=1)内,并且单调递增。
		//     这是"3D 深度/面朝向异常"的回归锚点:只翻 Y 不补 Z 时,Vulkan 下的近平面
		//     端点会是 -1(落在合法范围外),近处几何被裁掉、深度比较失真。
		{
			const float nearClip = camera.GetNearClip();
			const float farClip = camera.GetFarClip();
			const glm::vec3 origin = camera.GetPosition();
			const glm::vec3 forward = camera.GetForward();
			const auto depthAt = [&](const glm::mat4& viewProjection, float distance)
			{
				const glm::vec4 clip = viewProjection * glm::vec4(origin + forward * distance, 1.0f);
				return clip.z / clip.w;
			};

			const glm::mat4 gl = camera.GetViewProjectionMatrix(false);
			CHECK(std::fabs(depthAt(gl, nearClip) + 1.0f) < 1e-3f);
			CHECK(std::fabs(depthAt(gl, farClip) - 1.0f) < 1e-3f);
			CHECK(depthAt(gl, nearClip) < depthAt(gl, 0.5f * (nearClip + farClip)));
			CHECK(depthAt(gl, 0.5f * (nearClip + farClip)) < depthAt(gl, farClip));

			const glm::mat4 vk = camera.GetViewProjectionMatrix(true);
			CHECK(std::fabs(depthAt(vk, nearClip) - 0.0f) < 1e-3f);
			CHECK(std::fabs(depthAt(vk, farClip) - 1.0f) < 1e-3f);
			CHECK(depthAt(vk, nearClip) < depthAt(vk, 0.5f * (nearClip + farClip)));
			CHECK(depthAt(vk, 0.5f * (nearClip + farClip)) < depthAt(vk, farClip));

			// 同一世界点在两种约定下的深度都在各自合法范围内(不会再出现"一半深度越界")。
			for (float t = 0.0f; t <= 1.0f; t += 0.1f)
			{
				const float distance = nearClip + (farClip - nearClip) * t;
				const float glDepth = depthAt(gl, distance);
				const float vkDepth = depthAt(vk, distance);
				CHECK(glDepth >= -1.0f - 1e-3f && glDepth <= 1.0f + 1e-3f);
				CHECK(vkDepth >= -1e-3f && vkDepth <= 1.0f + 1e-3f);
			}
		}

		// 6. 射线与平面求交:命中 + 平行未命中 + 反方向未命中。
		{
			const Ray ray { { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } };
			float t = -1.0f;
			CHECK(IntersectPlane(ray, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, &t));
			CHECK(std::fabs(t - 1.0f) < 1e-5f);

			const Ray parallel { { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
			CHECK(!IntersectPlane(parallel, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, nullptr));

			const Ray away { { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
			CHECK(!IntersectPlane(away, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, nullptr));
		}

		std::printf("World.Camera3D: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Camera3D FAILED: %s\n", error.what());
		return 1;
	}
}
