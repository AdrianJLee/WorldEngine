#pragma once

#include "World/Level/Core/ChunkTypes.h"
#include <glm/glm.hpp>
#include <memory>

namespace World::Level::AI
{
	enum class PerceptionStatus : uint8_t
	{
		Clear = 0,
		BlockedByUnloadedChunk = 1,
		BlockedByGeometry = 2
	};

	class AiPerceptionGate
	{
	public:
		AiPerceptionGate();
		~AiPerceptionGate();

		void RegisterChunk(uint64_t chunkId, const ChunkBounds& bounds, bool isLoaded = false);
		void SetChunkLoaded(uint64_t chunkId, bool isLoaded);

		PerceptionStatus TestRay(const glm::vec3& start, const glm::vec3& end, uint64_t* outBlockedChunkId = nullptr) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
