#include "wldpch.h"

#include "World/Asset/AsyncLoader.h"

#include "World/Asset/WModelIO.h"
#include "World/Core/Thread/JobSystem.h"

namespace World
{
	namespace
	{
		// A+B 段(读盘 + 解析 + 纯 CPU 构造):工作线程与内联回退走的**同一份**。
		// 禁止在这里碰 RHI / Mesh 的进程内缓存 / ECS。
		Ref<Mesh> LoadMeshNow(const std::string& logicalPath, std::string* error)
		{
			std::vector<uint8_t> bytes;
			if (!Asset::WModelIO::ReadRawBytes(logicalPath, bytes, error))
				return nullptr;

			Asset::WModelData data;
			if (!Asset::WModelIO::Parse(bytes.data(), bytes.size(), data, error))
				return nullptr;

			return Mesh::BuildFromWModel(std::move(data), logicalPath, error);
		}

		// 工作项负载:只带"我是谁 + 加载哪个路径"。
		struct MeshJob
		{
			AsyncLoader* Self = nullptr;
			PathId Path;
			std::string LogicalPath;
		};
	}

	AsyncLoader::~AsyncLoader()
	{
		// 工作项持有 this ⇒ 必须先取消并等它归零,否则工作线程会写已释放对象。
		// (在飞任务无法中断,但它们会跑完并在这里被等到。)
		m_InFlight.Cancel();
		if (JobSystem::IsRunning())
			JobSystem::Wait(&m_InFlight);
		Clear();
	}

	AsyncLoader::Status AsyncLoader::RequestMesh(PathId path, bool* startedNew)
	{
		if (startedNew) *startedNew = false;
		if (!path.IsValid())
			return Status { Phase::Failed, "path is empty" };

		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			const auto found = m_Entries.find(path);
			if (found != m_Entries.end() && found->second.State != Phase::None)
				return Status { found->second.State, found->second.Error };
			m_Entries[path].State = Phase::Pending;
		}
		if (startedNew) *startedNew = true;

		const std::string logical = StringPool::Get().PathOf(path);
		// 并行被禁 / JobSystem 未运行 ⇒ 内联算完:结果与异步路径逐字节相同(共用 LoadMeshNow)。
		const bool runAsync = JobSystem::IsRunning() && JobSystem::ParallelAllowed();
		if (!runAsync)
		{
			std::string error;
			Ref<Mesh> mesh = LoadMeshNow(logical, &error);
			Publish(path, std::move(mesh), std::move(error));
			return Query(path);
		}

		JobDecl job;
		job.Emplace(MeshJob { this, path, logical });
		// 后台加载:低优先级,不与渲染/物理抢核。
		job.Priority = JobPriority::Low;
		job.Counter = &m_InFlight;
		job.Entry = [](void* data)
		{
			auto* payload = static_cast<MeshJob*>(data);
			std::string error;
			Ref<Mesh> mesh = LoadMeshNow(payload->LogicalPath, &error);
			payload->Self->Publish(payload->Path, std::move(mesh), std::move(error));
		};
		JobSystem::Kick(std::move(job));

		return Query(path);
	}

	void AsyncLoader::Publish(PathId path, Ref<Mesh> mesh, std::string error)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		const auto found = m_Entries.find(path);
		if (found == m_Entries.end() || found->second.State != Phase::Pending)
			return;   // Cancel 已经撤销过:结果直接丢弃,不复活条目

		Entry& entry = found->second;
		if (mesh)
		{
			entry.State = Phase::Ready;
			entry.Prepared = std::move(mesh);
			m_ReadyQueue.push_back(path);   // 稳定顺序(提交确定性)
		}
		else
		{
			entry.State = Phase::Failed;
			entry.Error = std::move(error);
		}
	}

	std::size_t AsyncLoader::PumpCompletions()
	{
		std::vector<PathId> ready;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			ready.swap(m_ReadyQueue);
		}

		std::size_t adopted = 0;
		for (const PathId path : ready)
		{
			Ref<Mesh> mesh;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				const auto found = m_Entries.find(path);
				if (found == m_Entries.end() || found->second.State != Phase::Ready)
					continue;
				mesh = std::move(found->second.Prepared);
				// 提交即交棒:此后权威状态在 Mesh 的进程内缓存里,异步层不再记账。
				m_Entries.erase(found);
			}
			if (Mesh::AdoptWModel(path, std::move(mesh)))
				++adopted;
		}
		return adopted;
	}

	void AsyncLoader::Cancel(PathId path)
	{
		if (!path.IsValid())
			return;
		std::lock_guard<std::mutex> lock(m_Mutex);
		// 在飞的工作项无法中断;删掉条目后它的 Publish 会因为找不到条目而丢弃结果。
		m_Entries.erase(path);
		m_ReadyQueue.erase(
			std::remove(m_ReadyQueue.begin(), m_ReadyQueue.end(), path), m_ReadyQueue.end());
	}

	void AsyncLoader::Clear()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Entries.clear();
		m_ReadyQueue.clear();
	}

	AsyncLoader::Status AsyncLoader::Query(PathId path) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		const auto found = m_Entries.find(path);
		if (found == m_Entries.end())
			return Status {};
		return Status { found->second.State, found->second.Error };
	}

	std::size_t AsyncLoader::PendingCount() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::size_t count = 0;
		for (const auto& [path, entry] : m_Entries)
			if (entry.State == Phase::Pending)
				++count;
		return count;
	}

	std::size_t AsyncLoader::ReadyCount() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::size_t count = 0;
		for (const auto& [path, entry] : m_Entries)
			if (entry.State == Phase::Ready)
				++count;
		return count;
	}

	std::size_t AsyncLoader::FailedCount() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::size_t count = 0;
		for (const auto& [path, entry] : m_Entries)
			if (entry.State == Phase::Failed)
				++count;
		return count;
	}

	std::vector<std::string> AsyncLoader::DescribeEntries() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::vector<std::string> lines;
		lines.reserve(m_Entries.size());
		for (const auto& [path, entry] : m_Entries)
		{
			std::string line(StringPool::Get().PathOf(path));
			switch (entry.State)
			{
				case Phase::Pending: line += " (pending)"; break;
				case Phase::Ready: line += " (ready)"; break;
				case Phase::Failed: line += " (failed: " + entry.Error + ")"; break;
				case Phase::None:
				default: break;
			}
			lines.push_back(std::move(line));
		}
		std::sort(lines.begin(), lines.end());   // 稳定输出(诊断/测试可比较)
		return lines;
	}

	std::string AsyncLoader::Describe() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::size_t pending = 0;
		std::size_t ready = 0;
		std::size_t failed = 0;
		for (const auto& [path, entry] : m_Entries)
		{
			switch (entry.State)
			{
				case Phase::Pending: ++pending; break;
				case Phase::Ready: ++ready; break;
				case Phase::Failed: ++failed; break;
				case Phase::None:
				default: break;
			}
		}
		return "loaded=" + std::to_string(ready)
			+ " pending=" + std::to_string(pending)
			+ " failed=" + std::to_string(failed);
	}
}
