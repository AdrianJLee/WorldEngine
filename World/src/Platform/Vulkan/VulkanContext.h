#pragma once
#include "World/Renderer/GraphicsContext.h"
#include "SingletonHandles/VulkanInstance.h"
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
	};
}