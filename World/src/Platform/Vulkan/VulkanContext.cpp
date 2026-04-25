#include "wldpch.h"
#define VMA_IMPLEMENTATION
#include "VulkanContext.h"

#include <GLFW/glfw3.h>
namespace World
{
	VulkanContext::VulkanContext(GLFWwindow* windowHandle)
		: m_WindowHandle(windowHandle)
	{
		WLD_CORE_ASSERT(windowHandle, "Window handle is null!");
	}
	void VulkanContext::Init()
	{
		WLD_PROFILE_FUNCTION();

		int width, height;
		glfwGetFramebufferSize(m_WindowHandle, &width, &height);

		m_Instance = CreateRef<VulkanInstance>();
		m_Surface = CreateRef<VulkanSurfaceKHR>(m_Instance, m_WindowHandle);
		m_PhysicalDevice = CreateRef<VulkanPhysicalDevice>(m_Instance, m_Surface);
		m_Device = CreateRef<VulkanDevice>(m_PhysicalDevice);
		m_Swapchain = CreateRef<VulkanSwapchainKHR>(m_Device, m_Surface, width, height);

		RenderPassSpec.Attachments = {
			RenderPassAttachment{
				m_Swapchain->GetSwapChainImageFormat(),
				AttachmentLoadOp::Clear,
				AttachmentStoreOp::Store,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
			}
		};
		m_RenderPass = CreateRef<VulkanRenderPass>(m_Device, m_Swapchain->GetSwapChainImageFormat());
	}
	void VulkanContext::SwapBuffers()
	{
		// Vulkan does not use traditional buffer swapping like OpenGL.
		// Instead, it uses a more complex presentation model involving command buffers and swapchains.
	}
}