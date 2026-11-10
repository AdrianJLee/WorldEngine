#pragma once

#include "World/Core/Export.h"
#include <vector>
#include <cstdint>
#include <cstddef>
#include <glm/glm.hpp>

namespace World
{
	struct WLD_API ParticleSoA
	{
		std::vector<float> PosX, PosY, PosZ;
		std::vector<float> VelX, VelY, VelZ;
		std::vector<float> ColorR, ColorG, ColorB, ColorA;
		std::vector<float> SizeX, SizeY;
		std::vector<float> Rotation, AngularVelocity;
		std::vector<float> Life, MaxLife;
		std::vector<uint32_t> SubUVFrameIndex;
		std::vector<uint8_t> Active;

		size_t Capacity = 0;
		size_t ActiveCount = 0;

		void Resize(size_t capacity);
		void Clear();
		size_t Allocate();
		void Kill(size_t index);
		void SwapAndPop(size_t index);
	};

	class CubicBezierCurve;
	class ColorGradient;

	struct ParticleBufferUpdateParams
	{
		float Dt = 0.016f;
		glm::vec3 Gravity { 0.0f, -9.8f, 0.0f };
		float LinearDrag = 0.0f;
		float VortexStrength = 0.0f;
		glm::vec3 VortexAxis { 0.0f, 1.0f, 0.0f };
		float TurbulenceStrength = 0.0f;
		float TurbulenceFrequency = 1.0f;
		bool LockToXY = false;
		float EmitterPosZ = 0.0f;

		const CubicBezierCurve* SizeCurve = nullptr;
		const CubicBezierCurve* SpeedCurve = nullptr;
		const ColorGradient* ColorGradientOverLife = nullptr;

		float SizeStart = 1.0f;
		float SizeEnd = 1.0f;
		glm::vec4 ColorStart { 1.0f };
		glm::vec4 ColorEnd { 1.0f };
		float EmissiveBoost = 1.0f;
		uint32_t SubUVTotalFrames = 1;
		float SubUVFrameRate = 0.0f;
	};

	class WLD_API ParticleBuffer
	{
	public:
		explicit ParticleBuffer(size_t capacity = 1000);
		~ParticleBuffer() = default;

		ParticleBuffer(const ParticleBuffer&) = delete;
		ParticleBuffer& operator=(const ParticleBuffer&) = delete;
		ParticleBuffer(ParticleBuffer&&) noexcept = default;
		ParticleBuffer& operator=(ParticleBuffer&&) noexcept = default;

		void Resize(size_t capacity) { m_Soa.Resize(capacity); }
		void Clear() { m_Soa.Clear(); }

		size_t Capacity() const { return m_Soa.Capacity; }
		size_t ActiveCount() const { return m_Soa.ActiveCount; }

		ParticleSoA& GetSoA() { return m_Soa; }
		const ParticleSoA& GetSoA() const { return m_Soa; }

		size_t Allocate() { return m_Soa.Allocate(); }
		void Kill(size_t index) { m_Soa.Kill(index); }
		void SwapAndPop(size_t index) { m_Soa.SwapAndPop(index); }

		void UpdateStep(const ParticleBufferUpdateParams& params);

	private:
		ParticleSoA m_Soa;
	};
}
