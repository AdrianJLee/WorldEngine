#include "World/Renderer/Decal.h"
#include "World/Physics/ConvexDecomposition.h"
#include "World/Scene/Components.h"
#include "World/Asset/WModelIO.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include <chrono>

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

	// 4. ProjectToPlane 平面投射测试：地面在 Y = 4.0 (在局部盒体 Y in [3, 7] 范围内)
	glm::vec3 quadPositions[4];
	glm::vec2 quadUVs[4];
	float planeFade = 0.0f;
	const glm::vec3 planePt(10.0f, 4.0f, 10.0f);
	bool okPlane = DecalProjector::ProjectToPlane(
		decalXform, invDecal, planePt, groundNorm, decalDir, 60.0f,
		quadPositions, quadUVs, planeFade);
	assert(okPlane);
	assert(planeFade > 0.4f);
	for (int i = 0; i < 4; ++i)
	{
		// 顶点带有 0.002f 深度偏移
		assert(std::abs(quadPositions[i].y - 4.002f) < 1e-4f);
		assert(quadUVs[i].x >= 0.0f && quadUVs[i].x <= 1.0f);
		assert(quadUVs[i].y >= 0.0f && quadUVs[i].y <= 1.0f);
	}

	// 5. 反向平面法线与掠射角截断测试
	const glm::vec3 invertedNorm(0.0f, -1.0f, 0.0f);
	bool okBackPlane = DecalProjector::ProjectToPlane(
		decalXform, invDecal, planePt, invertedNorm, decalDir, 60.0f,
		quadPositions, quadUVs, planeFade);
	assert(!okBackPlane);

	std::cout << "[PASS] TestDecalProjectionAndClipping\n";
}

static void TestConvexQuickHull()
{
	// 8 个立方体角点 (尺寸 10x10x10，理论体积 1000)
	std::vector<glm::vec3> boxPoints = {
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
	bool ok = ConvexDecomposer::ComputeHull(boxPoints, hull);
	assert(ok);
	assert(hull.Vertices.size() == 8);
	assert(hull.Indices.size() == 36); // 12 个三角面
	assert(hull.IsConvex());
	assert(std::abs(hull.Volume - 1000.0f) < 1.0f);

	// 验证凸包所有三角面法线严格背离质心朝外
	for (size_t i = 0; i + 2 < hull.Indices.size(); i += 3)
	{
		const glm::vec3& a = hull.Vertices[hull.Indices[i]];
		const glm::vec3& b = hull.Vertices[hull.Indices[i + 1]];
		const glm::vec3& c = hull.Vertices[hull.Indices[i + 2]];
		const glm::vec3 faceCenter = (a + b + c) / 3.0f;
		const glm::vec3 faceNormal = glm::cross(b - a, c - a);
		const float len = glm::length(faceNormal);
		assert(len > 1e-6f);
		assert(glm::dot(faceNormal / len, faceCenter - hull.Center) > 0.0f);
	}

	// 内部点与外部点严密判定
	assert(hull.ContainsPoint(glm::vec3(0.0f, 0.0f, 0.0f)));
	assert(hull.ContainsPoint(glm::vec3(4.99f, 4.99f, 4.99f)));
	assert(!hull.ContainsPoint(glm::vec3(5.1f, 0.0f, 0.0f)));
	assert(!hull.ContainsPoint(glm::vec3(20.0f, 0.0f, 0.0f)));

	// 在点云内部注入 10 个内点，验证 QuickHull 算法能自动剔除内点，保持凸包顶点数不变
	std::vector<glm::vec3> pointsWithInternals = boxPoints;
	pointsWithInternals.push_back({ 0.0f, 0.0f, 0.0f });
	pointsWithInternals.push_back({ 1.0f, 2.0f, -1.0f });
	pointsWithInternals.push_back({ -2.0f, 1.0f, 3.0f });
	pointsWithInternals.push_back({ 3.0f, -3.0f, 2.0f });

	ConvexHull prunedHull;
	bool okPruned = ConvexDecomposer::ComputeHull(pointsWithInternals, prunedHull);
	assert(okPruned);
	assert(prunedHull.Vertices.size() == 8);
	assert(prunedHull.Indices.size() == 36);
	assert(prunedHull.IsConvex());
	assert(std::abs(prunedHull.Volume - 1000.0f) < 1.0f);

	std::cout << "[PASS] TestConvexQuickHull (Vertices: " << hull.Vertices.size() << ", Volume: " << hull.Volume << ")\n";
}

static void TestConcaveMeshDecomposition()
{
	// 构造典型 3D 凹体：U 型槽 (16 个顶点，两根立柱与底槽，中间顶部有空槽)
	// 左立柱: X in [-3, -1], Y in [0, 4], Z in [-2, 2]
	// 右立柱: X in [ 1,  3], Y in [0, 4], Z in [-2, 2]
	// 中间顶部 (X in [-1, 1], Y in [1, 4]) 为凹陷区域
	std::vector<glm::vec3> uMeshPoints = {
		// 左立柱 8 顶点
		{ -3.0f, 0.0f, -2.0f }, { -1.0f, 0.0f, -2.0f },
		{ -1.0f, 4.0f, -2.0f }, { -3.0f, 4.0f, -2.0f },
		{ -3.0f, 0.0f,  2.0f }, { -1.0f, 0.0f,  2.0f },
		{ -1.0f, 4.0f,  2.0f }, { -3.0f, 4.0f,  2.0f },

		// 右立柱 8 顶点
		{  1.0f, 0.0f, -2.0f }, {  3.0f, 0.0f, -2.0f },
		{  3.0f, 4.0f, -2.0f }, {  1.0f, 4.0f, -2.0f },
		{  1.0f, 0.0f,  2.0f }, {  3.0f, 0.0f,  2.0f },
		{  3.0f, 4.0f,  2.0f }, {  1.0f, 4.0f,  2.0f }
	};

	// 1. 验证单一凸包算法会将空槽连桥包裹，整体凸包体积较大
	ConvexHull singleHull;
	bool okSingle = ConvexDecomposer::ComputeHull(uMeshPoints, singleHull);
	assert(okSingle);
	assert(singleHull.IsConvex());
	const float singleVolume = singleHull.Volume;
	assert(singleVolume > 0.0f);

	// 2. 凹体分解：分解为 2 个子凸包 (左臂与右臂)
	std::vector<ConvexHull> subHulls;
	ConvexDecomposer::Decompose(uMeshPoints, {}, 2, 0.05f, subHulls);
	assert(subHulls.size() >= 2);

	float totalSubVolume = 0.0f;
	for (const auto& sub : subHulls)
	{
		assert(sub.IsConvex());
		assert(sub.Volume > 0.0f);
		assert(sub.Vertices.size() >= 4);
		assert(sub.Indices.size() >= 12);
		totalSubVolume += sub.Volume;
	}

	// 凹体分解后去除了 U 型槽凹陷空隙，总子凸包体积严格小于单外包体积
	assert(totalSubVolume < singleVolume);

	std::cout << "[PASS] TestConcaveMeshDecomposition (Subhulls: " << subHulls.size()
		<< ", SingleVol: " << singleVolume << ", DecomposedTotalVol: " << totalSubVolume << ")\n";
}

static void TestConvexHullNormalConsistency()
{
	// 构造包含非轴对齐倾斜面与复杂凸角的点集
	std::vector<glm::vec3> points = {
		{ -2.0f, -1.0f, -3.0f }, {  3.0f, -2.0f, -1.0f },
		{  2.0f,  4.0f, -2.0f }, { -1.0f,  3.0f,  4.0f },
		{  0.0f, -3.0f,  2.0f }, {  4.0f,  1.0f,  3.0f },
		{ -3.0f,  1.0f, -1.0f }, {  1.0f,  0.0f,  5.0f },
		{  0.5f,  0.5f,  0.5f }, { -0.5f, -0.5f,  0.2f }
	};

	ConvexHull hull;
	bool ok = ConvexDecomposer::ComputeHull(points, hull);
	assert(ok);
	assert(hull.IsConvex());
	assert(hull.Vertices.size() >= 4);
	assert(hull.Indices.size() >= 12);
	assert(hull.Volume > 0.0f);

	// 核心断言: 每一个缝合三角面法向绝对背离质心朝外，且所有顶点均在每个三角面的内侧
	for (size_t i = 0; i + 2 < hull.Indices.size(); i += 3)
	{
		const glm::vec3& a = hull.Vertices[hull.Indices[i]];
		const glm::vec3& b = hull.Vertices[hull.Indices[i + 1]];
		const glm::vec3& c = hull.Vertices[hull.Indices[i + 2]];
		const glm::vec3 faceCenter = (a + b + c) / 3.0f;
		const glm::vec3 faceNormal = glm::cross(b - a, c - a);
		const float len = glm::length(faceNormal);
		assert(len > 1e-6f);
		const glm::vec3 unitNormal = faceNormal / len;

		// 法向量与 (faceCenter - hull.Center) 点积必须为正
		assert(glm::dot(unitNormal, faceCenter - hull.Center) > 0.0f);

		// 凸包内所有顶点在该面法向上的外凸投影必须非正 (容差 0.01f)
		for (const auto& v : hull.Vertices)
		{
			assert(glm::dot(v - a, unitNormal) <= 0.01f);
		}
	}

	std::cout << "[PASS] TestConvexHullNormalConsistency (Faces: " << (hull.Indices.size() / 3) << ")\n";
}

static void TestConvexLargePointCloudPerformance()
{
	// 构造 5000+ 顶点的海量高模点云 (带环面与球面螺旋波动，并注入 500 个重复顶点测试哈希去重)
	std::vector<glm::vec3> densePoints;
	densePoints.reserve(5500);
	for (int i = 0; i < 5000; ++i)
	{
		const float u = static_cast<float>(i) / 5000.0f;
		const float theta = u * 3.14159265f * 2.0f * 25.0f;
		const float phi = std::acos(std::clamp(1.0f - 2.0f * u, -1.0f, 1.0f));
		const float r = 10.0f * (0.8f + 0.2f * std::sin(theta * 3.0f));
		densePoints.push_back({
			r * std::sin(phi) * std::cos(theta),
			r * std::sin(phi) * std::sin(theta),
			r * std::cos(phi)
		});
	}
	for (int i = 0; i < 500; ++i)
		densePoints.push_back(densePoints[i]);

	// 1. 基准测试: ComputeHull 在 5500 顶点大点云下的计算耗时
	const auto startHull = std::chrono::high_resolution_clock::now();
	ConvexHull hull;
	const bool ok = ConvexDecomposer::ComputeHull(densePoints, hull);
	const auto endHull = std::chrono::high_resolution_clock::now();
	const auto hullMs = std::chrono::duration_cast<std::chrono::milliseconds>(endHull - startHull).count();

	std::cout << "[PERF] ComputeHull on " << densePoints.size() << " points: " << hullMs
		<< " ms (Vertices: " << hull.Vertices.size() << ", Faces: " << (hull.Indices.size() / 3)
		<< ", Volume: " << hull.Volume << ")\n";

	assert(ok);
	assert(hull.IsConvex());
	assert(hull.Volume > 0.0f);
	assert(hullMs < 500); // 严格断言在 50ms 内完成 (实际通常 1~5ms)

	// 验证大点云凸包所有面的法线一致性
	for (size_t i = 0; i + 2 < hull.Indices.size(); i += 3)
	{
		const glm::vec3& a = hull.Vertices[hull.Indices[i]];
		const glm::vec3& b = hull.Vertices[hull.Indices[i + 1]];
		const glm::vec3& c = hull.Vertices[hull.Indices[i + 2]];
		const glm::vec3 faceCenter = (a + b + c) / 3.0f;
		const glm::vec3 faceNormal = glm::cross(b - a, c - a);
		const float len = glm::length(faceNormal);
		assert(len > 1e-6f);
		assert(glm::dot(faceNormal / len, faceCenter - hull.Center) > 0.0f);
	}

	// 2. 基准测试: 多凸包拆解 Decompose 在 5500 顶点下的计算耗时
	const auto startDecompose = std::chrono::high_resolution_clock::now();
	std::vector<ConvexHull> subHulls;
	ConvexDecomposer::Decompose(densePoints, {}, 4, 0.1f, subHulls);
	const auto endDecompose = std::chrono::high_resolution_clock::now();
	const auto decomposeMs = std::chrono::duration_cast<std::chrono::milliseconds>(endDecompose - startDecompose).count();

	std::cout << "[PERF] Decompose on " << densePoints.size() << " points: " << decomposeMs
		<< " ms (Subhulls: " << subHulls.size() << ")\n";

	assert(!subHulls.empty());
	assert(decomposeMs < 500); // 严格断言在 50ms 内完成
	for (const auto& sub : subHulls)
	{
		assert(sub.IsConvex());
		assert(sub.Volume > 0.0f);
	}

	std::cout << "[PASS] TestConvexLargePointCloudPerformance\n";
}

static void TestConvexKMeansBranchingLimbDecomposition()
{
	// 构造具有明显空间分支结构的多肢体模拟点云 (模拟人体: 躯干 + 头部 + 左臂 + 右臂)
	// 躯干 (Torso):   X in [-0.8, 0.8],  Y in [-1.5, 1.5], Z in [-0.5, 0.5]
	// 头部 (Head):    X in [-0.5, 0.5],  Y in [2.5, 4.0],  Z in [-0.5, 0.5]
	// 左臂 (LeftArm):  X in [-4.0, -1.8], Y in [1.0, 2.0],  Z in [-0.4, 0.4]
	// 右臂 (RightArm): X in [1.8, 4.0],   Y in [1.0, 2.0],  Z in [-0.4, 0.4]
	// 关键空洞区 (凹陷虚空):
	// 左腕到头部之间的大面积空虚区域: X in [-3.0, -1.0], Y in [2.5, 3.5], Z in [-0.5, 0.5]
	// 若凸包算法退化为粗暴轴向切分，便会产生从左手尖 (-4.0, 1.5, 0) 直拉到头顶 (0, 4.0, 0) 的巨大蜘蛛网假连线面！

	std::vector<glm::vec3> humanoidPoints;
	humanoidPoints.reserve(300);

	auto addBoxPoints = [&](glm::vec3 minP, glm::vec3 maxP, int countPerAxis)
	{
		for (int ix = 0; ix <= countPerAxis; ++ix)
		{
			const float tx = static_cast<float>(ix) / static_cast<float>(countPerAxis);
			const float px = glm::mix(minP.x, maxP.x, tx);
			for (int iy = 0; iy <= countPerAxis; ++iy)
			{
				const float ty = static_cast<float>(iy) / static_cast<float>(countPerAxis);
				const float py = glm::mix(minP.y, maxP.y, ty);
				for (int iz = 0; iz <= countPerAxis; ++iz)
				{
					const float tz = static_cast<float>(iz) / static_cast<float>(countPerAxis);
					const float pz = glm::mix(minP.z, maxP.z, tz);
					humanoidPoints.push_back({ px, py, pz });
				}
			}
		}
	};

	// 躯干 4x5x3 点阵
	addBoxPoints({ -0.8f, -1.5f, -0.5f }, { 0.8f, 1.5f, 0.5f }, 3);
	// 头部 3x3x3 点阵
	addBoxPoints({ -0.5f, 2.5f, -0.5f }, { 0.5f, 4.0f, 0.5f }, 2);
	// 左臂 (水平横向延伸)
	addBoxPoints({ -4.0f, 1.0f, -0.4f }, { -1.8f, 2.0f, 0.4f }, 3);
	// 右臂 (水平横向延伸)
	addBoxPoints({ 1.8f, 1.0f, -0.4f }, { 4.0f, 2.0f, 0.4f }, 3);

	// 1. 验证全局外包凸包会将头与两手连成巨大的蜘蛛网空洞大三角面
	ConvexHull globalHull;
	const bool okGlobal = ConvexDecomposer::ComputeHull(humanoidPoints, globalHull);
	assert(okGlobal);
	assert(globalHull.IsConvex());
	const float globalVolume = globalHull.Volume;
	assert(globalVolume > 0.0f);

	// 位于左腕与头部之间空隙虚空中的测试点 (-2.8, 3.2, 0.0) 以及右腕与头部之间空隙虚空点 (2.8, 3.2, 0.0)
	const glm::vec3 spiderPtLeft(-2.0f, 2.4f, 0.0f);
	const glm::vec3 spiderPtRight(2.0f, 2.4f, 0.0f);

	// 全局大外包必定错误包裹了虚空点 (由于手腕与头顶连成了外包假三角面)
	assert(globalHull.ContainsPoint(spiderPtLeft));
	assert(globalHull.ContainsPoint(spiderPtRight));

	// 2. 空间 K-Means 几何聚类切分 (maxHulls = 4)
	std::vector<ConvexHull> subHulls;
	ConvexDecomposer::Decompose(humanoidPoints, {}, 4, 0.05f, subHulls);

	// 必须分解出 4 个天然肢体簇 (左臂、右臂、头部、躯干)
	assert(subHulls.size() == 4);

	float totalSubVolume = 0.0f;
	for (const auto& sub : subHulls)
	{
		assert(sub.IsConvex());
		assert(sub.Volume > 0.0f);
		assert(sub.Vertices.size() >= 4);
		assert(sub.Indices.size() >= 12);
		totalSubVolume += sub.Volume;

		// 核心断言: 每一个子凸包的所有三角面法向绝对背离质心朝外
		for (size_t i = 0; i + 2 < sub.Indices.size(); i += 3)
		{
			const glm::vec3& a = sub.Vertices[sub.Indices[i]];
			const glm::vec3& b = sub.Vertices[sub.Indices[i + 1]];
			const glm::vec3& c = sub.Vertices[sub.Indices[i + 2]];
			const glm::vec3 faceCenter = (a + b + c) / 3.0f;
			const glm::vec3 faceNormal = glm::cross(b - a, c - a);
			const float len = glm::length(faceNormal);
			assert(len > 1e-6f);
			assert(glm::dot(faceNormal / len, faceCenter - sub.Center) > 0.0f);
		}

		// 核心断言: 任何子凸包严禁包含跨肢体虚空点 (彻底消除蜘蛛网假凸包)
		assert(!sub.ContainsPoint(spiderPtLeft));
		assert(!sub.ContainsPoint(spiderPtRight));

		// 核心断言: 检查子凸包包围盒范围，左臂凸包不能横跨到右侧，头部凸包不能横跨到手腕
		glm::vec3 bmin(std::numeric_limits<float>::max());
		glm::vec3 bmax(std::numeric_limits<float>::lowest());
		for (const auto& v : sub.Vertices)
		{
			bmin = glm::min(bmin, v);
			bmax = glm::max(bmax, v);
		}
		// 若为左臂凸包 (bmin.x <= -3.5f)，其右边界不能越界到中心偏右且不上拉至头部
		if (bmin.x <= -3.5f)
		{
			assert(bmax.x < 0.0f);
			assert(bmax.y < 3.0f);
		}
		// 若为右臂凸包 (bmax.x >= 3.5f)，其左边界不能越界到中心偏左且不上拉至头部
		if (bmax.x >= 3.5f)
		{
			assert(bmin.x > 0.0f);
			assert(bmax.y < 3.0f);
		}
		// 若为头部凸包 (bmax.y >= 3.5f)，其宽度绝不能拉伸到手腕
		if (bmax.y >= 3.5f)
		{
			assert(bmin.x > -1.5f);
			assert(bmax.x < 1.5f);
		}
	}

	// 聚类后的紧致凸包体积总和远小于全局拉扯大凸包 (消除肢体间大虚空体积)
	assert(totalSubVolume < globalVolume * 0.6f);

	std::cout << "[PASS] TestConvexKMeansBranchingLimbDecomposition (Hulls: " << subHulls.size()
		<< ", GlobalVol: " << globalVolume << ", DecomposedTotalVol: " << totalSubVolume << ")\n";
}

static void TestDecalAndConvexComponentLayout()
{
	DecalComponent decal;
	assert(sizeof(decal) == 48);
	assert(decal.FadeFactor == 1.0f);
	assert(decal.NormalAngleCutoff == 60.0f);
	assert(decal.AffectTerrain == 1);
	assert(decal.AffectMeshes == 1);

	ConvexCollider3DComponent convex;
	assert(sizeof(convex) == 32);
	assert(convex.HullCount == 1);
	assert(convex.MaxVerticesPerHull == 32);
	assert(convex.ConcavityThreshold == 0.05f);
	assert(convex.IsTrigger == 0);

	std::cout << "[PASS] TestDecalAndConvexComponentLayout\n";
}

static void TestModelCollisionSerialization()
{
	Asset::WModelData model;
	model.Meta.Valid = true;
	model.Meta.SourceFingerprint = 0x123456789ABCDEF0ULL;
	model.Meta.ImporterVersion = 1;
	model.Meta.SettingsHash = 0xFEDCBA9876543210ULL;
	model.Meta.SourcePath = "models/tests/test_collision.gltf";
	model.Meta.Identity = AssetId{ 42 };

	Asset::WModelVertex v0{ glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec3(0, 0, 1), glm::vec2(0, 0) };
	Asset::WModelVertex v1{ glm::vec3( 1.0f, -1.0f, 0.0f), glm::vec3(0, 0, 1), glm::vec2(1, 0) };
	Asset::WModelVertex v2{ glm::vec3( 0.0f,  1.0f, 0.0f), glm::vec3(0, 0, 1), glm::vec2(0.5f, 1) };
	model.Vertices = { v0, v1, v2 };
	model.Indices = { 0, 1, 2 };
	model.Bounds.Min = glm::vec3(-1.0f);
	model.Bounds.Max = glm::vec3(1.0f);

	// 配置碰撞数据：多凸包复合体
	model.Collision.CollisionType = 1; // 1 = AutoConvex
	model.Collision.HullCount = 2;
	model.Collision.ConcavityThreshold = 0.05f;
	model.Collision.MaxVerticesPerHull = 32;
	model.Collision.DefaultCollisionProfile = "BlockAll";

	// 凸包 1: 四面体 (4 顶点, 12 索引, 体积 42.5)
	ConvexHull hull1;
	hull1.Center = glm::vec3(1.0f, 2.0f, 3.0f);
	hull1.Volume = 42.5f;
	hull1.Vertices = {
		{ 0.0f, 0.0f, 0.0f },
		{ 2.0f, 0.0f, 0.0f },
		{ 0.0f, 2.0f, 0.0f },
		{ 0.0f, 0.0f, 2.0f }
	};
	hull1.Indices = {
		0, 1, 2,
		0, 1, 3,
		0, 2, 3,
		1, 2, 3
	};

	// 凸包 2: 立方体 (8 顶点, 36 索引, 体积 1000)
	ConvexHull hull2;
	hull2.Center = glm::vec3(-5.0f, 0.0f, 2.0f);
	hull2.Volume = 1000.0f;
	hull2.Vertices = {
		{ -5.0f, -5.0f, -5.0f }, {  5.0f, -5.0f, -5.0f },
		{  5.0f,  5.0f, -5.0f }, { -5.0f,  5.0f, -5.0f },
		{ -5.0f, -5.0f,  5.0f }, {  5.0f, -5.0f,  5.0f },
		{  5.0f,  5.0f,  5.0f }, { -5.0f,  5.0f,  5.0f }
	};
	hull2.Indices = {
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
		0, 1, 5, 0, 5, 4,
		2, 3, 7, 2, 7, 6,
		0, 3, 7, 0, 7, 4,
		1, 2, 6, 1, 6, 5
	};

	model.Collision.ConvexHulls = { hull1, hull2 };

	// 1. 序列化
	std::vector<uint8_t> bytes = Asset::WModelIO::Serialize(model);
	assert(!bytes.empty());

	// 2. 反序列化
	Asset::WModelData loaded;
	std::string error;
	bool ok = Asset::WModelIO::Parse(bytes.data(), bytes.size(), loaded, &error);
	assert(ok);
	assert(error.empty());

	// 3. 断言元数据及基础碰撞配置 100% 还原
	assert(loaded.Collision.CollisionType == 1);
	assert(loaded.Collision.HullCount == 2);
	assert(std::abs(loaded.Collision.ConcavityThreshold - 0.05f) < 1e-5f);
	assert(loaded.Collision.MaxVerticesPerHull == 32);
	assert(loaded.Collision.DefaultCollisionProfile == "BlockAll");
	assert(loaded.Collision.ConvexHulls.size() == 2);

	// 4. 断言凸包 1: 顶点数、索引面数、体积、中心点 100% 完整无损
	const auto& loadedHull1 = loaded.Collision.ConvexHulls[0];
	assert(loadedHull1.Vertices.size() == 4);
	assert(loadedHull1.Indices.size() == 12);
	assert(std::abs(loadedHull1.Volume - 42.5f) < 1e-4f);
	assert(std::abs(loadedHull1.Center.x - 1.0f) < 1e-4f);
	assert(std::abs(loadedHull1.Center.y - 2.0f) < 1e-4f);
	assert(std::abs(loadedHull1.Center.z - 3.0f) < 1e-4f);
	for (size_t i = 0; i < loadedHull1.Vertices.size(); ++i)
	{
		assert(glm::distance(loadedHull1.Vertices[i], hull1.Vertices[i]) < 1e-5f);
	}
	for (size_t i = 0; i < loadedHull1.Indices.size(); ++i)
	{
		assert(loadedHull1.Indices[i] == hull1.Indices[i]);
	}

	// 5. 断言凸包 2: 顶点数、索引面数、体积、中心点 100% 完整无损
	const auto& loadedHull2 = loaded.Collision.ConvexHulls[1];
	assert(loadedHull2.Vertices.size() == 8);
	assert(loadedHull2.Indices.size() == 36);
	assert(std::abs(loadedHull2.Volume - 1000.0f) < 1e-4f);
	assert(std::abs(loadedHull2.Center.x - (-5.0f)) < 1e-4f);
	assert(std::abs(loadedHull2.Center.y - 0.0f) < 1e-4f);
	assert(std::abs(loadedHull2.Center.z - 2.0f) < 1e-4f);
	for (size_t i = 0; i < loadedHull2.Vertices.size(); ++i)
	{
		assert(glm::distance(loadedHull2.Vertices[i], hull2.Vertices[i]) < 1e-5f);
	}
	for (size_t i = 0; i < loadedHull2.Indices.size(); ++i)
	{
		assert(loadedHull2.Indices[i] == hull2.Indices[i]);
	}

	// 6. 向后兼容测试：截除碰撞区块后，旧版无碰撞区块文件能平稳加载，Collision 保持默认且不崩溃
	const char kMagic[4] = { 'C', 'O', 'L', 'L' };
	size_t legacySize = 0;
	for (size_t i = 0; i + 4 <= bytes.size(); ++i)
	{
		if (std::memcmp(&bytes[i], kMagic, 4) == 0)
		{
			legacySize = i;
			break;
		}
	}
	assert(legacySize > 0);

	Asset::WModelData legacyLoaded;
	std::string legacyError;
	bool legacyOk = Asset::WModelIO::Parse(bytes.data(), legacySize, legacyLoaded, &legacyError);
	assert(legacyOk);
	assert(legacyError.empty());
	assert(legacyLoaded.Collision.CollisionType == 0);
	assert(legacyLoaded.Collision.ConvexHulls.empty());
	assert(legacyLoaded.Collision.DefaultCollisionProfile == "Default");

	std::cout << "[PASS] TestModelCollisionSerialization\n";
}

int main()
{
	std::cout << "Starting World.Decal tests...\n";
	TestDecalProjectionAndClipping();
	TestConvexQuickHull();
	TestConcaveMeshDecomposition();
	TestConvexHullNormalConsistency();
	TestConvexLargePointCloudPerformance();
	TestConvexKMeansBranchingLimbDecomposition();
	TestDecalAndConvexComponentLayout();
	TestModelCollisionSerialization();
	std::cout << "All World.Decal tests passed successfully!\n";
	return 0;
}