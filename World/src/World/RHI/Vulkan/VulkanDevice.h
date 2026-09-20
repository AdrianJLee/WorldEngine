#pragma once

#include "World/RHI/RhiDevice.h"

#include <volk.h>

#include <memory>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace World::Rhi::Vulkan
{
	class VulkanUploadRing;

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
		// D8b:VkPhysicalDeviceLimits::timestampPeriod = 一次 tick 的纳秒数。
		double GetTimestampPeriodNanoseconds() const override { return m_TimestampPeriodNs; }
		void BeginFrame() override {}
		void EndFrame() override {}

		static Handle<VulkanDevice> Create(const DeviceDesc& desc, std::string* error);

		VkDevice GetNativeDevice() const { return m_Device; }
		VkInstance GetInstance() const { return m_Instance; }
		VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
		VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
		uint32_t GetGraphicsQueueFamily() const { return m_GraphicsFamily; }
		VkCommandPool GetCommandPool() const { return m_CommandPool; }
		// 每线程命令池:命令缓冲的分配/释放是线程相关的(Vulkan 规范),
		// 多线程录制必须用各自线程的池。首次调用时按当前线程创建。
		VkCommandPool GetThreadCommandPool();
		void SetLastPipelineLayout(VkPipelineLayout layout) { m_LastPipelineLayout = layout; }
		VkPipelineLayout GetLastPipelineLayout() const { return m_LastPipelineLayout; }
		// B2:常驻 staging 环形缓冲(异步上传)。设备初始化后有效,渲染线程使用。
		VulkanUploadRing& GetUploadRing() { return *m_UploadRing; }
		// 一次性录制+提交(内部槽位环):wait=false 时 fire-and-forget,
		// 槽位都在飞时只等待最老的一个,替代"每次提交都 vkQueueWaitIdle"。
		// waitSemaphore/signalSemaphore 用于需要与渲染提交排序的场景(如呈现前的布局转换)。
		bool SubmitOneShot(const std::function<void(VkCommandBuffer)>& record, bool wait,
			VkSemaphore waitSemaphore = VK_NULL_HANDLE, VkSemaphore signalSemaphore = VK_NULL_HANDLE,
			const char* label = nullptr);

		// 设备已丢失(vkQueueSubmit 返回 VK_ERROR_DEVICE_LOST):
		// GPU 已被 TDR/reset 重置,后续任何提交都只会连锁报错。宿主应按"设备失效"处理。
		bool IsDeviceLost() const { return m_DeviceLost; }
		// P4-UX5:设备丢失取证(VK_EXT_device_fault)。故障发生时把地址/引擎/厂商数据写进日志,
		// 并把厂商二进制落到文件里 —— 没有验证层 VUID 的 GPU 引擎错误(如 nvlddmkm Event 153)
		// 只有这里能给出线索。
		void LogDeviceFault(const char* where);

	private:
		bool Initialize(const DeviceDesc& desc, std::string* error);

		DeviceDesc m_Desc;
		Capabilities m_Capabilities;
		DeviceLimits m_Limits;
		double m_TimestampPeriodNs = 1.0;   // D8b:limits.timestampPeriod(ns / tick)
		VkInstance m_Instance = VK_NULL_HANDLE;
		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_Device = VK_NULL_HANDLE;
		VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
		uint32_t m_GraphicsFamily = 0;
		VkCommandPool m_CommandPool = VK_NULL_HANDLE;
		std::mutex m_PoolMutex;                                  // 保护 m_ThreadPools 的创建
		// 每线程一个池(含主线程的第一个),按 thread::id 归属**设备自身**:
		// 池与设备同生共死,不允许存在跨设备的线程局部缓存(地址复用会伪造"同一设备")。
		std::unordered_map<std::thread::id, VkCommandPool> m_ThreadPools;
		VkPipelineLayout m_LastPipelineLayout = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
		bool m_DeviceLost = false;
		bool m_DeviceFaultSupported = false;
		std::unique_ptr<VulkanUploadRing> m_UploadRing;
		struct OneShotSlot
		{
			VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
			VkFence Fence = VK_NULL_HANDLE;
		};
		VkCommandPool m_TransientPool = VK_NULL_HANDLE;
		std::vector<OneShotSlot> m_OneShotSlots;
		size_t m_NextOneShot = 0;
	};
}
