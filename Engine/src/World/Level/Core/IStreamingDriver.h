#pragma once

#include "World/Level/Core/ChunkTypes.h"
#include <vector>

namespace World::Level
{
	class IStreamingDriver
	{
	public:
		virtual ~IStreamingDriver() = default;

		virtual const char* GetDriverName() const = 0;

		virtual void EvaluateStreaming(
			const glm::vec3& viewPosition,
			const glm::mat4& viewProjection,
			std::vector<uint64_t>& outChunksToLoad,
			std::vector<uint64_t>& outChunksToUnload) = 0;
	};
}
