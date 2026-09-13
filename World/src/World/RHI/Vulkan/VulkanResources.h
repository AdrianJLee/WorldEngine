#pragma once

#include "World/RHI/RhiBuffer.h"
#include "World/RHI/RhiDescriptorSet.h"
#include "World/RHI/RhiPipeline.h"
#include "World/RHI/RhiRenderPass.h"
#include "World/RHI/RhiSampler.h"
#include "World/RHI/RhiShader.h"
#include "World/RHI/RhiSync.h"
#include "World/RHI/RhiTexture.h"

#include <volk.h>

#include <functional>

namespace World::Rhi::Vulkan
{
	class VulkanDevice;

	class VulkanBuffer final : public Buffer
	{
	public:
		VulkanBuffer(VulkanDevice& device, const BufferDesc& desc);
		~VulkanBuffer() override;
		const BufferDesc& GetDesc() const override { return m_Desc; }
		void* Map(uint64_t offset = 0, uint64_t size = 0) override;
		void Unmap() override;
		void SetData(const void* data, uint64_t size, uint64_t offset = 0) override;
		VkBuffer GetBuffer() const { return m_Buffer; }
	private:
		VulkanDevice& m_Device;
		BufferDesc m_Desc;
		VkBuffer m_Buffer = VK_NULL_HANDLE;
		VkDeviceMemory m_Memory = VK_NULL_HANDLE;
		void* m_Mapped = nullptr;
	};

	class VulkanTexture final : public Texture
	{
	public:
		VulkanTexture(VulkanDevice& device, const TextureDesc& desc);
		~VulkanTexture() override;
		const TextureDesc& GetDesc() const override { return m_Desc; }
		void SetData(const void* data, uint64_t size, uint32_t layer = 0, uint32_t mip = 0) override;
		VkImage GetImage() const { return m_Image; }
		VkImageView GetView() const { return m_View; }
	private:
		void Transition(VkImageLayout oldLayout, VkImageLayout newLayout);
		VulkanDevice& m_Device;
		TextureDesc m_Desc;
		VkImage m_Image = VK_NULL_HANDLE;
		VkDeviceMemory m_Memory = VK_NULL_HANDLE;
		VkImageView m_View = VK_NULL_HANDLE;
		VkImageLayout m_Layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	class VulkanSampler final : public Sampler
	{
	public:
		VulkanSampler(VulkanDevice& device, const SamplerDesc& desc);
		~VulkanSampler() override;
		const SamplerDesc& GetDesc() const override { return m_Desc; }
		VkSampler GetSampler() const { return m_Sampler; }
	private:
		VulkanDevice& m_Device;
		SamplerDesc m_Desc;
		VkSampler m_Sampler = VK_NULL_HANDLE;
	};

	class VulkanShader final : public Shader
	{
	public:
		VulkanShader(VulkanDevice& device, const ShaderDesc& desc);
		~VulkanShader() override;
		const ShaderDesc& GetDesc() const override { return m_Desc; }
		const std::vector<VkShaderModule>& GetModules() const { return m_Modules; }
	private:
		VulkanDevice& m_Device;
		ShaderDesc m_Desc;
		std::vector<VkShaderModule> m_Modules;
	};

	class VulkanRenderPass final : public RenderPass
	{
	public:
		VulkanRenderPass(VulkanDevice& device, const RenderPassDesc& desc);
		~VulkanRenderPass() override;
		const RenderPassDesc& GetDesc() const override { return m_Desc; }
		VkRenderPass GetRenderPass() const { return m_RenderPass; }
	private:
		VulkanDevice& m_Device;
		RenderPassDesc m_Desc;
		VkRenderPass m_RenderPass = VK_NULL_HANDLE;
	};

	class VulkanFramebuffer final : public Framebuffer
	{
	public:
		VulkanFramebuffer(VulkanDevice& device, const FramebufferDesc& desc);
		~VulkanFramebuffer() override;
		const FramebufferDesc& GetDesc() const override { return m_Desc; }
		VkFramebuffer GetFramebuffer() const { return m_Framebuffer; }
	private:
		VulkanDevice& m_Device;
		FramebufferDesc m_Desc;
		VkFramebuffer m_Framebuffer = VK_NULL_HANDLE;
	};

	class VulkanDescriptorSetLayout final : public DescriptorSetLayout
	{
	public:
		VulkanDescriptorSetLayout(VulkanDevice& device, const DescriptorSetLayoutDesc& desc);
		~VulkanDescriptorSetLayout() override;
		const DescriptorSetLayoutDesc& GetDesc() const override { return m_Desc; }
		VkDescriptorSetLayout GetLayout() const { return m_Layout; }
	private:
		VulkanDevice& m_Device;
		DescriptorSetLayoutDesc m_Desc;
		VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
	};

	class VulkanDescriptorSet final : public DescriptorSet
	{
	public:
		VulkanDescriptorSet(VulkanDevice& device, const Handle<VulkanDescriptorSetLayout>& layout);
		~VulkanDescriptorSet() override;
		void Update(const std::vector<DescriptorWrite>& writes) override;
		VkDescriptorSet GetSet() const { return m_Set; }
	private:
		VulkanDevice& m_Device;
		Handle<VulkanDescriptorSetLayout> m_Layout;
		VkDescriptorSet m_Set = VK_NULL_HANDLE;
		VkDescriptorPool m_Pool = VK_NULL_HANDLE;
	};

	class VulkanFence final : public Fence
	{
	public:
		VulkanFence(VulkanDevice& device, bool signaled);
		~VulkanFence() override;
		void Wait(uint64_t timeoutNs = UINT64_MAX) override;
		bool IsSignaled() const override;
		void Reset() override;
		VkFence GetFence() const { return m_Fence; }
	private:
		VulkanDevice& m_Device;
		VkFence m_Fence = VK_NULL_HANDLE;
	};

	class VulkanSemaphore final : public Semaphore
	{
	public:
		VulkanSemaphore(VulkanDevice& device, const SemaphoreCreateDesc& desc);
		~VulkanSemaphore() override;
		void Signal(uint64_t value = 0) override;
		void Wait(uint64_t value = 0) override;
		bool IsTimeline() const override { return m_Desc.Timeline; }
		VkSemaphore GetSemaphore() const { return m_Semaphore; }
	private:
		VulkanDevice& m_Device;
		SemaphoreCreateDesc m_Desc;
		VkSemaphore m_Semaphore = VK_NULL_HANDLE;
	};

	class VulkanQueryPool final : public QueryPool
	{
	public:
		VulkanQueryPool(VulkanDevice& device, QueryType type, uint32_t count);
		~VulkanQueryPool() override;
		QueryType GetType() const override { return m_Type; }
		uint32_t GetCount() const override { return m_Count; }
		VkQueryPool GetPool() const { return m_Pool; }
	private:
		VulkanDevice& m_Device;
		QueryType m_Type;
		uint32_t m_Count = 0;
		VkQueryPool m_Pool = VK_NULL_HANDLE;
	};

	// 一次性上传/转换:创建临时命令缓冲,提交到图形队列并等待。
	void ExecuteOneShot(VulkanDevice& device, const std::function<void(VkCommandBuffer)>& record);
	VkFormat ToVkFormat(Format format);
	VkImageUsageFlags ToVkImageUsage(uint32_t usage);
	VkBufferUsageFlags ToVkBufferUsage(uint32_t usage);
}
