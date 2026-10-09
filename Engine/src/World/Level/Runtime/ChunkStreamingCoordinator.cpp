#include "wldpch.h"
#include "World/Level/Runtime/ChunkStreamingCoordinator.h"
#include <algorithm>
#include <vector>

namespace World::Level
{
	struct ChunkEntry
	{
		uint64_t ChunkID{ 0 };
		std::shared_ptr<StreamableChunk> Chunk;
	};

	struct ChunkStreamingCoordinator::Impl
	{
		Scene* ActiveScene{ nullptr };
		DeltaStateRegistry* DeltaState{ nullptr };
		float TimeSliceBudgetMs{ 2.0f };

		std::vector<ChunkEntry> Chunks;
		std::vector<std::unique_ptr<IStreamingDriver>> Drivers;
	};

	ChunkStreamingCoordinator::ChunkStreamingCoordinator()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	ChunkStreamingCoordinator::~ChunkStreamingCoordinator() = default;

	void ChunkStreamingCoordinator::SetActiveScene(Scene* scene) { m_Impl->ActiveScene = scene; }
	Scene* ChunkStreamingCoordinator::GetActiveScene() const { return m_Impl->ActiveScene; }

	void ChunkStreamingCoordinator::SetDeltaStateRegistry(DeltaStateRegistry* deltaState) { m_Impl->DeltaState = deltaState; }
	DeltaStateRegistry* ChunkStreamingCoordinator::GetDeltaStateRegistry() const { return m_Impl->DeltaState; }

	void ChunkStreamingCoordinator::SetTimeSliceBudgetMs(float budgetMs) { m_Impl->TimeSliceBudgetMs = budgetMs; }
	float ChunkStreamingCoordinator::GetTimeSliceBudgetMs() const { return m_Impl->TimeSliceBudgetMs; }

	void ChunkStreamingCoordinator::RegisterChunk(std::shared_ptr<StreamableChunk> chunk)
	{
		if (!chunk)
			return;

		const uint64_t cid = chunk->GetChunkID();
		auto it = std::find_if(m_Impl->Chunks.begin(), m_Impl->Chunks.end(), [cid](const ChunkEntry& e) {
			return e.ChunkID == cid;
		});

		if (it != m_Impl->Chunks.end())
		{
			it->Chunk = chunk;
		}
		else
		{
			m_Impl->Chunks.push_back({ cid, chunk });
		}
	}

	std::shared_ptr<StreamableChunk> ChunkStreamingCoordinator::FindChunk(uint64_t chunkId) const
	{
		auto it = std::find_if(m_Impl->Chunks.begin(), m_Impl->Chunks.end(), [chunkId](const ChunkEntry& e) {
			return e.ChunkID == chunkId;
		});

		if (it != m_Impl->Chunks.end())
		{
			return it->Chunk;
		}
		return nullptr;
	}

	void ChunkStreamingCoordinator::RegisterDriver(std::unique_ptr<IStreamingDriver> driver)
	{
		if (driver)
		{
			m_Impl->Drivers.push_back(std::move(driver));
		}
	}

	void ChunkStreamingCoordinator::RequestLoad(uint64_t chunkId)
	{
		auto chunk = FindChunk(chunkId);
		if (chunk && chunk->GetState() == ChunkState::MemoryReady && m_Impl->ActiveScene)
		{
			chunk->OnActivateWithDelta(*m_Impl->ActiveScene, m_Impl->DeltaState);
		}
	}

	void ChunkStreamingCoordinator::RequestUnload(uint64_t chunkId)
	{
		auto chunk = FindChunk(chunkId);
		if (chunk && chunk->GetState() == ChunkState::Active && m_Impl->ActiveScene)
		{
			chunk->OnDeactivate(*m_Impl->ActiveScene);
		}
	}

	void ChunkStreamingCoordinator::Update(float, const glm::vec3& viewPosition, const glm::mat4& viewProjection)
	{
		std::vector<uint64_t> toLoad;
		std::vector<uint64_t> toUnload;

		for (auto& driver : m_Impl->Drivers)
		{
			if (driver)
			{
				driver->EvaluateStreaming(viewPosition, viewProjection, toLoad, toUnload);
			}
		}

		for (uint64_t id : toLoad)
		{
			RequestLoad(id);
		}

		for (uint64_t id : toUnload)
		{
			RequestUnload(id);
		}
	}

	size_t ChunkStreamingCoordinator::ActiveChunkCount() const
	{
		size_t count = 0;
		for (const auto& entry : m_Impl->Chunks)
		{
			if (entry.Chunk && entry.Chunk->GetState() == ChunkState::Active)
			{
				count++;
			}
		}
		return count;
	}

	size_t ChunkStreamingCoordinator::TotalRegisteredChunks() const
	{
		return m_Impl->Chunks.size();
	}

	bool ChunkStreamingCoordinator::IsChunkActive(uint64_t chunkId) const
	{
		auto chunk = FindChunk(chunkId);
		return chunk && (chunk->GetState() == ChunkState::Active);
	}
}
