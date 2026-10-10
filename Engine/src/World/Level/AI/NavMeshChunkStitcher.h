#pragma once

#include "World/Core/Export.h"

#include "World/Level/AI/NavMeshTileTypes.h"
#include <memory>

namespace World::Level::AI
{
	class WLD_API NavMeshChunkStitcher
	{
	public:
		NavMeshChunkStitcher();
		~NavMeshChunkStitcher();

		void RegisterTile(uint64_t chunkId, const NavMeshTileKey& key, const NavMeshTileHeader& header);
		void UnregisterTile(uint64_t chunkId);

		TileStitchState GetTileState(const NavMeshTileKey& key) const;
		size_t GetActiveTileCount() const;
		size_t GetStitchedBorderCount() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
