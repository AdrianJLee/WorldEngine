#include "wldpch.h"
#include "VulkanDevice.h"

#include <set>
namespace World
{
	VulkanDevice::VulkanDevice(const Ref<VulkanPhysicalDevice>& physicalDevice)
		:m_PhysicalDevice(physicalDevice)
	{
		WLD_PROFILE_FUNCTION();

		auto indices = m_PhysicalDevice->GetQueueFamilyIndices();

		std::set<uint32_t> uniqueQueueFamilies = {
			indices.GraphicsFamily.value(),
			indices.PresentFamily.value()
		};
		if (indices.ComputeFamily.has_value())
			uniqueQueueFamilies.insert(indices.ComputeFamily.value());
		if (indices.TransferFamily.has_value())
			uniqueQueueFamilies.insert(indices.TransferFamily.value());

		// Create the device queue create info
		std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
		float queuePriority = 1.0f;
		for (uint32_t queueFamily : uniqueQueueFamilies)
		{
			VkDeviceQueueCreateInfo queueCreateInfo {};
			queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueCreateInfo.queueFamilyIndex = queueFamily;
			queueCreateInfo.queueCount = 1;
			queueCreateInfo.pQueuePriorities = &queuePriority;
			queueCreateInfos.push_back(queueCreateInfo);
		}

		// Enable features
		VkPhysicalDeviceFeatures deviceFeatures {};

		// Create the device create info
		VkDeviceCreateInfo createInfo {};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
		createInfo.pQueueCreateInfos = queueCreateInfos.data();
		std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
		createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
		createInfo.ppEnabledExtensionNames = deviceExtensions.data();
		createInfo.pEnabledFeatures = &deviceFeatures;

		if (vkCreateDevice(m_PhysicalDevice->GetPhysicalDevice(), &createInfo, nullptr, &m_LogicalDevice) != VK_SUCCESS)
		{
			WLD_CORE_ERROR("Failed to create Vulkan Logical Device!");
			return;
		}

		// Get the graphics queue
		vkGetDeviceQueue(m_LogicalDevice, indices.GraphicsFamily.value(), 0, &m_GraphicsQueue);
		vkGetDeviceQueue(m_LogicalDevice, indices.PresentFamily.value(), 0, &m_PresentQueue);
		if (indices.ComputeFamily.has_value())
			vkGetDeviceQueue(m_LogicalDevice, indices.ComputeFamily.value(), 0, &m_ComputeQueue);
		if (indices.TransferFamily.has_value())
			vkGetDeviceQueue(m_LogicalDevice, indices.TransferFamily.value(), 0, &m_TransferQueue);

		// Create VMA allocator
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