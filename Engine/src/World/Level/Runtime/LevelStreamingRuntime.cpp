#include "wldpch.h"
#include "World/Level/Runtime/LevelStreamingRuntime.h"

#include "World/Core/Log.h"
#include "World/Core/UUID.h"
#include "World/Core/Vfs/Vfs.h"
#include "World/Scene/Scene.h"

#include <fstream>
#include <unordered_map>
#include <vector>

namespace World::Level
{
	struct LevelStreamingRuntime::Impl
	{
		WorldContext* Context{ nullptr };
		std::filesystem::path PhysicalDir;   // 可空:发行包里没有内容根目录
		std::string LevelId;
		ChunkIndex Index;

		ChunkStreamingCoordinator Coordinator;
		std::unique_ptr<SpatialGridDriver> Driver;
		std::unordered_map<uint64_t, std::shared_ptr<StreamableChunk>> Chunks;

		float LoadDistance{ 256.0f };
		float UnloadDistance{ 320.0f };
		size_t LastLoggedActive{ static_cast<size_t>(-1) };   // -1 ⇒ 首帧必打一条

		World::Vfs::Path LogicalDir() const
		{
			return "levels/" + LevelId + "/cooked";
		}

		bool ReadFile(const std::string& fileName, std::vector<uint8_t>* out) const
		{
			// 开发树优先磁盘(内容根是目录 provider,直接读文件更省一层),否则走 Vfs
			// (发行包里块在 vpak 内 —— Vfs 只有 Exists/Read,没有目录遍历,这正是索引存在的原因)。
			if (!PhysicalDir.empty())
			{
				const std::filesystem::path physical =
					PhysicalDir / "cooked" / std::filesystem::u8path(fileName);
				if (std::filesystem::is_regular_file(physical))
				{
					std::ifstream fin(physical, std::ios::binary);
					if (!fin.is_open())
						return false;
					out->assign(std::istreambuf_iterator<char>(fin), std::istreambuf_iterator<char>());
					return true;
				}
			}

			if (!Context)
				return false;
			const World::Vfs::Path logical = LogicalDir() + "/" + fileName;
			std::error_code ec;
			if (!Context->Vfs().Exists(logical))
				return false;
			return Context->Vfs().Read(logical, *out, ec) && !ec;
		}

		bool ReadChunkBytes(const ChunkIndexEntry& entry, std::vector<uint8_t>* out) const
		{
			return ReadFile(entry.File, out);
		}

		Scene* scene{ nullptr };
	};

	LevelStreamingRuntime::LevelStreamingRuntime()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	LevelStreamingRuntime::~LevelStreamingRuntime() = default;

	bool LevelStreamingRuntime::IsReady() const { return m_Impl && !m_Impl->Index.IsEmpty(); }

	const std::string& LevelStreamingRuntime::GetLevelId() const { return m_Impl->LevelId; }

	bool LevelStreamingRuntime::Initialize(WorldContext& context, const std::string& levelId,
		const std::filesystem::path& physicalDir, std::string* error)
	{
		if (levelId.empty())
		{
			if (error) *error = "level id is empty";
			return false;
		}

		m_Impl->Context = &context;
		m_Impl->PhysicalDir = physicalDir;
		m_Impl->LevelId = levelId;
		m_Impl->Chunks.clear();

		// 索引:磁盘优先,否则 Vfs。两处都没有 = 关卡未烘焙。
		const std::filesystem::path physicalIndex =
			physicalDir.empty() ? std::filesystem::path()
				: (physicalDir / "cooked" / ChunkIndex::FileName);
		if (!physicalIndex.empty() && std::filesystem::is_regular_file(physicalIndex))
		{
			if (!ChunkIndex::LoadFromFile(physicalIndex, &m_Impl->Index, error))
				return false;
		}
		else
		{
			std::vector<uint8_t> bytes;
			if (!m_Impl->ReadFile(ChunkIndex::FileName, &bytes) || bytes.empty())
			{
				if (error) *error = "no cooked chunk index for level '" + levelId + "'";
				return false;
			}
			if (!ChunkIndex::Parse(std::string(bytes.begin(), bytes.end()), &m_Impl->Index, error))
				return false;
		}

		if (m_Impl->Index.IsEmpty())
		{
			if (error) *error = "level '" + m_Impl->LevelId + "' has an empty chunk index";
			return false;
		}

		m_Impl->Driver = std::make_unique<SpatialGridDriver>(m_Impl->LoadDistance, m_Impl->UnloadDistance);
		for (const ChunkIndexEntry& entry : m_Impl->Index.Entries())
			m_Impl->Driver->AddChunk(entry.ChunkID, entry.Bounds);

		// 驱动器由本类持有并**由本类**求值:协调器的 RequestLoad 只接受"已在内存"的块,
		// 而"该读哪几块"要由驱动器先裁决 —— 两阶段(先裁决、再按需读盘)在宿主侧。
		m_Impl->Coordinator.SetTimeSliceBudgetMs(2.0f);
		WLD_CORE_WARN("[level] 流送就绪: '{0}', {1} 块, 视距 {2}/{3}m",
			m_Impl->LevelId, m_Impl->Index.Entries().size(),
			m_Impl->LoadDistance, m_Impl->UnloadDistance);
		return true;
	}

	void LevelStreamingRuntime::SetScene(Scene* scene)
	{
		m_Impl->scene = scene;
		m_Impl->Coordinator.SetActiveScene(scene);
	}

	void LevelStreamingRuntime::SetDeltaState(DeltaStateRegistry* deltaState)
	{
		m_Impl->Coordinator.SetDeltaStateRegistry(deltaState);
	}

	void LevelStreamingRuntime::SetViewDistances(float loadDistance, float unloadDistance)
	{
		m_Impl->LoadDistance = loadDistance;
		m_Impl->UnloadDistance = unloadDistance > loadDistance ? unloadDistance : loadDistance + 32.0f;
		if (m_Impl->Driver)
		{
			m_Impl->Driver->SetLoadDistance(m_Impl->LoadDistance);
			m_Impl->Driver->SetUnloadDistance(m_Impl->UnloadDistance);
		}
	}

	void LevelStreamingRuntime::Update(Timestep frameTime, const glm::vec3& viewPosition,
		const glm::mat4& viewProjection)
	{
		if (!IsReady() || !m_Impl->scene)
			return;

		// 两阶段:① 驱动器按空间裁决"该装谁/该卸谁";② 只把**要装的那几块**读进内存,
		// 再交给协调器激活。反过来(先把所有块读进来)会让整关常驻内存,视距形同虚设。
		std::vector<uint64_t> toLoad;
		std::vector<uint64_t> toUnload;
		m_Impl->Driver->EvaluateStreaming(viewPosition, viewProjection, toLoad, toUnload);

		for (const uint64_t chunkId : toLoad)
		{
			if (m_Impl->Chunks.count(chunkId) == 0)
			{
				const ChunkIndexEntry* entry = nullptr;
				for (const ChunkIndexEntry& candidate : m_Impl->Index.Entries())
					if (candidate.ChunkID == chunkId)
					{
						entry = &candidate;
						break;
					}
				if (!entry)
					continue;

				std::vector<uint8_t> bytes;
				if (!m_Impl->ReadChunkBytes(*entry, &bytes))
				{
					WLD_CORE_ERROR("[level] 块 {0} 读不到字节({1})", chunkId, entry->File);
					continue;
				}

				auto chunk = std::make_shared<StreamableChunk>();
				std::string loadError;
				if (!chunk->LoadFromMemory(bytes, entry->File, &loadError))
				{
					WLD_CORE_ERROR("[level] 块 {0} 载入失败: {1}", chunkId, loadError);
					continue;
				}
				m_Impl->Coordinator.RegisterChunk(chunk);
				m_Impl->Chunks.emplace(chunkId, std::move(chunk));
			}
			m_Impl->Coordinator.RequestLoad(chunkId);
		}

		for (const uint64_t chunkId : toUnload)
			m_Impl->Coordinator.RequestUnload(chunkId);

		// 只在**活跃集变化**时打一条(WARN 会即时落盘,进程被强杀也不丢)—— 流送是否真的
		// 在工作、块里有没有实体,是启动期最需要事后核对的事实;逐帧打则会刷屏。
		const size_t active = ActiveChunks();
		if (active != m_Impl->LastLoggedActive)
		{
			m_Impl->LastLoggedActive = active;
			WLD_CORE_WARN("[level] 流送 '{0}': 活跃 {1}/{2} 块, 实体 {3}",
				m_Impl->LevelId, active, m_Impl->Index.Entries().size(), ActiveEntityCount());
		}
	}

	size_t LevelStreamingRuntime::TotalChunks() const { return m_Impl->Index.Entries().size(); }
	size_t LevelStreamingRuntime::LoadedChunks() const { return m_Impl->Chunks.size(); }
	size_t LevelStreamingRuntime::ActiveChunks() const { return m_Impl->Coordinator.ActiveChunkCount(); }

	size_t LevelStreamingRuntime::ActiveEntityCount() const
	{
		size_t total = 0;
		for (const auto& [id, chunk] : m_Impl->Chunks)
			if (chunk && chunk->GetState() == ChunkState::Active)
				total += chunk->GetSpawnedCount();
		return total;
	}
}
