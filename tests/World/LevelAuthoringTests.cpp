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

	// 1. 参数采样 (Catmull-Rom)
	glm::vec3 pos0, tan0;
	spline.SamplePoint(0.0f, pos0, tan0);
	assert(std::abs(pos0.x - 0.0f) < 1e-3f);

	glm::vec3 pos1, tan1;
	spline.SamplePoint(1.0f, pos1, tan1);
	assert(std::abs(pos1.x - 30.0f) < 1e-3f);

	// 2. 弧长累加与等距采样
	float len = spline.GetTotalLength();
	assert(len > 30.0f);

	glm::vec3 dPos0, dTan0, dPosHalf, dTanHalf;
	spline.SampleAtDistance(0.0f, dPos0, dTan0);
	assert(std::abs(dPos0.x - 0.0f) < 1e-2f);

	spline.SampleAtDistance(len * 0.5f, dPosHalf, dTanHalf);
	assert(glm::length(dTanHalf) > 0.99f);

	// 3. 沿线放样生成开环道路网格
	std::vector<glm::vec3> verts, normals;
	std::vector<glm::vec2> uvs;
	std::vector<uint32_t> indices;
	spline.GenerateRoadMesh(4.0f, 2.0f, verts, normals, uvs, indices);

	assert(!verts.empty());
	assert(!indices.empty());
	assert(verts.size() == normals.size());
	assert(verts.size() == uvs.size());
	assert(indices.size() % 6 == 0); // 每个四边形 2 个三角形 6 个索引

	// 4. 闭环路网格
	spline.SetClosedLoop(true);
	assert(spline.IsClosedLoop());
	std::vector<glm::vec3> cVerts, cNormals;
	std::vector<glm::vec2> cUVs;
	std::vector<uint32_t> cIndices;
	spline.GenerateRoadMesh(4.0f, 2.0f, cVerts, cNormals, cUVs, cIndices);
	assert(!cVerts.empty());
	assert(cIndices.size() % 6 == 0);

	// 5. Bezier 曲线模式
	spline.SetClosedLoop(false);
	spline.SetMode(SplineMode::Bezier);
	assert(spline.GetMode() == SplineMode::Bezier);
	glm::vec3 bPos0, bTan0;
	spline.SamplePoint(0.0f, bPos0, bTan0);
	assert(std::abs(bPos0.x - 0.0f) < 1e-3f);

	// 6. 控制点编辑
	spline.SetPoint(1, glm::vec3(12.0f, 2.0f, 0.0f));
	assert(std::abs(spline.GetPoint(1).Position.x - 12.0f) < 1e-4f);
	spline.RemovePoint(3);
	assert(spline.GetPointCount() == 3);

	std::cout << "[PASS] TestSpline3DAndRoadExtrusion (Road Verts: " << verts.size() << ")\n";
}

static void TestWhiteboxBuilder()
{
	// 1. Box (24 顶点，36 索引)
	WhiteboxMesh box;
	WhiteboxBuilder::BuildBox(glm::vec3(2.0f, 4.0f, 6.0f), 0.0f, box);
	assert(box.Vertices.size() == 24);
	assert(box.Indices.size() == 36);

	// 2. Cylinder (侧面 + 上下底封盖)
	WhiteboxMesh cylinder;
	WhiteboxBuilder::BuildCylinder(1.5f, 5.0f, 16, cylinder);
	assert(!cylinder.Vertices.empty());
	assert(!cylinder.Indices.empty());

	// 3. Sphere (UV 球体，法线归一化)
	WhiteboxMesh sphere;
	WhiteboxBuilder::BuildSphere(2.0f, 16, 8, sphere);
	assert(!sphere.Vertices.empty());
	assert(!sphere.Indices.empty());
	for (const auto& n : sphere.Normals)
	{
		assert(std::abs(glm::length(n) - 1.0f) < 1e-4f);
	}

	// 4. Stairs (6 级台阶，踏面垂直 +Y，踢面垂直 -Z)
	WhiteboxMesh stairs;
	WhiteboxBuilder::BuildStairs(glm::vec3(2.0f, 3.0f, 6.0f), 6, stairs);
	assert(stairs.Vertices.size() == 6 * 8); // 6 级台阶，每级 8 个顶点 (踏面4 + 踢面4)
	assert(stairs.Indices.size() == 6 * 12); // 每级 12 个索引
	for (size_t i = 0; i < 6; ++i)
	{
		// 踏面 4 个顶点法线必为 (0, 1, 0)
		for (size_t k = 0; k < 4; ++k)
		{
			const auto& nT = stairs.Normals[i * 8 + k];
			assert(std::abs(nT.x) < 1e-4f && std::abs(nT.y - 1.0f) < 1e-4f && std::abs(nT.z) < 1e-4f);
		}
		// 踢面 4 个顶点法线必为 (0, 0, -1)
		for (size_t k = 4; k < 8; ++k)
		{
			const auto& nR = stairs.Normals[i * 8 + k];
			assert(std::abs(nR.x) < 1e-4f && std::abs(nR.y) < 1e-4f && std::abs(nR.z - (-1.0f)) < 1e-4f);
		}
	}

	// 5. Wedge (三角斜坡原型：底面矩形、垂直后背面矩形、两侧直角三角形、倾斜坡面矩形)
	WhiteboxMesh wedge;
	const glm::vec3 wedgeDim(4.0f, 3.0f, 8.0f);
	WhiteboxBuilder::BuildWedge(wedgeDim, wedge);
	assert(wedge.Vertices.size() == 18);
	assert(wedge.Indices.size() == 24);

	// 坡面法线验证：坡面倾斜方向为 (0, 3.0, 8.0)，法线必须严格精准垂直于斜面 (点积为 0)
	const glm::vec3 inclineDir = glm::normalize(glm::vec3(0.0f, wedgeDim.y, wedgeDim.z));
	// 坡面为第 3 个面 (顶点下标 8..11)
	for (size_t i = 8; i < 12; ++i)
	{
		const auto& n = wedge.Normals[i];
		assert(std::abs(glm::dot(n, inclineDir)) < 1e-5f);
		assert(n.y > 0.0f); // 向上
		assert(n.z < 0.0f); // 向前
	}

	// 6. Arch (拱形门廊原型)
	WhiteboxMesh arch;
	WhiteboxBuilder::BuildArch(glm::vec3(4.0f, 6.0f, 2.0f), 1.0f, arch);
	assert(!arch.Vertices.empty());
	assert(!arch.Indices.empty());

	// 7. WhiteboxBuilder::BuildMesh 静态分发
	WhiteboxComponent comp;
	WhiteboxMesh outMesh;
	comp.Shape = WhiteboxComponent::ShapeType::Box;
	WhiteboxBuilder::BuildMesh(comp, outMesh);
	assert(outMesh.Vertices.size() == 24);

	comp.Shape = WhiteboxComponent::ShapeType::Cylinder;
	WhiteboxBuilder::BuildMesh(comp, outMesh);
	assert(!outMesh.Vertices.empty());

	comp.Shape = WhiteboxComponent::ShapeType::Sphere;
	WhiteboxBuilder::BuildMesh(comp, outMesh);
	assert(!outMesh.Vertices.empty());

	comp.Shape = WhiteboxComponent::ShapeType::Stairs;
	WhiteboxBuilder::BuildMesh(comp, outMesh);
	assert(!outMesh.Vertices.empty());

	comp.Shape = WhiteboxComponent::ShapeType::Wedge;
	WhiteboxBuilder::BuildMesh(comp, outMesh);
	assert(!outMesh.Vertices.empty());

	comp.Shape = WhiteboxComponent::ShapeType::Arch;
	WhiteboxBuilder::BuildMesh(comp, outMesh);
	assert(!outMesh.Vertices.empty());

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

	// 测试散布点生成
	std::vector<glm::vec3> scatters = PhysicsPainter::ScatterPoints(
		glm::vec3(15.0f, 0.0f, 15.0f), 10.0f, 20, 1.0f, 2026);
	assert(!scatters.empty());
	for (const auto& pt : scatters)
	{
		const float distCenter = glm::distance(glm::vec2(pt.x, pt.z), glm::vec2(15.0f, 15.0f));
		assert(distCenter <= 10.05f);
	}

	std::cout << "[PASS] TestPhysicsPainterSettle\n";
}

static void TestLevelAuthoringComponentLayout()
{
	SplineComponent spline;
	assert(sizeof(spline) == 32);
	assert(spline.MeshSpacing == 2.0f);
	assert(spline.RoadWidth == 4.0f);
	assert(spline.ClosedLoop == 0);
	assert(spline.Mode == 0);

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
