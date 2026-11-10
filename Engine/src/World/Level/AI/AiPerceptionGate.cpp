#include "wldpch.h"
#include "World/Level/AI/AiPerceptionGate.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace World::Level::AI
{
	namespace
	{
		bool RayIntersectsAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float rayLength, const ChunkBounds& box)
		{
			float tmin = 0.0f;
			float tmax = rayLength;

			for (int i = 0; i < 3; ++i)
			{
				if (std::abs(rayDir[i]) < 1e-6f)
				{
					if (rayOrigin[i] < box.Min[i] || rayOrigin[i] > box.Max[i])
						return false;
				}
				else
				{
					float invD = 1.0f / rayDir[i];
					float t0 = (box.Min[i] - rayOrigin[i]) * invD;
					float t1 = (box.Max[i] - rayOrigin[i]) * invD;
					if (invD < 0.0f)
					{
						std::swap(t0, t1);
					}
					tmin = std::max(tmin, t0);
					tmax = std::min(tmax, t1);
					if (tmax < tmin)
					{
						return false;
					}
				}
			}
			return true;
		}
	}

	struct ChunkPerceptionRecord
	{
		uint64_t ChunkID{ 0 };
		ChunkBounds Bounds;
		bool IsLoaded{ false };
	};

	struct AiPerceptionGate::Impl
	{
		std::vector<ChunkPerceptionRecord> Chunks;
	};

	AiPerceptionGate::AiPerceptionGate()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	AiPerceptionGate::~AiPerceptionGate() = default;

	void AiPerceptionGate::RegisterChunk(uint64_t chunkId, const ChunkBounds& bounds, bool isLoaded)
	{
		for (auto& c : m_Impl->Chunks)
		{
			if (c.ChunkID == chunkId)
			{
				c.Bounds = bounds;
				c.IsLoaded = isLoaded;
				return;
			}
		}
		m_Impl->Chunks.push_back({ chunkId, bounds, isLoaded });
	}

	void AiPerceptionGate::SetChunkLoaded(uint64_t chunkId, bool isLoaded)
	{
		for (auto& c : m_Impl->Chunks)
		{
			if (c.ChunkID == chunkId)
			{
				c.IsLoaded = isLoaded;
				return;
			}
		}
	}

	PerceptionStatus AiPerceptionGate::TestRay(const glm::vec3& start, const glm::vec3& end, uint64_t* outBlockedChunkId) const
	{
		const glm::vec3 delta = end - start;
		const float rayLength = glm::length(delta);
		if (rayLength < 1e-6f)
			return PerceptionStatus::Clear;

		const glm::vec3 rayDir = delta / rayLength;

		for (const auto& c : m_Impl->Chunks)
		{
			if (!c.IsLoaded)
			{
				if (RayIntersectsAABB(start, rayDir, rayLength, c.Bounds))
				{
					if (outBlockedChunkId)
					{
						*outBlockedChunkId = c.ChunkID;
					}
					return PerceptionStatus::BlockedByUnloadedChunk;
				}
			}
		}

		return PerceptionStatus::Clear;
	}
}
