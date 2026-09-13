#pragma once

#include "World/RHI/RhiSwapchain.h"

namespace World::Rhi::OpenGL
{
	class OpenGLSwapchain : public Swapchain
	{
	public:
		explicit OpenGLSwapchain(const SwapchainDesc& desc)
			: m_Desc(desc) {}

		AcquireResult AcquireNext(const Handle<Semaphore>& signalWhenReady = nullptr) override
		{
			// GL 默认帧缓冲由宿主窗口交换;无独立交换链图像。
			return AcquireResult{ 0, nullptr, false };
		}
		void Present(const Handle<Semaphore>& waitBeforePresent = nullptr) override
		{
			// 宿主通过 GLFW SwapBuffers 呈现。
		}
		Extent2D GetExtent() const override { return m_Extent; }
		void Resize(Extent2D extent) override { m_Extent = extent; }

	private:
		SwapchainDesc m_Desc;
		Extent2D m_Extent;
	};
}
