#include "World/Renderer/Decal.h"
#include "World/Physics/ConvexDecomposition.h"
#include "World/Scene/Components.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestDecalProjectionAndClipping()
{
	// 贴花位于 (10, 5, 10)，半尺寸 (2, 2, 2)，朝向垂直向下 (0, -1, 0)
	glm::mat4 decalXform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 5.0f, 10.0f));
	decalXform = glm::scale(decalXform, glm::vec3(2.0f, 2.0f, 2.0f));
	const glm::mat4 invDecal = glm::inverse(decalXform);

	const glm::vec3 decalDir(0.0f, -1.0f, 0.0f);
	const glm::vec3 groundNorm(0.0f, 1.0f, 0.0f);

	glm::vec2 uv;
	float fade = 0.0f;

	// 1. 地面点在盒体正中心 (10, 5, 10) 投影：UV 必须为中心 (0.5, 0.5)
	bool ok = DecalProjector::ProjectWorldPoint(invDecal, glm::vec3(10.0f, 5.0f, 10.0f), groundNorm, decalDir, 60.0f, uv, fade);
	assert(ok);
	assert(std::abs(uv.x - 0.5f) < 1e-4f);
	assert(std::abs(uv.y - 0.5f) < 1e-4f);
	assert(fade > 0.9f);

	// 2. 点超出盒体 X 边界 (10 + 3 = 13 > 10 + 2)：必须返回 false 被剔除
	bool okOut = DecalProjector::ProjectWorldPoint(invDecal, glm::vec3(13.0f, 5.0f, 10.0f), groundNorm, decalDir, 60.0f, uv, fade);
	assert(!okOut);

	// 3. 掠射角法线测试：竖直墙壁法线 (1, 0, 0) 与朝下贴花夹角为 90 度 (> 60度截断)：必须被剔除
	const glm::vec3 wallNorm(1.0f, 0.0f, 0.0f);
	bool okGrazing = DecalProjector::ProjectWorldPoint(invDecal, glm::vec3(10.0f, 5.0f, 10.0f), wallNorm, decalDir, 60.0f, uv, fade);
	assert(!okGrazing);

	std::cout << "[PASS] TestDecalProjectionAndClipping\n";
}

static void TestConvexDecomposition()
{
	std::vector<glm::vec3> points = {
		{ -5.0f, -5.0f, -5.0f },
		{  5.0f, -5.0f, -5.0f },
		{  5.0f,  5.0f, -5.0f },
		{ -5.0f,  5.0f, -5.0f },
		{ -5.0f, -5.0f,  5.0f },
		{  5.0f, -5.0f,  5.0f },
		{  5.0f,  5.0f,  5.0f },
		{ -5.0f,  5.0f,  5.0f }
	};

	ConvexHull hull;
	bool ok = ConvexDecomposer::ComputeHull(points, hull);
	assert(ok);
	assert(!hull.Vertices.empty());
	assert(!hull.Indices.empty());
	assert(hull.Volume > 0.0f);

	// 内部点判定
	assert(hull.ContainsPoint(glm::vec3(0.0f, 0.0f, 0.0f)));
	// 外部点判定
	assert(!hull.ContainsPoint(glm::vec3(20.0f, 0.0f, 0.0f)));

	// 网格分解
	std::vector<ConvexHull> subHulls;
	ConvexDecomposer::Decompose(points, hull.Indices, 2, 0.05f, subHulls);
	assert(subHulls.size() >= 1);

	std::cout << "[PASS] TestConvexDecomposition (Subhulls: " << subHulls.size() << ")\n";
}

static void TestDecalAndConvexComponentLayout()
{
	DecalComponent decal;
	assert(sizeof(decal) == 48);
	assert(decal.FadeFactor == 1.0f);
	assert(decal.NormalAngleCutoff == 60.0f);

	ConvexCollider3DComponent convex;
	assert(sizeof(convex) == 32);
	assert(convex.HullCount == 1);
	assert(convex.MaxVerticesPerHull == 32);

	std::cout << "[PASS] TestDecalAndConvexComponentLayout\n";
}

int main()
{
	std::cout << "Starting World.Decal tests...\n";
	TestDecalProjectionAndClipping();
	TestConvexDecomposition();
	TestDecalAndConvexComponentLayout();
	std::cout << "All World.Decal tests passed successfully!\n";
	return 0;
}
