#include "wldpch.h"
#include "World/Core/Memory/Allocator.h"
#include "World/Profiling/MemoryTrack.h"
namespace World
{
	Allocator::Allocator(size_t size, void* start, const char* debugName, AllocatorType type, bool isEphemeral)
		: m_Size(size), m_Start(start), m_UsedMemory(0), m_NumAllocations(0), m_DebugName(debugName), m_Type(type)
	{
		// 单一注册点:所有 Allocator 子类都在这里注册一次(派生类**不得**再注册,
		// 否则快照/泄漏报告里会重复计数 —— 这正是 PoolAllocator 曾经的缺陷)。
		(void)isEphemeral;
		Profiling::MemoryTrack::RegisterAllocator(this);
	}
	Allocator::~Allocator()
	{
		Profiling::MemoryTrack::UnregisterAllocator(this);
		m_Start = nullptr; m_Size = 0;
	}
}