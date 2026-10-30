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
}
