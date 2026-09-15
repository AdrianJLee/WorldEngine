#pragma once

#include <volk.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace World::Rhi::Vulkan
{
	class VulkanDevice;

	// 一次 staging 分配:常驻段内的一段映射内存切片。
	struct VulkanUploadAllocation
	{
		VkBuffer Buffer = VK_NULL_HANDLE;
		uint64_t Offset = 0;
		uint64_t Size = 0;
		void* Mapped = nullptr;

		bool IsValid() const { return Buffer != VK_NULL_HANDLE && Mapped != nullptr; }
	};

	// B2 异步上传基座:常驻 staging 段(host visible 常驻映射)+ 段内子分配。
	//
	// 与旧路径的差别:旧 `ExecuteOneShot` 每次上传都要新建命令池/命令缓冲并
	// `vkQueueWaitIdle`(一次纹理上传 = 3 次整队列排空)。这里:
	//   Allocate → 从当前段切一块(调用方 memcpy);
	//   Submit   → 把这份命令缓冲入 graphics 队列,**不等待**,栅栏归属该段;
	//   段在栅栏信号前不会被复用,空间不足时优先扩容,只有段数达到上限才等待。
	// 由于上传与渲染提交在同一队列,队列顺序天然保证后续绘制能看到上传结果,
	// 调用方不需要额外的信号量/等待。
	//
	// 线程约定:仅限渲染线程使用(vkQueueSubmit 需要对队列外部同步)。
	class VulkanUploadRing
	{
	public:
		VulkanUploadRing(VulkanDevice& device, uint64_t segmentSize = 4ull * 1024 * 1024,
			uint32_t maxSegments = 8);
		~VulkanUploadRing();

		VulkanUploadRing(const VulkanUploadRing&) = delete;
		VulkanUploadRing& operator=(const VulkanUploadRing&) = delete;

		// 申请一段 staging 内存;失败(设备丢失等)返回 IsValid()==false,调用方回退旧路径。
		VulkanUploadAllocation Allocate(uint64_t size, uint64_t alignment = 4);
		// 录制并提交本批拷贝(不等待)。record 内可引用本批已分配的所有切片。
		bool Submit(const std::function<void(VkCommandBuffer)>& record);
		// 等待所有在飞段(设备空闲时调用;正常路径不需要)。
		void WaitAll();

		// 统计:段数/扩容次数/等待次数/提交次数,用于回归与性能观测。
		uint32_t GetSegmentCount() const { return static_cast<uint32_t>(m_Segments.size()); }
		uint64_t GetSegmentSize() const { return m_SegmentSize; }
		uint64_t GetSubmitCount() const { return m_SubmitCount; }
		uint64_t GetGrowCount() const { return m_GrowCount; }
		uint64_t GetStallCount() const { return m_StallCount; }
		uint64_t GetPendingBytes() const { return m_PendingBytes; }

	private:
		struct Segment
		{
			VkBuffer Buffer = VK_NULL_HANDLE;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			void* Mapped = nullptr;
			uint64_t Capacity = 0;
			uint64_t Offset = 0;             // 本批已用字节
			VkFence OwnFence = VK_NULL_HANDLE;    // 本段最近一次"作为提交拥有者"的栅栏
			VkFence GuardFence = VK_NULL_HANDLE;  // 最近一次引用本段的提交栅栏(可能来自别的段)
			VkCommandBuffer Cmd = VK_NULL_HANDLE;
			bool Submitted = false;          // 已提交,等待 GuardFence
			bool Dirty = false;              // 有已写入但尚未提交的数据
		};

		Segment& Advance(uint64_t required);
		bool TryRecycle(Segment& segment);
		bool CreateSegment(Segment& segment, uint64_t capacity);

		VulkanDevice& m_Device;
		VkCommandPool m_Pool = VK_NULL_HANDLE;
		std::vector<Segment> m_Segments;
		size_t m_Current = 0;
		uint64_t m_SegmentSize;
		uint32_t m_MaxSegments;
		uint64_t m_SubmitCount = 0;
		uint64_t m_GrowCount = 0;
		uint64_t m_StallCount = 0;
		uint64_t m_PendingBytes = 0;
	};
}
