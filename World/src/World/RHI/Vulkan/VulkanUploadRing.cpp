#include "wldpch.h"
#include "VulkanUploadRing.h"
#include "VulkanDevice.h"

#include <algorithm>

namespace World::Rhi::Vulkan
{
	namespace
	{
		uint64_t AlignUp(uint64_t value, uint64_t alignment)
		{
			if (alignment <= 1)
				return value;
			return (value + alignment - 1) / alignment * alignment;
		}

		uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties)
		{
			VkPhysicalDeviceMemoryProperties memoryProperties{};
			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
			for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++)
			{
				const bool supported = (typeBits & (1u << i)) != 0;
				const bool matches = (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
				if (supported && matches)
					return i;
			}
			return 0;
		}
	}

	VulkanUploadRing::VulkanUploadRing(VulkanDevice& device, uint64_t segmentSize, uint32_t maxSegments)
		: m_Device(device), m_SegmentSize(std::max<uint64_t>(segmentSize, 64 * 1024)),
		  m_MaxSegments(std::max<uint32_t>(maxSegments, 2))
	{
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = device.GetGraphicsQueueFamily();
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		if (vkCreateCommandPool(device.GetNativeDevice(), &poolInfo, nullptr, &m_Pool) != VK_SUCCESS)
		{
			WLD_CORE_ERROR("[upload-ring] command pool creation failed; async uploads disabled");
			return;
		}

		m_Segments.resize(1);
		if (!CreateSegment(m_Segments[0], m_SegmentSize))
		{
			m_Segments.clear();
			WLD_CORE_ERROR("[upload-ring] staging segment creation failed; async uploads disabled");
		}
		m_Current = 0;
	}

	VulkanUploadRing::~VulkanUploadRing()
	{
		WaitAll();
		const VkDevice device = m_Device.GetNativeDevice();
		for (Segment& segment : m_Segments)
		{
			if (segment.OwnFence) vkDestroyFence(device, segment.OwnFence, nullptr);
			if (segment.Buffer) vkDestroyBuffer(device, segment.Buffer, nullptr);
			if (segment.Memory) vkFreeMemory(device, segment.Memory, nullptr);
		}
		m_Segments.clear();
		if (m_Pool) vkDestroyCommandPool(device, m_Pool, nullptr);
	}

	bool VulkanUploadRing::CreateSegment(Segment& segment, uint64_t capacity)
	{
		const VkDevice device = m_Device.GetNativeDevice();

		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = capacity;
		bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device, &bufferInfo, nullptr, &segment.Buffer) != VK_SUCCESS)
			return false;

		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(device, segment.Buffer, &requirements);
		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = requirements.size;
		allocInfo.memoryTypeIndex = FindMemoryType(m_Device.GetPhysicalDevice(), requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (vkAllocateMemory(device, &allocInfo, nullptr, &segment.Memory) != VK_SUCCESS)
			return false;
		if (vkBindBufferMemory(device, segment.Buffer, segment.Memory, 0) != VK_SUCCESS)
			return false;
		if (vkMapMemory(device, segment.Memory, 0, capacity, 0, &segment.Mapped) != VK_SUCCESS)
			return false;

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // 初始视为"空闲"
		if (vkCreateFence(device, &fenceInfo, nullptr, &segment.OwnFence) != VK_SUCCESS)
			return false;

		VkCommandBufferAllocateInfo cmdInfo{};
		cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cmdInfo.commandPool = m_Pool;
		cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmdInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(device, &cmdInfo, &segment.Cmd) != VK_SUCCESS)
			return false;

		segment.Capacity = capacity;
		segment.Offset = 0;
		segment.GuardFence = segment.OwnFence;
		segment.Submitted = false;
		segment.Dirty = false;
		return true;
	}

	bool VulkanUploadRing::TryRecycle(Segment& segment)
	{
		if (segment.Dirty)
			return false;                     // 有未提交数据,不能覆盖
		if (!segment.Submitted)
			return true;                      // 从未提交(或已回收)

		VkFence guard = segment.GuardFence ? segment.GuardFence : segment.OwnFence;
		if (guard == VK_NULL_HANDLE)
			return true;
		if (vkGetFenceStatus(m_Device.GetNativeDevice(), guard) != VK_SUCCESS)
			return false;                     // 仍在飞

		// guard 已信号说明本段引用它的那次提交已完成(同队列按提交顺序信号),
		// 本段自己的栅栏此时必然也已信号,可以直接复位复用。
		if (segment.OwnFence)
			vkResetFences(m_Device.GetNativeDevice(), 1, &segment.OwnFence);
		if (segment.Cmd)
			vkResetCommandBuffer(segment.Cmd, 0);
		segment.Submitted = false;
		segment.GuardFence = segment.OwnFence;
		segment.Offset = 0;
		return true;
	}

	VulkanUploadRing::Segment& VulkanUploadRing::Advance(uint64_t required)
	{
		const size_t count = m_Segments.size();
		for (size_t i = 1; i <= count; i++)
		{
			Segment& candidate = m_Segments[(m_Current + i) % count];
			if (TryRecycle(candidate))
			{
				m_Current = (m_Current + i) % count;
				return candidate;
			}
		}

		// 所有段都在飞:优先扩容(不阻塞主线程),到达上限才等待最"老"的一段。
		if (m_Segments.size() < m_MaxSegments)
		{
			Segment fresh;
			const uint64_t capacity = std::max(m_SegmentSize, AlignUp(required, 64 * 1024));
			if (CreateSegment(fresh, capacity))
			{
				m_Segments.push_back(fresh);
				m_Current = m_Segments.size() - 1;
				m_GrowCount++;
				return m_Segments.back();
			}
		}

		// 选一个不持有未提交数据的段等待(当前段可能有本批未提交数据,不能覆盖)。
		size_t stallIndex = (m_Current + 1) % count;
		for (size_t i = 1; i <= count; i++)
			if (!m_Segments[(m_Current + i) % count].Dirty)
			{
				stallIndex = (m_Current + i) % count;
				break;
			}
		Segment& stalled = m_Segments[stallIndex];
		VkFence guard = stalled.GuardFence ? stalled.GuardFence : stalled.OwnFence;
		if (guard)
			vkWaitForFences(m_Device.GetNativeDevice(), 1, &guard, VK_TRUE, UINT64_MAX);
		if (stalled.OwnFence)
			vkResetFences(m_Device.GetNativeDevice(), 1, &stalled.OwnFence);
		if (stalled.Cmd)
			vkResetCommandBuffer(stalled.Cmd, 0);
		stalled.Submitted = false;
		stalled.Dirty = false;
		stalled.GuardFence = stalled.OwnFence;
		stalled.Offset = 0;
		m_Current = stallIndex;
		m_StallCount++;
		WLD_CORE_WARN("[upload-ring] all {0} segments in flight; stalled on oldest (bytes={1})",
			m_Segments.size(), required);
		return stalled;
	}

	VulkanUploadAllocation VulkanUploadRing::Allocate(uint64_t size, uint64_t alignment)
	{
		VulkanUploadAllocation allocation;
		if (size == 0 || m_Segments.empty())
			return allocation;

		Segment* segment = &m_Segments[m_Current];
		// 当前段已提交(命令已结束录制)或装不下:换段/扩容;同一批的多次小分配共用当前段。
		if (segment->Submitted || AlignUp(segment->Offset, alignment) + size > segment->Capacity)
			segment = &Advance(size);

		const uint64_t offset = AlignUp(segment->Offset, alignment);
		if (offset + size > segment->Capacity)
			return allocation;                 // 扩容失败,调用方回退

		segment->Offset = offset + size;
		segment->Dirty = true;
		m_PendingBytes += size;
		allocation.Buffer = segment->Buffer;
		allocation.Offset = offset;
		allocation.Size = size;
		allocation.Mapped = static_cast<uint8_t*>(segment->Mapped) + offset;
		return allocation;
	}

	bool VulkanUploadRing::Submit(const std::function<void(VkCommandBuffer)>& record)
	{
		if (!record || !m_Pool || m_Segments.empty())
			return false;

		// 已提交的段不能再写入(命令已结束录制),换一个干净段承载本批。
		if (m_Segments[m_Current].Submitted)
			Advance(0);

		Segment& segment = m_Segments[m_Current];
		if (!segment.Cmd)
			return false;

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkResetCommandBuffer(segment.Cmd, 0);
		if (vkBeginCommandBuffer(segment.Cmd, &beginInfo) != VK_SUCCESS)
			return false;
		record(segment.Cmd);
		if (vkEndCommandBuffer(segment.Cmd) != VK_SUCCESS)
			return false;

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &segment.Cmd;
		// 栅栏初始为"已信号"(表示空闲):提交前必须复位,否则 VUID-vkQueueSubmit-fence-00063。
		if (segment.OwnFence)
			vkResetFences(m_Device.GetNativeDevice(), 1, &segment.OwnFence);
		if (vkQueueSubmit(m_Device.GetGraphicsQueue(), 1, &submitInfo, segment.OwnFence) != VK_SUCCESS)
			return false;
		m_SubmitCount++;

		// 本批命令缓冲引用到的所有段(含跨段分配)统一由这次提交的栅栏守护。
		for (Segment& other : m_Segments)
		{
			if (!other.Dirty)
				continue;
			other.Dirty = false;
			other.Submitted = true;
			other.GuardFence = segment.OwnFence;
		}
		if (!segment.Submitted)
		{
			segment.Submitted = true;
			segment.GuardFence = segment.OwnFence;
		}
		m_PendingBytes = 0;
		return true;
	}

	void VulkanUploadRing::WaitAll()
	{
		if (!m_Device.GetNativeDevice())
			return;
		for (Segment& segment : m_Segments)
		{
			if (segment.Submitted && segment.OwnFence)
			{
				vkWaitForFences(m_Device.GetNativeDevice(), 1, &segment.OwnFence, VK_TRUE, UINT64_MAX);
				vkResetFences(m_Device.GetNativeDevice(), 1, &segment.OwnFence);
				segment.Submitted = false;
			}
			segment.Dirty = false;
			segment.GuardFence = segment.OwnFence;
			segment.Offset = 0;
		}
		m_PendingBytes = 0;
	}
}
