#include "wldpch.h"
#include "World/Level/Runtime/LevelStreamingRuntime.h"

#include "World/Core/Log.h"
#include "World/Core/UUID.h"
#include "World/Core/Log.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/Vfs/Vfs.h"
#include "World/Scene/Scene.h"

#include <atomic>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
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
		float TimeBudgetMs{ 2.0f };
		bool AsyncIO{ true };
		size_t LastLoggedActive{ static_cast<size_t>(-1) };   // -1 ⇒ 首帧必打一条

		// 在飞的后台预读:块 Id → (字节, 是否就绪)。主线程只搬指针,不读盘。
		// 跨线程交接:后台线程先填 Bytes、再置 Failed/Ready;主线程以 acquire 读 Ready
		// 之后才碰 Bytes。缺这个 release/acquire 对就是数据竞争 —— 实测表现为
		// "偶尔某一帧读不到已写好的字节",且不可复现。
		struct Prefetch
		{
			std::vector<uint8_t> Bytes;
			std::atomic<bool> Ready{ false };
			std::atomic<bool> Failed{ false };
		};
		std::unordered_map<uint64_t, Prefetch> Prefetches;
		// "驱动器说该装、但还没真正装上"的块。必需:驱动器在**第一次**裁决时就把块标成
		// IsLoaded,异步读盘跨帧时它不会再重复裁决 —— 只靠驱动器的话,这一块永远装不上。
		std::unordered_set<uint64_t> PendingLoads;

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

	LevelStreamingRuntime::~LevelStreamingRuntime()
	{
		// 后台预读任务持有本类 Impl 与槽位的**裸指针**(JobDecl 的负载必须平凡可拷贝,
		// 不能捕获智能指针)。因此销毁前必须把在飞任务排干 —— 否则任务落在已释放的
		// Impl 上就是 use-after-free(实测表现为 0xC0000005 随机崩溃)。
		if (m_Impl && !m_Impl->Prefetches.empty() && JobSystem::IsRunning())
			JobSystem::WaitAll();
	}

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
		m_Impl->Coordinator.SetTimeSliceBudgetMs(m_Impl->TimeBudgetMs);
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

	void LevelStreamingRuntime::SetViewDistances(float viewDistance, float unloadDistance)
	{
		m_Impl->LoadDistance = viewDistance;
		// 缓冲带必须存在(与清单校验同一口径):卸载视距不合法就退回"视距 + 25%",
		// 而不是接受一个必然抖动的配置。
		m_Impl->UnloadDistance = unloadDistance > viewDistance ? unloadDistance : viewDistance * 1.25f;
		if (m_Impl->Driver)
		{
			m_Impl->Driver->SetLoadDistance(m_Impl->LoadDistance);
			m_Impl->Driver->SetUnloadDistance(m_Impl->UnloadDistance);
		}
	}

	void LevelStreamingRuntime::SetTimeBudgetMs(float budgetMs)
	{
		m_Impl->TimeBudgetMs = budgetMs > 0.0f ? budgetMs : 0.0f;
		m_Impl->Coordinator.SetTimeSliceBudgetMs(m_Impl->TimeBudgetMs);
	}

	void LevelStreamingRuntime::SetAsyncIO(bool enabled) { m_Impl->AsyncIO = enabled; }

	size_t LevelStreamingRuntime::PendingPrefetches() const
	{
		size_t pending = 0;
		for (const auto& [id, prefetch] : m_Impl->Prefetches)
			if (!prefetch.Ready.load(std::memory_order_acquire)
				&& !prefetch.Failed.load(std::memory_order_relaxed))
				++pending;
		return pending;
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

		const auto findEntry = [this](uint64_t chunkId) -> const ChunkIndexEntry*
		{
			for (const ChunkIndexEntry& candidate : m_Impl->Index.Entries())
				if (candidate.ChunkID == chunkId)
					return &candidate;
			return nullptr;
		};

		// 驱动器的裁决并入待装集合:它只在"第一次判断该装"时报一次,而异步读盘要跨帧,
		// 所以"还没装上的"必须由本类自己记住并逐帧重试。
		for (const uint64_t chunkId : toLoad)
			m_Impl->PendingLoads.insert(chunkId);
		// 已被裁决卸载(或已装上)的不再重试。
		for (const uint64_t chunkId : toUnload)
			m_Impl->PendingLoads.erase(chunkId);
		for (auto it = m_Impl->PendingLoads.begin(); it != m_Impl->PendingLoads.end();)
			it = (m_Impl->Chunks.count(*it) != 0
				&& m_Impl->Chunks[*it]->GetState() == ChunkState::Active)
				? m_Impl->PendingLoads.erase(it) : std::next(it);

		// ②a 先把"裁决要装、字节还没到"的块排进后台预读(不阻塞本帧)。
		//     异步关掉时跳过这一步,走下面的同步读分支。
		if (m_Impl->AsyncIO && JobSystem::IsRunning())
		{
			const std::vector<uint64_t> prefetchSnapshot(m_Impl->PendingLoads.begin(), m_Impl->PendingLoads.end());
			for (const uint64_t chunkId : prefetchSnapshot)
			{
				if (m_Impl->Prefetches.count(chunkId) != 0)
					continue;   // 已经在读了
				const ChunkIndexEntry* entry = findEntry(chunkId);
				if (!entry)
					continue;

				// 陷阱:JobDecl 按值传递且没有自定义拷贝/移动构造(见 Core/Thread/Thread.h),
				// 负载因此**必须平凡可拷贝** —— 捕获 std::string / vector 会位拷贝后被释放,
				// 得到一个悬空对象,任务静默跑不起来。这里只捕获指针与整数:文件名在任务内
				// 用 chunkId 从只读的索引里查(索引在 Initialize 之后不再改)。
				Impl* impl = m_Impl.get();
				Impl::Prefetch* slot = &m_Impl->Prefetches[chunkId];
				const auto payload = [impl, slot, chunkId]()
				{
					const char* fileName = nullptr;
					for (const ChunkIndexEntry& candidate : impl->Index.Entries())
						if (candidate.ChunkID == chunkId)
						{
							fileName = candidate.File.c_str();
							break;
						}

					std::vector<uint8_t> bytes;
					const bool ok = fileName && impl->ReadFile(fileName, &bytes) && !bytes.empty();
					if (ok)
						slot->Bytes = std::move(bytes);
					else
						slot->Failed.store(true, std::memory_order_relaxed);
					slot->Ready.store(true, std::memory_order_release);
				};

				// `Emplace` 只负责**装填负载并登记析构**;入口函数必须由调用方显式设置。
				// 漏设 Entry 的表现是任务被正常排队、正常窃取,却因为 Entry==nullptr 被
				// Execute 静默跳过(executed 不涨、pending 不降)—— 极难从日志看出来。
				using Payload = std::decay_t<decltype(payload)>;
				JobDecl job;
				job.Emplace(payload);
				job.Entry = [](void* data) { (*static_cast<Payload*>(data))(); };
				JobSystem::Kick(std::move(job), JobPriority::Low);
			}
		}

		// ②b 主线程实例化:字节已到手的块才激活。
		const std::vector<uint64_t> pendingSnapshot(m_Impl->PendingLoads.begin(), m_Impl->PendingLoads.end());
		for (const uint64_t chunkId : pendingSnapshot)
		{
			if (m_Impl->Chunks.count(chunkId) == 0)
			{
				std::vector<uint8_t> bytes;
				if (m_Impl->AsyncIO && JobSystem::IsRunning())
				{
					const auto it = m_Impl->Prefetches.find(chunkId);
					// acquire 读 Ready:之后的 Bytes 访问才算有保证。
					if (it == m_Impl->Prefetches.end()
						|| !it->second.Ready.load(std::memory_order_acquire))
						continue;   // 本帧还没读到 ⇒ 下帧再试(不阻塞主线程)
					if (it->second.Failed.load(std::memory_order_relaxed))
					{
						WLD_CORE_ERROR("[level] 块 {0} 后台读取失败", chunkId);
						m_Impl->Prefetches.erase(it);
						continue;
					}
					bytes = std::move(it->second.Bytes);
					m_Impl->Prefetches.erase(it);
				}
				else
				{
					const ChunkIndexEntry* entry = findEntry(chunkId);
					if (!entry || !m_Impl->ReadChunkBytes(*entry, &bytes))
					{
						WLD_CORE_ERROR("[level] 块 {0} 读不到字节", chunkId);
						continue;
					}
				}

				auto chunk = std::make_shared<StreamableChunk>();
				std::string loadError;
				if (!chunk->LoadFromMemory(bytes, std::to_string(chunkId), &loadError))
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
