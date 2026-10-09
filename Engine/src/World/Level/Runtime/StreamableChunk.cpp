#include "wldpch.h"
#include "World/Level/Runtime/StreamableChunk.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include "World/Scene/SceneSerializer.h"
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
		std::vector<std::string> DocumentYaml;   // 每个静态实体一份组件文档(可为空 = v1 老块)
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

		// 实体文档段(v2):记录表之后是 ChunkDocumentEntry[] 与其后的 YAML 字节。
		m_Impl->DocumentYaml.assign(m_Impl->RecordCount, std::string());
		if (m_Impl->Header.DocumentSize > 0 && m_Impl->RecordCount > 0)
		{
			fin.seekg(static_cast<std::streamoff>(m_Impl->Header.DocumentOffset), std::ios::beg);
			std::vector<ChunkDocumentEntry> documents(m_Impl->RecordCount);
			const size_t tableBytes = documents.size() * sizeof(ChunkDocumentEntry);
			fin.read(reinterpret_cast<char*>(documents.data()), tableBytes);
			if (static_cast<size_t>(fin.gcount()) != tableBytes)
			{
				if (outError) *outError = "Chunk document table truncated in: " + chunkPath.string();
				m_Impl->State = ChunkState::Unloaded;
				return false;
			}

			for (size_t r = 0; r < documents.size(); ++r)
			{
				const ChunkDocumentEntry& entry = documents[r];
				if (entry.Size == 0)
					continue;
				std::string& text = m_Impl->DocumentYaml[r];
				text.resize(entry.Size);
				fin.seekg(static_cast<std::streamoff>(entry.Offset), std::ios::beg);
				fin.read(text.data(), static_cast<std::streamsize>(entry.Size));
				if (static_cast<size_t>(fin.gcount()) != entry.Size)
				{
					if (outError) *outError = "Chunk entity document truncated in: " + chunkPath.string();
					m_Impl->State = ChunkState::Unloaded;
					return false;
				}
			}
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

		// 差分区:已摧毁的实体本轮不生成(记录表与文档表按记录序号一一对应)。
		std::vector<uint8_t> skip(m_Impl->RecordCount, 0);
		for (size_t i = 0; i < m_Impl->RecordCount; ++i)
			if (deltaState && deltaState->IsDestroyed(m_Impl->Records[i].EntityUUID))
				skip[i] = 1;

		bool hasDocuments = false;
		for (const std::string& text : m_Impl->DocumentYaml)
			if (!text.empty())
			{
				hasDocuments = true;
				break;
			}

		if (hasDocuments)
		{
			// 带组件载荷:走与编辑器/关卡包**同一条**实体级读路径(全部组件)。
			SceneSerializer::AppendEntityDocuments(targetScene, m_Impl->DocumentYaml, &skip,
				&m_Impl->SpawnedEntities, nullptr);
			SceneSerializer::FinalizeEntityImport(targetScene);
		}
		else
		{
			// v1 老块没有载荷:退化为"名字 + Transform"(不静默假装完整 —— 见 HasComponentPayload)。
			for (size_t i = 0; i < m_Impl->RecordCount; ++i)
			{
				const auto& rec = m_Impl->Records[i];
				if (skip[i] != 0)
					continue;

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

	bool StreamableChunk::HasComponentPayload() const
	{
		for (const std::string& text : m_Impl->DocumentYaml)
			if (!text.empty())
				return true;
		return false;
	}

	size_t StreamableChunk::GetDocumentCount() const
	{
		size_t count = 0;
		for (const std::string& text : m_Impl->DocumentYaml)
			if (!text.empty())
				++count;
		return count;
	}
}
