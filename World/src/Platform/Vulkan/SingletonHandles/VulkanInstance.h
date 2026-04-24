#pragma once
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>

namespace World
{
	class VulkanInstance
	{
	public:
		VulkanInstance();
		~VulkanInstance();

		VulkanInstance(const VulkanInstance&) = delete;
		VulkanInstance& operator=(const VulkanInstance&) = delete;

	public:
		VkInstance GetInstance() const { return m_Instance; }

	private:
		// Helper function to check validation layer support
		bool CheckValidationLayerSupport();

		// Helper function to get required extensions
		std::vector<const char*> GetRequiredExtensions();

	private:
		VkInstance m_Instance = VK_NULL_HANDLE;

		const bool m_EnableValidationLayers = false;

		const std::vector<const char*> m_ValidationLayers = {
			"VK_LAYER_KHRONOS_validation"
		};
	};
}