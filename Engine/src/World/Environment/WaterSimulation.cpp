#include "wldpch.h"
#include "World/Environment/WaterSimulation.h"
#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
	}

	float WaterSimulation::SampleWaveHeight(
		float worldX,
		float worldZ,
		float time,
		const WaterBodyComponent& comp)
	{
		const float baseL = std::max(comp.WaveLength, 1.0f);
		const float baseS = comp.WaveSpeed;
		const float baseAngleRad = glm::radians(comp.WaveDirectionAngle);
		const uint32_t octaves = std::clamp(static_cast<uint32_t>(comp.WaveOctaves), 1u, 8u);

		float totalH = 0.0f;

		for (uint32_t i = 0; i < octaves; ++i)
		{
			const float angleSpread = (i == 0) ? 0.0f : (((i % 2 == 1) ? 1.0f : -1.0f) * 0.28f * static_cast<float>(i));
			const float curAngle = baseAngleRad + angleSpread;
			const glm::vec2 dir = glm::vec2(std::cos(curAngle), std::sin(curAngle));

			const float lambda = std::max(baseL * std::pow(0.56f, static_cast<float>(i)), 0.5f);
			const float k = (2.0f * kPi) / lambda;
			const float w = std::sqrt(9.81f * k) * (baseS * 0.5f) * (1.0f + 0.12f * static_cast<float>(i));

			const float weight = 1.0f / (1.0f + 0.65f * static_cast<float>(i));
			const float amp = (lambda * 0.04f * comp.WaveSteepness) * weight;

			const float phase = k * (dir.x * worldX + dir.y * worldZ) + w * time;
			totalH += std::sin(phase) * amp;
		}

		return totalH;
	}

	glm::vec3 WaterSimulation::SampleWaveNormal(
		float worldX,
		float worldZ,
		float time,
		const WaterBodyComponent& comp)
	{
		const float baseL = std::max(comp.WaveLength, 1.0f);
		const float baseS = comp.WaveSpeed;
		const float baseAngleRad = glm::radians(comp.WaveDirectionAngle);
		const uint32_t octaves = std::clamp(static_cast<uint32_t>(comp.WaveOctaves), 1u, 8u);

		float dhdx = 0.0f;
		float dhdz = 0.0f;

		for (uint32_t i = 0; i < octaves; ++i)
		{
			const float angleSpread = (i == 0) ? 0.0f : (((i % 2 == 1) ? 1.0f : -1.0f) * 0.28f * static_cast<float>(i));
			const float curAngle = baseAngleRad + angleSpread;
			const glm::vec2 dir = glm::vec2(std::cos(curAngle), std::sin(curAngle));

			const float lambda = std::max(baseL * std::pow(0.56f, static_cast<float>(i)), 0.5f);
			const float k = (2.0f * kPi) / lambda;
			const float w = std::sqrt(9.81f * k) * (baseS * 0.5f) * (1.0f + 0.12f * static_cast<float>(i));

			const float weight = 1.0f / (1.0f + 0.65f * static_cast<float>(i));
			const float amp = (lambda * 0.04f * comp.WaveSteepness) * weight;

			const float phase = k * (dir.x * worldX + dir.y * worldZ) + w * time;
			const float deriv = std::cos(phase) * amp * k;

			dhdx += dir.x * deriv;
			dhdz += dir.y * deriv;
		}

		const glm::vec3 n(-dhdx, 1.0f, -dhdz);
		return glm::normalize(n);
	}

	void WaterSimulation::ComputeBuoyancy(
		const glm::vec3& bodyCenter,
		const glm::vec3& bodyHalfExtents,
		float bodyMass,
		float time,
		const WaterBodyComponent& comp,
		glm::vec3& outBuoyancyForce,
		glm::vec3& outTorque)
	{
		(void)bodyMass;
		outBuoyancyForce = glm::vec3(0.0f);
		outTorque = glm::vec3(0.0f);

		const float waterH = SampleWaveHeight(bodyCenter.x, bodyCenter.z, time, comp);
		const float bodyBottom = bodyCenter.y - bodyHalfExtents.y;
		const float bodyTop    = bodyCenter.y + bodyHalfExtents.y;

		if (bodyBottom >= waterH)
			return;

		const float submersion = std::clamp((waterH - bodyBottom) / std::max(bodyTop - bodyBottom, 0.01f), 0.0f, 1.0f);
		const float waterDensity = std::clamp(comp.FluidDensity, 500.0f, 2000.0f);
		const float bodyVolume = 8.0f * bodyHalfExtents.x * bodyHalfExtents.y * bodyHalfExtents.z;
		const float displacedVolume = bodyVolume * submersion;

		const float forceMag = displacedVolume * waterDensity * 9.81f * 0.001f;
		outBuoyancyForce = glm::vec3(0.0f, forceMag, 0.0f);

		const glm::vec3 waterNormal = SampleWaveNormal(bodyCenter.x, bodyCenter.z, time, comp);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		const float damping = std::clamp(comp.FluidDamping, 0.05f, 1.0f);
		outTorque = glm::cross(up, waterNormal) * (forceMag * bodyHalfExtents.x * 0.5f * damping);
	}

	void WaterSimulation::GenerateWaterSurfaceMesh(
		const WaterBodyComponent& comp,
		const glm::vec2& extents,
		uint32_t subdivisions,
		float time,
		std::vector<WaterMeshVertex>& outVertices,
		std::vector<uint32_t>& outIndices)
	{
		outVertices.clear();
		outIndices.clear();

		const uint32_t segs = std::clamp(subdivisions, 4u, 128u);
		const float halfW = extents.x * 0.5f;
		const float halfH = extents.y * 0.5f;

		outVertices.reserve((segs + 1) * (segs + 1));
		outIndices.reserve(segs * segs * 12);

		for (uint32_t z = 0; z <= segs; ++z)
		{
			const float v = static_cast<float>(z) / static_cast<float>(segs);
			const float worldZ = -halfH + v * extents.y;

			for (uint32_t x = 0; x <= segs; ++x)
			{
				const float u = static_cast<float>(x) / static_cast<float>(segs);
				const float worldX = -halfW + u * extents.x;

				WaterMeshVertex vert;
				vert.Position = glm::vec3(worldX, SampleWaveHeight(worldX, worldZ, time, comp), worldZ);
				vert.Normal   = SampleWaveNormal(worldX, worldZ, time, comp);
				vert.TexCoord = glm::vec2(u, v);
				outVertices.push_back(vert);
			}
		}

		for (uint32_t z = 0; z < segs; ++z)
		{
			for (uint32_t x = 0; x < segs; ++x)
			{
				const uint32_t v0 = z * (segs + 1) + x;
				const uint32_t v1 = v0 + 1;
				const uint32_t v2 = (z + 1) * (segs + 1) + (x + 1);
				const uint32_t v3 = (z + 1) * (segs + 1) + x;

				outIndices.push_back(v0);
				outIndices.push_back(v2);
				outIndices.push_back(v1);

				outIndices.push_back(v0);
				outIndices.push_back(v3);
				outIndices.push_back(v2);

				outIndices.push_back(v0);
				outIndices.push_back(v1);
				outIndices.push_back(v2);

				outIndices.push_back(v0);
				outIndices.push_back(v2);
				outIndices.push_back(v3);
			}
		}
	}

	float WaterSimulation::SampleWaveFoam(
		float worldX,
		float worldZ,
		float time,
		const WaterBodyComponent& comp)
	{
		if (!comp.EnableFoam)
			return 0.0f;

		const float h = SampleWaveHeight(worldX, worldZ, time, comp);
		const glm::vec3 n = SampleWaveNormal(worldX, worldZ, time, comp);

		// 浪尖尖锐度与物理坡度：当水面高度接近波幅且法线偏离垂直方向较大时，波峰产生翻滚白浪
		const float maxAmp = comp.WaveLength * comp.WaveSteepness * 0.12f + 1e-4f;
		const float heightFactor = std::clamp((h - maxAmp * 0.25f) / (maxAmp * 0.75f + 1e-4f), 0.0f, 1.0f);
		const float slopeFactor = std::clamp(1.0f - n.y, 0.0f, 1.0f);

		const float foam = std::clamp(heightFactor * 0.7f + slopeFactor * 1.5f, 0.0f, 1.0f);
		return foam;
	}

	void WaterSimulation::GenerateOceanCameraCenteredMesh(
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
		std::vector<uint32_t>& outIndices)
	{
		outVertices.clear();
		outIndices.clear();

		const float step = std::max(0.5f, snapGridStep);
		const float snappedX = std::floor(cameraPosition.x / step) * step;
		const float snappedZ = std::floor(cameraPosition.z / step) * step;

		const uint32_t innerN = std::clamp(innerGridRes, 8u, 96u);
		const float innerHalf = std::max(50.0f, innerRadius);
		const float innerStep = (innerHalf * 2.0f) / static_cast<float>(innerN);

		// 1. 相机近处密集正方形网格 (Near-Camera Dense Grid)
		for (uint32_t z = 0; z <= innerN; ++z)
		{
			const float lz = -innerHalf + static_cast<float>(z) * innerStep;
			const float wz = snappedZ + lz;

			for (uint32_t x = 0; x <= innerN; ++x)
			{
				const float lx = -innerHalf + static_cast<float>(x) * innerStep;
				const float wx = snappedX + lx;

				WaterMeshVertex vert;
				vert.Position = glm::vec3(wx, SampleWaveHeight(wx, wz, time, comp), wz);
				vert.Normal   = SampleWaveNormal(wx, wz, time, comp);
				vert.TexCoord = glm::vec2(wx, wz) * 0.05f;
				outVertices.push_back(vert);
			}
		}

		// 近处网格双面索引
		for (uint32_t z = 0; z < innerN; ++z)
		{
			for (uint32_t x = 0; x < innerN; ++x)
			{
				const uint32_t v0 = z * (innerN + 1) + x;
				const uint32_t v1 = v0 + 1;
				const uint32_t v2 = (z + 1) * (innerN + 1) + (x + 1);
				const uint32_t v3 = (z + 1) * (innerN + 1) + x;

				// 上表面 (+Y 朝向)
				outIndices.push_back(v0); outIndices.push_back(v2); outIndices.push_back(v1);
				outIndices.push_back(v0); outIndices.push_back(v3); outIndices.push_back(v2);
				// 下表面 (-Y 水下视角)
				outIndices.push_back(v0); outIndices.push_back(v1); outIndices.push_back(v2);
				outIndices.push_back(v0); outIndices.push_back(v2); outIndices.push_back(v3);
			}
		}

		// 2. 远处同心几何递增级联大洋圈 (Far Ocean Radial Expansion to Horizon)
		const uint32_t ringCount = std::clamp(outerRings, 4u, 48u);
		const uint32_t sectorCount = std::clamp(sectors, 16u, 128u);
		const float maxR = std::max(innerHalf * 2.0f, outerRadius);
		const uint32_t ringStartVertex = static_cast<uint32_t>(outVertices.size());

		for (uint32_t r = 0; r <= ringCount; ++r)
		{
			const float t = static_cast<float>(r) / static_cast<float>(ringCount);
			// 几何级数递增半径：近处密集过渡，远处扩展至数公里地平线
			const float radius = innerHalf + (maxR - innerHalf) * (t * t);

			for (uint32_t s = 0; s < sectorCount; ++s)
			{
				const float angle = (static_cast<float>(s) / static_cast<float>(sectorCount)) * (2.0f * kPi);
				const float wx = snappedX + radius * std::cos(angle);
				const float wz = snappedZ + radius * std::sin(angle);

				WaterMeshVertex vert;
				// 极远处波高平滑衰减至海平面，保证与天空球地平线绝对贴合
				const float waveFade = std::clamp((maxR - radius) / (maxR * 0.4f + 1e-4f), 0.0f, 1.0f);
				const float h = SampleWaveHeight(wx, wz, time, comp) * waveFade;
				vert.Position = glm::vec3(wx, h, wz);
				vert.Normal   = glm::normalize(glm::mix(glm::vec3(0, 1, 0), SampleWaveNormal(wx, wz, time, comp), waveFade));
				vert.TexCoord = glm::vec2(wx, wz) * 0.02f;
				outVertices.push_back(vert);
			}
		}

		for (uint32_t r = 0; r < ringCount; ++r)
		{
			for (uint32_t s = 0; s < sectorCount; ++s)
			{
				const uint32_t nextS = (s + 1) % sectorCount;
				const uint32_t v0 = ringStartVertex + r * sectorCount + s;
				const uint32_t v1 = ringStartVertex + r * sectorCount + nextS;
				const uint32_t v2 = ringStartVertex + (r + 1) * sectorCount + nextS;
				const uint32_t v3 = ringStartVertex + (r + 1) * sectorCount + s;

				// 上表面
				outIndices.push_back(v0); outIndices.push_back(v2); outIndices.push_back(v1);
				outIndices.push_back(v0); outIndices.push_back(v3); outIndices.push_back(v2);
				// 下表面
				outIndices.push_back(v0); outIndices.push_back(v1); outIndices.push_back(v2);
				outIndices.push_back(v0); outIndices.push_back(v2); outIndices.push_back(v3);
			}
		}
	}
}
