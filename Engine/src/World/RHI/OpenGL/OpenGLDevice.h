#pragma once

#include "World/RHI/RhiDevice.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	// 上下文是否具备 GL 的 SPIR-V 摄入能力(GL 4.6 core / GL_ARB_gl_spirv)。
	// 由 OpenGLDevice 构造时按 `RhiCapabilities::SpirVShaderModules` 同一份探测结果写入 ——
	// OpenGLPipeline 建管线时没有设备句柄,只能从这里读;同一进程的所有 GL 上下文
	// 由同一驱动创建,该能力一致(T1 试点;多后端/多驱动场景由 T2 的正式描述符模型接管)。
	bool SupportsSpirVShaderModules();

	class OpenGLDevice : public Device
	{
	public:
		explicit OpenGLDevice(const DeviceDesc& desc);
		~OpenGLDevice() override;

		const Capabilities& GetCapabilities() const override { return m_Capabilities; }
		DeviceLimits GetLimits() const override { return m_Limits; }
		void WaitIdle() override { glFinish(); }

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

	private:
		DeviceDesc m_Desc;
		Capabilities m_Capabilities;
		DeviceLimits m_Limits;
	};
}
