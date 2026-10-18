#include "wldpch.h"
#include "World/Math/Simd/CurlNoise.h"
#include "World/Math/Simd/SimdConfig.h"

#include <cmath>
#include <algorithm>

namespace World::Math::Simd
{
	namespace
	{
		inline uint32_t Hash3D(int32_t x, int32_t y, int32_t z)
		{
			uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u;
			h = (h ^ (h >> 13)) * 0x5bd1e995u;
			h ^= (h >> 15);
			return h;
		}

		static const glm::vec3 s_Gradients[16] = {
			{  1.0f,  1.0f,  0.0f }, { -1.0f,  1.0f,  0.0f }, {  1.0f, -1.0f,  0.0f }, { -1.0f, -1.0f,  0.0f },
			{  1.0f,  0.0f,  1.0f }, { -1.0f,  0.0f,  1.0f }, {  1.0f,  0.0f, -1.0f }, { -1.0f,  0.0f, -1.0f },
			{  0.0f,  1.0f,  1.0f }, {  0.0f, -1.0f,  1.0f }, {  0.0f,  1.0f, -1.0f }, {  0.0f, -1.0f, -1.0f },
			{  1.0f,  1.0f,  0.0f }, { -1.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  1.0f }, {  0.0f, -1.0f, -1.0f }
		};

		inline float Grad(int32_t x, int32_t y, int32_t z, float dx, float dy, float dz)
		{
			uint32_t h = Hash3D(x, y, z) & 15u;
			return s_Gradients[h].x * dx + s_Gradients[h].y * dy + s_Gradients[h].z * dz;
		}

		inline float Fade(float t)
		{
			return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
		}

		inline float Lerp(float a, float b, float t)
		{
			return a + t * (b - a);
		}

		float Noise3D(const glm::vec3& p)
		{
			int32_t X = static_cast<int32_t>(std::floor(p.x));
			int32_t Y = static_cast<int32_t>(std::floor(p.y));
			int32_t Z = static_cast<int32_t>(std::floor(p.z));

			float fx = p.x - static_cast<float>(X);
			float fy = p.y - static_cast<float>(Y);
			float fz = p.z - static_cast<float>(Z);

			float u = Fade(fx);
			float v = Fade(fy);
			float w = Fade(fz);

			float g000 = Grad(X,     Y,     Z,     fx,        fy,        fz);
			float g100 = Grad(X + 1, Y,     Z,     fx - 1.0f, fy,        fz);
			float g010 = Grad(X,     Y + 1, Z,     fx,        fy - 1.0f, fz);
			float g110 = Grad(X + 1, Y + 1, Z,     fx - 1.0f, fy - 1.0f, fz);
			float g001 = Grad(X,     Y,     Z + 1, fx,        fy,        fz - 1.0f);
			float g101 = Grad(X + 1, Y,     Z + 1, fx - 1.0f, fy,        fz - 1.0f);
			float g011 = Grad(X,     Y + 1, Z + 1, fx,        fy - 1.0f, fz - 1.0f);
			float g111 = Grad(X + 1, Y + 1, Z + 1, fx - 1.0f, fy - 1.0f, fz - 1.0f);

			float x00 = Lerp(g000, g100, u);
			float x10 = Lerp(g010, g110, u);
			float x01 = Lerp(g001, g101, u);
			float x11 = Lerp(g011, g111, u);

			float y0 = Lerp(x00, x10, v);
			float y1 = Lerp(x01, x11, v);

			return Lerp(y0, y1, w);
		}

		static const glm::vec3 s_OffsetPsiX { 0.0f, 0.0f, 0.0f };
		static const glm::vec3 s_OffsetPsiY { 31.4159265f, -47.1532f, 62.83185f };
		static const glm::vec3 s_OffsetPsiZ { -19.7392f, 73.1294f, -31.4159f };

		inline glm::vec3 EvaluatePotential(const glm::vec3& p)
		{
			return glm::vec3(
				Noise3D(p + s_OffsetPsiX),
				Noise3D(p + s_OffsetPsiY),
				Noise3D(p + s_OffsetPsiZ)
			);
		}

		glm::vec3 ComputeSingleOctaveCurl(const glm::vec3& p)
		{
			constexpr float h = 1e-3f;
			constexpr float inv2h = 0.5f / h;

			glm::vec3 dx_plus  = EvaluatePotential(glm::vec3(p.x + h, p.y, p.z));
			glm::vec3 dx_minus = EvaluatePotential(glm::vec3(p.x - h, p.y, p.z));

			glm::vec3 dy_plus  = EvaluatePotential(glm::vec3(p.x, p.y + h, p.z));
			glm::vec3 dy_minus = EvaluatePotential(glm::vec3(p.x, p.y - h, p.z));

			glm::vec3 dz_plus  = EvaluatePotential(glm::vec3(p.x, p.y, p.z + h));
			glm::vec3 dz_minus = EvaluatePotential(glm::vec3(p.x, p.y, p.z - h));

			float dPsiZ_dy = (dy_plus.z - dy_minus.z) * inv2h;
			float dPsiY_dz = (dz_plus.y - dz_minus.y) * inv2h;

			float dPsiX_dz = (dz_plus.x - dz_minus.x) * inv2h;
			float dPsiZ_dx = (dx_plus.z - dx_minus.z) * inv2h;

			float dPsiY_dx = (dx_plus.y - dx_minus.y) * inv2h;
			float dPsiX_dy = (dy_plus.x - dy_minus.x) * inv2h;

			return glm::vec3(
				dPsiZ_dy - dPsiY_dz,
				dPsiX_dz - dPsiZ_dx,
				dPsiY_dx - dPsiX_dy
			);
		}
	}

	glm::vec3 ComputeCurlNoise(
		const glm::vec3& position,
		float frequency,
		uint32_t octaves,
		float persistence,
		float lacunarity)
	{
		glm::vec3 totalCurl(0.0f);
		float curFreq = frequency;
		float curAmp = 1.0f;
		uint32_t count = std::max(1u, octaves);

		for (uint32_t i = 0; i < count; ++i)
		{
			totalCurl += ComputeSingleOctaveCurl(position * curFreq) * curAmp;
			curFreq *= lacunarity;
			curAmp *= persistence;
		}

		return totalCurl;
	}

	glm::vec3 ComputeCurlNoise(
		const glm::vec3& position,
		float frequency,
		float octaves)
	{
		uint32_t oct = (octaves > 1.0f) ? static_cast<uint32_t>(octaves + 0.5f) : 1u;
		return ComputeCurlNoise(position, frequency, oct);
	}

	void ComputeCurlNoiseBatch(
		const float* posX, const float* posY, const float* posZ,
		float* outCurlX, float* outCurlY, float* outCurlZ,
		size_t count,
		float frequency,
		uint32_t octaves)
	{
		// S3: Scalar batch path
		for (size_t i = 0; i < count; ++i)
		{
			glm::vec3 p(posX[i], posY[i], posZ[i]);
			glm::vec3 c = ComputeCurlNoise(p, frequency, octaves);
			outCurlX[i] = c.x;
			outCurlY[i] = c.y;
			outCurlZ[i] = c.z;
		}
	}
}
