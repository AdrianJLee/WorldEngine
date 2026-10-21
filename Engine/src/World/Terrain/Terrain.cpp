#include "World/Terrain/Terrain.h"
#include "World/Core/Log.h"
#include "World/Core/StringPool.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace World
{
	namespace
	{
		constexpr uint32_t kTerrainMagic = 0x57544552; // 'WTER'
		constexpr uint32_t kTerrainVersion = 1;
	}

	TerrainData::TerrainData()
	{
		m_Layers[0].Name = "Grass";
		m_Layers[0].Roughness = 0.8f;
		m_Layers[0].Metallic = 0.0f;
		m_Layers[0].Tiling = glm::vec2(10.0f, 10.0f);

		m_Layers[1].Name = "Dirt";
		m_Layers[1].Roughness = 0.9f;
		m_Layers[1].Metallic = 0.0f;
		m_Layers[1].Tiling = glm::vec2(10.0f, 10.0f);

		m_Layers[2].Name = "Rock";
		m_Layers[2].Roughness = 0.6f;
		m_Layers[2].Metallic = 0.0f;
		m_Layers[2].Tiling = glm::vec2(5.0f, 5.0f);

		m_Layers[3].Name = "Sand";
		m_Layers[3].Roughness = 0.9f;
		m_Layers[3].Metallic = 0.0f;
		m_Layers[3].Tiling = glm::vec2(10.0f, 10.0f);

		Resize(256, 256, 1.0f);
	}

	TerrainData::TerrainData(uint32_t width, uint32_t height, float cellSize, float heightScale)
		: m_Width(std::max(2u, width))
		, m_Height(std::max(2u, height))
		, m_CellSize(std::max(0.001f, cellSize))
		, m_HeightScale(heightScale)
	{
		m_Layers[0].Name = "Grass";
		m_Layers[0].Roughness = 0.8f;
		m_Layers[0].Metallic = 0.0f;
		m_Layers[0].Tiling = glm::vec2(10.0f, 10.0f);

		m_Layers[1].Name = "Dirt";
		m_Layers[1].Roughness = 0.9f;
		m_Layers[1].Metallic = 0.0f;
		m_Layers[1].Tiling = glm::vec2(10.0f, 10.0f);

		m_Layers[2].Name = "Rock";
		m_Layers[2].Roughness = 0.6f;
		m_Layers[2].Metallic = 0.0f;
		m_Layers[2].Tiling = glm::vec2(5.0f, 5.0f);

		m_Layers[3].Name = "Sand";
		m_Layers[3].Roughness = 0.9f;
		m_Layers[3].Metallic = 0.0f;
		m_Layers[3].Tiling = glm::vec2(10.0f, 10.0f);

		Resize(m_Width, m_Height, m_CellSize);
	}

	void TerrainData::Resize(uint32_t width, uint32_t height, float cellSize)
	{
		m_Width = std::max(2u, width);
		m_Height = std::max(2u, height);
		m_CellSize = std::max(0.001f, cellSize);

		const size_t total = static_cast<size_t>(m_Width) * m_Height;
		m_Heights.assign(total, 0.0f);
		m_Splatmap.assign(total, 0x000000FF); // Layer 0 = 255 (1.0)
		m_HoleMask.assign(total, 0);          // 0 = solid

		m_IsDirty = true;
	}

	float TerrainData::GetHeight(uint32_t x, uint32_t z) const
	{
		if (x >= m_Width || z >= m_Height)
			return 0.0f;
		return m_Heights[static_cast<size_t>(z) * m_Width + x] * m_HeightScale;
	}

	void TerrainData::SetHeight(uint32_t x, uint32_t z, float h)
	{
		if (x >= m_Width || z >= m_Height)
			return;
		m_Heights[static_cast<size_t>(z) * m_Width + x] = (m_HeightScale != 0.0f ? h / m_HeightScale : h);
		m_IsDirty = true;
	}

	float TerrainData::SampleHeight(float worldX, float worldZ) const
	{
		if (m_Width < 2 || m_Height < 2 || m_CellSize <= 0.0f)
			return 0.0f;

		float gridX = worldX / m_CellSize;
		float gridZ = worldZ / m_CellSize;

		const float maxGridX = static_cast<float>(m_Width - 1);
		const float maxGridZ = static_cast<float>(m_Height - 1);

		gridX = glm::clamp(gridX, 0.0f, maxGridX);
		gridZ = glm::clamp(gridZ, 0.0f, maxGridZ);

		const uint32_t x0 = static_cast<uint32_t>(gridX);
		const uint32_t z0 = static_cast<uint32_t>(gridZ);
		const uint32_t x1 = std::min(x0 + 1, m_Width - 1);
		const uint32_t z1 = std::min(z0 + 1, m_Height - 1);

		const float fx = gridX - static_cast<float>(x0);
		const float fz = gridZ - static_cast<float>(z0);

		const float h00 = m_Heights[static_cast<size_t>(z0) * m_Width + x0];
		const float h10 = m_Heights[static_cast<size_t>(z0) * m_Width + x1];
		const float h01 = m_Heights[static_cast<size_t>(z1) * m_Width + x0];
		const float h11 = m_Heights[static_cast<size_t>(z1) * m_Width + x1];

		const float h0 = h00 * (1.0f - fx) + h10 * fx;
		const float h1 = h01 * (1.0f - fx) + h11 * fx;
		return (h0 * (1.0f - fz) + h1 * fz) * m_HeightScale;
	}

	glm::vec3 TerrainData::SampleNormal(float worldX, float worldZ) const
	{
		const float eps = std::max(m_CellSize * 0.5f, 0.01f);
		const float hL = SampleHeight(worldX - eps, worldZ);
		const float hR = SampleHeight(worldX + eps, worldZ);
		const float hD = SampleHeight(worldX, worldZ - eps);
		const float hU = SampleHeight(worldX, worldZ + eps);

		const float dX = (hR - hL) / (2.0f * eps);
		const float dZ = (hU - hD) / (2.0f * eps);

		glm::vec3 normal(-dX, 1.0f, -dZ);
		const float len2 = glm::dot(normal, normal);
		if (len2 > 1e-6f)
			return glm::normalize(normal);
		return glm::vec3(0.0f, 1.0f, 0.0f);
	}

	glm::vec4 TerrainData::GetSplatWeights(uint32_t x, uint32_t z) const
	{
		if (x >= m_Width || z >= m_Height)
			return glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
		const uint32_t val = m_Splatmap[static_cast<size_t>(z) * m_Width + x];
		const float r = static_cast<float>(val & 0xFF) / 255.0f;
		const float g = static_cast<float>((val >> 8) & 0xFF) / 255.0f;
		const float b = static_cast<float>((val >> 16) & 0xFF) / 255.0f;
		const float a = static_cast<float>((val >> 24) & 0xFF) / 255.0f;
		return glm::vec4(r, g, b, a);
	}

	void TerrainData::SetSplatWeights(uint32_t x, uint32_t z, const glm::vec4& weights)
	{
		if (x >= m_Width || z >= m_Height)
			return;
		const uint32_t r = static_cast<uint32_t>(glm::clamp(weights.x, 0.0f, 1.0f) * 255.0f + 0.5f);
		const uint32_t g = static_cast<uint32_t>(glm::clamp(weights.y, 0.0f, 1.0f) * 255.0f + 0.5f);
		const uint32_t b = static_cast<uint32_t>(glm::clamp(weights.z, 0.0f, 1.0f) * 255.0f + 0.5f);
		const uint32_t a = static_cast<uint32_t>(glm::clamp(weights.w, 0.0f, 1.0f) * 255.0f + 0.5f);
		m_Splatmap[static_cast<size_t>(z) * m_Width + x] = r | (g << 8) | (b << 16) | (a << 24);
		m_IsDirty = true;
	}

	bool TerrainData::IsHole(uint32_t x, uint32_t z) const
	{
		if (x >= m_Width || z >= m_Height)
			return false;
		return m_HoleMask[static_cast<size_t>(z) * m_Width + x] != 0;
	}

	void TerrainData::SetHole(uint32_t x, uint32_t z, bool hole)
	{
		if (x >= m_Width || z >= m_Height)
			return;
		m_HoleMask[static_cast<size_t>(z) * m_Width + x] = hole ? 1 : 0;
		m_IsDirty = true;
	}

	TerrainLayer& TerrainData::GetLayer(uint32_t index)
	{
		return m_Layers[std::min(index, 3u)];
	}

	const TerrainLayer& TerrainData::GetLayer(uint32_t index) const
	{
		return m_Layers[std::min(index, 3u)];
	}

	bool TerrainData::Raycast(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDistance, TerrainRaycastHit& outHit) const
	{
		outHit.Hit = false;
		if (m_Width < 2 || m_Height < 2 || m_CellSize <= 0.0f || maxDistance <= 0.0f)
			return false;

		const float len = glm::length(rayDir);
		if (len < 1e-6f)
			return false;
		const glm::vec3 dir = rayDir / len;

		const float worldW = GetWorldWidth();
		const float worldH = GetWorldHeight();
		float minH = 1e9f;
		float maxH = -1e9f;
		for (float h : m_Heights)
		{
			const float wh = h * m_HeightScale;
			minH = std::min(minH, wh);
			maxH = std::max(maxH, wh);
		}
		if (minH > maxH)
		{
			minH = 0.0f;
			maxH = 0.0f;
		}
		minH -= 1.0f;
		maxH += 1.0f;

		const glm::vec3 boxMin(0.0f, minH, 0.0f);
		const glm::vec3 boxMax(worldW, maxH, worldH);

		float tMin = 0.0f;
		float tMax = maxDistance;

		for (int i = 0; i < 3; ++i)
		{
			if (std::abs(dir[i]) < 1e-8f)
			{
				if (rayOrigin[i] < boxMin[i] || rayOrigin[i] > boxMax[i])
					return false;
			}
			else
			{
				const float invD = 1.0f / dir[i];
				float t1 = (boxMin[i] - rayOrigin[i]) * invD;
				float t2 = (boxMax[i] - rayOrigin[i]) * invD;
				if (t1 > t2) std::swap(t1, t2);
				tMin = std::max(tMin, t1);
				tMax = std::min(tMax, t2);
				if (tMin > tMax)
					return false;
			}
		}

		if (tMax < 0.0f || tMin > maxDistance)
			return false;

		const float tStart = std::max(0.0f, tMin);
		const float tEnd = std::min(maxDistance, tMax);

		const float stepSize = std::max(m_CellSize * 0.4f, 0.05f);
		float t = tStart;
		glm::vec3 prevPoint = rayOrigin + t * dir;
		float prevDiff = prevPoint.y - SampleHeight(prevPoint.x, prevPoint.z);

		if (prevDiff <= 0.0f && rayOrigin.y >= SampleHeight(rayOrigin.x, rayOrigin.z))
		{
			const uint32_t sx = static_cast<uint32_t>(glm::clamp(std::round(prevPoint.x / m_CellSize), 0.0f, static_cast<float>(m_Width - 1)));
			const uint32_t sz = static_cast<uint32_t>(glm::clamp(std::round(prevPoint.z / m_CellSize), 0.0f, static_cast<float>(m_Height - 1)));
			if (!IsHole(sx, sz))
			{
				outHit.Hit = true;
				outHit.Point = prevPoint;
				outHit.Distance = t;
				outHit.Normal = SampleNormal(prevPoint.x, prevPoint.z);
				outHit.SampleX = sx;
				outHit.SampleZ = sz;
				return true;
			}
		}

		while (t < tEnd)
		{
			const float nextT = std::min(t + stepSize, tEnd);
			const glm::vec3 currPoint = rayOrigin + nextT * dir;
			const float currDiff = currPoint.y - SampleHeight(currPoint.x, currPoint.z);

			if (currDiff <= 0.0f && prevDiff > 0.0f)
			{
				float tA = t;
				float tB = nextT;
				for (int iter = 0; iter < 10; ++iter)
				{
					const float tMid = 0.5f * (tA + tB);
					const glm::vec3 midP = rayOrigin + tMid * dir;
					const float midDiff = midP.y - SampleHeight(midP.x, midP.z);
					if (midDiff <= 0.0f)
						tB = tMid;
					else
						tA = tMid;
				}

				const float hitT = tB;
				const glm::vec3 hitPoint = rayOrigin + hitT * dir;
				const uint32_t sx = static_cast<uint32_t>(glm::clamp(std::round(hitPoint.x / m_CellSize), 0.0f, static_cast<float>(m_Width - 1)));
				const uint32_t sz = static_cast<uint32_t>(glm::clamp(std::round(hitPoint.z / m_CellSize), 0.0f, static_cast<float>(m_Height - 1)));

				if (!IsHole(sx, sz))
				{
					outHit.Hit = true;
					outHit.Point = hitPoint;
					outHit.Distance = hitT;
					outHit.Normal = SampleNormal(hitPoint.x, hitPoint.z);
					outHit.SampleX = sx;
					outHit.SampleZ = sz;
					return true;
				}
			}

			t = nextT;
			prevPoint = currPoint;
			prevDiff = currDiff;
		}

		return false;
	}

	float TerrainData::EvaluateFalloff(float distance, float radius, TerrainBrushFalloff falloff)
	{
		if (radius <= 0.0f || distance >= radius)
			return 0.0f;
		if (distance <= 0.0f)
			return 1.0f;

		const float t = distance / radius; // in (0, 1)
		switch (falloff)
		{
		case TerrainBrushFalloff::Linear:
			return 1.0f - t;
		case TerrainBrushFalloff::Smooth:
		{
			const float w = 1.0f - t;
			return w * w * (3.0f - 2.0f * w);
		}
		case TerrainBrushFalloff::Sharp:
		{
			const float w = 1.0f - t;
			return w * w * w;
		}
		case TerrainBrushFalloff::Constant:
			return 1.0f;
		default:
			return 1.0f - t;
		}
	}

	void TerrainData::SculptRaiseLower(float worldX, float worldZ, float radius, float deltaHeight, TerrainBrushFalloff falloff)
	{
		if (radius <= 0.0f || std::abs(deltaHeight) < 1e-6f || m_CellSize <= 0.0f)
			return;

		const int minX = std::max(0, static_cast<int>(std::floor((worldX - radius) / m_CellSize)));
		const int maxX = std::min(static_cast<int>(m_Width - 1), static_cast<int>(std::ceil((worldX + radius) / m_CellSize)));
		const int minZ = std::max(0, static_cast<int>(std::floor((worldZ - radius) / m_CellSize)));
		const int maxZ = std::min(static_cast<int>(m_Height - 1), static_cast<int>(std::ceil((worldZ + radius) / m_CellSize)));

		for (int z = minZ; z <= maxZ; ++z)
		{
			const float pz = static_cast<float>(z) * m_CellSize;
			for (int x = minX; x <= maxX; ++x)
			{
				const float px = static_cast<float>(x) * m_CellSize;
				const float dist = std::hypot(px - worldX, pz - worldZ);
				if (dist <= radius)
				{
					const float factor = EvaluateFalloff(dist, radius, falloff);
					const float currentH = GetHeight(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
					SetHeight(static_cast<uint32_t>(x), static_cast<uint32_t>(z), currentH + deltaHeight * factor);
				}
			}
		}

		MarkDirty(worldX - radius, worldZ - radius, worldX + radius, worldZ + radius);
	}

	void TerrainData::SculptSmooth(float worldX, float worldZ, float radius, float strength, TerrainBrushFalloff falloff)
	{
		if (radius <= 0.0f || strength <= 0.0f || m_CellSize <= 0.0f)
			return;

		const int minX = std::max(0, static_cast<int>(std::floor((worldX - radius) / m_CellSize)));
		const int maxX = std::min(static_cast<int>(m_Width - 1), static_cast<int>(std::ceil((worldX + radius) / m_CellSize)));
		const int minZ = std::max(0, static_cast<int>(std::floor((worldZ - radius) / m_CellSize)));
		const int maxZ = std::min(static_cast<int>(m_Height - 1), static_cast<int>(std::ceil((worldZ + radius) / m_CellSize)));

		std::vector<float> blurredHeights;
		blurredHeights.reserve(static_cast<size_t>(maxX - minX + 1) * (maxZ - minZ + 1));

		for (int z = minZ; z <= maxZ; ++z)
		{
			for (int x = minX; x <= maxX; ++x)
			{
				float sum = 0.0f;
				float weightSum = 0.0f;
				for (int dz = -1; dz <= 1; ++dz)
				{
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int nx = x + dx;
						const int nz = z + dz;
						if (nx >= 0 && nx < static_cast<int>(m_Width) && nz >= 0 && nz < static_cast<int>(m_Height))
						{
							const float w = (dx == 0 && dz == 0) ? 4.0f : ((dx == 0 || dz == 0) ? 2.0f : 1.0f);
							sum += GetHeight(static_cast<uint32_t>(nx), static_cast<uint32_t>(nz)) * w;
							weightSum += w;
						}
					}
				}
				blurredHeights.push_back(weightSum > 0.0f ? sum / weightSum : GetHeight(static_cast<uint32_t>(x), static_cast<uint32_t>(z)));
			}
		}

		size_t idx = 0;
		const float clampedStrength = glm::clamp(strength, 0.0f, 1.0f);
		for (int z = minZ; z <= maxZ; ++z)
		{
			const float pz = static_cast<float>(z) * m_CellSize;
			for (int x = minX; x <= maxX; ++x)
			{
				const float px = static_cast<float>(x) * m_CellSize;
				const float dist = std::hypot(px - worldX, pz - worldZ);
				const float blurredH = blurredHeights[idx++];
				if (dist <= radius)
				{
					const float factor = EvaluateFalloff(dist, radius, falloff) * clampedStrength;
					const float currentH = GetHeight(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
					const float newH = glm::mix(currentH, blurredH, factor);
					SetHeight(static_cast<uint32_t>(x), static_cast<uint32_t>(z), newH);
				}
			}
		}

		MarkDirty(worldX - radius, worldZ - radius, worldX + radius, worldZ + radius);
	}

	void TerrainData::SculptFlatten(float worldX, float worldZ, float radius, float targetHeight, float strength, TerrainBrushFalloff falloff)
	{
		if (radius <= 0.0f || strength <= 0.0f || m_CellSize <= 0.0f)
			return;

		const int minX = std::max(0, static_cast<int>(std::floor((worldX - radius) / m_CellSize)));
		const int maxX = std::min(static_cast<int>(m_Width - 1), static_cast<int>(std::ceil((worldX + radius) / m_CellSize)));
		const int minZ = std::max(0, static_cast<int>(std::floor((worldZ - radius) / m_CellSize)));
		const int maxZ = std::min(static_cast<int>(m_Height - 1), static_cast<int>(std::ceil((worldZ + radius) / m_CellSize)));

		const float clampedStrength = glm::clamp(strength, 0.0f, 1.0f);
		for (int z = minZ; z <= maxZ; ++z)
		{
			const float pz = static_cast<float>(z) * m_CellSize;
			for (int x = minX; x <= maxX; ++x)
			{
				const float px = static_cast<float>(x) * m_CellSize;
				const float dist = std::hypot(px - worldX, pz - worldZ);
				if (dist <= radius)
				{
					const float factor = EvaluateFalloff(dist, radius, falloff) * clampedStrength;
					const float currentH = GetHeight(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
					const float newH = glm::mix(currentH, targetHeight, factor);
					SetHeight(static_cast<uint32_t>(x), static_cast<uint32_t>(z), newH);
				}
			}
		}

		MarkDirty(worldX - radius, worldZ - radius, worldX + radius, worldZ + radius);
	}

	void TerrainData::SculptErode(float worldX, float worldZ, float radius, float strength, int iterations)
	{
		if (radius <= 0.0f || strength <= 0.0f || iterations <= 0 || m_CellSize <= 0.0f)
			return;

		const int minX = std::max(0, static_cast<int>(std::floor((worldX - radius) / m_CellSize)));
		const int maxX = std::min(static_cast<int>(m_Width - 1), static_cast<int>(std::ceil((worldX + radius) / m_CellSize)));
		const int minZ = std::max(0, static_cast<int>(std::floor((worldZ - radius) / m_CellSize)));
		const int maxZ = std::min(static_cast<int>(m_Height - 1), static_cast<int>(std::ceil((worldZ + radius) / m_CellSize)));

		const float talus = m_CellSize * 0.5f;
		const float clampedRate = glm::clamp(strength, 0.01f, 1.0f) * 0.25f;

		for (int iter = 0; iter < iterations; ++iter)
		{
			for (int z = minZ; z <= maxZ; ++z)
			{
				const float pz = static_cast<float>(z) * m_CellSize;
				for (int x = minX; x <= maxX; ++x)
				{
					const float px = static_cast<float>(x) * m_CellSize;
					const float dist = std::hypot(px - worldX, pz - worldZ);
					if (dist > radius)
						continue;

					const float hCenter = GetHeight(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
					int bestNx = x;
					int bestNz = z;
					float maxDiff = 0.0f;

					const int dxs[] = { 1, -1, 0, 0 };
					const int dzs[] = { 0, 0, 1, -1 };
					for (int i = 0; i < 4; ++i)
					{
						const int nx = x + dxs[i];
						const int nz = z + dzs[i];
						if (nx >= 0 && nx < static_cast<int>(m_Width) && nz >= 0 && nz < static_cast<int>(m_Height))
						{
							const float nh = GetHeight(static_cast<uint32_t>(nx), static_cast<uint32_t>(nz));
							const float diff = hCenter - nh;
							if (diff > maxDiff)
							{
								maxDiff = diff;
								bestNx = nx;
								bestNz = nz;
							}
						}
					}

					if (maxDiff > talus && (bestNx != x || bestNz != z))
					{
						const float transfer = (maxDiff - talus) * clampedRate;
						SetHeight(static_cast<uint32_t>(x), static_cast<uint32_t>(z), hCenter - transfer);
						const float targetH = GetHeight(static_cast<uint32_t>(bestNx), static_cast<uint32_t>(bestNz));
						SetHeight(static_cast<uint32_t>(bestNx), static_cast<uint32_t>(bestNz), targetH + transfer);
					}
				}
			}
		}

		MarkDirty(worldX - radius, worldZ - radius, worldX + radius, worldZ + radius);
	}

	void TerrainData::PaintLayer(float worldX, float worldZ, float radius, uint32_t layerIndex, float strength, TerrainBrushFalloff falloff)
	{
		if (radius <= 0.0f || strength <= 0.0f || m_CellSize <= 0.0f)
			return;

		const uint32_t targetLayer = std::min(layerIndex, 3u);
		const int minX = std::max(0, static_cast<int>(std::floor((worldX - radius) / m_CellSize)));
		const int maxX = std::min(static_cast<int>(m_Width - 1), static_cast<int>(std::ceil((worldX + radius) / m_CellSize)));
		const int minZ = std::max(0, static_cast<int>(std::floor((worldZ - radius) / m_CellSize)));
		const int maxZ = std::min(static_cast<int>(m_Height - 1), static_cast<int>(std::ceil((worldZ + radius) / m_CellSize)));

		const float clampedStrength = glm::clamp(strength, 0.0f, 1.0f);
		for (int z = minZ; z <= maxZ; ++z)
		{
			const float pz = static_cast<float>(z) * m_CellSize;
			for (int x = minX; x <= maxX; ++x)
			{
				const float px = static_cast<float>(x) * m_CellSize;
				const float dist = std::hypot(px - worldX, pz - worldZ);
				if (dist <= radius)
				{
					const float factor = EvaluateFalloff(dist, radius, falloff) * clampedStrength;
					if (factor <= 0.0f)
						continue;

					glm::vec4 weights = GetSplatWeights(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
					const float oldTarget = weights[static_cast<int>(targetLayer)];
					const float newTarget = glm::clamp(oldTarget + factor, 0.0f, 1.0f);
					const float delta = newTarget - oldTarget;

					if (delta > 0.0f)
					{
						float otherSum = 0.0f;
						for (int i = 0; i < 4; ++i)
						{
							if (i != static_cast<int>(targetLayer))
								otherSum += weights[i];
						}

						if (otherSum > 0.0001f)
						{
							for (int i = 0; i < 4; ++i)
							{
								if (i != static_cast<int>(targetLayer))
								{
									weights[i] -= delta * (weights[i] / otherSum);
									weights[i] = std::max(0.0f, weights[i]);
								}
							}
						}
						else
						{
							for (int i = 0; i < 4; ++i)
							{
								if (i != static_cast<int>(targetLayer))
									weights[i] = 0.0f;
							}
						}
						weights[static_cast<int>(targetLayer)] = newTarget;

						const float totalSum = weights.x + weights.y + weights.z + weights.w;
						if (totalSum > 0.0001f)
							weights /= totalSum;
						else
							weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);

						SetSplatWeights(static_cast<uint32_t>(x), static_cast<uint32_t>(z), weights);
					}
				}
			}
		}

		MarkDirty(worldX - radius, worldZ - radius, worldX + radius, worldZ + radius);
	}

	void TerrainData::PaintHole(float worldX, float worldZ, float radius, bool holeCutout)
	{
		if (radius <= 0.0f || m_CellSize <= 0.0f)
			return;

		const int minX = std::max(0, static_cast<int>(std::floor((worldX - radius) / m_CellSize)));
		const int maxX = std::min(static_cast<int>(m_Width - 1), static_cast<int>(std::ceil((worldX + radius) / m_CellSize)));
		const int minZ = std::max(0, static_cast<int>(std::floor((worldZ - radius) / m_CellSize)));
		const int maxZ = std::min(static_cast<int>(m_Height - 1), static_cast<int>(std::ceil((worldZ + radius) / m_CellSize)));

		for (int z = minZ; z <= maxZ; ++z)
		{
			const float pz = static_cast<float>(z) * m_CellSize;
			for (int x = minX; x <= maxX; ++x)
			{
				const float px = static_cast<float>(x) * m_CellSize;
				const float dist = std::hypot(px - worldX, pz - worldZ);
				if (dist <= radius)
				{
					SetHole(static_cast<uint32_t>(x), static_cast<uint32_t>(z), holeCutout);
				}
			}
		}

		MarkDirty(worldX - radius, worldZ - radius, worldX + radius, worldZ + radius);
	}

	uint32_t TerrainData::GetChunkCountX(uint32_t chunkResolution) const
	{
		const uint32_t res = std::max(1u, chunkResolution);
		if (m_Width <= 1)
			return 1;
		return (m_Width - 1 + res - 1) / res;
	}

	uint32_t TerrainData::GetChunkCountZ(uint32_t chunkResolution) const
	{
		const uint32_t res = std::max(1u, chunkResolution);
		if (m_Height <= 1)
			return 1;
		return (m_Height - 1 + res - 1) / res;
	}

	void TerrainData::GenerateChunkMesh(uint32_t chunkX, uint32_t chunkZ, uint32_t chunkResolution, uint32_t lod, TerrainChunk& outChunk) const
	{
		const uint32_t res = std::max(1u, chunkResolution);
		const uint32_t startX = chunkX * res;
		const uint32_t startZ = chunkZ * res;
		if (startX >= m_Width || startZ >= m_Height)
		{
			outChunk.Vertices.clear();
			outChunk.Indices.clear();
			return;
		}

		const uint32_t endX = std::min(startX + res, m_Width - 1);
		const uint32_t endZ = std::min(startZ + res, m_Height - 1);

		uint32_t step = 1u << lod;
		step = std::min(step, res);
		step = std::max(1u, step);

		outChunk.ChunkX = chunkX;
		outChunk.ChunkZ = chunkZ;
		outChunk.CurrentLod = lod;
		outChunk.Vertices.clear();
		outChunk.Indices.clear();

		std::vector<uint32_t> xs;
		for (uint32_t x = startX; x < endX; x += step)
			xs.push_back(x);
		if (xs.empty() || xs.back() != endX)
			xs.push_back(endX);

		std::vector<uint32_t> zs;
		for (uint32_t z = startZ; z < endZ; z += step)
			zs.push_back(z);
		if (zs.empty() || zs.back() != endZ)
			zs.push_back(endZ);

		const uint32_t vertsX = static_cast<uint32_t>(xs.size());
		const uint32_t vertsZ = static_cast<uint32_t>(zs.size());

		const float minX = static_cast<float>(startX) * m_CellSize;
		const float maxX = static_cast<float>(endX) * m_CellSize;
		const float minZ = static_cast<float>(startZ) * m_CellSize;
		const float maxZ = static_cast<float>(endZ) * m_CellSize;
		float minY = 1e9f;
		float maxY = -1e9f;

		outChunk.Vertices.reserve(static_cast<size_t>(vertsX) * vertsZ);

		const float invWidth = (m_Width > 1) ? 1.0f / static_cast<float>(m_Width - 1) : 1.0f;
		const float invHeight = (m_Height > 1) ? 1.0f / static_cast<float>(m_Height - 1) : 1.0f;

		for (uint32_t iz = 0; iz < vertsZ; ++iz)
		{
			const uint32_t gz = zs[iz];
			const float worldPosZ = static_cast<float>(gz) * m_CellSize;

			for (uint32_t ix = 0; ix < vertsX; ++ix)
			{
				const uint32_t gx = xs[ix];
				const float worldPosX = static_cast<float>(gx) * m_CellSize;
				const float worldPosY = GetHeight(gx, gz);

				minY = std::min(minY, worldPosY);
				maxY = std::max(maxY, worldPosY);

				TerrainVertex vert;
				vert.Position = glm::vec3(worldPosX, worldPosY, worldPosZ);
				vert.Normal = SampleNormal(worldPosX, worldPosZ);
				vert.TexCoord = glm::vec2(static_cast<float>(gx) * invWidth, static_cast<float>(gz) * invHeight);
				vert.SplatWeights = GetSplatWeights(gx, gz);
				vert.Hole = IsHole(gx, gz) ? 1.0f : 0.0f;
				outChunk.Vertices.push_back(vert);
			}
		}

		outChunk.Indices.reserve(static_cast<size_t>(vertsX - 1) * (vertsZ - 1) * 6);
		for (uint32_t iz = 0; iz < vertsZ - 1; ++iz)
		{
			for (uint32_t ix = 0; ix < vertsX - 1; ++ix)
			{
				const uint32_t i0 = iz * vertsX + ix;
				const uint32_t i1 = iz * vertsX + (ix + 1);
				const uint32_t i2 = (iz + 1) * vertsX + ix;
				const uint32_t i3 = (iz + 1) * vertsX + (ix + 1);

				outChunk.Indices.push_back(i0);
				outChunk.Indices.push_back(i2);
				outChunk.Indices.push_back(i1);

				outChunk.Indices.push_back(i1);
				outChunk.Indices.push_back(i2);
				outChunk.Indices.push_back(i3);
			}
		}

		if (minY > maxY)
		{
			minY = 0.0f;
			maxY = 0.0f;
		}
		outChunk.BoundsMin = glm::vec3(minX, minY, minZ);
		outChunk.BoundsMax = glm::vec3(maxX, maxY, maxZ);
		outChunk.DirtyMesh = false;
	}

	void TerrainData::MarkDirty(float minWorldX, float minWorldZ, float maxWorldX, float maxWorldZ)
	{
		(void)minWorldX;
		(void)minWorldZ;
		(void)maxWorldX;
		(void)maxWorldZ;
		m_IsDirty = true;
	}

	bool TerrainData::SaveToFile(const std::string& path) const
	{
		std::ofstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return false;

		stream.write(reinterpret_cast<const char*>(&kTerrainMagic), sizeof(kTerrainMagic));
		stream.write(reinterpret_cast<const char*>(&kTerrainVersion), sizeof(kTerrainVersion));
		stream.write(reinterpret_cast<const char*>(&m_Width), sizeof(m_Width));
		stream.write(reinterpret_cast<const char*>(&m_Height), sizeof(m_Height));
		stream.write(reinterpret_cast<const char*>(&m_CellSize), sizeof(m_CellSize));
		stream.write(reinterpret_cast<const char*>(&m_HeightScale), sizeof(m_HeightScale));

		for (int i = 0; i < 4; ++i)
		{
			const TerrainLayer& layer = m_Layers[i];
			const uint32_t nameLen = static_cast<uint32_t>(layer.Name.size());
			stream.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
			if (nameLen > 0)
				stream.write(layer.Name.data(), nameLen);

			const std::string albedoPath = std::string(StringPool::Get().PathOf(layer.AlbedoTexture.Path));
			const uint32_t albedoLen = static_cast<uint32_t>(albedoPath.size());
			stream.write(reinterpret_cast<const char*>(&albedoLen), sizeof(albedoLen));
			if (albedoLen > 0)
				stream.write(albedoPath.data(), albedoLen);
			const uint64_t albedoId = layer.AlbedoTexture.Identity.Value;
			stream.write(reinterpret_cast<const char*>(&albedoId), sizeof(albedoId));

			const std::string normalPath = std::string(StringPool::Get().PathOf(layer.NormalTexture.Path));
			const uint32_t normalLen = static_cast<uint32_t>(normalPath.size());
			stream.write(reinterpret_cast<const char*>(&normalLen), sizeof(normalLen));
			if (normalLen > 0)
				stream.write(normalPath.data(), normalLen);
			const uint64_t normalId = layer.NormalTexture.Identity.Value;
			stream.write(reinterpret_cast<const char*>(&normalId), sizeof(normalId));

			stream.write(reinterpret_cast<const char*>(&layer.Roughness), sizeof(layer.Roughness));
			stream.write(reinterpret_cast<const char*>(&layer.Metallic), sizeof(layer.Metallic));
			stream.write(reinterpret_cast<const char*>(&layer.Tiling), sizeof(layer.Tiling));
		}

		const size_t count = static_cast<size_t>(m_Width) * m_Height;
		stream.write(reinterpret_cast<const char*>(m_Heights.data()), count * sizeof(float));
		stream.write(reinterpret_cast<const char*>(m_Splatmap.data()), count * sizeof(uint32_t));
		stream.write(reinterpret_cast<const char*>(m_HoleMask.data()), count * sizeof(uint8_t));

		return stream.good();
	}

	bool TerrainData::LoadFromFile(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return false;

		uint32_t magic = 0;
		uint32_t version = 0;
		stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
		stream.read(reinterpret_cast<char*>(&version), sizeof(version));

		if (magic != kTerrainMagic || version != kTerrainVersion)
			return false;

		uint32_t width = 0;
		uint32_t height = 0;
		float cellSize = 1.0f;
		float heightScale = 1.0f;

		stream.read(reinterpret_cast<char*>(&width), sizeof(width));
		stream.read(reinterpret_cast<char*>(&height), sizeof(height));
		stream.read(reinterpret_cast<char*>(&cellSize), sizeof(cellSize));
		stream.read(reinterpret_cast<char*>(&heightScale), sizeof(heightScale));

		if (width < 2 || height < 2 || width > 65536 || height > 65536)
			return false;

		m_Width = width;
		m_Height = height;
		m_CellSize = cellSize;
		m_HeightScale = heightScale;

		for (int i = 0; i < 4; ++i)
		{
			TerrainLayer& layer = m_Layers[i];
			uint32_t nameLen = 0;
			stream.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
			if (nameLen > 256)
				return false;
			layer.Name.resize(nameLen);
			if (nameLen > 0)
				stream.read(&layer.Name[0], nameLen);

			uint32_t albedoLen = 0;
			stream.read(reinterpret_cast<char*>(&albedoLen), sizeof(albedoLen));
			if (albedoLen > 1024)
				return false;
			std::string albedoPath(albedoLen, '\0');
			if (albedoLen > 0)
				stream.read(&albedoPath[0], albedoLen);
			uint64_t albedoId = 0;
			stream.read(reinterpret_cast<char*>(&albedoId), sizeof(albedoId));
			layer.AlbedoTexture.Path = albedoPath.empty() ? PathId() : StringPool::Get().InternPath(albedoPath);
			layer.AlbedoTexture.Identity = AssetId { albedoId };

			uint32_t normalLen = 0;
			stream.read(reinterpret_cast<char*>(&normalLen), sizeof(normalLen));
			if (normalLen > 1024)
				return false;
			std::string normalPath(normalLen, '\0');
			if (normalLen > 0)
				stream.read(&normalPath[0], normalLen);
			uint64_t normalId = 0;
			stream.read(reinterpret_cast<char*>(&normalId), sizeof(normalId));
			layer.NormalTexture.Path = normalPath.empty() ? PathId() : StringPool::Get().InternPath(normalPath);
			layer.NormalTexture.Identity = AssetId { normalId };

			stream.read(reinterpret_cast<char*>(&layer.Roughness), sizeof(layer.Roughness));
			stream.read(reinterpret_cast<char*>(&layer.Metallic), sizeof(layer.Metallic));
			stream.read(reinterpret_cast<char*>(&layer.Tiling), sizeof(layer.Tiling));
		}

		const size_t count = static_cast<size_t>(m_Width) * m_Height;
		m_Heights.resize(count);
		m_Splatmap.resize(count);
		m_HoleMask.resize(count);

		stream.read(reinterpret_cast<char*>(m_Heights.data()), count * sizeof(float));
		stream.read(reinterpret_cast<char*>(m_Splatmap.data()), count * sizeof(uint32_t));
		stream.read(reinterpret_cast<char*>(m_HoleMask.data()), count * sizeof(uint8_t));

		m_IsDirty = true;
		return stream.good();
	}

	bool TerrainData::ImportHeightmapRaw16(const std::string& path, uint32_t width, uint32_t height, float minH, float maxH)
	{
		if (width < 2 || height < 2)
			return false;

		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream.is_open())
			return false;

		const std::streamsize fileSize = stream.tellg();
		stream.seekg(0, std::ios::beg);

		const size_t count = static_cast<size_t>(width) * height;
		if (fileSize < static_cast<std::streamsize>(count * sizeof(uint16_t)))
			return false;

		std::vector<uint16_t> raw16(count);
		stream.read(reinterpret_cast<char*>(raw16.data()), count * sizeof(uint16_t));
		if (!stream.good())
			return false;

		// Endianness auto-detection heuristic using Total Variation of adjacent samples
		const size_t sampleProbe = std::min(count - 1, static_cast<size_t>(1024));
		uint64_t tvLe = 0;
		uint64_t tvBe = 0;
		for (size_t i = 0; i < sampleProbe; ++i)
		{
			const uint16_t v0 = raw16[i];
			const uint16_t v1 = raw16[i + 1];
			const uint16_t v0Swapped = static_cast<uint16_t>((v0 >> 8) | (v0 << 8));
			const uint16_t v1Swapped = static_cast<uint16_t>((v1 >> 8) | (v1 << 8));

			tvLe += static_cast<uint64_t>(std::abs(static_cast<int32_t>(v1) - static_cast<int32_t>(v0)));
			tvBe += static_cast<uint64_t>(std::abs(static_cast<int32_t>(v1Swapped) - static_cast<int32_t>(v0Swapped)));
		}

		const bool isBigEndian = (tvBe < tvLe / 2);
		if (isBigEndian)
		{
			for (size_t i = 0; i < count; ++i)
			{
				const uint16_t v = raw16[i];
				raw16[i] = static_cast<uint16_t>((v >> 8) | (v << 8));
			}
		}

		Resize(width, height, m_CellSize);

		const float range = maxH - minH;
		for (size_t i = 0; i < count; ++i)
		{
			const float t = static_cast<float>(raw16[i]) / 65535.0f;
			const float h = minH + t * range;
			m_Heights[i] = (m_HeightScale != 0.0f ? h / m_HeightScale : h);
		}

		m_IsDirty = true;
		return true;
	}

	bool TerrainData::ExportHeightmapRaw16(const std::string& path) const
	{
		std::ofstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return false;

		float minH = 1e9f;
		float maxH = -1e9f;
		for (float h : m_Heights)
		{
			const float wh = h * m_HeightScale;
			minH = std::min(minH, wh);
			maxH = std::max(maxH, wh);
		}
		if (minH >= maxH)
		{
			minH = 0.0f;
			maxH = 256.0f;
		}

		const float range = maxH - minH;
		const size_t count = static_cast<size_t>(m_Width) * m_Height;
		std::vector<uint16_t> raw16(count);

		for (size_t i = 0; i < count; ++i)
		{
			const float wh = m_Heights[i] * m_HeightScale;
			const float t = (wh - minH) / range;
			raw16[i] = static_cast<uint16_t>(glm::clamp(t, 0.0f, 1.0f) * 65535.0f + 0.5f);
		}

		stream.write(reinterpret_cast<const char*>(raw16.data()), count * sizeof(uint16_t));
		return stream.good();
	}

	namespace
	{
		inline float Hash2D(int x, int y, uint32_t seed)
		{
			uint32_t n = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 1013904223u;
			n = (n ^ (n >> 13)) * 1274126177u;
			return static_cast<float>(n % 16777216u) / 16777216.0f;
		}

		inline float SmoothNoise(float x, float y, uint32_t seed)
		{
			int ix = static_cast<int>(std::floor(x));
			int iy = static_cast<int>(std::floor(y));
			float fx = x - static_cast<float>(ix);
			float fy = y - static_cast<float>(iy);

			float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
			float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

			float v00 = Hash2D(ix, iy, seed);
			float v10 = Hash2D(ix + 1, iy, seed);
			float v01 = Hash2D(ix, iy + 1, seed);
			float v11 = Hash2D(ix + 1, iy + 1, seed);

			float v0 = glm::mix(v00, v10, ux);
			float v1 = glm::mix(v01, v11, ux);
			return glm::mix(v0, v1, uy);
		}
	}

	void TerrainData::GenerateProceduralLandscape(uint32_t seed)
	{
		if (m_Width < 2 || m_Height < 2)
			return;

		m_Layers[0].Name = "Alpine Grass (高山草甸)";
		m_Layers[0].AlbedoTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/grass_albedo.png"), AssetId{} };
		m_Layers[0].NormalTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/grass_normal.png"), AssetId{} };
		m_Layers[0].Roughness = 0.85f;
		m_Layers[0].Metallic = 0.0f;
		m_Layers[0].Tiling = glm::vec2(16.0f, 16.0f);

		m_Layers[1].Name = "Forest Dirt (林地黑土)";
		m_Layers[1].AlbedoTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/dirt_albedo.png"), AssetId{} };
		m_Layers[1].NormalTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/dirt_normal.png"), AssetId{} };
		m_Layers[1].Roughness = 0.90f;
		m_Layers[1].Metallic = 0.0f;
		m_Layers[1].Tiling = glm::vec2(14.0f, 14.0f);

		m_Layers[2].Name = "Mountain Rock (花岗岩壁)";
		m_Layers[2].AlbedoTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/rock_albedo.png"), AssetId{} };
		m_Layers[2].NormalTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/rock_normal.png"), AssetId{} };
		m_Layers[2].Roughness = 0.65f;
		m_Layers[2].Metallic = 0.0f;
		m_Layers[2].Tiling = glm::vec2(10.0f, 10.0f);

		m_Layers[3].Name = "Riverbed Sand (河滩细沙)";
		m_Layers[3].AlbedoTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/sand_albedo.png"), AssetId{} };
		m_Layers[3].NormalTexture = AssetRef { StringPool::Get().InternPath("textures/terrain/sand_normal.png"), AssetId{} };
		m_Layers[3].Roughness = 0.92f;
		m_Layers[3].Metallic = 0.0f;
		m_Layers[3].Tiling = glm::vec2(18.0f, 18.0f);

		const float invW = 1.0f / static_cast<float>(m_Width);
		const float invH = 1.0f / static_cast<float>(m_Height);

		// 1. 雄浑连绵的次世代自然山系高程生成 (消除生硬阶梯波与尖刺)
		for (uint32_t z = 0; z < m_Height; ++z)
		{
			for (uint32_t x = 0; x < m_Width; ++x)
			{
				float nx = static_cast<float>(x) * invW;
				float nz = static_cast<float>(z) * invH;

				// 宏观主山体与深谷架构 (低频大尺度)
				float macroContinent = (SmoothNoise(nx * 1.8f, nz * 1.8f, seed) - 0.5f) * 22.0f;

				// 主山脉脊线 (平滑连续多重分形，产生雄奇陡峭的山脊主峰)
				float r1 = SmoothNoise(nx * 3.5f + 0.3f, nz * 3.5f + 0.5f, seed + 101u);
				float ridge1 = 1.0f - std::abs(r1 * 2.0f - 1.0f);
				ridge1 = ridge1 * ridge1 * (SmoothNoise(nx * 1.2f, nz * 1.2f, seed + 55u) * 26.0f);

				// 次级山峦起伏 (中频自然褶皱)
				float r2 = SmoothNoise(nx * 7.0f, nz * 7.0f, seed + 202u);
				float hills = (r2 - 0.5f) * 8.0f;

				// 微地形山脚坡降
				float micro = (SmoothNoise(nx * 18.0f, nz * 18.0f, seed + 303u) - 0.5f) * 2.0f;

				// 基础海拔总和
				float rawHeight = macroContinent + ridge1 + hills + micro;

				// 边缘平缓落入山谷盆地 (平滑衰减，抬高基准高程至 Y >= 2.5m 彻底杜绝与 Y=0 参考网格穿插)
				float dx = (nx - 0.5f) * 2.0f;
				float dz = (nz - 0.5f) * 2.0f;
				float edgeDist = std::sqrt(dx * dx + dz * dz);
				float edgeFalloff = glm::clamp(1.0f - (edgeDist - 0.65f) / 0.35f, 0.0f, 1.0f);
				edgeFalloff = edgeFalloff * edgeFalloff * (3.0f - 2.0f * edgeFalloff);

				float finalHeight = std::max(2.5f, (rawHeight + 10.0f) * edgeFalloff + 2.5f);
				SetHeight(x, z, finalHeight);
			}
		}

		// 2. 邻域柔化迭代 (Gaussian-like Relaxation 消除网格尖峰噪点)
		std::vector<float> smoothed(m_Heights.size());
		for (uint32_t z = 0; z < m_Height; ++z)
		{
			for (uint32_t x = 0; x < m_Width; ++x)
			{
				float sum = 0.0f;
				float weight = 0.0f;
				for (int dz = -1; dz <= 1; ++dz)
				{
					for (int dx = -1; dx <= 1; ++dx)
					{
						int nx = static_cast<int>(x) + dx;
						int nz = static_cast<int>(z) + dz;
						if (nx >= 0 && nx < static_cast<int>(m_Width) && nz >= 0 && nz < static_cast<int>(m_Height))
						{
							float w = (dx == 0 && dz == 0) ? 4.0f : ((dx == 0 || dz == 0) ? 2.0f : 1.0f);
							sum += GetHeight(static_cast<uint32_t>(nx), static_cast<uint32_t>(nz)) * w;
							weight += w;
						}
					}
				}
				smoothed[z * m_Width + x] = sum / weight;
			}
		}
		for (size_t i = 0; i < smoothed.size(); ++i)
			m_Heights[i] = smoothed[i];

		// 3. 次世代生物群落权重分布 (平滑连续过渡，彻底消灭孤立色块与方格感)
		for (uint32_t z = 0; z < m_Height; ++z)
		{
			for (uint32_t x = 0; x < m_Width; ++x)
			{
				float worldX = static_cast<float>(x) * m_CellSize;
				float worldZ = static_cast<float>(z) * m_CellSize;
				float h = GetHeight(x, z);
				glm::vec3 normal = SampleNormal(worldX, worldZ);

				// 坡度计算: 0.0 为纯平原缓坡，1.0 为90度峭壁
				float slope = 1.0f - glm::clamp(normal.y, 0.0f, 1.0f);

				// 引入高频有机扰动打破几何单调边界
				float nx = static_cast<float>(x) * invW;
				float nz = static_cast<float>(z) * invH;
				float warp = SmoothNoise(nx * 14.0f, nz * 14.0f, seed + 888u) * 0.12f - 0.06f;

				float grassWeight = 0.0f;
				float dirtWeight = 0.0f;
				float rockWeight = 0.0f;
				float sandWeight = 0.0f;

				// 陡峭山体与绝壁: 花岗岩壁
				if (slope + warp > 0.32f)
				{
					rockWeight = glm::clamp(((slope + warp) - 0.32f) / 0.18f, 0.0f, 1.0f);
					dirtWeight = 1.0f - rockWeight;
				}
				else if (slope + warp > 0.14f) // 山腰与山麓过渡带: 碎石林土
				{
					dirtWeight = glm::clamp(((slope + warp) - 0.14f) / 0.18f, 0.0f, 1.0f);
					grassWeight = 1.0f - dirtWeight;
				}
				else // 广阔舒展平原: 茂密高山草甸
				{
					grassWeight = 1.0f;
				}

				// 蜿蜒溪谷与河滩低地: 仅在低洼且平缓地带自然沉积河滩细沙与卵石
				if (h < 5.0f && slope < 0.20f)
				{
					float riverWarp = SmoothNoise(nx * 9.0f, nz * 9.0f, seed + 999u) * 1.5f;
					float sandFactor = glm::clamp((5.0f - (h + riverWarp)) / 2.5f, 0.0f, 0.85f);
					sandWeight = sandFactor;
					grassWeight *= (1.0f - sandFactor);
					dirtWeight *= (1.0f - sandFactor);
				}

				float total = grassWeight + dirtWeight + rockWeight + sandWeight;
				if (total > 0.0001f)
				{
					grassWeight /= total;
					dirtWeight /= total;
					rockWeight /= total;
					sandWeight /= total;
				}
				else
				{
					grassWeight = 1.0f;
				}

				SetSplatWeights(x, z, glm::vec4(grassWeight, dirtWeight, rockWeight, sandWeight));
			}
		}

		m_IsDirty = true;
	}

}
