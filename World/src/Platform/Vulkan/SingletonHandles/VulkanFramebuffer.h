#pragma once
#include "World/Renderer/Framebuffer.h"
#include "Platform/Vulkan/SingletonHandles/VulkanDevice.h"
#include "Platform/Vulkan/SingletonHandles/VulkanRenderPass.h"
#include "Platform/Vulkan/SingletonHandles/VulkanSwapchainKHR.h"

namespace World
{
	class VulkanFramebuffer :public Framebuffer
	{
	public:
		VulkanFramebuffer(Ref<VulkanDevice> device, Ref<VulkanRenderPass> renderPass, Ref<VulkanSwapchainKHR> swapchain);

		~VulkanFramebuffer();

		VulkanFramebuffer(const VulkanFramebuffer&) = delete;
		VulkanFramebuffer& operator=(const VulkanFramebuffer&) = delete;

		VkFramebuffer GetFramebuffer(uint32_t index) const { return m_Framebuffers[index]; }
		size_t GetBufferCount() const { return m_Framebuffers.size(); }


		#pragma region  Framebuffer Override
	public:
		void Bind() override { WLD_CORE_WARN("VulkanFramebuffer::Bind() is not implemented yet"); }
		void Unbind() override { WLD_CORE_WARN("VulkanFramebuffer::Unbind() is not implemented yet"); }
		#pragma endregion

	private:
		std::vector<VkFramebuffer> m_Framebuffers;

		Ref<VulkanDevice> m_Device;

	};
}