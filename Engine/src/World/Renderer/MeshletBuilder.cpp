#include "wldpch.h"
#include "World/Renderer/MeshletBuilder.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace World
{
	void MeshletBuilder::BuildMeshlets(
		const std::vector<glm::vec3>& inVertices,
		const std::vector<uint32_t>& inIndices,
		std::vector<Meshlet>& outMeshlets,
		std::vector<uint32_t>& outMeshletVertices,
		std::vector<uint8_t>& outMeshletTriangles,
		uint32_t maxVertices,
		uint32_t maxTriangles)
	{
		outMeshlets.clear();
		outMeshletVertices.clear();
		outMeshletTriangles.clear();

		const size_t totalTriangles = inIndices.size() / 3;
		if (totalTriangles == 0 || inVertices.empty())
			return;

		const uint32_t maxV = std::clamp(maxVertices, 8u, kMaxMeshletVertices);
		const uint32_t maxT = std::clamp(maxTriangles, 4u, kMaxMeshletTriangles);

		Meshlet currentMeshlet;
		currentMeshlet.VertexOffset = 0;
		currentMeshlet.TriangleOffset = 0;

		std::unordered_map<uint32_t, uint8_t> localVertexMap;
		std::vector<uint32_t> currentVerts;
		std::vector<uint8_t> currentTris;

		auto FlushMeshlet = [&]()
		{
			if (currentTris.empty())
				return;

			currentMeshlet.VertexCount = static_cast<uint32_t>(currentVerts.size());
			currentMeshlet.TriangleCount = static_cast<uint32_t>(currentTris.size() / 3);

			// 计算包围球与法向圆锥
			glm::vec3 center(0.0f);
			for (uint32_t vi : currentVerts)
				center += inVertices[vi];
			if (!currentVerts.empty())
				center /= static_cast<float>(currentVerts.size());

			float radius = 0.0f;
			for (uint32_t vi : currentVerts)
				radius = std::max(radius, glm::distance(inVertices[vi], center));

			currentMeshlet.BoundingCenter = center;
			currentMeshlet.BoundingRadius = radius;

			glm::vec3 avgNormal(0.0f);
			for (size_t t = 0; t < currentTris.size(); t += 3)
			{
				const glm::vec3& v0 = inVertices[currentVerts[currentTris[t]]];
				const glm::vec3& v1 = inVertices[currentVerts[currentTris[t + 1]]];
				const glm::vec3& v2 = inVertices[currentVerts[currentTris[t + 2]]];
				const glm::vec3 fn = glm::cross(v1 - v0, v2 - v0);
				if (glm::length(fn) > 1e-4f)
					avgNormal += glm::normalize(fn);
			}

			if (glm::length(avgNormal) > 1e-4f)
				currentMeshlet.ConeAxis = glm::normalize(avgNormal);
			else
				currentMeshlet.ConeAxis = glm::vec3(0.0f, 1.0f, 0.0f);

			float minCos = 1.0f;
			for (size_t t = 0; t < currentTris.size(); t += 3)
			{
				const glm::vec3& v0 = inVertices[currentVerts[currentTris[t]]];
				const glm::vec3& v1 = inVertices[currentVerts[currentTris[t + 1]]];
				const glm::vec3& v2 = inVertices[currentVerts[currentTris[t + 2]]];
				const glm::vec3 fn = glm::cross(v1 - v0, v2 - v0);
				if (glm::length(fn) > 1e-4f)
					minCos = std::min(minCos, glm::dot(glm::normalize(fn), currentMeshlet.ConeAxis));
			}
			currentMeshlet.ConeCutoff = std::clamp(minCos, -1.0f, 1.0f);

			outMeshlets.push_back(currentMeshlet);

			// 更新偏移指针
			currentMeshlet.VertexOffset += static_cast<uint32_t>(currentVerts.size());
			currentMeshlet.TriangleOffset += static_cast<uint32_t>(currentTris.size());

			currentVerts.clear();
			currentTris.clear();
			localVertexMap.clear();
		};

		for (size_t t = 0; t < totalTriangles; ++t)
		{
			const uint32_t i0 = inIndices[t * 3 + 0];
			const uint32_t i1 = inIndices[t * 3 + 1];
			const uint32_t i2 = inIndices[t * 3 + 2];

			// 预先统计新加入该三角形后是否会导致顶点溢出
			uint32_t extraVertices = 0;
			if (localVertexMap.find(i0) == localVertexMap.end()) ++extraVertices;
			if (localVertexMap.find(i1) == localVertexMap.end()) ++extraVertices;
			if (localVertexMap.find(i2) == localVertexMap.end()) ++extraVertices;

			if (currentVerts.size() + extraVertices > maxV || (currentTris.size() / 3) + 1 > maxT)
			{
				FlushMeshlet();
			}

			// 录入顶点
			auto GetOrAdd = [&](uint32_t globalIdx) -> uint8_t
			{
				auto it = localVertexMap.find(globalIdx);
				if (it != localVertexMap.end())
					return it->second;

				const uint8_t localIdx = static_cast<uint8_t>(currentVerts.size());
				localVertexMap[globalIdx] = localIdx;
				currentVerts.push_back(globalIdx);
				outMeshletVertices.push_back(globalIdx);
				return localIdx;
			};

			const uint8_t l0 = GetOrAdd(i0);
			const uint8_t l1 = GetOrAdd(i1);
			const uint8_t l2 = GetOrAdd(i2);

			currentTris.push_back(l0);
			currentTris.push_back(l1);
			currentTris.push_back(l2);

			outMeshletTriangles.push_back(l0);
			outMeshletTriangles.push_back(l1);
			outMeshletTriangles.push_back(l2);
		}

		FlushMeshlet();
	}
}
