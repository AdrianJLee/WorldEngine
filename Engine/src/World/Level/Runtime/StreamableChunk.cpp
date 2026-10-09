#include "wldpch.h"
#include "World/Level/Runtime/StreamableChunk.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace World::Level
{
	struct StreamableChunk::Impl
	{
		ChunkHeader Header;
		ChunkBounds Bounds;
		ChunkState State{ ChunkState::Unloaded };
		std::unique_ptr<StaticEntityRecord[]> Records;
		size_t RecordCount{ 0 };
		std::vector<Entity> SpawnedEntities;
		Scene* ActiveScene{ nullptr };
	};

	StreamableChunk::StreamableChunk()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	StreamableChunk::~StreamableChunk()
	{
		if (m_Impl && m_Impl->ActiveScene && !m_Impl->SpawnedEntities.empty())
		{
			for (auto& ent : m_Impl->SpawnedEntities)
			{
				if (ent.IsValid())
				{
					Entity::DestroyEntity(m_Impl->ActiveScene, ent);
				}
			}
			m_Impl->SpawnedEntities.clear();
		}
	}

	bool StreamableChunk::LoadFromFile(const std::filesystem::path& chunkPath, std::string* outError)
	{
		if (!std::filesystem::exists(chunkPath))
		{
			if (outError) *outError = "Chunk file not found: " + chunkPath.string();
			m_Impl->State = ChunkState::Unloaded;
			return false;
		}

		m_Impl->State = ChunkState::IO_Loading;
		std::ifstream fin(chunkPath, std::ios::binary);
		if (!fin.is_open())
		{
			if (outError) *outError = "Failed to open chunk file: " + chunkPath.string();
			m_Impl->State = ChunkState::Unloaded;
			return false;
		}

		fin.read(reinterpret_cast<char*>(&m_Impl->Header), sizeof(ChunkHeader));
		if (fin.gcount() != sizeof(ChunkHeader))
		{
			if (outError) *outError = "Chunk header read incomplete: " + chunkPath.string();
			m_Impl->State = ChunkState::Unloaded;
			return false;
		}

		if (m_Impl->Header.Magic != ChunkMagic || m_Impl->Header.Version != ChunkVersion)
		{
			if (outError) *outError = "Invalid chunk magic or version in: " + chunkPath.string();
			m_Impl->State = ChunkState::Unloaded;
			return false;
		}

		m_Impl->Bounds.Min = m_Impl->Header.BoundsMin;
		m_Impl->Bounds.Max = m_Impl->Header.BoundsMax;

		m_Impl->RecordCount = m_Impl->Header.StaticEntityCount;
		if (m_Impl->RecordCount > 0)
		{
			m_Impl->Records = std::make_unique<StaticEntityRecord[]>(m_Impl->RecordCount);
			const size_t bytesToRead = m_Impl->RecordCount * sizeof(StaticEntityRecord);
			fin.read(reinterpret_cast<char*>(m_Impl->Records.get()), bytesToRead);
			if (static_cast<size_t>(fin.gcount()) != bytesToRead)
			{
				if (outError) *outError = "Corrupted chunk payload in: " + chunkPath.string();
				m_Impl->State = ChunkState::Unloaded;
				return false;
			}
		}
		else
		{
			m_Impl->Records.reset();
		}

		m_Impl->State = ChunkState::MemoryReady;
		return true;
	}

	uint64_t StreamableChunk::GetChunkID() const { return m_Impl->Header.ChunkID; }
	SubspaceID StreamableChunk::GetSubspaceID() const { return m_Impl->Header.SubspaceID; }
	const ChunkBounds& StreamableChunk::GetBounds() const { return m_Impl->Bounds; }
	ChunkState StreamableChunk::GetState() const { return m_Impl->State; }
	void StreamableChunk::SetState(ChunkState state) { m_Impl->State = state; }

	void StreamableChunk::OnActivate(Scene& targetScene)
	{
		OnActivateWithDelta(targetScene, nullptr);
	}

	void StreamableChunk::OnActivateWithDelta(Scene& targetScene, const DeltaStateRegistry* deltaState)
	{
		m_Impl->ActiveScene = &targetScene;
		m_Impl->State = ChunkState::Registering;

		m_Impl->SpawnedEntities.clear();
		m_Impl->SpawnedEntities.reserve(m_Impl->RecordCount);

		for (size_t i = 0; i < m_Impl->RecordCount; ++i)
		{
			const auto& rec = m_Impl->Records[i];
			if (deltaState && deltaState->IsDestroyed(rec.EntityUUID))
			{
				continue;
			}

			const std::string name = rec.Name[0] != '\0' ? rec.Name : "StaticEntity";
			Entity ent = Entity::CreateEntity(&targetScene, name, UUID(rec.EntityUUID));
			if (ent.IsValid())
			{
				ent.AddComponent<TransformComponent>(
					rec.Position,
					glm::radians(rec.RotationEuler),
					glm::vec3(rec.ScaleX, rec.ScaleY, rec.ScaleZ)
				);
				m_Impl->SpawnedEntities.push_back(ent);
			}
		}

		m_Impl->State = ChunkState::Active;
	}

	void StreamableChunk::OnDeactivate(Scene& targetScene)
	{
		m_Impl->State = ChunkState::Unregistering;
		for (auto& ent : m_Impl->SpawnedEntities)
		{
			if (ent.IsValid())
			{
				Entity::DestroyEntity(&targetScene, ent);
			}
		}
		m_Impl->SpawnedEntities.clear();
		m_Impl->ActiveScene = nullptr;
		m_Impl->State = ChunkState::MemoryReady;
	}

	const ChunkHeader& StreamableChunk::GetHeader() const { return m_Impl->Header; }
	size_t StreamableChunk::GetRecordCount() const { return m_Impl->RecordCount; }
	const StaticEntityRecord* StreamableChunk::GetRecords() const { return m_Impl->Records.get(); }
	size_t StreamableChunk::GetSpawnedCount() const { return m_Impl->SpawnedEntities.size(); }
}
