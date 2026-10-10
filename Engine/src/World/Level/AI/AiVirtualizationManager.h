#pragma once

#include "World/Core/Export.h"

#include "World/Level/AI/AiVirtualizationTypes.h"
#include "World/Level/Core/DeltaState.h"
#include <memory>
#include <vector>

namespace World::Level::AI
{
	class WLD_API AiVirtualizationManager
	{
	public:
		AiVirtualizationManager();
		~AiVirtualizationManager();

		void RegisterAi(uint64_t entityUuid, uint64_t homeChunkId, const glm::vec3& pos, float health = 100.0f, uint32_t waypoint = 0);
		void UnregisterAi(uint64_t entityUuid);

		void UpdateLOD(const glm::vec3& playerPos, float activeRadius = 60.0f, float hibernateRadius = 120.0f);
		AiLodTier GetAiTier(uint64_t entityUuid) const;

		void OnChunkUnloaded(uint64_t chunkId, DeltaStateRegistry& deltaState);
		void OnChunkLoaded(uint64_t chunkId, const DeltaStateRegistry& deltaState, std::vector<AiVirtualSnapshot>& outRestoredAi);

		size_t GetTrackedAiCount() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
