#include "wldpch.h"
#include "VulkanDevice.h"

namespace World
{
	VulkanDevice::VulkanDevice(const Ref<VulkanPhysicalDevice>& physicalDevice)
		:m_PhysicalDevice(physicalDevice)
	{
		WLD_PROFILE_FUNCTION();

		float queuePriority = 1.0f;
		uint32_t queueFamilyIndex = m_PhysicalDevice->GetQueueFamilyIndices().GraphicsFamily.value();

		// Create the device queue create info
		VkDeviceQueueCreateInfo queueCreateInfo {};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;

		// Enable features
		VkPhysicalDeviceFeatures deviceFeatures {};


		// Create the device create info
		VkDeviceCreateInfo createInfo {};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		createInfo.queueCreateInfoCount = 1;
		createInfo.pQueueCreateInfos = &queueCreateInfo;
		std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
		createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
		createInfo.ppEnabledExtensionNames = deviceExtensions.data();
		createInfo.pEnabledFeatures = &deviceFeatures;

		if (vkCreateDevice(m_PhysicalDevice->GetPhysicalDevice(), &createInfo, nullptr, &m_LogicalDevice) != VK_SUCCESS)
		{
			WLD_CORE_ERROR("Failed to create Vulkan Logical Device!");
			return;
		}

		//TODO 注意这里多个队列的情况，当前只创建了一个队列，所以直接获取即可
		// Get the graphics queue
		vkGetDeviceQueue(m_LogicalDevice, queueFamilyIndex, 0, &m_PresentQueue);

		VmaAllocatorCreateInfo allocatorInfo {};
		allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;
		allocatorInfo.physicalDevice = m_PhysicalDevice->GetPhysicalDevice();
		allocatorInfo.device = m_LogicalDevice;
		allocatorInfo.instance = m_PhysicalDevice->GetInstanceHandle();

		if (vmaCreateAllocator(&allocatorInfo, &m_Allocator) != VK_SUCCESS)
		{
			WLD_CORE_ERROR("Failed to create VMA Allocator!");
		}

		WLD_CORE_INFO("Vulkan Device and VMA initialized successfully.");
	}

	VulkanDevice::~VulkanDevice()
	{
		WLD_PROFILE_FUNCTION();

		if (m_Allocator)
		{
			vmaDestroyAllocator(m_Allocator);
		}

		if (m_LogicalDevice)
		{
			vkDeviceWaitIdle(m_LogicalDevice);
			vkDestroyDevice(m_LogicalDevice, nullptr);
		}
	}

}