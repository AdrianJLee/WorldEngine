#include "wldpch.h"
#include "World/Math/Simd/ParticleSimdBatch.h"
#include "World/Math/Simd/SimdConfig.h"

namespace World::Math::Simd
{
	namespace
	{
		// S3: Scalar fallback equivalent to SIMD path
		void StepParticles_Scalar(
			float* posX, float* posY, float* posZ,
			float* velX, float* velY, float* velZ,
			float* life,
			size_t count,
			const ParticleBatchParams& params)
		{
			for (size_t i = 0; i < count; ++i)
			{
				life[i] -= params.Dt;
				velX[i] = velX[i] * params.DragFactor + params.GravityX * params.Dt;
				velY[i] = velY[i] * params.DragFactor + params.GravityY * params.Dt;
				velZ[i] = velZ[i] * params.DragFactor + params.GravityZ * params.Dt;
				posX[i] += velX[i] * params.Dt;
				posY[i] += velY[i] * params.Dt;
				posZ[i] += velZ[i] * params.Dt;
			}
		}
	}

	void StepParticlesSimd8(
		float* posX, float* posY, float* posZ,
		float* velX, float* velY, float* velZ,
		float* life,
		const ParticleBatchParams& params)
	{
#if WLD_SIMD_SSE2
		const __m128 dtVec = _mm_set1_ps(params.Dt);
		const __m128 dragVec = _mm_set1_ps(params.DragFactor);
		const __m128 gxDt = _mm_set1_ps(params.GravityX * params.Dt);
		const __m128 gyDt = _mm_set1_ps(params.GravityY * params.Dt);
		const __m128 gzDt = _mm_set1_ps(params.GravityZ * params.Dt);

		for (size_t offset = 0; offset < 8; offset += 4)
		{
			// Life: life -= dt
			__m128 l = _mm_loadu_ps(life + offset);
			l = _mm_sub_ps(l, dtVec);
			_mm_storeu_ps(life + offset, l);

			// VelX: velX = velX * drag + gravityX * dt
			__m128 vx = _mm_loadu_ps(velX + offset);
			vx = _mm_add_ps(_mm_mul_ps(vx, dragVec), gxDt);
			_mm_storeu_ps(velX + offset, vx);

			// PosX: posX += velX * dt
			__m128 px = _mm_loadu_ps(posX + offset);
			px = _mm_add_ps(px, _mm_mul_ps(vx, dtVec));
			_mm_storeu_ps(posX + offset, px);

			// VelY & PosY
			__m128 vy = _mm_loadu_ps(velY + offset);
			vy = _mm_add_ps(_mm_mul_ps(vy, dragVec), gyDt);
			_mm_storeu_ps(velY + offset, vy);

			__m128 py = _mm_loadu_ps(posY + offset);
			py = _mm_add_ps(py, _mm_mul_ps(vy, dtVec));
			_mm_storeu_ps(posY + offset, py);

			// VelZ & PosZ
			__m128 vz = _mm_loadu_ps(velZ + offset);
			vz = _mm_add_ps(_mm_mul_ps(vz, dragVec), gzDt);
			_mm_storeu_ps(velZ + offset, vz);

			__m128 pz = _mm_loadu_ps(posZ + offset);
			pz = _mm_add_ps(pz, _mm_mul_ps(vz, dtVec));
			_mm_storeu_ps(posZ + offset, pz);
		}
#else
		StepParticles_Scalar(posX, posY, posZ, velX, velY, velZ, life, 8, params);
#endif
	}

	void StepParticlesSimdBatch(
		float* posX, float* posY, float* posZ,
		float* velX, float* velY, float* velZ,
		float* life,
		size_t count,
		const ParticleBatchParams& params)
	{
		const size_t simdBlocks = count / 8;
		for (size_t b = 0; b < simdBlocks; ++b)
		{
			const size_t offset = b * 8;
			StepParticlesSimd8(
				posX + offset, posY + offset, posZ + offset,
				velX + offset, velY + offset, velZ + offset,
				life + offset,
				params);
		}

		const size_t remainder = count % 8;
		if (remainder > 0)
		{
			const size_t offset = simdBlocks * 8;
			StepParticles_Scalar(
				posX + offset, posY + offset, posZ + offset,
				velX + offset, velY + offset, velZ + offset,
				life + offset,
				remainder,
				params);
		}
	}
}
