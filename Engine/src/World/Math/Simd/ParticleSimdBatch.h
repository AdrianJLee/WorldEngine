#pragma once

#include "World/Core/Export.h"
#include <cstddef>

namespace World::Math::Simd
{
	struct ParticleBatchParams
	{
		float Dt = 0.0f;
		float DragFactor = 1.0f;
		float GravityX = 0.0f;
		float GravityY = 0.0f;
		float GravityZ = 0.0f;
	};

	// Steps exactly 8 particles using SIMD intrinsics (or scalar fallback)
	WLD_API void StepParticlesSimd8(
		float* posX, float* posY, float* posZ,
		float* velX, float* velY, float* velZ,
		float* life,
		const ParticleBatchParams& params);

	// Batches N particles (chunks of 8 via SIMD + remainder scalar loop)
	WLD_API void StepParticlesSimdBatch(
		float* posX, float* posY, float* posZ,
		float* velX, float* velY, float* velZ,
		float* life,
		size_t count,
		const ParticleBatchParams& params);
}
