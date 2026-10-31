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

		// 采样波面浪尖白沫强度 [0.0..1.0] (基于波浪峰顶曲率与陡度)
		static float SampleWaveFoam(
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

		// 生成水面顶点与索引网格 (常规矩形范围，适合湖泊、海湾与泳池)
		static void GenerateWaterSurfaceMesh(
			const WaterBodyComponent& comp,
			const glm::vec2& extents,
			uint32_t subdivisions,
			float time,
			std::vector<WaterMeshVertex>& outVertices,
			std::vector<uint32_t>& outIndices);

		// 生成以相机为中心对齐无缝大洋网格 (Infinite Horizon Ocean Snapped Grid)
		// 采用栅格对齐 (Snapped Grid) 消除视口游移抖动，结合几何同心级联延伸至地平线
		static void GenerateOceanCameraCenteredMesh(
			const WaterBodyComponent& comp,
			const glm::vec3& cameraPosition,
			float snapGridStep,
			float innerRadius,
			float outerRadius,
			uint32_t innerGridRes,
			uint32_t outerRings,
			uint32_t sectors,
			float time,
			std::vector<WaterMeshVertex>& outVertices,
			std::vector<uint32_t>& outIndices);
	};
}
