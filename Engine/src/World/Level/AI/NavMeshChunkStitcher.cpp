#include "wldpch.h"
#include "World/Level/AI/NavMeshChunkStitcher.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace World::Level::AI
{
	struct ActiveTileRecord
	{
		uint64_t ChunkID{ 0 };
		NavMeshTileKey Key;
		NavMeshTileHeader Header;
		TileStitchState State{ TileStitchState::Loaded };
	};

	struct NavMeshChunkStitcher::Impl
	{
		std::vector<ActiveTileRecord> Tiles;
		size_t StitchedBorderCount{ 0 };

		static bool AreNeighbors(const NavMeshTileKey& a, const NavMeshTileKey& b)
		{
			if (a.SubspaceID != b.SubspaceID)
				return false;

			const int dx = std::abs(a.TileX - b.TileX);
			const int dy = std::abs(a.TileY - b.TileY);
			return (dx == 1 && dy == 0) || (dx == 0 && dy == 1);
		}
	};

	NavMeshChunkStitcher::NavMeshChunkStitcher()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	NavMeshChunkStitcher::~NavMeshChunkStitcher() = default;

	void NavMeshChunkStitcher::RegisterTile(uint64_t chunkId, const NavMeshTileKey& key, const NavMeshTileHeader& header)
	{
		ActiveTileRecord record;
		record.ChunkID = chunkId;
		record.Key = key;
		record.Header = header;
		record.State = TileStitchState::Loaded;

		bool hasNeighbor = false;
		for (auto& existing : m_Impl->Tiles)
		{
			if (Impl::AreNeighbors(key, existing.Key))
			{
				m_Impl->StitchedBorderCount++;
				existing.State = TileStitchState::Stitched;
				hasNeighbor = true;
			}
		}

		if (hasNeighbor)
		{
			record.State = TileStitchState::Stitched;
		}

		m_Impl->Tiles.push_back(record);
	}

	void NavMeshChunkStitcher::UnregisterTile(uint64_t chunkId)
	{
		std::vector<NavMeshTileKey> removedKeys;
		for (const auto& tile : m_Impl->Tiles)
		{
			if (tile.ChunkID == chunkId)
			{
				removedKeys.push_back(tile.Key);
			}
		}

		for (const auto& rk : removedKeys)
		{
			for (auto& existing : m_Impl->Tiles)
			{
				if (existing.ChunkID != chunkId && Impl::AreNeighbors(rk, existing.Key))
				{
					if (m_Impl->StitchedBorderCount > 0)
					{
						m_Impl->StitchedBorderCount--;
					}

					bool stillHasNeighbor = false;
					for (const auto& other : m_Impl->Tiles)
					{
						if (other.ChunkID != chunkId && other.Key != existing.Key && Impl::AreNeighbors(existing.Key, other.Key))
						{
							stillHasNeighbor = true;
							break;
						}
					}
					if (!stillHasNeighbor)
					{
						existing.State = TileStitchState::Loaded;
					}
				}
			}
		}

		m_Impl->Tiles.erase(
			std::remove_if(m_Impl->Tiles.begin(), m_Impl->Tiles.end(), [chunkId](const ActiveTileRecord& t) {
				return t.ChunkID == chunkId;
			}),
			m_Impl->Tiles.end());
	}

	TileStitchState NavMeshChunkStitcher::GetTileState(const NavMeshTileKey& key) const
	{
		for (const auto& tile : m_Impl->Tiles)
		{
			if (tile.Key == key)
			{
				return tile.State;
			}
		}
		return TileStitchState::Unloaded;
	}

	size_t NavMeshChunkStitcher::GetActiveTileCount() const
	{
		return m_Impl->Tiles.size();
	}

	size_t NavMeshChunkStitcher::GetStitchedBorderCount() const
	{
		return m_Impl->StitchedBorderCount;
	}
}
