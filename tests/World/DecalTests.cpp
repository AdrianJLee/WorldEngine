#include "World/Renderer/Decal.h"
#include "World/Physics/ConvexDecomposition.h"
#include "World/Scene/Components.h"
#include "World/Asset/WModelIO.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

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
	TestDecalAndConvexComponentLayout();
	TestModelCollisionSerialization();
	std::cout << "All World.Decal tests passed successfully!\n";
	return 0;
}