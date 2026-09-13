#include "wldpch.h"
#include "OpenGLDevice.h"

#include "OpenGLBuffer.h"
#include "OpenGLCommandBuffer.h"
#include "OpenGLCommandQueue.h"
#include "OpenGLDescriptorSet.h"
#include "OpenGLPipeline.h"
#include "OpenGLQueryPool.h"
#include "OpenGLRenderPass.h"
#include "OpenGLSampler.h"
#include "OpenGLShader.h"
#include "OpenGLSwapchain.h"
#include "OpenGLSync.h"
#include "OpenGLTexture.h"

namespace World::Rhi::OpenGL
{
	namespace
	{
		std::pair<uint32_t, uint32_t> ParseVersion(const char* version)
		{
			uint32_t major = 4, minor = 6;
			if (version)
				std::sscanf(version, "%u.%u", &major, &minor);
			return { major, minor };
		}
	}

	OpenGLDevice::OpenGLDevice(const DeviceDesc& desc)
		: m_Desc(desc)
	{
		WLD_CORE_ASSERT(glGetString(GL_VERSION) != nullptr, "OpenGL context is not current");

		const auto version = ParseVersion(reinterpret_cast<const char*>(glGetString(GL_VERSION)));
		m_Capabilities.BackendName = "OpenGL";
		m_Capabilities.RendererName = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
		m_Capabilities.ApiMajor = version.first;
		m_Capabilities.ApiMinor = version.second;
		m_Capabilities.Compute = true;
		m_Capabilities.DrawIndirect = true;
		m_Capabilities.MultiDrawIndirect = true;
		m_Capabilities.TimestampQueries = true;
		m_Capabilities.TextureCompressionBC = true;
		m_Capabilities.AnisotropicFiltering = true;

		GLint value = 0;
		glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &value);
		m_Capabilities.MaxColorAttachments = static_cast<uint32_t>(value);
		glGetIntegerv(GL_MAX_SAMPLES, &value);
		m_Capabilities.MaxSampleCount = value >= 8 ? 8u : static_cast<uint32_t>(value);
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
		m_Capabilities.MaxTextureSize = static_cast<uint32_t>(value);
		glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &value);
		m_Capabilities.MaxImageArrayLayers = static_cast<uint32_t>(value);
		glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &value);
		m_Capabilities.MaxUniformBufferSize = static_cast<uint32_t>(value);
		glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &value);
		m_Capabilities.MaxStorageBufferSize = static_cast<uint32_t>(value);
		glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &value);
		m_Limits.MinUniformBufferOffsetAlignment = static_cast<uint64_t>(value);
		m_Limits.NonCoherentAtomSize = 1;
		m_Limits.TimestampPeriod = 1.0f;

		// 非 GL 功能位保持默认 false,前端按能力表降级。
		WLD_CORE_INFO("OpenGL device created: {0} ({1}.{2})", m_Capabilities.RendererName,
			m_Capabilities.ApiMajor, m_Capabilities.ApiMinor);
	}

	OpenGLDevice::~OpenGLDevice()
	{
		WaitIdle();
	}

	Handle<CommandQueue> OpenGLDevice::CreateQueue(const std::string& name)
	{
		return CreateRef<OpenGLCommandQueue>(name);
	}

	Handle<CommandBuffer> OpenGLDevice::CreateCommandBuffer(const std::string& /*name*/)
	{
		return CreateRef<OpenGLCommandBuffer>();
	}

	Handle<Swapchain> OpenGLDevice::CreateSwapchain(const SwapchainDesc& desc)
	{
		return CreateRef<OpenGLSwapchain>(desc);
	}

	Handle<RenderPass> OpenGLDevice::CreateRenderPass(const RenderPassDesc& desc)
	{
		return CreateRef<OpenGLRenderPass>(desc);
	}

	Handle<Framebuffer> OpenGLDevice::CreateFramebuffer(const FramebufferDesc& desc)
	{
		return CreateRef<OpenGLFramebuffer>(desc);
	}

	Handle<Pipeline> OpenGLDevice::CreatePipeline(const PipelineDesc& desc)
	{
		return CreateRef<OpenGLPipeline>(desc);
	}

	Handle<Shader> OpenGLDevice::CreateShader(const ShaderDesc& desc)
	{
		return CreateRef<OpenGLShader>(desc);
	}

	Handle<Buffer> OpenGLDevice::CreateBuffer(const BufferDesc& desc)
	{
		return CreateRef<OpenGLBuffer>(desc);
	}

	Handle<Texture> OpenGLDevice::CreateTexture(const TextureDesc& desc)
	{
		return CreateRef<OpenGLTexture>(desc);
	}

	Handle<Sampler> OpenGLDevice::CreateSampler(const SamplerDesc& desc)
	{
		return CreateRef<OpenGLSampler>(desc);
	}

	Handle<DescriptorSetLayout> OpenGLDevice::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc)
	{
		return CreateRef<OpenGLDescriptorSetLayout>(desc);
	}

	Handle<DescriptorSet> OpenGLDevice::CreateDescriptorSet(const Handle<DescriptorSetLayout>& layout)
	{
		return CreateRef<OpenGLDescriptorSet>(layout);
	}

	Handle<Fence> OpenGLDevice::CreateFence(bool /*signaled*/)
	{
		return CreateRef<OpenGLFence>();
	}

	Handle<Semaphore> OpenGLDevice::CreateSemaphore(const SemaphoreCreateDesc& desc)
	{
		return CreateRef<OpenGLSemaphore>(desc);
	}

	Handle<QueryPool> OpenGLDevice::CreateQueryPool(QueryType type, uint32_t count)
	{
		return CreateRef<OpenGLQueryPool>(type, count);
	}
}
