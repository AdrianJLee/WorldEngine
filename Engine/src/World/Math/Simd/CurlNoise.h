#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <cstddef>

namespace World::Math::Simd
{
	// Computes divergence-free 3D curl noise vector at the specified position.
	// Curl(Psi) = (dPsi_z/dy - dPsi_y/dz, dPsi_x/dz - dPsi_z/dx, dPsi_y/dx - dPsi_x/dy)
	// Guaranteed div(v) = 0 by vector calculus identity div(curl(Psi)) = 0.
	WLD_API glm::vec3 ComputeCurlNoise(
		const glm::vec3& position,
		float frequency = 1.0f,
		uint32_t octaves = 1,
		float persistence = 0.5f,
		float lacunarity = 2.0f);

	// Overload accepting float octaves (for compatibility with particle component float parameters)
	WLD_API glm::vec3 ComputeCurlNoise(
		const glm::vec3& position,
		float frequency,
		float octaves);

	// Batch computation of 3D curl noise over N particles:
	// Output curl vectors are written into outCurlX, outCurlY, outCurlZ.
	WLD_API void ComputeCurlNoiseBatch(
		const float* posX, const float* posY, const float* posZ,
		float* outCurlX, float* outCurlY, float* outCurlZ,
		size_t count,
		float frequency = 1.0f,
		uint32_t octaves = 1);
}
