#include "wldpch.h"
#define VK_USE_PLATFORM_WIN32_KHR
#include "World/RHI/Vulkan/VulkanDevice.h"
#include "World/RHI/Vulkan/VulkanCommand.h"
#include "World/RHI/Vulkan/VulkanPipeline.h"
#include "World/RHI/Vulkan/VulkanResources.h"
#include "World/RHI/Vulkan/VulkanSwapchain.h"
#include "World/RHI/Vulkan/VulkanUploadRing.h"
#include "World/Core/Log.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace World::Rhi::Vulkan
{
	namespace
	{
		VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
			VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT /*type*/,
			const VkDebugUtilsMessengerCallbackDataEXT* data,
			void* /*user*/)
		{
			WLD_CORE_WARN("[vk-validation] {0}", data->pMessage ? data->pMessage : "");
			return VK_FALSE;
		}

		bool HasValidationLayer()
		{
			// 实例创建前只有 loader(vkGetInstanceProcAddr)可用,全局入口未装载。
			const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
				vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
			if (!enumerate)
				return false;
			uint32_t count = 0;
			enumerate(&count, nullptr);
			std::vector<VkLayerProperties> layers(count);
			if (count)
				enumerate(&count, layers.data());
			for (const auto& layer : layers)
				if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
					return true;
			return false;
		}

		int GraphicsQueueFamily(VkPhysicalDevice device)
		{
			uint32_t count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
			std::vector<VkQueueFamilyProperties> families(count);
			vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
			for (uint32_t i = 0; i < count; ++i)
				if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
					return static_cast<int>(i);
			return -1;
		}
	}

	VulkanDevice::VulkanDevice(const DeviceDesc& desc) : m_Desc(desc)
	{
		std::string error;
		if (!Initialize(desc, &error))
			throw std::runtime_error(error);
	}

	VulkanDevice::~VulkanDevice()
	{
		if (m_Device)
		{
			vkDeviceWaitIdle(m_Device);
			// 先回收上传环形缓冲(释放 staging 缓冲/栅栏/命令池),再销毁设备。
			m_UploadRing.reset();
			for (OneShotSlot& slot : m_OneShotSlots)
				if (slot.Fence)
					vkDestroyFence(m_Device, slot.Fence, nullptr);
			m_OneShotSlots.clear();
			if (m_TransientPool)
				vkDestroyCommandPool(m_Device, m_TransientPool, nullptr);
			for (const auto& threadPool : m_ThreadPools)
				if (threadPool.second)
					vkDestroyCommandPool(m_Device, threadPool.second, nullptr);
			m_ThreadPools.clear();
			if (m_CommandPool)
				vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
			vkDestroyDevice(m_Device, nullptr);
		}
		if (m_Instance)
		{
			if (m_DebugMessenger)
				vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
			vkDestroyInstance(m_Instance, nullptr);
			m_Instance = VK_NULL_HANDLE;
		}
	}

	bool VulkanDevice::Initialize(const DeviceDesc& desc, std::string* error)
	{
		if (volkInitialize() != VK_SUCCESS)
		{
			if (error) *error = "volkInitialize failed: no Vulkan driver (vulkan-1.dll)";
			return false;
		}

		VkApplicationInfo app{};
		app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app.pApplicationName = "WorldEngine";
		app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		app.pEngineName = "WorldEngine";
		app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		app.apiVersion = VK_API_VERSION_1_3;

		const bool enableValidation = desc.EnableValidation && HasValidationLayer();
		VkInstanceCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &app;
		static const char* instanceExtensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
		createInfo.enabledExtensionCount = 2;
		createInfo.ppEnabledExtensionNames = instanceExtensions;
		if (enableValidation)
		{
			static const char* layer = "VK_LAYER_KHRONOS_validation";
			createInfo.enabledLayerCount = 1;
			createInfo.ppEnabledLayerNames = &layer;
		}
		const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
			vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
		if (!createInstance || createInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS)
		{
			if (error) *error = "vkCreateInstance failed";
			return false;
		}
		volkLoadInstance(m_Instance);
		if (enableValidation)
		{
			VkDebugUtilsMessengerCreateInfoEXT messengerInfo {};
			messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			messengerInfo.pfnUserCallback = DebugCallback;
			if (vkCreateDebugUtilsMessengerEXT)
			{
				VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
				if (vkCreateDebugUtilsMessengerEXT(m_Instance, &messengerInfo, nullptr, &messenger) == VK_SUCCESS)
					m_DebugMessenger = messenger;
			}
		}

		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
		if (deviceCount == 0)
		{
			if (error) *error = "no Vulkan physical device";
			return false;
		}
		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());
		if (desc.AdapterIndex < deviceCount)
			m_PhysicalDevice = devices[desc.AdapterIndex];
		else
			m_PhysicalDevice = devices[0];

		const int graphicsFamily = GraphicsQueueFamily(m_PhysicalDevice);
		if (graphicsFamily < 0)
		{
			if (error) *error = "physical device has no graphics queue family";
			return false;
		}
		m_GraphicsFamily = static_cast<uint32_t>(graphicsFamily);

		const float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo{};
		queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfo.queueFamilyIndex = m_GraphicsFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;

		VkDeviceCreateInfo deviceInfo{};
		deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		VkPhysicalDeviceFeatures supportedFeatures{};
		vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supportedFeatures);
		VkPhysicalDeviceFeatures enabledFeatures{};
		if (supportedFeatures.wideLines)
			enabledFeatures.wideLines = VK_TRUE;
		// 多渲染目标下各附件的混合状态不同(颜色附件开混合、实体 ID 附件只写),
		// 需要 independentBlend。
		if (supportedFeatures.independentBlend)
			enabledFeatures.independentBlend = VK_TRUE;
		// P4-2:各向异性过滤必须**在这里启用**才合法 —— 之前只在能力表里查询、没启用,
		// 一旦 sampler 真的开各向异性就撞 VUID-VkSamplerCreateInfo-anisotropyEnable-01070
		// (实测:rendering.anisotropy=16 时 Editor/Runtime 各 1 条)。
		if (supportedFeatures.samplerAnisotropy)
			enabledFeatures.samplerAnisotropy = VK_TRUE;
		deviceInfo.pEnabledFeatures = &enabledFeatures;
		// 批绘制后端( WUI 字体图集 / Renderer2D 纹理槽 )在命令缓冲录制期间更新
		// 已绑定的描述符集,需要 UPDATE_AFTER_BIND;先在物理设备上查询支持情况。
		VkPhysicalDeviceDescriptorIndexingFeatures supportedIndexing{};
		supportedIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
		VkPhysicalDeviceFeatures2 supportedFeatures2{};
		supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		supportedFeatures2.pNext = &supportedIndexing;
		vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &supportedFeatures2);
		VkPhysicalDeviceDescriptorIndexingFeatures enabledIndexing{};
		enabledIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
		enabledIndexing.descriptorBindingUniformBufferUpdateAfterBind =
			supportedIndexing.descriptorBindingUniformBufferUpdateAfterBind;
		enabledIndexing.descriptorBindingSampledImageUpdateAfterBind =
			supportedIndexing.descriptorBindingSampledImageUpdateAfterBind;
		enabledIndexing.descriptorBindingStorageBufferUpdateAfterBind =
			supportedIndexing.descriptorBindingStorageBufferUpdateAfterBind;
		enabledIndexing.descriptorBindingStorageImageUpdateAfterBind =
			supportedIndexing.descriptorBindingStorageImageUpdateAfterBind;
		deviceInfo.pNext = &enabledIndexing;
		static const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
		deviceInfo.enabledExtensionCount = 1;
		deviceInfo.ppEnabledExtensionNames = extensions;
		if (vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_Device) != VK_SUCCESS)
		{
			if (error) *error = "vkCreateDevice failed";
			return false;
		}
		volkLoadDevice(m_Device);
		vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);

		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = m_GraphicsFamily;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool);

		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);
		VkPhysicalDeviceFeatures features{};
		vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &features);
		VkPhysicalDeviceVulkan12Features v12{};
		v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		VkPhysicalDeviceFeatures2 features2{};
		features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features2.pNext = &v12;
		vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &features2);

		m_Capabilities.BackendName = "Vulkan";
		m_Capabilities.RendererName = properties.deviceName;
		m_Capabilities.ApiMajor = VK_API_VERSION_MAJOR(properties.apiVersion);
		m_Capabilities.ApiMinor = VK_API_VERSION_MINOR(properties.apiVersion);
		m_Capabilities.Compute = true;
		m_Capabilities.DrawIndirect = true;
		// 每线程命令池:命令缓冲可在任意线程录制;帧深由渲染器控制(当前 2)。
		m_Capabilities.ParallelRecording = true;
		m_Capabilities.MaxFramesInFlight = 2;
		m_Capabilities.MultiDrawIndirect = features.multiDrawIndirect == VK_TRUE;
		m_Capabilities.PushConstants = true;
		m_Capabilities.TimelineSemaphores = v12.timelineSemaphore == VK_TRUE;
		m_Capabilities.AnisotropicFiltering = features.samplerAnisotropy == VK_TRUE;
		// P4-2:设备各向异性上限(maxSamplerAnisotropy);不支持时按 1 上报。
		m_Capabilities.MaxSamplerAnisotropy = m_Capabilities.AnisotropicFiltering
			? properties.limits.maxSamplerAnisotropy : 1.0f;
		m_Capabilities.DepthBiasClamp = features.depthBiasClamp == VK_TRUE;
		m_Capabilities.TimestampQueries = properties.limits.timestampComputeAndGraphics == VK_TRUE;
		// D8b:时间戳时基(0 是非法值,退回 1ns/tick)。
		m_TimestampPeriodNs = properties.limits.timestampPeriod > 0.0f
			? static_cast<double>(properties.limits.timestampPeriod) : 1.0;
		m_Capabilities.DescriptorIndexing = v12.descriptorIndexing == VK_TRUE;
		m_Capabilities.BindlessTextures = v12.descriptorIndexing == VK_TRUE;
		m_Capabilities.TextureCompressionBC = features.textureCompressionBC == VK_TRUE;
		m_Capabilities.MaxColorAttachments = properties.limits.maxColorAttachments;
		// framebufferColorSampleCounts 是位掩码,折算为最大可用采样数。
		{
			const VkSampleCountFlags samples = properties.limits.framebufferColorSampleCounts;
			uint32_t maxSamples = 1;
			for (uint32_t candidate : { 8u, 4u, 2u })
				if (samples & candidate)
				{
					maxSamples = candidate;
					break;
				}
			m_Capabilities.MaxSampleCount = maxSamples;
		}
		m_Capabilities.MaxTextureSize = properties.limits.maxImageDimension2D;
		m_Capabilities.MaxImageArrayLayers = properties.limits.maxImageArrayLayers;
		m_Capabilities.MaxUniformBufferSize = properties.limits.maxUniformBufferRange;
		m_Capabilities.MaxStorageBufferSize = properties.limits.maxStorageBufferRange;
		m_Capabilities.MaxPushConstantSize = properties.limits.maxPushConstantsSize;

		m_Limits.MinUniformBufferOffsetAlignment = properties.limits.minUniformBufferOffsetAlignment;
		m_Limits.NonCoherentAtomSize = properties.limits.nonCoherentAtomSize;
		m_Limits.TimestampPeriod = properties.limits.timestampPeriod;
		if (Log::GetCoreLogger())
			WLD_CORE_INFO("Vulkan device created: {0} ({1}.{2})", properties.deviceName,
				m_Capabilities.ApiMajor, m_Capabilities.ApiMinor);

		// B2:异步上传基座。段大小 4 MiB × 最多 8 段,按需扩容;失败时上传回退同步路径。
		m_UploadRing = std::make_unique<VulkanUploadRing>(*this);
		return true;
	}

	void VulkanDevice::WaitIdle()
	{
		if (m_Device)
			vkDeviceWaitIdle(m_Device);
	}

	bool VulkanDevice::SubmitOneShot(const std::function<void(VkCommandBuffer)>& record, bool wait,
		VkSemaphore waitSemaphore, VkSemaphore signalSemaphore)
	{
		if (!m_Device || !record)
			return false;

		if (m_OneShotSlots.empty())
		{
			VkCommandPoolCreateInfo poolInfo{};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.queueFamilyIndex = m_GraphicsFamily;
			poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_TransientPool) != VK_SUCCESS)
				return false;

			m_OneShotSlots.resize(4);
			std::vector<VkCommandBuffer> buffers(m_OneShotSlots.size(), VK_NULL_HANDLE);
			VkCommandBufferAllocateInfo allocInfo{};
			allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			allocInfo.commandPool = m_TransientPool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = static_cast<uint32_t>(buffers.size());
			if (vkAllocateCommandBuffers(m_Device, &allocInfo, buffers.data()) != VK_SUCCESS)
			{
				m_OneShotSlots.clear();
				return false;
			}
			for (size_t i = 0; i < m_OneShotSlots.size(); i++)
			{
				m_OneShotSlots[i].CommandBuffer = buffers[i];
				VkFenceCreateInfo fenceInfo{};
				fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
				fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // 初始即"空闲"
				if (vkCreateFence(m_Device, &fenceInfo, nullptr, &m_OneShotSlots[i].Fence) != VK_SUCCESS)
				{
					m_OneShotSlots.clear();
					return false;
				}
			}
		}

		// 优先找空闲槽位(栅栏已信号);全部在飞时只等待最老的那个槽位。
		OneShotSlot* slot = nullptr;
		const size_t count = m_OneShotSlots.size();
		for (size_t i = 0; i < count; i++)
		{
			OneShotSlot& candidate = m_OneShotSlots[(m_NextOneShot + i) % count];
			if (vkGetFenceStatus(m_Device, candidate.Fence) == VK_SUCCESS)
			{
				slot = &candidate;
				m_NextOneShot = (m_NextOneShot + i + 1) % count;
				break;
			}
		}
		if (!slot)
		{
			slot = &m_OneShotSlots[m_NextOneShot];
			vkWaitForFences(m_Device, 1, &slot->Fence, VK_TRUE, UINT64_MAX);
			m_NextOneShot = (m_NextOneShot + 1) % count;
		}

		vkResetFences(m_Device, 1, &slot->Fence);
		vkResetCommandBuffer(slot->CommandBuffer, 0);
		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(slot->CommandBuffer, &beginInfo) != VK_SUCCESS)
			return false;
		record(slot->CommandBuffer);
		if (vkEndCommandBuffer(slot->CommandBuffer) != VK_SUCCESS)
			return false;

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &slot->CommandBuffer;
		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		if (waitSemaphore != VK_NULL_HANDLE)
		{
			submitInfo.waitSemaphoreCount = 1;
			submitInfo.pWaitSemaphores = &waitSemaphore;
			submitInfo.pWaitDstStageMask = &waitStage;
		}
		if (signalSemaphore != VK_NULL_HANDLE)
		{
			submitInfo.signalSemaphoreCount = 1;
			submitInfo.pSignalSemaphores = &signalSemaphore;
		}
		const VkResult submitResult = vkQueueSubmit(m_GraphicsQueue, 1, &submitInfo, slot->Fence);
		if (submitResult != VK_SUCCESS)
		{
			// 提交失败会让调用方拿不到信号量/栅栏(实测:帧起始转换提交失败后,
			// 后续提交等待一个永远不会有 signal 的信号量 → VUID-...-pWaitSemaphores-03238)。
			// DEVICE_LOST 只报一次:设备已死,后续每帧的失败都是噪声。
			if (submitResult == VK_ERROR_DEVICE_LOST && !m_DeviceLost)
			{
				m_DeviceLost = true;
				WLD_CORE_ERROR("[RHI-VK] device lost (vkQueueSubmit VK_ERROR_DEVICE_LOST); GPU work stopped");
			}
			else if (!m_DeviceLost && std::getenv("WLD_VK_PRESENT_TRACE"))
				WLD_CORE_ERROR("[RHI-VK] SubmitOneShot vkQueueSubmit failed result={0}", static_cast<int>(submitResult));
			return false;
		}

		if (wait)
			vkWaitForFences(m_Device, 1, &slot->Fence, VK_TRUE, UINT64_MAX);
		return true;
	}

#define NOT_IMPLEMENTED() do { if (Log::GetCoreLogger()) WLD_CORE_WARN("[RHI-VK] {0} not implemented yet", __func__); } while (0)

	Handle<CommandQueue> VulkanDevice::CreateQueue(const std::string&) { return CreateRef<VulkanCommandQueue>(*this); }

	VkCommandPool VulkanDevice::GetThreadCommandPool()
	{
		// 每线程一个池:命令缓冲分配/释放必须在创建它的池所属线程上进行。
		// 池按 thread::id 归属设备自身(见 VulkanDevice.h 的 m_ThreadPools)。
		// 历史教训:曾用 thread_local{Device 指针, Pool} 做跨调用缓存并按指针判等,
		// 但热切换销毁旧设备后,新 VulkanDevice 常被分配在**同一地址**(分配器复用),
		// 指针比较通过 → 返回已销毁设备上的池 → vkAllocateCommandBuffers 在 NVIDIA
		// 驱动内 0xC0000005(World.RendererHotswap 连跑数轮必现的偶发崩溃)。
		const std::thread::id threadId = std::this_thread::get_id();
		std::lock_guard<std::mutex> lock(m_PoolMutex);
		if (const auto found = m_ThreadPools.find(threadId); found != m_ThreadPools.end())
			return found->second;

		VkCommandPoolCreateInfo poolInfo {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = m_GraphicsFamily;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VkCommandPool pool = VK_NULL_HANDLE;
		if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &pool) != VK_SUCCESS)
			return m_CommandPool;   // 退化为设备级池(单线程录制仍可用)
		m_ThreadPools.emplace(threadId, pool);
		return pool;
	}
	Handle<CommandBuffer> VulkanDevice::CreateCommandBuffer(const std::string&) { return CreateRef<VulkanCommandBuffer>(*this); }
	Handle<Swapchain> VulkanDevice::CreateSwapchain(const SwapchainDesc& desc) { return CreateRef<VulkanSwapchain>(*this, desc); }
	Handle<RenderPass> VulkanDevice::CreateRenderPass(const RenderPassDesc& desc) { return CreateRef<VulkanRenderPass>(*this, desc); }
	Handle<Framebuffer> VulkanDevice::CreateFramebuffer(const FramebufferDesc& desc) { return CreateRef<VulkanFramebuffer>(*this, desc); }
	Handle<Pipeline> VulkanDevice::CreatePipeline(const PipelineDesc& desc) { return CreateRef<VulkanPipeline>(*this, desc); }
	Handle<Shader> VulkanDevice::CreateShader(const ShaderDesc& desc) { return CreateRef<VulkanShader>(*this, desc); }
	Handle<Buffer> VulkanDevice::CreateBuffer(const BufferDesc& desc) { return CreateRef<VulkanBuffer>(*this, desc); }
	Handle<Texture> VulkanDevice::CreateTexture(const TextureDesc& desc) { return CreateRef<VulkanTexture>(*this, desc); }
	Handle<Sampler> VulkanDevice::CreateSampler(const SamplerDesc& desc) { return CreateRef<VulkanSampler>(*this, desc); }
	Handle<DescriptorSetLayout> VulkanDevice::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) { return CreateRef<VulkanDescriptorSetLayout>(*this, desc); }
	Handle<DescriptorSet> VulkanDevice::CreateDescriptorSet(const Handle<DescriptorSetLayout>& layout)
	{
		return CreateRef<VulkanDescriptorSet>(*this, std::static_pointer_cast<VulkanDescriptorSetLayout>(layout));
	}
	Handle<Fence> VulkanDevice::CreateFence(bool signaled) { return CreateRef<VulkanFence>(*this, signaled); }
	Handle<Semaphore> VulkanDevice::CreateSemaphore(const SemaphoreCreateDesc& desc) { return CreateRef<VulkanSemaphore>(*this, desc); }
	Handle<QueryPool> VulkanDevice::CreateQueryPool(QueryType type, uint32_t count) { return CreateRef<VulkanQueryPool>(*this, type, count); }

	Handle<VulkanDevice> VulkanDevice::Create(const DeviceDesc& desc, std::string* error)
	{
		if (error) error->clear();
		try
		{
			return CreateRef<VulkanDevice>(desc);
		}
		catch (const std::exception& exception)
		{
			if (error) *error = exception.what();
			return nullptr;
		}
	}
}
