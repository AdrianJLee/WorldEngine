#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World
{
	struct WaterMeshVertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 TexCoord;
	};

	class WLD_API WaterSimulation
	{
	public:
		// 采样 Gerstner 物理海洋波面高度
		static float SampleWaveHeight(
			float worldX,
			float worldZ,
			float time,
			const WaterBodyComponent& comp);

		// 采样波面表面法线
		static glm::vec3 SampleWaveNormal(
			float worldX,
			float worldZ,
			float time,
			const WaterBodyComponent& comp);

		// 计算刚体在水中的物理浮力与力矩 (Buoyancy)
		static void ComputeBuoyancy(
			const glm::vec3& bodyCenter,
			const glm::vec3& bodyHalfExtents,
			float bodyMass,
			float time,
			const WaterBodyComponent& comp,
			glm::vec3& outBuoyancyForce,
			glm::vec3& outTorque);

		// 生成水面顶点与索引网格
		static void GenerateWaterSurfaceMesh(
			const WaterBodyComponent& comp,
			const glm::vec2& extents,
			uint32_t subdivisions,
			float time,
			std::vector<WaterMeshVertex>& outVertices,
			std::vector<uint32_t>& outIndices);
	};
}
