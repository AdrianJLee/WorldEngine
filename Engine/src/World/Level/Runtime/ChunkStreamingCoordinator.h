#pragma once

#include "World/Level/Runtime/StreamableChunk.h"
#include "World/Level/Core/IStreamingDriver.h"
#include "World/Level/Core/DeltaState.h"
#include <memory>

namespace World::Level
{
	class ChunkStreamingCoordinator
	{
	public:
		ChunkStreamingCoordinator();
		~ChunkStreamingCoordinator();

		void SetActiveScene(Scene* scene);
		Scene* GetActiveScene() const;

		void SetDeltaStateRegistry(DeltaStateRegistry* deltaState);
		DeltaStateRegistry* GetDeltaStateRegistry() const;

		void SetTimeSliceBudgetMs(float budgetMs);
		float GetTimeSliceBudgetMs() const;

		void RegisterChunk(std::shared_ptr<StreamableChunk> chunk);
		std::shared_ptr<StreamableChunk> FindChunk(uint64_t chunkId) const;

		void RegisterDriver(std::unique_ptr<IStreamingDriver> driver);

		void RequestLoad(uint64_t chunkId);
		void RequestUnload(uint64_t chunkId);

		void Update(float deltaTime, const glm::vec3& viewPosition, const glm::mat4& viewProjection);

		size_t ActiveChunkCount() const;
		size_t TotalRegisteredChunks() const;
		bool IsChunkActive(uint64_t chunkId) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
