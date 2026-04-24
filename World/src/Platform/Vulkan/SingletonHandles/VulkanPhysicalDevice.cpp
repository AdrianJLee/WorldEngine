#include "wldpch.h"
#include "VulkanPhysicalDevice.h"

namespace World
{
	VulkanPhysicalDevice::VulkanPhysicalDevice(const Ref<VulkanInstance>& instance)
		: m_Instance(instance)
	{
		PickPhysicalDevice(m_Instance->GetInstance());
	}

	void VulkanPhysicalDevice::PickPhysicalDevice(VkInstance instance)
	{
		WLD_PROFILE_FUNCTION();

		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
		WLD_CORE_ASSERT(deviceCount > 0, "Failed to find GPUs with Vulkan support!");

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
		for (const auto& device : devices)
		{
			QueueFamilyIndices queueFamilies = FindQueueFamilies(device);
			if (queueFamilies.IsComplete())
			{
				m_PhysicalDevice = device;
				m_QueueIndices = queueFamilies;
				break;
			}
		}

		WLD_CORE_ASSERT(m_PhysicalDevice != VK_NULL_HANDLE, "Failed to find a suitable GPU!");

		vkGetPhysicalDeviceProperties(m_PhysicalDevice, &m_Properties);
		vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &m_Features);

		WLD_CORE_INFO("Selected GPU: {0}", m_Properties.deviceName);
	}

	VulkanPhysicalDevice::QueueFamilyIndices VulkanPhysicalDevice::FindQueueFamilies(VkPhysicalDevice device)
	{
		QueueFamilyIndices indices;
		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

		for (uint32_t i = 0; i < queueFamilies.size(); i++)
		{
			// Check for graphics support
			if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				indices.GraphicsFamily = i;
			}
			// Check for presentation support
			if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
			{
				indices.ComputeFamily = i;
			}
			// Check for transfer support
			if (queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
			{
				indices.TransferFamily = i;
			}
			//VkBool32 presentSupport = false;
			//vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
			//if (presentSupport)
			//{
			//	indices.PresentFamily = i;
			//}
			if (indices.IsComplete())
			{
				break;
			}
		}
		return indices;
	}
}