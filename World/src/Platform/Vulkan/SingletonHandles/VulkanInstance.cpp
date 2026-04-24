#include "wldpch.h"
#include "VulkanInstance.h"


namespace World
{
	VulkanInstance::VulkanInstance()
	{

	}

	VulkanInstance::~VulkanInstance()
	{
		if (m_Handle)
		{
			vkDestroyInstance(m_Handle, nullptr);
		}
	}
}