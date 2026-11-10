// P1b D8a:视锥剔除数学(纯 glm,无窗口/无设备)。
// 覆盖:平面抽取(透视/正交/Vulkan 适配矩阵)、AABB 变换、AABB × 平面判定。
#include "World/Renderer/FrustumCull.h"
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

	World::FrustumPlanes PerspectiveFrustum(bool vulkan)
	{
		const glm::mat4 projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
		const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
			glm::vec3(0.0f, 1.0f, 0.0f));
		return World::ExtractFrustumPlanes(
			World::AdaptViewProjectionForOffscreen(projection * view, vulkan));
	}

	void TestPerspectiveFrustum()
	{
		for (const bool vulkan : { false, true })
		{
			const World::FrustumPlanes frustum = PerspectiveFrustum(vulkan);
			// 相机朝 -Z,近 0.1 / 远 100。
			CHECK(World::AabbInFrustum(frustum, { -0.5f, -0.5f, -5.5f }, { 0.5f, 0.5f, -4.5f }));
			// 相机背后。
			CHECK(!World::AabbInFrustum(frustum, { -0.5f, -0.5f, 0.5f }, { 0.5f, 0.5f, 1.5f }));
			// 超出远平面。
			CHECK(!World::AabbInFrustum(frustum, { -0.5f, -0.5f, -201.0f }, { 0.5f, 0.5f, -200.0f }));
			// 侧面之外(60° 竖直 FOV、16:9,横向半角更大;放在 x=50 一定在外)。
			CHECK(!World::AabbInFrustum(frustum, { 49.0f, -0.5f, -5.5f }, { 51.0f, 0.5f, -4.5f }));
			// 跨近平面(部分可见)必须保留。
			CHECK(World::AabbInFrustum(frustum, { -0.2f, -0.2f, -0.2f }, { 0.2f, 0.2f, 0.2f }));
		}
	}

	void TestOrthographicFrustum()
	{
		// 阴影通道用的正交视锥:盒子 [-10,10]²、深度 [-1,1](GL 语义)。
		const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f));
		const glm::mat4 projection = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 20.0f);
		for (const bool vulkan : { false, true })
		{
			const World::FrustumPlanes frustum = World::ExtractFrustumPlanes(
				World::AdaptViewProjectionForOffscreen(projection * view, vulkan));
			CHECK(World::AabbInFrustum(frustum, { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f }));
			// 水平方向出界(正交盒 x∈[-10,10])。
			CHECK(!World::AabbInFrustum(frustum, { -40.0f, -1.0f, -1.0f }, { -30.0f, 1.0f, 1.0f }));
			// 光源"相机"上方之外(视锥 y 覆盖世界 x/z 的 ±10;世界 y 远高于光源高度)。
			CHECK(!World::AabbInFrustum(frustum, { -1.0f, 60.0f, -1.0f }, { 1.0f, 70.0f, 1.0f }));
			// 边界上的盒子(贴住边)保守判为可见。
			CHECK(World::AabbInFrustum(frustum, { 9.0f, -1.0f, -1.0f }, { 11.0f, 1.0f, 1.0f }));
		}
	}

	void TestTransformAabb()
	{
		// 绕 Z 旋转 45°:单位盒 → 世界 AABB 半宽 √2/2 ≈ 0.7071,z 不变。
		const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		glm::vec3 min;
		glm::vec3 max;
		World::TransformAabb(rotation, { -1.0f, -1.0f, -0.5f }, { 1.0f, 1.0f, 0.5f }, min, max);
		const float half = std::sqrt(2.0f);
		CHECK(std::fabs(min.x + half) < 1e-4f && std::fabs(max.x - half) < 1e-4f);
		CHECK(std::fabs(min.y + half) < 1e-4f && std::fabs(max.y - half) < 1e-4f);
		CHECK(std::fabs(min.z + 0.5f) < 1e-4f && std::fabs(max.z - 0.5f) < 1e-4f);

		// 平移 + 非均匀缩放:AABB 跟着走,不会缩成点。
		const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f))
			* glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 0.5f, 1.0f));
		World::TransformAabb(transform, { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f }, min, max);
		CHECK(std::fabs(min.x - 8.0f) < 1e-4f && std::fabs(max.x - 12.0f) < 1e-4f);
		CHECK(std::fabs(min.y + 0.5f) < 1e-4f && std::fabs(max.y - 0.5f) < 1e-4f);
	}
}

int main()
{
	try
	{
		TestPerspectiveFrustum();
		TestOrthographicFrustum();
		TestTransformAabb();
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "[FAIL] %s\n", exception.what());
		return 1;
	}
	std::printf("[PASS] World.Frustum\n");
	return 0;
}
