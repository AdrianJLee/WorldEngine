#pragma once

#include "World/Level/Core/ChunkTypes.h"
#include "World/Level/Core/ChunkHeader.h"

namespace World
{
	class Scene;
}

namespace World::Level
{
	class IStreamableChunk
	{
	public:
		virtual ~IStreamableChunk() = default;

		virtual uint64_t GetChunkID() const = 0;
		virtual SubspaceID GetSubspaceID() const = 0;
		virtual const ChunkBounds& GetBounds() const = 0;
		virtual ChunkState GetState() const = 0;

		virtual void OnActivate(Scene& targetScene) = 0;
		virtual void OnDeactivate(Scene& targetScene) = 0;
	};
}
