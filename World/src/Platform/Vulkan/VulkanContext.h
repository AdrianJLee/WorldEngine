#pragma once
#include "World/Renderer/GraphicsContext.h"
#include "SingletonHandles/VulkanInstance.h"
#include "SingletonHandles/VulkanSurfaceKHR.h"
#include "SingletonHandles/VulkanPhysicalDevice.h"
#include "SingletonHandles/VulkanDevice.h"
#include "SingletonHandles/VulkanRenderPass.h"
#include "SingletonHandles/VulkanSwapchainKHR.h"

struct GLFWwindow;
namespace World
{
	class VulkanContext : public GraphicsContext
	{
	public:
		VulkanContext(GLFWwindow* windowHandle);

		virtual ~VulkanContext() = default;
		virtual void Init() override;
		virtual void SwapBuffers() override;
	private:
		GLFWwindow* m_WindowHandle;
		Ref<VulkanInstance> m_Instance;
		Ref<VulkanSurfaceKHR> m_Surface;
		Ref<VulkanPhysicalDevice> m_PhysicalDevice;
		Ref<VulkanDevice> m_Device;
		Ref<VulkanSwapchainKHR> m_Swapchain;

		VulkanRenderPassSpecification RenderPassSpec;
		Ref<VulkanRenderPass> m_RenderPass;
	};
}