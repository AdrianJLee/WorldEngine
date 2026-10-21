#include "wldpch.h"
#include "World/Environment/WaterSimulation.h"

#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		// 4 组叠加 Gerstner 波浪参数 (方向角, 相对振幅, 相对波长, 相对波速)
		const float kWaveDirs[4][2] = {
			{ 1.0f,  0.2f },
			{ -0.6f, 0.8f },
			{ 0.3f, -0.95f },
			{ 0.7f,  0.7f }
		};
		const float kWaveAmps[4] = { 0.5f, 0.25f, 0.15f, 0.1f };
	}

	float WaterSimulation::SampleWaveHeight(
		float worldX,
		float worldZ,
		float time,
		const WaterBodyComponent& comp)
	{
		const float baseL = std::max(comp.WaveLength, 1.0f);
		const float baseS = comp.WaveSpeed;
		float totalH = 0.0f;

		for (int i = 0; i < 4; ++i)
		{
			const glm::vec2 dir = glm::normalize(glm::vec2(kWaveDirs[i][0], kWaveDirs[i][1]));
			const float lambda = baseL * (1.0f - static_cast<float>(i) * 0.18f);
			const float k = (2.0f * kPi) / lambda;
			const float w = std::sqrt(9.81f * k) * (baseS * 0.5f);
			const float amp = (lambda * 0.04f * comp.WaveSteepness) * kWaveAmps[i];

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
		const float eps = 0.1f;
		const float hL = SampleWaveHeight(worldX - eps, worldZ, time, comp);
		const float hR = SampleWaveHeight(worldX + eps, worldZ, time, comp);
		const float hD = SampleWaveHeight(worldX, worldZ - eps, time, comp);
		const float hU = SampleWaveHeight(worldX, worldZ + eps, time, comp);

		const glm::vec3 dx(2.0f * eps, hR - hL, 0.0f);
		const glm::vec3 dz(0.0f, hU - hD, 2.0f * eps);
		return glm::normalize(glm::cross(dz, dx));
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
		outBuoyancyForce = glm::vec3(0.0f);
		outTorque = glm::vec3(0.0f);

		if (bodyMass <= 0.0f)
			return;

		const float waterH = SampleWaveHeight(bodyCenter.x, bodyCenter.z, time, comp);
		const float bodyBottom = bodyCenter.y - bodyHalfExtents.y;
		const float bodyTop    = bodyCenter.y + bodyHalfExtents.y;

		if (bodyBottom >= waterH)
			return; // 完全处于水面之上，无浮力

		// 吃水深度计算
		const float submersion = std::clamp((waterH - bodyBottom) / std::max(bodyTop - bodyBottom, 0.01f), 0.0f, 1.0f);
		const float waterDensity = 1000.0f; // kg/m³
		const float bodyVolume = 8.0f * bodyHalfExtents.x * bodyHalfExtents.y * bodyHalfExtents.z;
		const float displacedVolume = bodyVolume * submersion;

		// 阿基米德浮力 F = rho * V * g (向上)
		const float forceMag = displacedVolume * waterDensity * 9.81f * 0.001f; // 缩放物理比例
		outBuoyancyForce = glm::vec3(0.0f, forceMag, 0.0f);

		// 波面法线引起自倾斜恢复力矩
		const glm::vec3 waterNormal = SampleWaveNormal(bodyCenter.x, bodyCenter.z, time, comp);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		outTorque = glm::cross(up, waterNormal) * (forceMag * bodyHalfExtents.x * 0.5f);
	}
}
