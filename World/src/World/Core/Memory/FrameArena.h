#pragma once

#include "Allocator.h"

#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace World
{
	class LinearAllocator;

	// 每线程一页(或多页)线性 arena:承载帧内临时分配(WLD_FRAME_NEW)。
	//
	// 设计要点(工业级):
	// - 每个线程只写自己的 arena → 无锁、无 false sharing 之外的竞争;
	// - 帧末由渲染线程调用 ResetAll 统一回卷(契约:调用时所有工作线程已 join/空闲);
	// - Debug 下 Reset 前把已用区域填充 0xDD,便于发现"跨帧引用已回收内存";
	// - 非平凡对象在 Reset 时按登记顺序逆序析构(与 DualTrack 一致)。
	class FrameArena final : public Allocator
	{
	public:
		// 当前线程的 arena(首次访问时创建,默认 1 MiB 首页,按需追加页)。
		static FrameArena& Get();
		// 帧末回卷全部线程的 arena。只允许在其它线程空闲时调用。
		static void ResetAll();
		// 进程退出:释放全部 arena(含登记到 MemoryTracker 的条目)。
		static void Shutdown();
		// 统计快照(内存面板/测试用);返回 arena 数量。
		static size_t CollectStats(std::vector<AllocatorStats>& out);
		// 所有 arena 的已使用/容量合计(快速查询)。
		static void TotalUsage(size_t& usedBytes, size_t& reservedBytes);

		void* Allocate(size_t size, size_t alignment = 8) override;
		// 回卷:先析构,再回退分配指针(仅所属线程或已确认空闲的其它线程调用)。
		void Reset();
		bool IsOwnerThread() const { return m_Owner == std::this_thread::get_id(); }
		size_t GetPageCount() const { return m_Pages.size(); }

	private:
		struct Page
		{
			std::unique_ptr<LinearAllocator> Alloc;
			void* Raw = nullptr;   // 页内存由 arena 自己持有
			size_t Size = 0;
		};

		explicit FrameArena(size_t firstPageSize);
	public:
		~FrameArena() override;
	private:

	protected:
		void RegisterDestructor(void* obj, DestructorFunc func) override;

	private:
		void ReleaseDestructors();
		LinearAllocator* CurrentPage();
		void AppendPage(size_t size);
		void RefreshUsage();

		std::thread::id m_Owner;
		size_t m_NextPageSize = 0;
		std::vector<Page> m_Pages;
		size_t m_CurrentPage = 0;
		DestructorNode* m_DestructorChain = nullptr;
	};
}
