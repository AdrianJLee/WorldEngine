#include "wldpch.h"
#include "World/Level/Streaming/SectorPortalDriver.h"
#include <algorithm>
#include <queue>
#include <unordered_set>
#include <vector>

namespace World::Level
{
	struct SectorNode
	{
		uint64_t ChunkID{ 0 };
		ChunkBounds Bounds;
		std::vector<Portal> Portals;
		bool IsLoaded{ false };
	};

	struct SectorPortalDriver::Impl
	{
		std::vector<SectorNode> Sectors;

		SectorNode* FindSector(uint64_t chunkId)
		{
			for (auto& s : Sectors)
			{
				if (s.ChunkID == chunkId)
					return &s;
			}
			return nullptr;
		}

		SectorNode* FindSectorContaining(const glm::vec3& point)
		{
			for (auto& s : Sectors)
			{
				if (s.Bounds.Contains(point))
					return &s;
			}

			// Fallback: closest by distance
			float closestDist = 1e9f;
			SectorNode* closest = nullptr;
			for (auto& s : Sectors)
			{
				glm::vec3 center = (s.Bounds.Min + s.Bounds.Max) * 0.5f;
				float d = glm::distance(point, center);
				if (d < closestDist)
				{
					closestDist = d;
					closest = &s;
				}
			}
			return closest;
		}
	};

	SectorPortalDriver::SectorPortalDriver()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	SectorPortalDriver::~SectorPortalDriver() = default;

	void SectorPortalDriver::AddSector(uint64_t chunkId, const ChunkBounds& bounds)
	{
		if (auto* existing = m_Impl->FindSector(chunkId))
		{
			existing->Bounds = bounds;
		}
		else
		{
			m_Impl->Sectors.push_back({ chunkId, bounds, {}, false });
		}
	}

	void SectorPortalDriver::AddPortal(uint64_t fromChunkId, uint64_t toChunkId, const glm::vec3& center, const glm::vec3& extents, bool isOpen)
	{
		if (auto* sector = m_Impl->FindSector(fromChunkId))
		{
			sector->Portals.push_back({ toChunkId, center, extents, isOpen });
		}
	}

	void SectorPortalDriver::SetPortalOpen(uint64_t fromChunkId, uint64_t toChunkId, bool isOpen)
	{
		if (auto* sector = m_Impl->FindSector(fromChunkId))
		{
			for (auto& p : sector->Portals)
			{
				if (p.TargetChunkID == toChunkId)
				{
					p.IsOpen = isOpen;
				}
			}
		}
	}

	void SectorPortalDriver::EvaluateStreaming(
		const glm::vec3& viewPosition,
		const glm::mat4&,
		std::vector<uint64_t>& outChunksToLoad,
		std::vector<uint64_t>& outChunksToUnload)
	{
		std::unordered_set<uint64_t> visibleSectors;

		SectorNode* current = m_Impl->FindSectorContaining(viewPosition);
		if (current)
		{
			visibleSectors.insert(current->ChunkID);

			std::queue<SectorNode*> traverseQueue;
			traverseQueue.push(current);

			while (!traverseQueue.empty())
			{
				SectorNode* node = traverseQueue.front();
				traverseQueue.pop();

				for (const auto& portal : node->Portals)
				{
					if (portal.IsOpen && visibleSectors.find(portal.TargetChunkID) == visibleSectors.end())
					{
						visibleSectors.insert(portal.TargetChunkID);
						if (auto* neighbor = m_Impl->FindSector(portal.TargetChunkID))
						{
							traverseQueue.push(neighbor);
						}
					}
				}
			}
		}

		for (auto& s : m_Impl->Sectors)
		{
			const bool shouldBeLoaded = (visibleSectors.find(s.ChunkID) != visibleSectors.end());

			if (shouldBeLoaded && !s.IsLoaded)
			{
				outChunksToLoad.push_back(s.ChunkID);
				s.IsLoaded = true;
			}
			else if (!shouldBeLoaded && s.IsLoaded)
			{
				outChunksToUnload.push_back(s.ChunkID);
				s.IsLoaded = false;
			}
		}
	}

	size_t SectorPortalDriver::GetSectorCount() const
	{
		return m_Impl->Sectors.size();
	}

	size_t SectorPortalDriver::GetPortalCount() const
	{
		size_t count = 0;
		for (const auto& s : m_Impl->Sectors)
		{
			count += s.Portals.size();
		}
		return count;
	}
}
