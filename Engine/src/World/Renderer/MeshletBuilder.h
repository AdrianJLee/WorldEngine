#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World
{
	constexpr uint32_t kMaxMeshletVertices = 64;
	constexpr uint32_t kMaxMeshletTriangles = 126;

	struct Meshlet
	{
		uint32_t VertexOffset { 0 };
		uint32_t TriangleOffset { 0 };
		uint32_t VertexCount { 0 };
		uint32_t TriangleCount { 0 };

		// 硬件视锥与背面剔除包络体 (Bounding Sphere & Normal Cone)
		glm::vec3 BoundingCenter { 0.0f };
		float BoundingRadius { 0.0f };
		glm::vec3 ConeAxis { 0.0f, 1.0f, 0.0f };
		float ConeCutoff { 1.0f }; // cos(半角)
	};

	class WLD_API MeshletBuilder
	{
	public:
		static void BuildMeshlets(
			const std::vector<glm::vec3>& inVertices,
			const std::vector<uint32_t>& inIndices,
			std::vector<Meshlet>& outMeshlets,
			std::vector<uint32_t>& outMeshletVertices,
			std::vector<uint8_t>& outMeshletTriangles,
			uint32_t maxVertices = kMaxMeshletVertices,
			uint32_t maxTriangles = kMaxMeshletTriangles);
	};
}
