#include "wldpch.h"
#include "FrameArena.h"

#include "LinearAllocator.h"
#include "MemoryTracker.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace World
{
	namespace
	{
		constexpr size_t kDefaultPageSize = 1024 * 1024;   // 1 MiB
		constexpr size_t kMaxPageSize = 32 * 1024 * 1024;  // 单页上限
		constexpr uint8_t kPoisonByte = 0xDD;

		std::mutex& RegistryMutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		std::vector<std::unique_ptr<FrameArena>>& Registry()
		{
			static auto* arenas = new std::vector<std::unique_ptr<FrameArena>>();
			return *arenas;
		}

		thread_local FrameArena* t_ThreadArena = nullptr;
	}

	FrameArena::FrameArena(size_t firstPageSize) : Allocator(0, nullptr, "FrameArena", AllocatorType::Linear)
	{
		m_Owner = std::this_thread::get_id();
		m_NextPageSize = firstPageSize;
		AppendPage(firstPageSize);
	}

	FrameArena::~FrameArena()
	{
		ReleaseDestructors();
		for (Page& page : m_Pages)
			if (page.Raw)
				_aligned_free(page.Raw);
		m_Pages.clear();
	}

	FrameArena& FrameArena::Get()
	{
		if (!t_ThreadArena)
		{
			std::lock_guard<std::mutex> lock(RegistryMutex());
			auto arena = std::unique_ptr<FrameArena>(new FrameArena(kDefaultPageSize));
			t_ThreadArena = arena.get();
			Registry().push_back(std::move(arena));
		}
		return *t_ThreadArena;
	}

	void FrameArena::AppendPage(size_t size)
	{
		const size_t bounded = std::min(size, kMaxPageSize);
		void* raw = _aligned_malloc(bounded, 64);
		WLD_CORE_ASSERT(raw != nullptr, "FrameArena page allocation failed");
		Page page;
		page.Raw = raw;
		page.Size = bounded;
		page.Alloc = std::make_unique<LinearAllocator>(bounded, raw, m_DebugName);
		m_Pages.push_back(std::move(page));
		m_CurrentPage = m_Pages.size() - 1;
		m_Size += bounded;
		m_NextPageSize = std::min(bounded * 2, kMaxPageSize);
	}

	LinearAllocator* FrameArena::CurrentPage()
	{
		return m_CurrentPage < m_Pages.size() ? m_Pages[m_CurrentPage].Alloc.get() : nullptr;
	}

	void FrameArena::RefreshUsage()
	{
		size_t used = 0;
		size_t allocations = 0;
		for (const Page& page : m_Pages)
		{
			if (!page.Alloc)
				continue;
			used += page.Alloc->GetUsedMemory();
			allocations += page.Alloc->GetNumAllocations();
		}
		m_UsedMemory = used;
		m_NumAllocations = allocations;
	}

	void* FrameArena::Allocate(size_t size, size_t alignment)
	{
		WLD_CORE_ASSERT(IsOwnerThread(), "FrameArena must be used from its owner thread");
		LinearAllocator* page = CurrentPage();
		void* ptr = page ? page->Allocate(size, alignment) : nullptr;
		if (!ptr)
		{
			AppendPage(m_NextPageSize);
			ptr = CurrentPage()->Allocate(size, alignment);
		}
		if (ptr)
			RefreshUsage();
		return ptr;
	}

	void FrameArena::RegisterDestructor(void* obj, DestructorFunc func)
	{
		void* nodeMemory = Allocate(sizeof(DestructorNode), alignof(DestructorNode));
		auto* node = static_cast<DestructorNode*>(nodeMemory);
		node->Callback = func;
		node->Object = obj;
		node->Next = m_DestructorChain;
		m_DestructorChain = node;
	}

	void FrameArena::ReleaseDestructors()
	{
		DestructorNode* node = m_DestructorChain;
		while (node)
		{
			if (node->Callback)
				node->Callback(node->Object);
			node = node->Next;
		}
		m_DestructorChain = nullptr;
	}

	void FrameArena::Reset()
	{
#ifdef WLD_DEBUG
		// 回收前污染已用区域:跨帧引用会在调试器里以 0xDD… 暴露,而不是读到旧数据。
		for (const Page& page : m_Pages)
		{
			if (!page.Alloc || !page.Alloc->GetStart())
				continue;
			std::memset(page.Alloc->GetStart(), kPoisonByte, page.Alloc->GetUsedMemory());
		}
#endif
		ReleaseDestructors();
		for (const Page& page : m_Pages)
			if (page.Alloc)
				page.Alloc->Reset();
		m_CurrentPage = 0;
		RefreshUsage();
	}

	void FrameArena::ResetAll()
	{
		std::lock_guard<std::mutex> lock(RegistryMutex());
		for (const std::unique_ptr<FrameArena>& arena : Registry())
			if (arena)
				arena->Reset();
	}

	void FrameArena::Shutdown()
	{
		std::lock_guard<std::mutex> lock(RegistryMutex());
		Registry().clear();
		t_ThreadArena = nullptr;
	}

	size_t FrameArena::CollectStats(std::vector<AllocatorStats>& out)
	{
		std::lock_guard<std::mutex> lock(RegistryMutex());
		for (const std::unique_ptr<FrameArena>& arena : Registry())
		{
			if (!arena)
				continue;
			AllocatorStats stats;
			stats.Name = arena->m_DebugName;
			stats.Type = arena->m_Type;
			stats.UsedBytes = arena->m_UsedMemory;
			stats.TotalReserved = arena->m_Size;
			stats.NumAllocations = arena->m_NumAllocations;
			out.push_back(stats);
		}
		return out.size();
	}

	void FrameArena::TotalUsage(size_t& usedBytes, size_t& reservedBytes)
	{
		usedBytes = 0;
		reservedBytes = 0;
		std::lock_guard<std::mutex> lock(RegistryMutex());
		for (const std::unique_ptr<FrameArena>& arena : Registry())
		{
			if (!arena)
				continue;
			usedBytes += arena->m_UsedMemory;
			reservedBytes += arena->m_Size;
		}
	}
}
