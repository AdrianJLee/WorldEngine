#include "wldpch.h"
#include "Allocator.h"
#include "MemoryTracker.h"
namespace World
{
	Allocator::Allocator(size_t size, void* start, const char* debugName, AllocatorType type, bool isEphemeral)
		: m_Size(size), m_Start(start), m_UsedMemory(0), m_NumAllocations(0), m_DebugName(debugName), m_Type(type)
	{
		MemoryTracker::Get().Register(this, m_DebugName, m_Type, isEphemeral);
	}
	Allocator::~Allocator()
	{
		MemoryTracker::Get().Unregister(this);
		m_Start = nullptr; m_Size = 0;
	}
}