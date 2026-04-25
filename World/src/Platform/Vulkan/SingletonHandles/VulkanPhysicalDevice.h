#pragma once
#include "VulkanInstance.h"
#include "VulkanSurfaceKHR.h"

#include <vk_mem_alloc.h>
#include <optional>

class VulkanInstance;
namespace World
{
	class VulkanPhysicalDevice
	{
	public:
		VulkanPhysicalDevice(const Ref<VulkanInstance>& instance, const Ref<VulkanSurfaceKHR>& surface);
		~VulkanPhysicalDevice() = default;

	public:
		// Struct to hold indices of queue families
		struct QueueFamilyIndices
		{
			std::optional<uint32_t> GraphicsFamily;
			std::optional<uint32_t> PresentFamily;
			std::optional<uint32_t> ComputeFamily;
			std::optional<uint32_t> TransferFamily;

			bool IsComplete() const
			{
				return GraphicsFamily.has_value() && PresentFamily.has_value();
			}
		};

	public:

		VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }

		VmaAllocator GetAllocator() const { return m_Allocator; }

		VkPhysicalDeviceProperties GetProperties() const { return m_Properties; }
		VkPhysicalDeviceFeatures GetFeatures() const { return m_Features; }
		VkInstance GetInstanceHandle() const { return m_Instance->GetInstance(); }

		QueueFamilyIndices GetQueueFamilyIndices() const { return m_QueueIndices; }
	private:
		void PickPhysicalDevice(VkInstance instance);
		QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface);
	private:
		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VmaAllocator m_Allocator = VK_NULL_HANDLE;
		QueueFamilyIndices m_QueueIndices {};

		VkPhysicalDeviceProperties m_Properties {};
		VkPhysicalDeviceFeatures m_Features {};

		Ref<VulkanInstance> m_Instance;
		Ref<VulkanSurfaceKHR> m_Surface;
	};
}