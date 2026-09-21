#include "wldpch.h"
#include "World/RHI/Vulkan/VulkanResources.h"
#include "World/RHI/Vulkan/VulkanDevice.h"
#include "World/RHI/Vulkan/VulkanUploadRing.h"

#include <cstring>

namespace World::Rhi::Vulkan
{
	namespace
	{
		uint32_t MemoryType(VkPhysicalDevice device, uint32_t typeBits, VkMemoryPropertyFlags properties)
		{
			VkPhysicalDeviceMemoryProperties memory{};
			vkGetPhysicalDeviceMemoryProperties(device, &memory);
			for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
				if ((typeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties)
					return i;
			return UINT32_MAX;
		}

		VkFormat ToVkFormatInternal(Format format)
		{
			switch (format)
			{
				case Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
				case Format::R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
				case Format::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
				case Format::B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
				case Format::R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
				case Format::B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
				case Format::R32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
				case Format::R32G32_SFLOAT: return VK_FORMAT_R32G32_SFLOAT;
				case Format::R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
				case Format::R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
				case Format::R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
				case Format::R32_UINT: return VK_FORMAT_R32_UINT;
				case Format::R32G32_UINT: return VK_FORMAT_R32G32_UINT;
				case Format::R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
				case Format::R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
				case Format::R32_SINT: return VK_FORMAT_R32_SINT;
				case Format::R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
				case Format::R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
				case Format::R16_UNORM: return VK_FORMAT_R16_UNORM;
				case Format::R16G16_UNORM: return VK_FORMAT_R16G16_UNORM;
				case Format::R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
				case Format::R8_SNORM: return VK_FORMAT_R8_SNORM;
				case Format::R8G8_SNORM: return VK_FORMAT_R8G8_SNORM;
				case Format::R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
				case Format::D16_UNORM: return VK_FORMAT_D16_UNORM;
				case Format::D32_SFLOAT: return VK_FORMAT_D32_SFLOAT;
				case Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
				case Format::D32_SFLOAT_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
				case Format::BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
				case Format::BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
				case Format::BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
				case Format::BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
				case Format::BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
				default: return VK_FORMAT_UNDEFINED;
			}
		}

		VkImageLayout TargetLayout(const TextureDesc& desc)
		{
			if (desc.Usage & TextureUsageDepthStencilAttachment)
				return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			if (desc.Usage & TextureUsageColorAttachment)
				return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}

		bool IsDepthFormat(Format format)
		{
			switch (format)
			{
				case Format::D16_UNORM:
				case Format::D32_SFLOAT:
				case Format::D24_UNORM_S8_UINT:
				case Format::D32_SFLOAT_S8_UINT:
					return true;
				default:
					return false;
			}
		}

		// RenderPassDesc 声明的附件布局 → Vulkan 布局(通道首尾的真实布局,见 VulkanCommand 的同步)。
		VkImageLayout AttachmentLayoutToVk(AttachmentLayout layout)
		{
			switch (layout)
			{
			case AttachmentLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case AttachmentLayout::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			case AttachmentLayout::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			case AttachmentLayout::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case AttachmentLayout::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case AttachmentLayout::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case AttachmentLayout::Undefined: break;
			}
			return VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}

	VkFormat ToVkFormat(Format format) { return ToVkFormatInternal(format); }

	VkImageUsageFlags ToVkImageUsage(uint32_t usage)
	{
		VkImageUsageFlags flags = 0;
		if (usage & TextureUsageSampled) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
		if (usage & TextureUsageColorAttachment) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (usage & TextureUsageDepthStencilAttachment) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		if (usage & TextureUsageStorage) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
		if (usage & TextureUsageTransferSrc) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (usage & TextureUsageTransferDst) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if (usage & TextureUsageInputAttachment) flags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
		return flags;
	}

	VkBufferUsageFlags ToVkBufferUsage(uint32_t usage)
	{
		VkBufferUsageFlags flags = 0;
		if (usage & BufferUsageVertex) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		if (usage & BufferUsageIndex) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if (usage & BufferUsageUniform) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		if (usage & BufferUsageStorage) flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		if (usage & BufferUsageIndirect) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		if (usage & BufferUsageTransferSrc) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		if (usage & BufferUsageTransferDst) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		return flags;
	}

	// 需要"完成后才继续"的一次性提交(初始化/布局过渡等);走设备级槽位环,
	// 不再每次新建命令池 + vkQueueWaitIdle。
	void ExecuteOneShot(VulkanDevice& device, const std::function<void(VkCommandBuffer)>& record)
	{
		device.SubmitOneShot(record, true, VK_NULL_HANDLE, VK_NULL_HANDLE, "resource-one-shot");
	}

	// ---- Buffer ----
	VulkanBuffer::VulkanBuffer(VulkanDevice& device, const BufferDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		VkBufferCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.size = desc.Size;
		// 所有缓冲都允许传输读写:命令缓冲里的 UpdateBuffer(staging 拷贝)、
		// 查询结果回读等路径要求 TRANSFER_DST/TRANSFER_SRC,否则
		// VUID-vkCmdUpdateBuffer-dstBuffer-00034(vector 缓冲区只声明了 VERTEX)。
		info.usage = ToVkBufferUsage(desc.Usage) | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		vkCreateBuffer(device.GetNativeDevice(), &info, nullptr, &m_Buffer);

		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(device.GetNativeDevice(), m_Buffer, &requirements);
		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = requirements.size;
		allocInfo.memoryTypeIndex = MemoryType(
			device.GetPhysicalDevice(), requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		vkAllocateMemory(device.GetNativeDevice(), &allocInfo, nullptr, &m_Memory);
		m_AllocationSize = requirements.size;
		{
			VkPhysicalDeviceMemoryProperties memoryProperties{};
			vkGetPhysicalDeviceMemoryProperties(device.GetPhysicalDevice(), &memoryProperties);
			m_Coherent = (memoryProperties.memoryTypes[allocInfo.memoryTypeIndex].propertyFlags &
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
		}
		vkBindBufferMemory(device.GetNativeDevice(), m_Buffer, m_Memory, 0);
		vkMapMemory(device.GetNativeDevice(), m_Memory, 0, desc.Size, 0, &m_Mapped);
		if (desc.InitialData)
			std::memcpy(m_Mapped, desc.InitialData, desc.Size);
	}

	VulkanBuffer::~VulkanBuffer()
	{
		if (m_Device.SkipNativeDestroy())
			return;   // 设备已丢失:交给 vkDestroyDevice/进程回收(见 VulkanDevice::SkipNativeDestroy)
		const VkDevice device = m_Device.GetNativeDevice();
		if (m_Mapped) vkUnmapMemory(device, m_Memory);
		if (m_Buffer) vkDestroyBuffer(device, m_Buffer, nullptr);
		if (m_Memory) vkFreeMemory(device, m_Memory, nullptr);
	}

	void* VulkanBuffer::Map(uint64_t offset, uint64_t)
	{
		return static_cast<uint8_t*>(m_Mapped) + offset;
	}

	void VulkanBuffer::Unmap() {}

	void VulkanBuffer::SetData(const void* data, uint64_t size, uint64_t offset)
	{
		std::memcpy(static_cast<uint8_t*>(m_Mapped) + offset, data, size);
		// 即使申请的是 HOST_COHERENT 内存,也显式刷新一次:批绘制后端每帧
		// 反复写入同一段映射内存,避免 GPU 读到陈旧数据。
		//
		// 刷新范围必须满足 VkMappedMemoryRange 的对齐规则:offset 按 nonCoherentAtomSize 对齐,
		// size 要么是原子大小的整数倍,要么让范围末尾正好落在映射末尾。
		// (此前用 VK_WHOLE_SIZE:当缓冲大小不是 64 的整数倍、且映射末尾不等于内存对象末尾时
		//  会触发 VUID-VkMappedMemoryRange-size-01389,例如 80 字节的对象 UBO。)
		const uint64_t atomSize = std::max<uint64_t>(1, m_Device.GetLimits().NonCoherentAtomSize);
		if (m_Coherent)
			return;   // 一致性内存:写入对设备立即可见,无需刷新(也避免对齐规则限制)
		const uint64_t alignedOffset = offset / atomSize * atomSize;
		uint64_t rangeEnd = (offset + size + atomSize - 1) / atomSize * atomSize;
		if (rangeEnd > m_AllocationSize)
			rangeEnd = m_AllocationSize / atomSize * atomSize;   // 只能刷到原子对齐的边界
		VkMappedMemoryRange range{};
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = m_Memory;
		range.offset = alignedOffset;
		range.size = rangeEnd > alignedOffset ? rangeEnd - alignedOffset : 0;
		if (range.size == 0)
			return;   // 没有可刷新的原子块
		vkFlushMappedMemoryRanges(m_Device.GetNativeDevice(), 1, &range);
	}

	// ---- Texture ----
	VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		VkImageCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		info.imageType = desc.Type == TextureType::Texture3D ? VK_IMAGE_TYPE_3D
			: desc.Type == TextureType::Texture1D ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
		info.format = ToVkFormatInternal(desc.Format);
		info.extent = { desc.Extent.Width, desc.Extent.Height, desc.Extent.Depth };
		info.mipLevels = std::max(1u, desc.MipLevels);
		info.arrayLayers = desc.Type == TextureType::Cube ? std::max(1u, desc.ArrayLayers) * 6 : std::max(1u, desc.ArrayLayers);
		info.samples = static_cast<VkSampleCountFlagBits>(static_cast<uint32_t>(desc.Samples));
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = ToVkImageUsage(desc.Usage) | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (desc.Type == TextureType::Cube)
			info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
		vkCreateImage(device.GetNativeDevice(), &info, nullptr, &m_Image);

		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(device.GetNativeDevice(), m_Image, &requirements);
		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = requirements.size;
		allocInfo.memoryTypeIndex = MemoryType(device.GetPhysicalDevice(), requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		vkAllocateMemory(device.GetNativeDevice(), &allocInfo, nullptr, &m_Memory);
		vkBindImageMemory(device.GetNativeDevice(), m_Image, m_Memory, 0);

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = m_Image;
		viewInfo.viewType = desc.Type == TextureType::Cube ? VK_IMAGE_VIEW_TYPE_CUBE
			: desc.Type == TextureType::Texture3D ? VK_IMAGE_VIEW_TYPE_3D
			: desc.Type == TextureType::Texture1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = info.format;
		viewInfo.subresourceRange.aspectMask = IsDepthFormat(desc.Format)
			? (VK_IMAGE_ASPECT_DEPTH_BIT | (desc.Format == Format::D16_UNORM || desc.Format == Format::D32_SFLOAT ? 0 : VK_IMAGE_ASPECT_STENCIL_BIT))
			: VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = info.mipLevels;
		viewInfo.subresourceRange.layerCount = info.arrayLayers;
		vkCreateImageView(device.GetNativeDevice(), &viewInfo, nullptr, &m_View);
	}

	VulkanTexture::VulkanTexture(VulkanDevice& device, const TextureDesc& desc, VkImage image)
		: m_Device(device), m_Desc(desc), m_Image(image), m_OwnsImage(false)
	{
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = m_Image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = ToVkFormatInternal(desc.Format);
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = std::max(1u, desc.MipLevels);
		viewInfo.subresourceRange.layerCount = std::max(1u, desc.ArrayLayers);
		vkCreateImageView(device.GetNativeDevice(), &viewInfo, nullptr, &m_View);
	}

	VulkanTexture::~VulkanTexture()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		const VkDevice device = m_Device.GetNativeDevice();
		if (m_View) vkDestroyImageView(device, m_View, nullptr);
		if (m_OwnsImage)
		{
			if (m_Image) vkDestroyImage(device, m_Image, nullptr);
			if (m_Memory) vkFreeMemory(device, m_Memory, nullptr);
		}
	}

	namespace
	{
		// 把一次图像布局转换录进命令缓冲(Transition 与异步上传共用同一套同步域)。
		// sourceAllCommands:上传路径重写整张图时用 ALL_COMMANDS 作为源域(重复上传同一纹理时
		// 需要覆盖上一轮采样的读访问),布局过渡路径保持原语义。
		void RecordImageTransition(VkCommandBuffer commandBuffer, VkImage image, const TextureDesc& desc,
			VkImageLayout oldLayout, VkImageLayout newLayout, bool sourceAllCommands)
		{
			if (oldLayout == newLayout)
				return;
			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = oldLayout;
			barrier.newLayout = newLayout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange.aspectMask = IsDepthFormat(desc.Format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.levelCount = std::max(1u, desc.MipLevels);
			barrier.subresourceRange.layerCount = desc.Type == TextureType::Cube ? std::max(1u, desc.ArrayLayers) * 6 : std::max(1u, desc.ArrayLayers);
			// 按目标布局选择同步域:之前固定为 TOP_OF_PIPE→TRANSFER,导致
			// TRANSFER_DST→SHADER_READ_ONLY 之后片元着色器看不到拷贝结果
			// (单次上传的小纹理,如图标,采样全黑)。
			VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			if (oldLayout != VK_IMAGE_LAYOUT_UNDEFINED && sourceAllCommands)
			{
				srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			}
			if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			{
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			}
			else if (newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
			{
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			}
			vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
	}

	void VulkanTexture::Transition(VkImageLayout oldLayout, VkImageLayout newLayout)
	{
		if (oldLayout == newLayout)
			return;
		ExecuteOneShot(m_Device, [&](VkCommandBuffer commandBuffer)
		{
			RecordImageTransition(commandBuffer, m_Image, m_Desc, oldLayout, newLayout, false);
		});
		m_Layout = newLayout;
	}

	void VulkanTexture::SetData(const void* data, uint64_t size, uint32_t layer, uint32_t mip)
	{
		if (!data || size == 0)
			return;

		// B2 异步上传:拷贝进常驻 staging 段,transition/copy/transition 一次提交完成,
		// 不再 wait idle(旧路径每次上传要排空队列 3 次)。上传提交与渲染提交同队列,
		// 队列顺序保证后续绘制能看到结果。
		if (m_OwnsImage)
		{
			VulkanUploadRing& ring = m_Device.GetUploadRing();
			const VulkanUploadAllocation allocation = ring.Allocate(size, 4);
			if (allocation.IsValid())
			{
				std::memcpy(allocation.Mapped, data, size);
				const VkImageLayout target = TargetLayout(m_Desc);
				const VkImageLayout source = m_Layout;
				const bool submitted = ring.Submit([&](VkCommandBuffer commandBuffer)
				{
					RecordImageTransition(commandBuffer, m_Image, m_Desc, source,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
					VkBufferImageCopy region{};
					region.bufferOffset = allocation.Offset;
					region.imageSubresource.aspectMask = IsDepthFormat(m_Desc.Format)
						? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
					region.imageSubresource.mipLevel = mip;
					region.imageSubresource.baseArrayLayer = layer;
					region.imageSubresource.layerCount = 1;
					region.imageExtent = {
						std::max(1u, m_Desc.Extent.Width >> mip),
						std::max(1u, m_Desc.Extent.Height >> mip),
						std::max(1u, m_Desc.Extent.Depth >> mip) };
					vkCmdCopyBufferToImage(commandBuffer, allocation.Buffer, m_Image,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
					RecordImageTransition(commandBuffer, m_Image, m_Desc,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target, false);
				});
				if (submitted)
				{
					m_Layout = target;
					return;
				}
			}
		}

		// 回退:上传环形缓冲不可用(设备初始化失败/包装外部图像)时走同步路径。
		BufferDesc stagingDesc;
		stagingDesc.Size = size;
		stagingDesc.Usage = BufferUsageTransferSrc;
		stagingDesc.Memory = MemoryHint::HostVisible;
		stagingDesc.InitialData = data;
		VulkanBuffer staging(m_Device, stagingDesc);

		Transition(m_Layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_UNDEFINED : m_Layout,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		ExecuteOneShot(m_Device, [&](VkCommandBuffer commandBuffer)
		{
			VkBufferImageCopy region{};
			region.bufferOffset = 0;
			region.imageSubresource.aspectMask = IsDepthFormat(m_Desc.Format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = mip;
			region.imageSubresource.baseArrayLayer = layer;
			region.imageSubresource.layerCount = 1;
			region.imageExtent = {
				std::max(1u, m_Desc.Extent.Width >> mip),
				std::max(1u, m_Desc.Extent.Height >> mip),
				std::max(1u, m_Desc.Extent.Depth >> mip) };
			vkCmdCopyBufferToImage(commandBuffer, staging.GetBuffer(), m_Image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		});
		Transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, TargetLayout(m_Desc));
	}

	// ---- Sampler ----
	VulkanSampler::VulkanSampler(VulkanDevice& device, const SamplerDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		auto filter = [](Filter f) { return f == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; };
		auto wrap = [](SamplerAddressMode mode)
		{
			switch (mode)
			{
				case SamplerAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
				case SamplerAddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				case SamplerAddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
				default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			}
		};
		VkSamplerCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = filter(desc.MagFilter);
		info.minFilter = filter(desc.MinFilter);
		info.mipmapMode = desc.MipmapMode == SamplerMipmapMode::Linear
			? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
		info.addressModeU = wrap(desc.AddressU);
		info.addressModeV = wrap(desc.AddressV);
		info.addressModeW = wrap(desc.AddressW);
		info.mipLodBias = desc.MipLodBias;
		info.minLod = desc.MinLod;
		info.maxLod = desc.MaxLod;
		info.maxAnisotropy = desc.MaxAnisotropy;
		info.anisotropyEnable = desc.MaxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
		info.compareEnable = desc.EnableCompare ? VK_TRUE : VK_FALSE;
		info.compareOp = static_cast<VkCompareOp>(static_cast<uint8_t>(desc.Compare));
		info.unnormalizedCoordinates = desc.UnnormalizedCoordinates ? VK_TRUE : VK_FALSE;
		vkCreateSampler(device.GetNativeDevice(), &info, nullptr, &m_Sampler);
	}

	VulkanSampler::~VulkanSampler()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		if (m_Sampler) vkDestroySampler(m_Device.GetNativeDevice(), m_Sampler, nullptr);
	}

	// ---- Shader ----
	VulkanShader::VulkanShader(VulkanDevice& device, const ShaderDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		m_Modules.reserve(desc.Stages.size());
		for (const ShaderStageSource& stage : desc.Stages)
		{
			if (stage.SpirV.empty())
				continue;
			VkShaderModuleCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			info.codeSize = stage.SpirV.size();
			info.pCode = reinterpret_cast<const uint32_t*>(stage.SpirV.data());
			VkShaderModule module = VK_NULL_HANDLE;
			if (vkCreateShaderModule(device.GetNativeDevice(), &info, nullptr, &module) == VK_SUCCESS)
				m_Modules.push_back(module);
		}
	}

	VulkanShader::~VulkanShader()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		for (VkShaderModule module : m_Modules)
			vkDestroyShaderModule(m_Device.GetNativeDevice(), module, nullptr);
	}

	// ---- RenderPass ----
	VulkanRenderPass::VulkanRenderPass(VulkanDevice& device, const RenderPassDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		std::vector<VkAttachmentDescription> attachments;
		attachments.reserve(desc.Attachments.size());
		for (const RenderPassAttachment& attachment : desc.Attachments)
		{
			VkAttachmentDescription out{};
			out.format = ToVkFormatInternal(attachment.Format);
			out.samples = static_cast<VkSampleCountFlagBits>(static_cast<uint32_t>(attachment.Samples));
			out.loadOp = attachment.Load == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
				: attachment.Load == LoadOp::DontCare ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
			out.storeOp = attachment.Store == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
			out.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			out.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// 使用描述声明的布局:调用方(SceneRenderer/WUI/Renderer)声明的 Initial/FinalLayout
			// 就是附件在通道首尾的真实布局,VulkanCommandBuffer::Begin/EndRenderPass 会据此
			// 同步纹理跟踪。此前硬编码 UNDEFINED + 颜色/深度最优布局,忽略了描述声明。
			out.initialLayout = AttachmentLayoutToVk(attachment.InitialLayout);
			out.finalLayout = attachment.FinalLayout == AttachmentLayout::Undefined
				? (IsDepthFormat(attachment.Format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
				: AttachmentLayoutToVk(attachment.FinalLayout);
			attachments.push_back(out);
		}

		std::vector<VkSubpassDescription> subpasses;
		std::vector<std::vector<VkAttachmentReference>> colorRefs(desc.Subpasses.size());
		// P4-4a:resolve 引用数组。pResolveAttachments 的长度**必须**等于 colorAttachmentCount,
		// 因此这里按颜色附件数量分配,未声明的项留 VK_ATTACHMENT_UNUSED。
		std::vector<std::vector<VkAttachmentReference>> resolveRefs(desc.Subpasses.size());
		std::vector<VkAttachmentReference> depthRefs(desc.Subpasses.size());
		for (size_t i = 0; i < desc.Subpasses.size(); ++i)
		{
			const SubpassDesc& subpass = desc.Subpasses[i];
			for (const AttachmentRef& ref : subpass.ColorAttachments)
				colorRefs[i].push_back({ ref.Index, AttachmentLayoutToVk(ref.Layout) });
			depthRefs[i] = { subpass.DepthStencilAttachment.Index, AttachmentLayoutToVk(subpass.DepthStencilAttachment.Layout) };
			VkSubpassDescription out{};
			out.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			out.colorAttachmentCount = static_cast<uint32_t>(colorRefs[i].size());
			out.pColorAttachments = colorRefs[i].empty() ? nullptr : colorRefs[i].data();
			out.pDepthStencilAttachment = subpass.HasDepthStencil() ? &depthRefs[i] : nullptr;

			// P4-4a:接上 SubpassDesc::ResolveAttachments。
			// 索引语义与 ColorAttachments/DepthStencilAttachment 一致:**渲染通道级**附件索引,
			// 位置语义是"与同 subpass 的 ColorAttachments 按下标一一对应"(第 k 项是第 k 个颜色
			// 附件的 resolve 目标);UINT32_MAX = VK_ATTACHMENT_UNUSED = 该颜色附件不 resolve。
			// 空数组 → pResolveAttachments 保持 nullptr,与改动前逐字节一致(单采样通道零影响)。
			if (!subpass.ResolveAttachments.empty())
			{
				resolveRefs[i].assign(colorRefs[i].size(),
					{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
				const size_t resolveCount = std::min(subpass.ResolveAttachments.size(), colorRefs[i].size());
				for (size_t k = 0; k < resolveCount; ++k)
				{
					const uint32_t resolveIndex = subpass.ResolveAttachments[k];
					if (resolveIndex == UINT32_MAX)
						continue;
					resolveRefs[i][k] = { resolveIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
					// 合同检查(resolve 的 Vulkan 硬约束,违反时 vkCreateRenderPass 直接失败):
					// 颜色附件多采样 + resolve 目标单采样。这里给出能定位到具体通道/附件的日志。
					const bool indicesValid = resolveIndex < desc.Attachments.size() &&
						colorRefs[i][k].attachment < desc.Attachments.size();
					const bool samplesValid = indicesValid &&
						desc.Attachments[resolveIndex].Samples == SampleCount::Count1 &&
						desc.Attachments[colorRefs[i][k].attachment].Samples != SampleCount::Count1;
					if (!samplesValid)
						WLD_CORE_WARN("[RHI-VK] render pass '{0}' subpass {1} color#{2}: resolve target #{3} must be a "
							"single-sampled attachment resolved from a multisampled color attachment",
							desc.DebugName, i, k, resolveIndex);
				}
				if (subpass.ResolveAttachments.size() != colorRefs[i].size())
					WLD_CORE_WARN("[RHI-VK] render pass '{0}' subpass {1}: ResolveAttachments has {2} entries but "
						"there are {3} color attachments; missing entries are treated as VK_ATTACHMENT_UNUSED",
						desc.DebugName, i, subpass.ResolveAttachments.size(), colorRefs[i].size());
				out.pResolveAttachments = resolveRefs[i].empty() ? nullptr : resolveRefs[i].data();
			}
			subpasses.push_back(out);
		}

		VkRenderPassCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		info.attachmentCount = static_cast<uint32_t>(attachments.size());
		info.pAttachments = attachments.empty() ? nullptr : attachments.data();
		info.subpassCount = static_cast<uint32_t>(subpasses.size());
		info.pSubpasses = subpasses.empty() ? nullptr : subpasses.data();
		vkCreateRenderPass(device.GetNativeDevice(), &info, nullptr, &m_RenderPass);
	}

	VulkanRenderPass::~VulkanRenderPass()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		if (m_RenderPass) vkDestroyRenderPass(m_Device.GetNativeDevice(), m_RenderPass, nullptr);
	}

	// ---- Framebuffer ----
	VulkanFramebuffer::VulkanFramebuffer(VulkanDevice& device, const FramebufferDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		std::vector<VkImageView> views;
		views.reserve(desc.Attachments.size());
		for (const Handle<Texture>& attachment : desc.Attachments)
			if (const auto texture = std::dynamic_pointer_cast<VulkanTexture>(attachment))
				views.push_back(texture->GetView());
		VkFramebufferCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		info.renderPass = std::static_pointer_cast<VulkanRenderPass>(desc.RenderPass)->GetRenderPass();
		info.attachmentCount = static_cast<uint32_t>(views.size());
		info.pAttachments = views.empty() ? nullptr : views.data();
		info.width = desc.Extent.Width;
		info.height = desc.Extent.Height;
		info.layers = std::max(1u, desc.Layers);
		vkCreateFramebuffer(device.GetNativeDevice(), &info, nullptr, &m_Framebuffer);
	}

	VulkanFramebuffer::~VulkanFramebuffer()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		if (m_Framebuffer) vkDestroyFramebuffer(m_Device.GetNativeDevice(), m_Framebuffer, nullptr);
	}

	// ---- DescriptorSet ----
	VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(VulkanDevice& device, const DescriptorSetLayoutDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		bindings.reserve(desc.Bindings.size());
		for (const DescriptorBinding& binding : desc.Bindings)
		{
			VkDescriptorType type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			switch (binding.Type)
			{
				case DescriptorType::UniformBuffer: type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
				case DescriptorType::StorageBuffer: type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
				case DescriptorType::SampledImage: type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; break;
				case DescriptorType::StorageImage: type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
				case DescriptorType::Sampler: type = VK_DESCRIPTOR_TYPE_SAMPLER; break;
				case DescriptorType::InputAttachment: type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; break;
				default: type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
			}
			bindings.push_back({ binding.Binding, type, binding.Count,
				VK_SHADER_STAGE_ALL, nullptr });
		}
		VkDescriptorSetLayoutCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		info.bindingCount = static_cast<uint32_t>(bindings.size());
		info.pBindings = bindings.empty() ? nullptr : bindings.data();
		std::vector<VkDescriptorBindingFlags> bindingFlags(bindings.size(),
			VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
		VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
		bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
		bindingFlagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
		bindingFlagsInfo.pBindingFlags = bindingFlags.empty() ? nullptr : bindingFlags.data();
		info.pNext = &bindingFlagsInfo;
		info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
		vkCreateDescriptorSetLayout(device.GetNativeDevice(), &info, nullptr, &m_Layout);
	}

	VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		if (m_Layout) vkDestroyDescriptorSetLayout(m_Device.GetNativeDevice(), m_Layout, nullptr);
	}

	VulkanDescriptorSet::VulkanDescriptorSet(VulkanDevice& device, const Handle<VulkanDescriptorSetLayout>& layout)
		: m_Device(device), m_Layout(layout)
	{
		VkDescriptorPoolSize sizes[] = {
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 },
		};
		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		// 池标志必须与布局匹配:spirv-cross 为 combinedImageSampler 生成的布局带
		// VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,池少了这个标志
		// 会让 vkAllocateDescriptorSets 失败(返回空 set),随后 Update 直接崩
		// (实测 VUID-VkDescriptorSetAllocateInfo-pSetLayouts-03044)。
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
		poolInfo.maxSets = 1;
		poolInfo.poolSizeCount = 3;
		poolInfo.pPoolSizes = sizes;
		vkCreateDescriptorPool(device.GetNativeDevice(), &poolInfo, nullptr, &m_Pool);

		const VkDescriptorSetLayout nativeLayout = m_Layout->GetLayout();
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = m_Pool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &nativeLayout;
		const VkResult allocateResult = vkAllocateDescriptorSets(device.GetNativeDevice(), &allocInfo, &m_Set);
		if (allocateResult != VK_SUCCESS)
		{
			// 分配失败时 m_Set 无效,后续 Update 会在驱动里解引用空句柄直接崩;
			// 这里明确报错并保持 m_Set 为空,让 Update 能安全跳过。
			WLD_CORE_ERROR("[RHI-VK] vkAllocateDescriptorSets failed (result={0}); descriptor writes will be skipped",
				static_cast<int>(allocateResult));
			m_Set = VK_NULL_HANDLE;
		}
	}

	VulkanDescriptorSet::~VulkanDescriptorSet()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		if (m_Pool)
			vkDestroyDescriptorPool(m_Device.GetNativeDevice(), m_Pool, nullptr);
	}

	void VulkanDescriptorSet::Update(const std::vector<DescriptorWrite>& writes)
	{
		if (m_Set == VK_NULL_HANDLE)
			return;   // 分配失败的 set:静默跳过,避免驱动解引用空句柄
		std::vector<VkWriteDescriptorSet> out;
		std::vector<VkDescriptorBufferInfo> buffers;
		std::vector<VkDescriptorImageInfo> images;
		out.reserve(writes.size());
		// **必须预留到不会重分配**:entry.pBufferInfo/pImageInfo 指向下面容器里的元素,
		// 边遍历边 push 一旦触发扩容,先前的指针就变悬垂 —— 驱动会读到已释放内存
		// (实测:材质一次写 albedo+normal 两个 image info 时崩在 vkUpdateDescriptorSets,
		//  验证层报 imageView=0xDDDDDDDD/imageLayout=堆毒)。
		buffers.reserve(writes.size());
		images.reserve(writes.size());
		for (const DescriptorWrite& write : writes)
		{
			VkWriteDescriptorSet entry{};
			entry.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			entry.dstSet = m_Set;
			entry.dstBinding = write.Binding;
			entry.dstArrayElement = write.ArrayIndex;
			entry.descriptorCount = 1;
			if (write.Type == DescriptorType::UniformBuffer || write.Type == DescriptorType::StorageBuffer)
			{
				const auto buffer = std::dynamic_pointer_cast<VulkanBuffer>(write.Buffer);
				if (!buffer)
					continue;
				VkDescriptorBufferInfo info{ buffer->GetBuffer(), write.BufferOffset,
					write.BufferRange ? write.BufferRange : buffer->GetDesc().Size - write.BufferOffset };
				buffers.push_back(info);
				entry.descriptorType = write.Type == DescriptorType::UniformBuffer
					? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				entry.pBufferInfo = &buffers.back();
			}
			else if (write.Type == DescriptorType::Sampler)
			{
				const auto sampler = std::dynamic_pointer_cast<VulkanSampler>(write.Sampler);
				if (!sampler)
					continue;
				VkDescriptorImageInfo info{ sampler->GetSampler(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
				images.push_back(info);
				entry.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
				entry.pImageInfo = &images.back();
			}
			else
			{
				const auto texture = std::dynamic_pointer_cast<VulkanTexture>(write.Texture);
				if (!texture)
					continue;
				const auto sampler = std::dynamic_pointer_cast<VulkanSampler>(write.Sampler);
				VkDescriptorImageInfo info{ sampler ? sampler->GetSampler() : VK_NULL_HANDLE,
					texture->GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
				images.push_back(info);
				entry.descriptorType = write.Type == DescriptorType::SampledImage
					? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
					: write.Type == DescriptorType::StorageImage
						? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				entry.pImageInfo = &images.back();
			}
			out.push_back(entry);
		}
		if (!out.empty())
			vkUpdateDescriptorSets(m_Device.GetNativeDevice(), static_cast<uint32_t>(out.size()), out.data(), 0, nullptr);
	}

	// ---- Sync ----
	VulkanFence::VulkanFence(VulkanDevice& device, bool signaled) : m_Device(device)
	{
		VkFenceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		info.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
		vkCreateFence(device.GetNativeDevice(), &info, nullptr, &m_Fence);
	}

	VulkanFence::~VulkanFence()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		if (m_Fence) vkDestroyFence(m_Device.GetNativeDevice(), m_Fence, nullptr);
	}

	void VulkanFence::Wait(uint64_t timeoutNs)
	{
		vkWaitForFences(m_Device.GetNativeDevice(), 1, &m_Fence, VK_TRUE, timeoutNs);
	}

	bool VulkanFence::IsSignaled() const
	{
		return vkGetFenceStatus(m_Device.GetNativeDevice(), m_Fence) == VK_SUCCESS;
	}

	void VulkanFence::Reset()
	{
		vkResetFences(m_Device.GetNativeDevice(), 1, &m_Fence);
	}

	VulkanSemaphore::VulkanSemaphore(VulkanDevice& device, const SemaphoreCreateDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		VkSemaphoreTypeCreateInfo typeInfo{};
		typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		typeInfo.semaphoreType = desc.Timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY;
		typeInfo.initialValue = desc.InitialValue;
		VkSemaphoreCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		info.pNext = desc.Timeline ? &typeInfo : nullptr;
		vkCreateSemaphore(device.GetNativeDevice(), &info, nullptr, &m_Semaphore);
	}

	VulkanSemaphore::~VulkanSemaphore()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		if (m_Semaphore) vkDestroySemaphore(m_Device.GetNativeDevice(), m_Semaphore, nullptr);
	}

	void VulkanSemaphore::Signal(uint64_t value)
	{
		if (!m_Desc.Timeline)
			return;
		VkSemaphoreSignalInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
		info.semaphore = m_Semaphore;
		info.value = value;
		vkSignalSemaphore(m_Device.GetNativeDevice(), &info);
	}

	void VulkanSemaphore::Wait(uint64_t value)
	{
		if (!m_Desc.Timeline)
			return;
		VkSemaphoreWaitInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
		info.semaphoreCount = 1;
		info.pSemaphores = &m_Semaphore;
		info.pValues = &value;
		vkWaitSemaphores(m_Device.GetNativeDevice(), &info, UINT64_MAX);
	}

	// ---- QueryPool ----
	VulkanQueryPool::VulkanQueryPool(VulkanDevice& device, QueryType type, uint32_t count)
		: m_Device(device), m_Type(type), m_Count(count)
	{
		VkQueryPoolCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		info.queryType = type == QueryType::Occlusion ? VK_QUERY_TYPE_OCCLUSION : VK_QUERY_TYPE_TIMESTAMP;
		info.queryCount = count;
		vkCreateQueryPool(device.GetNativeDevice(), &info, nullptr, &m_Pool);
	}

	VulkanQueryPool::~VulkanQueryPool()
	{
		if (m_Device.SkipNativeDestroy())
			return;
		if (m_Pool) vkDestroyQueryPool(m_Device.GetNativeDevice(), m_Pool, nullptr);
	}
}
