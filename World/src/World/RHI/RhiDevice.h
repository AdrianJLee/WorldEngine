#pragma once

#include "World/RHI/RhiBuffer.h"
#include "World/RHI/RhiCapabilities.h"
#include "World/RHI/RhiCommandBuffer.h"
#include "World/RHI/RhiCommandQueue.h"
#include "World/RHI/RhiDescriptorSet.h"
#include "World/RHI/RhiPipeline.h"
#include "World/RHI/RhiRenderPass.h"
#include "World/RHI/RhiSampler.h"
#include "World/RHI/RhiShader.h"
#include "World/RHI/RhiSwapchain.h"
#include "World/RHI/RhiSync.h"
#include "World/RHI/RhiTexture.h"

namespace World::Rhi
{
	struct DeviceDesc
	{
		bool EnableValidation = false;    // 开发期验证层(存在才启用)
		bool Headless = false;            // 无窗口(离屏/测试)
		std::string DebugName;
	};

	class WLD_API Device
	{
	public:
		virtual ~Device() = default;

		virtual const Capabilities& GetCapabilities() const = 0;
		virtual void WaitIdle() = 0;

		virtual Handle<CommandQueue> CreateQueue(const std::string& name = {}) = 0;
		virtual Handle<CommandBuffer> CreateCommandBuffer(const std::string& name = {}) = 0;
		virtual Handle<Swapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;
		virtual Handle<RenderPass> CreateRenderPass(const RenderPassDesc& desc) = 0;
		virtual Handle<Framebuffer> CreateFramebuffer(const FramebufferDesc& desc) = 0;
		virtual Handle<Pipeline> CreatePipeline(const PipelineDesc& desc) = 0;
		virtual Handle<Shader> CreateShader(const ShaderDesc& desc) = 0;
		virtual Handle<Buffer> CreateBuffer(const BufferDesc& desc) = 0;
		virtual Handle<Texture> CreateTexture(const TextureDesc& desc) = 0;
		virtual Handle<Sampler> CreateSampler(const SamplerDesc& desc) = 0;
		virtual Handle<DescriptorSetLayout> CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) = 0;
		virtual Handle<DescriptorSet> CreateDescriptorSet(const Handle<DescriptorSetLayout>& layout) = 0;
		virtual Handle<Fence> CreateFence(bool signaled = false) = 0;
		virtual Handle<Semaphore> CreateSemaphore() = 0;

		// 帧资源回收:渲染器完成 fence 后调用,释放该帧标记的临时资源。
		virtual void BeginFrame() = 0;
		virtual void EndFrame() = 0;
	};
}
