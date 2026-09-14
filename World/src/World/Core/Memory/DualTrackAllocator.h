#pragma once
#include "Allocator.h"
#include "FrameArena.h"

namespace World
{
	#pragma region Macro

	// 帧内临时分配走"每线程 arena"(FrameArena),无锁且可在工作线程使用;
	// 引擎级长生命周期对象仍走 Application 的引擎分配器。
	#define WLD_FRAME_NEW(T, ...) ::World::FrameArena::Get().New<T>(__VA_ARGS__)
	#define WLD_ENGINE_NEW(T, ...) Application::Get().GetEngineAllocator().New<T>(__VA_ARGS__)

	#pragma endregion

	class LinearAllocator;

	struct SOSPage
	{
		LinearAllocator* Alloc = nullptr; // 每一页都是一个线性分配器
		void* RawMemory = nullptr;        // 存储实际申请的大块内存地址，用于最后释放
		SOSPage* Next = nullptr;
		std::string DebugName;
	};

	struct LOSPage
	{
		void* Data = nullptr;
		size_t Size = 0;
		LOSPage* Next = nullptr;
	};

	// 双轨分配器（Dual-Track Allocator）结合了小对象轨（SOS）和大对象轨（LOS）的优点
	// DualTrack, 每帧通用的临时数据, 1帧, 自动分流大小对象，兼顾速度与容量
	class DualTrackAllocator : public Allocator
	{
	public:
		DualTrackAllocator(const char* debugName, size_t pageSize = 2 * 1024 * 1024);

		void Reset();

		~DualTrackAllocator()override;

		void* Allocate(size_t size, size_t alignment = 8) override;

	protected:
		void RegisterDestructor(void* obj, DestructorFunc func) override;
	private:
		SOSPage* CreateSOSPage(size_t size);

		void* AllocateLOS(size_t size, size_t alignment);
	private:
		size_t m_PageSize = 0;
		size_t m_Threshold = 0; // 超过这个大小的请求走大对象轨
		DestructorNode* m_DestructorChain = nullptr;
		SOSPage* m_HeadSOS = nullptr;
		SOSPage* m_CurrentSOS = nullptr;
		LOSPage* m_HeadLOS = nullptr;
	};
}

