#include "wldpch.h"
#include "World/RHI/Vulkan/VulkanDevice.h"
#include "World/Core/Log.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace World::Rhi::Vulkan
{
	namespace
	{
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
			vkDestroyDevice(m_Device, nullptr);
		}
		if (m_Instance)
		{
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
		m_Capabilities.MultiDrawIndirect = features.multiDrawIndirect == VK_TRUE;
		m_Capabilities.PushConstants = true;
		m_Capabilities.TimelineSemaphores = v12.timelineSemaphore == VK_TRUE;
		m_Capabilities.AnisotropicFiltering = features.samplerAnisotropy == VK_TRUE;
		m_Capabilities.DepthBiasClamp = features.depthBiasClamp == VK_TRUE;
		m_Capabilities.TimestampQueries = properties.limits.timestampComputeAndGraphics == VK_TRUE;
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
		return true;
	}

	void VulkanDevice::WaitIdle()
	{
		if (m_Device)
			vkDeviceWaitIdle(m_Device);
	}

#define NOT_IMPLEMENTED() do { if (Log::GetCoreLogger()) WLD_CORE_WARN("[RHI-VK] {0} not implemented yet", __func__); } while (0)

	Handle<CommandQueue> VulkanDevice::CreateQueue(const std::string&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<CommandBuffer> VulkanDevice::CreateCommandBuffer(const std::string&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<Swapchain> VulkanDevice::CreateSwapchain(const SwapchainDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<RenderPass> VulkanDevice::CreateRenderPass(const RenderPassDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<Framebuffer> VulkanDevice::CreateFramebuffer(const FramebufferDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<Pipeline> VulkanDevice::CreatePipeline(const PipelineDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<Shader> VulkanDevice::CreateShader(const ShaderDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<Buffer> VulkanDevice::CreateBuffer(const BufferDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<Texture> VulkanDevice::CreateTexture(const TextureDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<Sampler> VulkanDevice::CreateSampler(const SamplerDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<DescriptorSetLayout> VulkanDevice::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<DescriptorSet> VulkanDevice::CreateDescriptorSet(const Handle<DescriptorSetLayout>&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<Fence> VulkanDevice::CreateFence(bool) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<Semaphore> VulkanDevice::CreateSemaphore(const SemaphoreCreateDesc&) { NOT_IMPLEMENTED(); return nullptr; }
	Handle<QueryPool> VulkanDevice::CreateQueryPool(QueryType, uint32_t) { NOT_IMPLEMENTED(); return nullptr; }

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
