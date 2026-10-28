#include "World/Renderer/MeshSimplifier.h"
#include "World/Renderer/MeshLODGenerator.h"
#include "World/Asset/WModelIO.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace World;

// 生成 24 顶点、12 三角形的立方体网格
static void CreateCubeMesh(std::vector<WModelVertex>& outVerts, std::vector<uint32_t>& outIndices)
{
	outVerts.clear();
	outIndices.clear();

	struct FaceDef
	{
		glm::vec3 normal;
		glm::vec3 p0, p1, p2, p3;
	};

	FaceDef faces[6] = {
		// Front
		{ {0, 0, 1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1} },
		// Back
		{ {0, 0, -1}, {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1} },
		// Left
		{ {-1, 0, 0}, {-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1} },
		// Right
		{ {1, 0, 0}, {1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1} },
		// Top
		{ {0, 1, 0}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1} },
		// Bottom
		{ {0, -1, 0}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1} },
	};

	for (int f = 0; f < 6; ++f)
	{
		uint32_t baseIdx = static_cast<uint32_t>(outVerts.size());

		WModelVertex v0{ faces[f].p0, faces[f].normal, {0.0f, 0.0f} };
		WModelVertex v1{ faces[f].p1, faces[f].normal, {1.0f, 0.0f} };
		WModelVertex v2{ faces[f].p2, faces[f].normal, {1.0f, 1.0f} };
		WModelVertex v3{ faces[f].p3, faces[f].normal, {0.0f, 1.0f} };

		outVerts.push_back(v0);
		outVerts.push_back(v1);
		outVerts.push_back(v2);
		outVerts.push_back(v3);

		// Triangle 1
		outIndices.push_back(baseIdx + 0);
		outIndices.push_back(baseIdx + 1);
		outIndices.push_back(baseIdx + 2);

		// Triangle 2
		outIndices.push_back(baseIdx + 0);
		outIndices.push_back(baseIdx + 2);
		outIndices.push_back(baseIdx + 3);
	}

	assert(outVerts.size() == 24);
	assert(outIndices.size() == 36);
}

// 生成细分 UV 球体网格
static void CreateSphereMesh(uint32_t rings, uint32_t sectors,
                             std::vector<WModelVertex>& outVerts,
                             std::vector<uint32_t>& outIndices)
{
	outVerts.clear();
	outIndices.clear();

	const float kPi = 3.14159265358979323846f;
	float const R = 1.0f / static_cast<float>(rings - 1);
	float const S = 1.0f / static_cast<float>(sectors - 1);

	for (uint32_t r = 0; r < rings; ++r)
	{
		for (uint32_t s = 0; s < sectors; ++s)
		{
			float const y = std::sin(-kPi * 0.5f + kPi * static_cast<float>(r) * R);
			float const x = std::cos(2.0f * kPi * static_cast<float>(s) * S) * std::sin(kPi * static_cast<float>(r) * R);
			float const z = std::sin(2.0f * kPi * static_cast<float>(s) * S) * std::sin(kPi * static_cast<float>(r) * R);

			glm::vec3 pos(x, y, z);
			glm::vec3 norm = glm::normalize(pos);
			glm::vec2 uv(static_cast<float>(s) * S, static_cast<float>(r) * R);

			outVerts.push_back({ pos, norm, uv });
		}
	}

	for (uint32_t r = 0; r < rings - 1; ++r)
	{
		for (uint32_t s = 0; s < sectors - 1; ++s)
		{
			uint32_t cur = r * sectors + s;
			uint32_t next = cur + sectors;

			outIndices.push_back(cur);
			outIndices.push_back(cur + 1);
			outIndices.push_back(next + 1);

			outIndices.push_back(cur);
			outIndices.push_back(next + 1);
			outIndices.push_back(next);
		}
	}
}

static void TestCubeSimplification()
{
	std::vector<WModelVertex> verts;
	std::vector<uint32_t> indices;
	CreateCubeMesh(verts, indices);

	SimplificationOptions options;
	options.TargetRatio = 0.5f;
	options.PreserveBorders = true;
	options.CheckNormalFlip = true;

	SimplificationResult result;
	std::string error;
	bool ok = MeshSimplifier::Simplify(verts.data(), verts.size(),
	                                   indices.data(), indices.size(),
	                                   options, result, &error);

	assert(ok && "MeshSimplifier::Simplify failed on cube");
	assert(result.OriginalTriangles == 12);
	assert(result.SimplifiedTriangles <= 12 && result.SimplifiedTriangles > 0);
	assert(result.Indices.size() == result.SimplifiedTriangles * 3);

	// 验证所有索引在合法顶点区间内
	for (uint32_t idx : result.Indices)
	{
		assert(idx < result.Vertices.size() && "Index out of vertex buffer bounds");
	}

	// 验证无未引用悬空顶点
	std::vector<bool> referenced(result.Vertices.size(), false);
	for (uint32_t idx : result.Indices)
	{
		referenced[idx] = true;
	}
	for (size_t i = 0; i < referenced.size(); ++i)
	{
		assert(referenced[i] && "Dangling unreferenced vertex found in simplified output");
	}

	std::cout << "[PASS] TestCubeSimplification (Orig: " << result.OriginalTriangles
	          << ", Simp: " << result.SimplifiedTriangles
	          << ", Ratio: " << result.ActualRatio << ")\n";
}

static void TestSphereSimplification()
{
	std::vector<WModelVertex> verts;
	std::vector<uint32_t> indices;
	CreateSphereMesh(16, 16, verts, indices);

	uint32_t origTriangles = static_cast<uint32_t>(indices.size() / 3);
	assert(origTriangles >= 400);

	// 1. 50% 减面测试
	{
		SimplificationOptions options;
		options.TargetRatio = 0.5f;
		SimplificationResult result50;
		bool ok = MeshSimplifier::Simplify(verts.data(), verts.size(),
		                                   indices.data(), indices.size(),
		                                   options, result50);
		assert(ok);
		assert(result50.OriginalTriangles == origTriangles);
		assert(result50.SimplifiedTriangles < origTriangles);
		assert(result50.ActualRatio <= 0.65f && result50.ActualRatio >= 0.35f);

		// 验证索引范围与闭合性
		for (uint32_t idx : result50.Indices)
		{
			assert(idx < result50.Vertices.size());
		}
	}

	// 2. 25% 减面测试
	{
		SimplificationOptions options;
		options.TargetRatio = 0.25f;
		SimplificationResult result25;
		bool ok = MeshSimplifier::Simplify(verts.data(), verts.size(),
		                                   indices.data(), indices.size(),
		                                   options, result25);
		assert(ok);
		assert(result25.OriginalTriangles == origTriangles);
		assert(result25.SimplifiedTriangles < origTriangles);
		assert(result25.ActualRatio <= 0.40f && result25.ActualRatio >= 0.15f);

		for (uint32_t idx : result25.Indices)
		{
			assert(idx < result25.Vertices.size());
		}
	}

	std::cout << "[PASS] TestSphereSimplification (Sphere 50% and 25% passed)\n";
}

static void TestDegenerateAndEdgeCases()
{
	// 1. 空输入测试
	{
		SimplificationOptions options;
		SimplificationResult result;
		std::string err;
		bool ok = MeshSimplifier::Simplify(nullptr, 0, nullptr, 0, options, result, &err);
		assert(!ok && "Empty input should fail gracefully");
	}

	// 2. 只有 1 个三角形的极小网格测试
	{
		std::vector<WModelVertex> triVerts = {
			{ {0, 0, 0}, {0, 1, 0}, {0, 0} },
			{ {1, 0, 0}, {0, 1, 0}, {1, 0} },
			{ {0, 1, 0}, {0, 1, 0}, {0, 1} }
		};
		std::vector<uint32_t> triIndices = { 0, 1, 2 };

		SimplificationOptions options;
		options.TargetRatio = 0.5f;
		SimplificationResult result;
		bool ok = MeshSimplifier::Simplify(triVerts.data(), triVerts.size(),
		                                   triIndices.data(), triIndices.size(),
		                                   options, result);
		assert(ok);
		assert(result.SimplifiedTriangles == 1 && "Single triangle cannot be reduced further");
	}

	// 3. 包含退化面（共线/重复顶点）网格测试
	{
		std::vector<WModelVertex> degenVerts = {
			{ {0, 0, 0}, {0, 1, 0}, {0, 0} },
			{ {1, 0, 0}, {0, 1, 0}, {1, 0} },
			{ {2, 0, 0}, {0, 1, 0}, {2, 0} }
		};
		std::vector<uint32_t> degenIndices = { 0, 1, 2 }; // 三点共线退化面

		SimplificationOptions options;
		SimplificationResult result;
		bool ok = MeshSimplifier::Simplify(degenVerts.data(), degenVerts.size(),
		                                   degenIndices.data(), degenIndices.size(),
		                                   options, result);
		assert(ok);
		assert(result.SimplifiedTriangles == 0);
	}

	std::cout << "[PASS] TestDegenerateAndEdgeCases\n";
}

static void TestMeshLODGenerator()
{
	// 构建带 2 个 Submesh 的 WModelData
	Asset::WModelData model;
	model.Meta.Valid = true;
	model.MaterialSlots = { "materials/matA.wmat", "materials/matB.wmat" };

	std::vector<WModelVertex> cubeA, cubeB;
	std::vector<uint32_t> idxA, idxB;
	CreateCubeMesh(cubeA, idxA);
	CreateCubeMesh(cubeB, idxB);

	// Submesh 0
	uint32_t baseA = 0;
	model.Vertices.insert(model.Vertices.end(), cubeA.begin(), cubeA.end());
	model.Indices.insert(model.Indices.end(), idxA.begin(), idxA.end());

	Asset::WModelSubmesh sub0;
	sub0.IndexOffset = 0;
	sub0.IndexCount = static_cast<uint32_t>(idxA.size());
	sub0.MaterialSlot = 0;
	sub0.Bounds.Min = { -1, -1, -1 };
	sub0.Bounds.Max = { 1, 1, 1 };
	model.Submeshes.push_back(sub0);

	// Submesh 1 (平移到 x = 5)
	uint32_t baseB = static_cast<uint32_t>(model.Vertices.size());
	for (auto& v : cubeB)
	{
		v.Position.x += 5.0f;
		model.Vertices.push_back(v);
	}
	for (uint32_t idx : idxB)
	{
		model.Indices.push_back(idx + baseB);
	}

	Asset::WModelSubmesh sub1;
	sub1.IndexOffset = static_cast<uint32_t>(sub0.IndexCount);
	sub1.IndexCount = static_cast<uint32_t>(idxB.size());
	sub1.MaterialSlot = 1;
	sub1.Bounds.Min = { 4, -1, -1 };
	sub1.Bounds.Max = { 6, 1, 1 };
	model.Submeshes.push_back(sub1);

	model.Bounds.Min = { -1, -1, -1 };
	model.Bounds.Max = { 6, 1, 1 };

	std::vector<LODLevelConfig> configs = {
		{ 1, 0.5f, 30.0f },
		{ 2, 0.25f, 60.0f }
	};

	std::vector<GeneratedLODAsset> outLODs;
	std::string err;
	bool ok = MeshLODGenerator::GenerateLODs(model, configs, outLODs, &err);
	assert(ok && "MeshLODGenerator::GenerateLODs failed");
	assert(outLODs.size() == 2);

	// 验证 LOD1
	assert(outLODs[0].Level == 1);
	assert(outLODs[0].ModelData.Submeshes.size() == 2);
	assert(outLODs[0].ModelData.Submeshes[0].MaterialSlot == 0);
	assert(outLODs[0].ModelData.Submeshes[1].MaterialSlot == 1);
	assert(outLODs[0].ModelData.MaterialSlots.size() == 2);
	assert(outLODs[0].TriangleCount <= 24);

	// 验证 LOD2
	assert(outLODs[1].Level == 2);
	assert(outLODs[1].ModelData.Submeshes.size() == 2);
	assert(outLODs[1].ModelData.Submeshes[0].MaterialSlot == 0);
	assert(outLODs[1].ModelData.Submeshes[1].MaterialSlot == 1);
	assert(outLODs[1].TriangleCount <= outLODs[0].TriangleCount);

	// 磁盘写出与读回验证
	std::string tempPath = "build/temp_test_model.wmodel";
	std::filesystem::create_directories("build");
	bool writeOk = Asset::WModelIO::WriteFile(tempPath, model, &err);
	assert(writeOk && "WriteFile source temp model failed");

	std::vector<GeneratedLODAsset> fileLODs;
	bool saveOk = MeshLODGenerator::GenerateAndSaveLODFiles(tempPath, configs, fileLODs, &err);
	if (!saveOk) std::cerr << "GenerateAndSaveLODFiles error: " << err << std::endl;
	assert(saveOk && "GenerateAndSaveLODFiles failed");
	assert(fileLODs.size() == 2);

	for (const auto& flod : fileLODs)
	{
		assert(std::filesystem::exists(flod.RelativePath));
		Asset::WModelData loadedLOD;
		bool readOk = Asset::WModelIO::ReadFile(flod.RelativePath, loadedLOD, &err);
		assert(readOk && "Read generated LOD file failed");
		assert(loadedLOD.Submeshes.size() == 2);
		std::filesystem::remove(flod.RelativePath);
	}
	std::filesystem::remove(tempPath);

	std::cout << "[PASS] TestMeshLODGenerator\n";
}

static Asset::WModelData BuildPrimitiveModelData(const std::vector<WModelVertex>& verts,
                                                 const std::vector<uint32_t>& indices,
                                                 const std::string& name)
{
	Asset::WModelData data;
	data.Meta.Valid = true;
	data.Meta.SourceFingerprint = 0x12345678ABCDULL;
	data.Meta.ImporterVersion = 1;
	data.Meta.Scale = 1.0f;
	data.Vertices = verts;
	data.Indices = indices;

	glm::vec3 bMin(1e9f), bMax(-1e9f);
	for (const auto& v : verts)
	{
		bMin = glm::min(bMin, v.Position);
		bMax = glm::max(bMax, v.Position);
	}
	data.Bounds.Min = bMin;
	data.Bounds.Max = bMax;

	Asset::WModelSubmesh sub;
	sub.IndexOffset = 0;
	sub.IndexCount = static_cast<uint32_t>(indices.size());
	sub.MaterialSlot = -1;
	sub.Bounds.Min = bMin;
	sub.Bounds.Max = bMax;
	data.Submeshes.push_back(sub);

	Asset::WModelMeshRange mr;
	mr.FirstSubmesh = 0;
	mr.SubmeshCount = 1;
	data.Meshes.push_back(mr);

	Asset::WModelNode node;
	node.Parent = -1;
	node.MeshIndex = 0;
	node.Name = name;
	data.Nodes.push_back(node);

	return data;
}

static void CreatePlaneMesh(std::vector<WModelVertex>& outVerts, std::vector<uint32_t>& outIndices)
{
	outVerts = {
		{ { -1.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
		{ {  1.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
		{ {  1.0f, 0.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
		{ { -1.0f, 0.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
	};
	outIndices = { 0, 1, 2, 0, 2, 3 };
}

static void TestGenerateEngineShowcaseLODs()
{
	std::filesystem::create_directories("Engine/content/models/primitives");

	// 1. 生成并持久化标准基础图元资产 (Cube, Sphere, Plane)
	{
		std::vector<WModelVertex> cubeVerts;
		std::vector<uint32_t> cubeIdx;
		CreateCubeMesh(cubeVerts, cubeIdx);
		auto cubeModel = BuildPrimitiveModelData(cubeVerts, cubeIdx, "Cube");
		std::string err;
		Asset::WModelIO::WriteFile("Engine/content/models/primitives/cube.wmodel", cubeModel, &err);

		std::vector<WModelVertex> sphereVerts;
		std::vector<uint32_t> sphereIdx;
		CreateSphereMesh(24, 24, sphereVerts, sphereIdx);
		auto sphereModel = BuildPrimitiveModelData(sphereVerts, sphereIdx, "Sphere");
		Asset::WModelIO::WriteFile("Engine/content/models/primitives/sphere.wmodel", sphereModel, &err);

		std::vector<WModelVertex> planeVerts;
		std::vector<uint32_t> planeIdx;
		CreatePlaneMesh(planeVerts, planeIdx);
		auto planeModel = BuildPrimitiveModelData(planeVerts, planeIdx, "Plane");
		Asset::WModelIO::WriteFile("Engine/content/models/primitives/plane.wmodel", planeModel, &err);
	}

	std::vector<LODLevelConfig> configs = {
		{ 1, 0.5f, 25.0f },
		{ 2, 0.25f, 60.0f }
	};

	const std::vector<std::string> modelPaths = {
		"Engine/content/models/primitives/sphere.wmodel",
		"Engine/content/models/primitives/cube.wmodel",
		"Engine/content/models/foliage/scatter_pebble.wmodel",
		"Engine/content/models/foliage/bush_cluster.wmodel",
		"Engine/content/models/debris_shard.wmodel",
		"templates/project-example/assets/models/tests/D5Fixture.wmodel"
	};

	for (const auto& path : modelPaths)
	{
		if (std::filesystem::exists(path))
		{
			std::vector<GeneratedLODAsset> generated;
			std::string err;
			bool ok = MeshLODGenerator::GenerateAndSaveLODFiles(path, configs, generated, &err);
			if (ok)
			{
				std::cout << "[LOD-GEN] Successfully generated LODs for " << path << " (" << generated.size() << " levels)\n";
			}
			else
			{
				std::cerr << "[LOD-GEN] Warning: Failed to generate LODs for " << path << ": " << err << "\n";
			}
		}
	}
}

int main()
{
#ifdef _WIN32
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
	std::cout << "=== Running MeshSimplifier and LOD Generator Tests ===\n";
	TestCubeSimplification();
	TestSphereSimplification();
	TestDegenerateAndEdgeCases();
	TestMeshLODGenerator();
	TestGenerateEngineShowcaseLODs();
	std::cout << "All MeshSimplifier tests passed successfully!\n";
	return 0;
}




