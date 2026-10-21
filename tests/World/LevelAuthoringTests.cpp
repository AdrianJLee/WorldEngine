#include "World/Level/Spline3D.h"
#include "World/Level/WhiteboxBuilder.h"
#include "World/Level/PhysicsPainter.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestSpline3DAndRoadExtrusion()
{
	Spline3D spline;
	spline.AddPoint(glm::vec3(0.0f, 0.0f, 0.0f));
	spline.AddPoint(glm::vec3(10.0f, 0.0f, 0.0f));
	spline.AddPoint(glm::vec3(20.0f, 0.0f, 10.0f));
	spline.AddPoint(glm::vec3(30.0f, 0.0f, 10.0f));

	assert(spline.GetPointCount() == 4);

	// 1. 参数采样
	glm::vec3 pos0, tan0;
	spline.SamplePoint(0.0f, pos0, tan0);
	assert(std::abs(pos0.x - 0.0f) < 1e-3f);

	glm::vec3 pos1, tan1;
	spline.SamplePoint(1.0f, pos1, tan1);
	assert(std::abs(pos1.x - 30.0f) < 1e-3f);

	// 2. 弧长累加
	float len = spline.GetTotalLength();
	assert(len > 30.0f);

	// 3. 沿线放样生成道路网格
	std::vector<glm::vec3> verts, normals;
	std::vector<glm::vec2> uvs;
	std::vector<uint32_t> indices;
	spline.GenerateRoadMesh(4.0f, 2.0f, verts, normals, uvs, indices);

	assert(!verts.empty());
	assert(!indices.empty());
	assert(verts.size() == normals.size());
	assert(verts.size() == uvs.size());
	assert(indices.size() % 6 == 0); // 每个四边形 2 个三角形 6 个索引

	std::cout << "[PASS] TestSpline3DAndRoadExtrusion (Road Verts: " << verts.size() << ")\n";
}

static void TestWhiteboxBuilder()
{
	// 1. Box
	WhiteboxMesh box;
	WhiteboxBuilder::BuildBox(glm::vec3(2.0f, 4.0f, 6.0f), 0.0f, box);
	assert(box.Vertices.size() == 24);
	assert(box.Indices.size() == 36);

	// 2. Cylinder
	WhiteboxMesh cylinder;
	WhiteboxBuilder::BuildCylinder(1.5f, 5.0f, 16, cylinder);
	assert(!cylinder.Vertices.empty());
	assert(!cylinder.Indices.empty());

	// 3. Stairs
	WhiteboxMesh stairs;
	WhiteboxBuilder::BuildStairs(glm::vec3(2.0f, 3.0f, 6.0f), 6, stairs);
	assert(stairs.Vertices.size() == 6 * 8); // 6 级台阶，每级 8 个顶点 (踏面4 + 踢面4)
	assert(stairs.Indices.size() == 6 * 12); // 每级 12 个索引

	std::cout << "[PASS] TestWhiteboxBuilder\n";
}

static void TestPhysicsPainterSettle()
{
	TerrainData terrain(32, 32, 1.0f, 1.0f);
	// 制作一个高度为 15.0f 的平台
	for (uint32_t z = 0; z < 32; ++z)
		for (uint32_t x = 0; x < 32; ++x)
			terrain.SetHeight(x, z, 15.0f);

	std::vector<glm::vec3> drops = {
		{ 10.0f, 50.0f, 10.0f },
		{ 20.0f, 60.0f, 20.0f }
	};
	const glm::vec3 halfExtents(1.0f, 1.0f, 1.0f);

	std::vector<glm::mat4> settled;
	PhysicsPainter::SettleOnTerrain(drops, halfExtents, terrain, settled);

	assert(settled.size() == 2);
	// 沉降高度 = 地面高度 15.0 + 半尺寸 1.0 = 16.0
	assert(std::abs(settled[0][3][1] - 16.0f) < 1e-4f);
	assert(std::abs(settled[1][3][1] - 16.0f) < 1e-4f);

	std::cout << "[PASS] TestPhysicsPainterSettle\n";
}

static void TestLevelAuthoringComponentLayout()
{
	SplineComponent spline;
	assert(sizeof(spline) == 48);
	assert(spline.MeshSpacing == 2.0f);
	assert(spline.RoadWidth == 4.0f);

	WhiteboxComponent whitebox;
	assert(sizeof(whitebox) == 32);
	assert(whitebox.Shape == WhiteboxComponent::ShapeType::Box);
	assert(whitebox.Steps == 8);

	std::cout << "[PASS] TestLevelAuthoringComponentLayout\n";
}

int main()
{
	std::cout << "Starting World.LevelAuthoring tests...\n";
	TestSpline3DAndRoadExtrusion();
	TestWhiteboxBuilder();
	TestPhysicsPainterSettle();
	TestLevelAuthoringComponentLayout();
	std::cout << "All World.LevelAuthoring tests passed successfully!\n";
	return 0;
}
