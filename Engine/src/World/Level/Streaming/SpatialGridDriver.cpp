#include "wldpch.h"
#include "World/Level/Streaming/SpatialGridDriver.h"
#include <algorithm>
#include <vector>

namespace World::Level
{
	namespace
	{
		float DistanceToAABB(const glm::vec3& p, const ChunkBounds& b)
		{
			glm::vec3 closest = glm::clamp(p, b.Min, b.Max);
			return glm::distance(p, closest);
		}
	}

	struct ChunkSpatialEntry
	{
		uint64_t ChunkID{ 0 };
		ChunkBounds Bounds;
		bool IsLoaded{ false };
	};

	struct SpatialGridDriver::Impl
	{
		float LoadDistance{ 128.0f };
		float UnloadDistance{ 160.0f };
		bool FrustumCullingEnabled{ false };
		std::vector<ChunkSpatialEntry> Chunks;
	};

	SpatialGridDriver::SpatialGridDriver(float loadDistance, float unloadDistance)
		: m_Impl(std::make_unique<Impl>())
	{
		m_Impl->LoadDistance = loadDistance;
		m_Impl->UnloadDistance = unloadDistance > loadDistance ? unloadDistance : (loadDistance + 32.0f);
	}

	SpatialGridDriver::~SpatialGridDriver() = default;

	void SpatialGridDriver::AddChunk(uint64_t chunkId, const ChunkBounds& bounds)
	{
		auto it = std::find_if(m_Impl->Chunks.begin(), m_Impl->Chunks.end(), [chunkId](const ChunkSpatialEntry& e) {
			return e.ChunkID == chunkId;
		});

		if (it != m_Impl->Chunks.end())
		{
			it->Bounds = bounds;
		}
		else
		{
			m_Impl->Chunks.push_back({ chunkId, bounds, false });
		}
	}

	void SpatialGridDriver::RemoveChunk(uint64_t chunkId)
	{
		m_Impl->Chunks.erase(
			std::remove_if(m_Impl->Chunks.begin(), m_Impl->Chunks.end(), [chunkId](const ChunkSpatialEntry& e) {
				return e.ChunkID == chunkId;
			}),
			m_Impl->Chunks.end());
	}

	void SpatialGridDriver::EvaluateStreaming(
		const glm::vec3& viewPosition,
		const glm::mat4&,
		std::vector<uint64_t>& outChunksToLoad,
		std::vector<uint64_t>& outChunksToUnload)
	{
		for (auto& entry : m_Impl->Chunks)
		{
			const float dist = DistanceToAABB(viewPosition, entry.Bounds);

			if (!entry.IsLoaded && dist <= m_Impl->LoadDistance)
			{
				outChunksToLoad.push_back(entry.ChunkID);
				entry.IsLoaded = true;
			}
			else if (entry.IsLoaded && dist > m_Impl->UnloadDistance)
			{
				outChunksToUnload.push_back(entry.ChunkID);
				entry.IsLoaded = false;
			}
		}
	}

	void SpatialGridDriver::SetLoadDistance(float d)
	{
		m_Impl->LoadDistance = d;
		if (m_Impl->UnloadDistance < m_Impl->LoadDistance)
		{
			m_Impl->UnloadDistance = m_Impl->LoadDistance + 32.0f;
		}
	}

	float SpatialGridDriver::GetLoadDistance() const { return m_Impl->LoadDistance; }

	void SpatialGridDriver::SetUnloadDistance(float d)
	{
		m_Impl->UnloadDistance = d > m_Impl->LoadDistance ? d : (m_Impl->LoadDistance + 32.0f);
	}

	float SpatialGridDriver::GetUnloadDistance() const { return m_Impl->UnloadDistance; }

	void SpatialGridDriver::SetFrustumCullingEnabled(bool enable) { m_Impl->FrustumCullingEnabled = enable; }
	bool SpatialGridDriver::IsFrustumCullingEnabled() const { return m_Impl->FrustumCullingEnabled; }

	size_t SpatialGridDriver::GetTrackedChunkCount() const { return m_Impl->Chunks.size(); }
}
