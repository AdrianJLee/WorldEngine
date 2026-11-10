#include "wldpch.h"
#include "World/Level/Streaming/SpatialGridManager.h"
#include <algorithm>

namespace World::Level
{
	SpatialGridManager::SpatialGridManager(float chunkSize, float nearRadius, float midRadius, float farRadius)
		: m_ChunkSize(chunkSize), m_NearRadius(nearRadius), m_MidRadius(midRadius), m_FarRadius(farRadius)
	{
	}

	SpatialGridManager::~SpatialGridManager() = default;

	void SpatialGridManager::SetChunkSize(float size)
	{
		m_ChunkSize = std::max(1.0f, size);
	}

	void SpatialGridManager::SetStreamingRadii(float nearRadius, float midRadius, float farRadius)
	{
		m_NearRadius = nearRadius;
		m_MidRadius = midRadius;
		m_FarRadius = farRadius;
	}

	bool SpatialGridManager::IsChunkActive(const ChunkCoord3D& coord) const
	{
		return m_ActiveChunks.find(coord) != m_ActiveChunks.end();
	}

	void SpatialGridManager::MarkChunkLoaded(const ChunkCoord3D& coord, StreamingLodTier tier)
	{
		m_ActiveChunks[coord] = tier;
	}

	void SpatialGridManager::MarkChunkUnloaded(const ChunkCoord3D& coord)
	{
		m_ActiveChunks.erase(coord);
	}

	ChunkStreamingEvaluation SpatialGridManager::Evaluate(const glm::vec3& viewerPosition, bool lockZPlane)
	{
		ChunkStreamingEvaluation eval;
		ChunkCoord3D centerChunk = ChunkCoord3D::WorldToChunk(viewerPosition, m_ChunkSize);

		int32_t chunkRadius = static_cast<int32_t>(std::ceil(m_FarRadius / m_ChunkSize));
		float nearSq = m_NearRadius * m_NearRadius;
		float midSq = m_MidRadius * m_MidRadius;
		float farSq = m_FarRadius * m_FarRadius;

		int32_t minZ = lockZPlane ? 0 : (centerChunk.z - chunkRadius);
		int32_t maxZ = lockZPlane ? 0 : (centerChunk.z + chunkRadius);

		std::unordered_set<ChunkCoord3D, ChunkCoord3DHash> requiredChunks;

		for (int32_t x = centerChunk.x - chunkRadius; x <= centerChunk.x + chunkRadius; ++x)
		{
			for (int32_t y = centerChunk.y - chunkRadius; y <= centerChunk.y + chunkRadius; ++y)
			{
				for (int32_t z = minZ; z <= maxZ; ++z)
				{
					ChunkCoord3D coord { x, y, z };
					glm::vec3 center = coord.GetWorldCenter(m_ChunkSize);
					glm::vec3 diff = center - viewerPosition;
					if (lockZPlane) diff.z = 0.0f;
					float distSq = glm::dot(diff, diff);

					if (distSq <= farSq)
					{
						requiredChunks.insert(coord);
						auto it = m_ActiveChunks.find(coord);
						StreamingLodTier targetTier = (distSq <= nearSq) ? StreamingLodTier::Near
						                            : (distSq <= midSq)  ? StreamingLodTier::Mid
						                                                 : StreamingLodTier::Far;

						if (it == m_ActiveChunks.end() || it->second != targetTier)
						{
							if (targetTier == StreamingLodTier::Near) eval.ChunksToLoadNear.push_back(coord);
							else if (targetTier == StreamingLodTier::Mid) eval.ChunksToLoadMid.push_back(coord);
							else eval.ChunksToLoadFar.push_back(coord);
						}
					}
				}
			}
		}

		for (const auto& [coord, tier] : m_ActiveChunks)
		{
			if (requiredChunks.find(coord) == requiredChunks.end())
			{
				eval.ChunksToUnload.push_back(coord);
			}
		}

		return eval;
	}
}
