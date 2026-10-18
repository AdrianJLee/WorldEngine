#include "wldpch.h"
#include "World/Renderer/ParticleBuffer.h"
#include "World/Math/Curve/CubicBezierCurve.h"
#include "World/Math/Curve/ColorGradient.h"
#include "World/Math/Simd/ParticleSimdBatch.h"

#include <algorithm>
#include <cmath>

namespace World
{
	void ParticleSoA::Resize(size_t capacity)
	{
		Capacity = capacity;
		ActiveCount = 0;

		PosX.assign(capacity, 0.0f);
		PosY.assign(capacity, 0.0f);
		PosZ.assign(capacity, 0.0f);

		VelX.assign(capacity, 0.0f);
		VelY.assign(capacity, 0.0f);
		VelZ.assign(capacity, 0.0f);

		ColorR.assign(capacity, 1.0f);
		ColorG.assign(capacity, 1.0f);
		ColorB.assign(capacity, 1.0f);
		ColorA.assign(capacity, 1.0f);

		SizeX.assign(capacity, 1.0f);
		SizeY.assign(capacity, 1.0f);

		Rotation.assign(capacity, 0.0f);
		AngularVelocity.assign(capacity, 0.0f);

		Life.assign(capacity, 0.0f);
		MaxLife.assign(capacity, 1.0f);

		SubUVFrameIndex.assign(capacity, 0);
		Active.assign(capacity, 0);
	}

	void ParticleSoA::Clear()
	{
		std::fill(Active.begin(), Active.end(), 0);
		ActiveCount = 0;
	}

	size_t ParticleSoA::Allocate()
	{
		if (ActiveCount >= Capacity)
			return static_cast<size_t>(-1);

		size_t index = ActiveCount;
		Active[index] = 1;
		ActiveCount++;
		return index;
	}

	void ParticleSoA::SwapAndPop(size_t index)
	{
		if (index >= ActiveCount || ActiveCount == 0)
			return;

		size_t last = ActiveCount - 1;
		if (index != last)
		{
			std::swap(PosX[index], PosX[last]);
			std::swap(PosY[index], PosY[last]);
			std::swap(PosZ[index], PosZ[last]);
			std::swap(VelX[index], VelX[last]);
			std::swap(VelY[index], VelY[last]);
			std::swap(VelZ[index], VelZ[last]);
			std::swap(ColorR[index], ColorR[last]);
			std::swap(ColorG[index], ColorG[last]);
			std::swap(ColorB[index], ColorB[last]);
			std::swap(ColorA[index], ColorA[last]);
			std::swap(SizeX[index], SizeX[last]);
			std::swap(SizeY[index], SizeY[last]);
			std::swap(Rotation[index], Rotation[last]);
			std::swap(AngularVelocity[index], AngularVelocity[last]);
			std::swap(Life[index], Life[last]);
			std::swap(MaxLife[index], MaxLife[last]);
			std::swap(SubUVFrameIndex[index], SubUVFrameIndex[last]);
			std::swap(Active[index], Active[last]);
		}
		Active[last] = 0;
		ActiveCount--;
	}

	void ParticleSoA::Kill(size_t index)
	{
		SwapAndPop(index);
	}

	ParticleBuffer::ParticleBuffer(size_t capacity)
	{
		m_Soa.Resize(capacity);
	}

	void ParticleBuffer::UpdateStep(const ParticleBufferUpdateParams& params)
	{
		if (m_Soa.ActiveCount == 0)
			return;

		Math::Simd::ParticleBatchParams simdParams;
		simdParams.Dt = params.Dt;
		simdParams.DragFactor = (params.LinearDrag > 0.0f)
			? std::max(0.0f, 1.0f - params.LinearDrag * params.Dt)
			: 1.0f;
		simdParams.GravityX = params.Gravity.x;
		simdParams.GravityY = params.Gravity.y;
		simdParams.GravityZ = params.Gravity.z;

		Math::Simd::StepParticlesSimdBatch(
			m_Soa.PosX.data(), m_Soa.PosY.data(), m_Soa.PosZ.data(),
			m_Soa.VelX.data(), m_Soa.VelY.data(), m_Soa.VelZ.data(),
			m_Soa.Life.data(),
			m_Soa.ActiveCount,
			simdParams
		);

		for (size_t i = 0; i < m_Soa.ActiveCount; )
		{
			if (m_Soa.Life[i] <= 0.0f)
			{
				m_Soa.SwapAndPop(i);
				continue;
			}

			if (std::abs(params.VortexStrength) > 1e-4f)
			{
				glm::vec3 relPos(m_Soa.PosX[i], m_Soa.PosY[i], m_Soa.PosZ[i]);
				glm::vec3 tangent = glm::cross(params.VortexAxis, relPos);
				float tLen = glm::length(tangent);
				if (tLen > 1e-4f)
				{
					glm::vec3 deltaV = (tangent / tLen) * (params.VortexStrength * params.Dt);
					m_Soa.VelX[i] += deltaV.x;
					m_Soa.VelY[i] += deltaV.y;
					m_Soa.VelZ[i] += deltaV.z;
				}
			}

			if (params.TurbulenceStrength > 1e-4f)
			{
				float freq = params.TurbulenceFrequency > 1e-4f ? params.TurbulenceFrequency : 1.0f;
				glm::vec3 sp(m_Soa.PosX[i] * freq, m_Soa.PosY[i] * freq, m_Soa.PosZ[i] * freq);
				float life = m_Soa.Life[i];
				glm::vec3 turb(
					std::sin(sp.y * 1.7f + life * 2.1f) + std::cos(sp.z * 1.3f),
					std::sin(sp.z * 1.9f + life * 2.3f) + std::cos(sp.x * 1.1f),
					std::sin(sp.x * 1.5f + life * 2.5f) + std::cos(sp.y * 1.4f)
				);
				glm::vec3 deltaV = turb * (params.TurbulenceStrength * params.Dt);
				m_Soa.VelX[i] += deltaV.x;
				m_Soa.VelY[i] += deltaV.y;
				m_Soa.VelZ[i] += deltaV.z;
			}

			if (params.LockToXY)
			{
				m_Soa.PosZ[i] = params.EmitterPosZ;
				m_Soa.VelZ[i] = 0.0f;
			}

			m_Soa.Rotation[i] += m_Soa.AngularVelocity[i] * params.Dt;

			float maxL = (m_Soa.MaxLife[i] > 1e-6f) ? m_Soa.MaxLife[i] : 1.0f;
			float t = 1.0f - (m_Soa.Life[i] / maxL);
			t = glm::clamp(t, 0.0f, 1.0f);

			glm::vec4 color;
			if (params.ColorGradientOverLife)
			{
				color = params.ColorGradientOverLife->EvaluateFastLut(t);
			}
			else
			{
				color = glm::mix(params.ColorStart, params.ColorEnd, t);
			}
			color.r *= params.EmissiveBoost;
			color.g *= params.EmissiveBoost;
			color.b *= params.EmissiveBoost;
			m_Soa.ColorR[i] = color.r;
			m_Soa.ColorG[i] = color.g;
			m_Soa.ColorB[i] = color.b;
			m_Soa.ColorA[i] = color.a;

			float s = 1.0f;
			if (params.SizeCurve)
			{
				float curveVal = params.SizeCurve->EvaluateFastLut(t);
				if (std::abs(params.SizeStart - params.SizeEnd) > 1e-5f)
					s = glm::mix(params.SizeStart, params.SizeEnd, curveVal);
				else if (std::abs(params.SizeStart - 1.0f) > 1e-5f)
					s = curveVal * params.SizeStart;
				else
					s = curveVal;
			}
			else
			{
				s = glm::mix(params.SizeStart, params.SizeEnd, t);
			}
			m_Soa.SizeX[i] = s;
			m_Soa.SizeY[i] = s;

			if (params.SpeedCurve)
			{
				float speedFactor = params.SpeedCurve->EvaluateFastLut(t);
				float curSpeedSq = m_Soa.VelX[i] * m_Soa.VelX[i] + m_Soa.VelY[i] * m_Soa.VelY[i] + m_Soa.VelZ[i] * m_Soa.VelZ[i];
				if (curSpeedSq > 1e-6f)
				{
					float curSpeed = std::sqrt(curSpeedSq);
					float targetSpeed = curSpeed * speedFactor;
					float scale = targetSpeed / curSpeed;
					m_Soa.VelX[i] *= scale;
					m_Soa.VelY[i] *= scale;
					m_Soa.VelZ[i] *= scale;
				}
			}

			if (params.SubUVTotalFrames > 1u)
			{
				if (params.SubUVFrameRate > 0.0f)
				{
					float elapsed = std::max(0.0f, m_Soa.MaxLife[i] - m_Soa.Life[i]);
					uint32_t step = static_cast<uint32_t>(elapsed * params.SubUVFrameRate);
					m_Soa.SubUVFrameIndex[i] = step % params.SubUVTotalFrames;
				}
				else
				{
					uint32_t lifeFrame = std::min(params.SubUVTotalFrames - 1u,
						static_cast<uint32_t>(t * static_cast<float>(params.SubUVTotalFrames)));
					m_Soa.SubUVFrameIndex[i] = lifeFrame % params.SubUVTotalFrames;
				}
			}

			++i;
		}
	}
}
