#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace World::Level::AI
{
	constexpr uint32_t NavMeshTileMagic = 0x4E56544C; // "NVTL"

	enum class TileStitchState : uint8_t
	{
		Unloaded = 0,
		Loaded = 1,
		Stitched = 2,
		Detached = 3
	};

	struct NavMeshTileKey
	{
		uint64_t ChunkID{ 0 };
		int32_t TileX{ 0 };
		int32_t TileY{ 0 };
		uint32_t SubspaceID{ 0 };

		bool operator==(const NavMeshTileKey& other) const
		{
			return ChunkID == other.ChunkID &&
			       TileX == other.TileX &&
			       TileY == other.TileY &&
			       SubspaceID == other.SubspaceID;
		}

		bool operator!=(const NavMeshTileKey& other) const
		{
			return !(*this == other);
		}
	};

	struct NavMeshTileHeader
	{
		uint32_t Magic{ NavMeshTileMagic };
		uint32_t Version{ 1 };
		uint32_t VertCount{ 0 };
		uint32_t PolyCount{ 0 };
		uint32_t DetailVertCount{ 0 };
		uint32_t DetailTriCount{ 0 };
		uint32_t OffMeshConnectionCount{ 0 };
		float WalkableHeight{ 2.0f };
		float WalkableRadius{ 0.6f };
		float WalkableClimb{ 0.9f };
		glm::vec3 BoundsMin{ 0.0f };
		glm::vec3 BoundsMax{ 0.0f };
	};
}
