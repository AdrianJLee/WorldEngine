#include "wldpch.h"
#include "World/Level/WhiteboxBuilder.h"
#include <cmath>

namespace World
{
	void WhiteboxBuilder::BuildBox(const glm::vec3& dimensions, float, WhiteboxMesh& outMesh)
	{
		outMesh.Vertices.clear();
		outMesh.Normals.clear();
		outMesh.UVs.clear();
		outMesh.Indices.clear();

		const glm::vec3 h = dimensions * 0.5f;

		// 6 个面的法线与四个角点
		const glm::vec3 faceNormals[6] = {
			{  0,  0,  1 }, {  0,  0, -1 },
			{  1,  0,  0 }, { -1,  0,  0 },
			{  0,  1,  0 }, {  0, -1,  0 }
		};

		const glm::vec3 faceCorners[6][4] = {
			{ {-h.x, -h.y,  h.z}, { h.x, -h.y,  h.z}, { h.x,  h.y,  h.z}, {-h.x,  h.y,  h.z} }, // +Z
			{ { h.x, -h.y, -h.z}, {-h.x, -h.y, -h.z}, {-h.x,  h.y, -h.z}, { h.x,  h.y, -h.z} }, // -Z
			{ { h.x, -h.y,  h.z}, { h.x, -h.y, -h.z}, { h.x,  h.y, -h.z}, { h.x,  h.y,  h.z} }, // +X
			{ {-h.x, -h.y, -h.z}, {-h.x, -h.y,  h.z}, {-h.x,  h.y,  h.z}, {-h.x,  h.y, -h.z} }, // -X
			{ {-h.x,  h.y,  h.z}, { h.x,  h.y,  h.z}, { h.x,  h.y, -h.z}, {-h.x,  h.y, -h.z} }, // +Y
			{ {-h.x, -h.y, -h.z}, { h.x, -h.y, -h.z}, { h.x, -h.y,  h.z}, {-h.x, -h.y,  h.z} }  // -Y
		};

		for (int f = 0; f < 6; ++f)
		{
			const uint32_t base = static_cast<uint32_t>(outMesh.Vertices.size());
			for (int i = 0; i < 4; ++i)
			{
				outMesh.Vertices.push_back(faceCorners[f][i]);
				outMesh.Normals.push_back(faceNormals[f]);
			}
			outMesh.UVs.push_back({ 0, 0 });
			outMesh.UVs.push_back({ 1, 0 });
			outMesh.UVs.push_back({ 1, 1 });
			outMesh.UVs.push_back({ 0, 1 });

			outMesh.Indices.push_back(base + 0);
			outMesh.Indices.push_back(base + 1);
			outMesh.Indices.push_back(base + 2);

			outMesh.Indices.push_back(base + 0);
			outMesh.Indices.push_back(base + 2);
			outMesh.Indices.push_back(base + 3);
		}
	}

	void WhiteboxBuilder::BuildCylinder(float radius, float height, uint32_t segments, WhiteboxMesh& outMesh)
	{
		outMesh.Vertices.clear();
		outMesh.Normals.clear();
		outMesh.UVs.clear();
		outMesh.Indices.clear();

		const uint32_t seg = std::max(6u, segments);
		const float halfH = height * 0.5f;
		constexpr float k2Pi = 6.28318530718f;

		// 侧面
		for (uint32_t i = 0; i <= seg; ++i)
		{
			const float theta = (static_cast<float>(i) / static_cast<float>(seg)) * k2Pi;
			const float x = std::cos(theta) * radius;
			const float z = std::sin(theta) * radius;
			const glm::vec3 n = glm::normalize(glm::vec3(x, 0.0f, z));

			outMesh.Vertices.push_back({ x, -halfH, z });
			outMesh.Vertices.push_back({ x,  halfH, z });

			outMesh.Normals.push_back(n);
			outMesh.Normals.push_back(n);

			const float u = static_cast<float>(i) / static_cast<float>(seg);
			outMesh.UVs.push_back({ u, 0.0f });
			outMesh.UVs.push_back({ u, 1.0f });

			if (i > 0)
			{
				const uint32_t base = (i - 1) * 2;
				outMesh.Indices.push_back(base + 0);
				outMesh.Indices.push_back(base + 1);
				outMesh.Indices.push_back(base + 2);

				outMesh.Indices.push_back(base + 1);
				outMesh.Indices.push_back(base + 3);
				outMesh.Indices.push_back(base + 2);
			}
		}
	}

	void WhiteboxBuilder::BuildStairs(const glm::vec3& dimensions, uint32_t steps, WhiteboxMesh& outMesh)
	{
		outMesh.Vertices.clear();
		outMesh.Normals.clear();
		outMesh.UVs.clear();
		outMesh.Indices.clear();

		const uint32_t nSteps = std::max(1u, steps);
		const float stepH = dimensions.y / static_cast<float>(nSteps);
		const float stepD = dimensions.z / static_cast<float>(nSteps);
		const float halfW = dimensions.x * 0.5f;

		for (uint32_t i = 0; i < nSteps; ++i)
		{
			const float y0 = static_cast<float>(i) * stepH;
			const float y1 = y0 + stepH;
			const float z0 = static_cast<float>(i) * stepD;
			const float z1 = z0 + stepD;

			// 踏面 (Tread, +Y)
			uint32_t baseT = static_cast<uint32_t>(outMesh.Vertices.size());
			outMesh.Vertices.push_back({ -halfW, y1, z0 });
			outMesh.Vertices.push_back({  halfW, y1, z0 });
			outMesh.Vertices.push_back({  halfW, y1, z1 });
			outMesh.Vertices.push_back({ -halfW, y1, z1 });
			for (int k = 0; k < 4; ++k) outMesh.Normals.push_back({ 0, 1, 0 });
			outMesh.UVs.push_back({ 0, 0 }); outMesh.UVs.push_back({ 1, 0 });
			outMesh.UVs.push_back({ 1, 1 }); outMesh.UVs.push_back({ 0, 1 });
			outMesh.Indices.push_back(baseT + 0); outMesh.Indices.push_back(baseT + 2); outMesh.Indices.push_back(baseT + 1);
			outMesh.Indices.push_back(baseT + 0); outMesh.Indices.push_back(baseT + 3); outMesh.Indices.push_back(baseT + 2);

			// 踢面 (Riser, -Z)
			uint32_t baseR = static_cast<uint32_t>(outMesh.Vertices.size());
			outMesh.Vertices.push_back({ -halfW, y0, z0 });
			outMesh.Vertices.push_back({  halfW, y0, z0 });
			outMesh.Vertices.push_back({  halfW, y1, z0 });
			outMesh.Vertices.push_back({ -halfW, y1, z0 });
			for (int k = 0; k < 4; ++k) outMesh.Normals.push_back({ 0, 0, -1 });
			outMesh.UVs.push_back({ 0, 0 }); outMesh.UVs.push_back({ 1, 0 });
			outMesh.UVs.push_back({ 1, 1 }); outMesh.UVs.push_back({ 0, 1 });
			outMesh.Indices.push_back(baseR + 0); outMesh.Indices.push_back(baseR + 2); outMesh.Indices.push_back(baseR + 1);
			outMesh.Indices.push_back(baseR + 0); outMesh.Indices.push_back(baseR + 3); outMesh.Indices.push_back(baseR + 2);
		}
	}

	void WhiteboxBuilder::BuildWedge(const glm::vec3& dimensions, WhiteboxMesh& outMesh)
	{
		BuildBox(dimensions, 0.0f, outMesh);
	}

	void WhiteboxBuilder::BuildArch(const glm::vec3& dimensions, float, WhiteboxMesh& outMesh)
	{
		BuildBox(dimensions, 0.0f, outMesh);
	}
}
