#pragma once

#include "World/RHI/RhiDevice.h"

#include <volk.h>

namespace World::Rhi::Vulkan
{
	// Vulkan 后端设备。W6-A:实例/物理设备/逻辑设备/队列与能力表;渲染资源类随后补齐。
	class VulkanDevice final : public Device
	{
	public:
		explicit VulkanDevice(const DeviceDesc& desc);
		~VulkanDevice() override;

		const Capabilities& GetCapabilities() const override { return m_Capabilities; }
		DeviceLimits GetLimits() const override { return m_Limits; }
		void WaitIdle() override;

		Handle<CommandQueue> CreateQueue(const std::string& name = {}) override;
		Handle<CommandBuffer> CreateCommandBuffer(const std::string& name = {}) override;
		Handle<Swapchain> CreateSwapchain(const SwapchainDesc& desc) override;
		Handle<RenderPass> CreateRenderPass(const RenderPassDesc& desc) override;
		Handle<Framebuffer> CreateFramebuffer(const FramebufferDesc& desc) override;
		Handle<Pipeline> CreatePipeline(const PipelineDesc& desc) override;
		Handle<Shader> CreateShader(const ShaderDesc& desc) override;
		Handle<Buffer> CreateBuffer(const BufferDesc& desc) override;
		Handle<Texture> CreateTexture(const TextureDesc& desc) override;
		Handle<Sampler> CreateSampler(const SamplerDesc& desc) override;
		Handle<DescriptorSetLayout> CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override;
		Handle<DescriptorSet> CreateDescriptorSet(const Handle<DescriptorSetLayout>& layout) override;
		Handle<Fence> CreateFence(bool signaled = false) override;
		Handle<Semaphore> CreateSemaphore(const SemaphoreCreateDesc& desc = {}) override;
		Handle<QueryPool> CreateQueryPool(QueryType type, uint32_t count) override;
		void BeginFrame() override {}
		void EndFrame() override {}

		static Handle<VulkanDevice> Create(const DeviceDesc& desc, std::string* error);

		VkDevice GetNativeDevice() const { return m_Device; }
		VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
		uint32_t GetGraphicsQueueFamily() const { return m_GraphicsFamily; }

	private:
		bool Initialize(const DeviceDesc& desc, std::string* error);

		DeviceDesc m_Desc;
		Capabilities m_Capabilities;
		DeviceLimits m_Limits;
		VkInstance m_Instance = VK_NULL_HANDLE;
		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_Device = VK_NULL_HANDLE;
		VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
		uint32_t m_GraphicsFamily = 0;
	};
}
