#include "wldpch.h"
#include "VulkanInstance.h"


namespace World
{
	VulkanInstance::VulkanInstance()
	{
		WLD_PROFILE_FUNCTION();

		// If validation layers are enabled, check if they are supported
		if (m_EnableValidationLayers && !CheckValidationLayerSupport())
		{
			WLD_CORE_ERROR("Vulkan validation layers requested, but not available!");
		}

		VkApplicationInfo appInfo {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pApplicationName = "World Engine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "World Engine";
		appInfo.apiVersion = VK_API_VERSION_1_4;

		VkInstanceCreateInfo createInfo {};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &appInfo;

		auto extensions = GetRequiredExtensions();
		createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		createInfo.ppEnabledExtensionNames = extensions.data();

		if (m_EnableValidationLayers)
		{
			createInfo.enabledLayerCount = static_cast<uint32_t>(m_ValidationLayers.size());
			createInfo.ppEnabledLayerNames = m_ValidationLayers.data();
		}
		else
		{
			createInfo.enabledLayerCount = 0;
		}


		if (vkCreateInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to create Vulkan instance!");
		}

		WLD_CORE_INFO("Vulkan Instance created successfully.");
	}

	VulkanInstance::~VulkanInstance()
	{
		WLD_PROFILE_FUNCTION();

		if (m_Instance)
		{
			vkDestroyInstance(m_Instance, nullptr);
			WLD_CORE_INFO("Vulkan Instance destroyed.");
		}
	}

	bool VulkanInstance::CheckValidationLayerSupport()
	{
		WLD_PROFILE_FUNCTION();

		uint32_t layerCount;
		vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

		std::vector<VkLayerProperties> availableLayers(layerCount);
		vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

		for (const char* layerName : m_ValidationLayers)
		{
			bool layerFound = false;
			for (const auto& layerProperties : availableLayers)
			{
				if (strcmp(layerName, layerProperties.layerName) == 0)
				{
					layerFound = true;
					break;
				}
			}
			if (!layerFound)
			{
				return false;
			}
		}
		return true;
	}

	std::vector<const char*> VulkanInstance::GetRequiredExtensions()
	{
		WLD_PROFILE_FUNCTION();

		uint32_t glfwExtensionCount = 0;
		const char** glfwExtensions;
		glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

		std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

		return extensions;
	}
}