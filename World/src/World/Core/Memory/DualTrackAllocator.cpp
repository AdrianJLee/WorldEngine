#include "wldpch.h"
#include "DualTrackAllocator.h"
#include "LinearAllocator.h"
namespace World
{
	DualTrackAllocator::DualTrackAllocator(const char* debugName, size_t pageSize)
		: Allocator(0, nullptr, debugName, AllocatorType::DualTrack), m_PageSize(pageSize)
	{
		m_Threshold = m_PageSize / 4; // 512KB 以上走大对象轨

		// 初始化第一页
		m_HeadSOS = CreateSOSPage(m_PageSize);
		m_CurrentSOS = m_HeadSOS;
	}

	void DualTrackAllocator::Reset()
	{
		DestructorNode* curr = m_DestructorChain;
		while (curr != nullptr)
		{
			if (curr->Callback)
			{
				curr->Callback(curr->Object); // 调用真正的 ~T()
			}
			curr = curr->Next;
		}
		m_DestructorChain = nullptr; // 清空清单

		// SOS 轨道：全员原地待命（只 Reset 指针，不销毁内存）
		SOSPage* currSOS = m_HeadSOS;
		while (currSOS)
		{
			currSOS->Alloc->Reset();
			currSOS = currSOS->Next;
		}
		m_CurrentSOS = m_HeadSOS;

		// LOS 轨道：由于大对象生命周期不可控，Reset 时全部销毁
		LOSPage* currLOS = m_HeadLOS;
		while (currLOS)
		{
			LOSPage* toDelete = currLOS;
			currLOS = currLOS->Next;

			m_Size -= toDelete->Size;

			_aligned_free(toDelete->Data);
			delete toDelete;
		}
		m_HeadLOS = nullptr;
		m_UsedMemory = 0;
		m_NumAllocations = 0;
	}
	DualTrackAllocator::~DualTrackAllocator()
	{
		Reset();
		// 析构时释放所有 SOS 页
		SOSPage* currSOS = m_HeadSOS;
		while (currSOS)
		{
			SOSPage* toDelete = currSOS;
			currSOS = currSOS->Next;
			delete toDelete->Alloc; // 先删除线性分配器
			_aligned_free(toDelete->RawMemory); // 再释放大块内存
			delete toDelete; // 最后删除页结构
		}
		m_HeadSOS = nullptr;
		// 析构时释放所有 LOS 页
		LOSPage* currLOS = m_HeadLOS;
		while (currLOS)
		{
			LOSPage* toDelete = currLOS;
			currLOS = currLOS->Next;
			_aligned_free(toDelete->Data);
			delete toDelete;
		}
		m_HeadLOS = nullptr;
	}
	void* DualTrackAllocator::Allocate(size_t size, size_t alignment)
	{
		// --- 策略分流 1: 大对象轨 (LOS) ---
		if (size > m_Threshold)
		{
			return AllocateLOS(size, alignment);
		}

		// --- 策略分流 2: 小对象轨 (SOS) ---
		// 委托给当前的 LinearAllocator 执行
		void* ptr = m_CurrentSOS->Alloc->Allocate(size, alignment);

		if (!ptr)
		{
			// 如果当前页满了，将军（DualTrack）命令申请新兵（新 LinearAllocator）
			if (m_CurrentSOS->Next)
			{
				// 清理下一页的线性分配器状态，复用它（如果已经存在）
				m_CurrentSOS = m_CurrentSOS->Next;
				m_CurrentSOS->Alloc->Reset();
			}
			else
			{
				SOSPage* newPage = CreateSOSPage(m_PageSize);
				m_CurrentSOS->Next = newPage;
				m_CurrentSOS = newPage;
			}
			ptr = m_CurrentSOS->Alloc->Allocate(size, alignment);
		}

		m_UsedMemory += size;
		m_NumAllocations++;
		return ptr;
	}

	void DualTrackAllocator::RegisterDestructor(void* obj, DestructorFunc func)
	{
		// 在内存池里为 Node 申请空间，这样 Node 本身也不需要手动 delete
		void* nodeMem = this->Allocate(sizeof(DestructorNode), alignof(DestructorNode));
		DestructorNode* node = reinterpret_cast<DestructorNode*>(nodeMem);

		node->Callback = func;
		node->Object = obj;

		// 头插法维护链表
		node->Next = m_DestructorChain;
		m_DestructorChain = node;
	}

	SOSPage* DualTrackAllocator::CreateSOSPage(size_t size)
	{
		void* raw = _aligned_malloc(size, 64);

		// 这里体现了组合：DualTrack 创建并拥有 LinearAllocator
		std::string debugName = (std::string(m_DebugName) + "_SOSPage");
		SOSPage* sosPage = new SOSPage { nullptr, raw, nullptr,debugName };
		LinearAllocator* linear = new LinearAllocator(size, raw, sosPage->DebugName.c_str());
		sosPage->Alloc = linear;

		m_Size += size;

		return sosPage;
	}


	void* DualTrackAllocator::AllocateLOS(size_t size, size_t alignment)
	{
		void* raw = _aligned_malloc(size, alignment);

		// 挂载到大对象链表
		LOSPage* newPage = new LOSPage { raw, size, m_HeadLOS };
		m_HeadLOS = newPage;

		m_Size += size;
		m_UsedMemory += size;
		m_NumAllocations++;

		return raw;
	}
}