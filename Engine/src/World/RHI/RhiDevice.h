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
		uint32_t AdapterIndex = 0;        // 多 GPU 时选择物理设备
		std::string DebugName;
	};

	struct DeviceLimits
	{
		uint64_t MinUniformBufferOffsetAlignment = 64;
		uint64_t NonCoherentAtomSize = 1;
		float TimestampPeriod = 0.0f;     // 0 = 不支持时间戳
	};

	class WLD_API Device
	{
	public:
		virtual ~Device() = default;

		virtual const Capabilities& GetCapabilities() const = 0;
		virtual DeviceLimits GetLimits() const = 0;
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
		virtual Handle<Semaphore> CreateSemaphore(const SemaphoreCreateDesc& desc = {}) = 0;
		virtual Handle<QueryPool> CreateQueryPool(QueryType type, uint32_t count) = 0;
		// 时间戳查询的时基:一次 tick 等于多少纳秒(Vulkan = limits.timestampPeriod;
		// OpenGL 的 glQueryCounter 直接把纳秒写进查询对象,因此默认 1.0)。
		// D8b:GPU 耗时(GpuTimer)用它把 tick 差换算成毫秒。
		virtual double GetTimestampPeriodNanoseconds() const { return 1.0; }

		// 帧资源回收:渲染器完成 fence 后调用,释放该帧标记的临时资源。
		virtual void BeginFrame() = 0;
		virtual void EndFrame() = 0;
	};
}
