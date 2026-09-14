#include "wldpch.h"
#include "Renderer.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Application.h"
#include "World/Renderer/RenderCommand.h"
#include "World/Renderer/VertexArray.h"
#include "World/Renderer/Shader.h"
#include "World/Renderer/Renderer2D.h"
#include "World/RHI/Vulkan/VulkanSwapchain.h"
#include "World/RHI/Vulkan/VulkanResources.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <fstream>

namespace World
{
	Renderer::SceneData* Renderer::m_SceneData = new Renderer::SceneData;
	Rhi::Handle<Rhi::Device> Renderer::m_Device = nullptr;
	static Rhi::Handle<Rhi::DescriptorSetLayout> s_GlobalDescriptorSetLayout;
	static std::string s_BackendName = "opengl";
	static Rhi::Handle<Rhi::CommandQueue> s_PresentQueue;
	static Rhi::Handle<Rhi::Swapchain> s_Swapchain;
	static Rhi::Handle<Rhi::RenderPass> s_PresentPass;
	static std::vector<Rhi::Handle<Rhi::Framebuffer>> s_PresentFramebuffers;
	static Rhi::Handle<Rhi::Texture> s_PresentImage;
	static Rhi::Handle<Rhi::Framebuffer> s_PresentFramebuffer;
	static Rhi::Handle<Rhi::Semaphore> s_ImageReady, s_RenderDone;
	static bool s_SwapchainDirty = true;
	static uint32_t s_CurrentImageIndex = 0;

	void Renderer::Init()
	{
		// 按 project.we.yaml 的 renderer 选择后端;Vulkan 不可用时降级 OpenGL。
		std::string requested = "opengl";
		std::filesystem::path manifestPath;
		if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			std::string manifestError;
			World::Asset::ProjectManifest manifest;
			if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &manifestError))
				requested = manifest.Renderer;
		}
		Init(requested);
	}

	void Renderer::Init(const std::string& backend)
	{
		if (m_Device)
			Shutdown();

		const Rhi::Backend requestedBackend =
			backend == "vulkan" ? Rhi::Backend::Vulkan : Rhi::Backend::OpenGL;
		std::string error;
		m_Device = Rhi::CreateDevice(requestedBackend, {}, &error);
		if (!m_Device)
		{
			WLD_CORE_WARN("Requested backend '{0}' unavailable ({1}); falling back to OpenGL",
				backend, error);
			error.clear();
			m_Device = Rhi::CreateDevice(Rhi::Backend::OpenGL, {}, &error);
		}
		WLD_CORE_ASSERT(m_Device, "Failed to create RHI device: {0}", error);

		// ShaderCompiler 依据 API 决定返回 SPIR-V 或 GLSL。
		RendererAPI::SetAPI(requestedBackend == Rhi::Backend::Vulkan && m_Device->GetCapabilities().BackendName == "Vulkan"
			? RendererAPI::API::Vulkan : RendererAPI::API::OpenGL);
		s_BackendName = RendererAPI::GetAPI() == RendererAPI::API::Vulkan ? "vulkan" : "opengl";

		RenderCommand::Init();
		Renderer2D::Init();
		WLD_CORE_INFO("RHI backend initialized: {0}", s_BackendName);
	}

	void Renderer::Shutdown()
	{
		WLD_PROFILE_FUNCTION();
		Renderer2D::Shutdown();
		s_GlobalDescriptorSetLayout = nullptr;
		s_PresentFramebuffer = nullptr;
		s_PresentImage = nullptr;
		s_PresentFramebuffers.clear();
		s_PresentPass = nullptr;
		s_Swapchain = nullptr;
		s_RenderDone = nullptr;
		s_ImageReady = nullptr;
		s_PresentQueue = nullptr;
		s_SwapchainDirty = true;
		m_Device = nullptr;
		s_BackendName = "opengl";
	}

	bool Renderer::SetRequestedRenderer(const std::string& name)
	{
		const std::string requested = name == "vulkan" ? "vulkan" : "opengl";
		return requested != s_BackendName;
	}

	std::string Renderer::GetBackendName()
	{
		return s_BackendName;
	}

	Rhi::Handle<Rhi::DescriptorSetLayout> Renderer::GetGlobalDescriptorSetLayout()
	{
		if (!s_GlobalDescriptorSetLayout && m_Device)
		{
			Rhi::DescriptorSetLayoutDesc desc;
			desc.Bindings.push_back({ 0, Rhi::DescriptorType::UniformBuffer,
				Rhi::ShaderStageFlag(Rhi::ShaderStage::Vertex), 1 });
			s_GlobalDescriptorSetLayout = m_Device->CreateDescriptorSetLayout(desc);
		}
		return s_GlobalDescriptorSetLayout;
	}

	void Renderer::OnWindowResize(uint32_t width, uint32_t height)
	{
		RenderCommand::SetViewport(0, 0, width, height);
		s_SwapchainDirty = true;
	}

	Rhi::Handle<Rhi::RenderPass> Renderer::GetPresentRenderPass()
	{
		if (!s_PresentPass && m_Device)
		{
			Rhi::RenderPassDesc desc;
			Rhi::RenderPassAttachment color;
			const bool vulkan = s_BackendName == "vulkan";
			color.Format = vulkan ? Rhi::Format::B8G8R8A8_UNORM : Rhi::Format::R8G8B8A8_UNORM;
			color.Samples = Rhi::SampleCount::Count1;
			color.Load = vulkan ? Rhi::LoadOp::Clear : Rhi::LoadOp::Load;
			color.Store = Rhi::StoreOp::Store;
			color.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
			color.FinalLayout = vulkan ? Rhi::AttachmentLayout::Present : Rhi::AttachmentLayout::ColorAttachment;
			desc.Attachments = { color };
			Rhi::SubpassDesc subpass;
			subpass.ColorAttachments = { { 0, Rhi::AttachmentLayout::ColorAttachment } };
			desc.Subpasses = { subpass };
			desc.DebugName = "Present";
			s_PresentPass = m_Device->CreateRenderPass(desc);
		}
		return s_PresentPass;
	}

	void Renderer::BeginFramePresent()
	{
		if (!m_Device || s_BackendName != "vulkan")
			return;
		if (s_SwapchainDirty || !s_Swapchain)
		{
			s_SwapchainDirty = false;
			s_PresentFramebuffers.clear();
			s_PresentFramebuffer = nullptr;
			s_Swapchain = nullptr;
			s_ImageReady = nullptr;
			s_RenderDone = nullptr;
			if (Application::HasInstance())
			{
				Rhi::SwapchainDesc swapDesc;
				swapDesc.NativeWindow = Application::Get().GetWindow().GetNativeWindow();
				swapDesc.Format = Rhi::Format::B8G8R8A8_UNORM;
				swapDesc.Present = Rhi::PresentMode::Fifo;
				swapDesc.DebugName = "MainSwapchain";
				s_Swapchain = m_Device->CreateSwapchain(swapDesc);
				s_PresentQueue = m_Device->CreateQueue("Present");
				s_ImageReady = m_Device->CreateSemaphore();
				s_RenderDone = m_Device->CreateSemaphore();
			}
		}
		if (!s_Swapchain)
			return;
		// 帧循环用队列 WaitIdle 串行化,但 acquire 必须携带信号量或栅栏(VUID 01780),
		// 该信号由 UI 提交等待消费,UI 提交再发出 s_RenderDone 供 Present 等待。
		const Rhi::AcquireResult acquired = s_Swapchain->AcquireNext(s_ImageReady);
		if (acquired.OutOfDate)
		{
			s_SwapchainDirty = true;
			return;
		}
		s_PresentImage = acquired.Image;
		s_CurrentImageIndex = acquired.ImageIndex;
		const auto vulkanSwapchain = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanSwapchain>(s_Swapchain);
		if (vulkanSwapchain && s_PresentImage)
			vulkanSwapchain->TransitionImage(s_CurrentImageIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		if (s_PresentImage)
		{
			if (s_PresentFramebuffers.size() <= s_CurrentImageIndex)
				s_PresentFramebuffers.resize(s_CurrentImageIndex + 1);
			if (!s_PresentFramebuffers[s_CurrentImageIndex])
			{
				Rhi::FramebufferDesc framebufferDesc;
				framebufferDesc.RenderPass = GetPresentRenderPass();
				framebufferDesc.Extent = s_Swapchain->GetExtent();
				framebufferDesc.Attachments = { s_PresentImage };
				framebufferDesc.DebugName = "Present";
				s_PresentFramebuffers[s_CurrentImageIndex] = m_Device->CreateFramebuffer(framebufferDesc);
			}
			s_PresentFramebuffer = s_PresentFramebuffers[s_CurrentImageIndex];
		}
	}

	void Renderer::EndFramePresent()
	{
		if (m_Device && s_Swapchain && s_RenderDone)
		{
			const auto vulkanSwapchain = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanSwapchain>(s_Swapchain);
			if (vulkanSwapchain && s_PresentImage)
				vulkanSwapchain->TransitionImage(s_CurrentImageIndex, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			s_Swapchain->Present(s_RenderDone);
			if (s_PresentQueue)
				s_PresentQueue->WaitIdle();
		}
		s_PresentImage = nullptr;
		s_PresentFramebuffer = nullptr;
	}

	Rhi::Handle<Rhi::Framebuffer> Renderer::GetPresentFramebuffer()
	{
		return s_PresentFramebuffer;
	}

	Rhi::Handle<Rhi::Texture> Renderer::GetPresentTarget()
	{
		return s_PresentImage;
	}

	void Renderer::SubmitUi(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer)
	{
		if (!m_Device || !commandBuffer)
			return;
		if (s_BackendName != "vulkan")
			return; // OpenGL 立即模式
		if (!s_PresentQueue || !s_PresentImage)
			return;
		Rhi::SubmitInfo submit;
		submit.CommandBuffers = { commandBuffer };
		if (s_ImageReady)
			submit.WaitSemaphores = { s_ImageReady };
		if (s_RenderDone)
			submit.SignalSemaphores = { s_RenderDone };
		s_PresentQueue->Submit(submit);
		// 帧循环串行化:提交后等待队列空闲,下一帧才可安全复用命令缓冲与资源。
		s_PresentQueue->WaitIdle();
	}

	void Renderer::SubmitScene(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer,
		const Rhi::Handle<Rhi::Texture>& colorTexture)
	{
		if (!m_Device || !commandBuffer)
			return;
		if (s_BackendName != "vulkan")
			return; // OpenGL 立即模式
		if (!s_PresentQueue)
			return;
		Rhi::SubmitInfo submit;
		submit.CommandBuffers = { commandBuffer };
		s_PresentQueue->Submit(submit);
		s_PresentQueue->WaitIdle();
		if (colorTexture)
		{
			const auto texture = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanTexture>(colorTexture);
			if (texture)
				texture->TransitionTo(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
	}

	void Renderer::Submit(const Ref<class Shader>& shader, const Ref<class VertexArray>& vertexArray, const  glm::mat4& transform)
	{
		shader->Bind();

		WLD_CORE_ERROR("Fix this.");
		//std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("u_ViewProjection", m_SceneData->ViewProjectionMatrix);
		//std::dynamic_pointer_cast<OpenGLShader>(shader)->UploadUniformMat4("u_Transform", transform);

		//vertexArray->Bind();
		// TODO: 这里应该允许用户指定索引数量，而不是每次都使用整个索引缓冲区的大小
		RenderCommand::DrawIndexed(vertexArray);
	}

	void Renderer::CaptureFrame(const std::filesystem::path& path)
	{
		if (!Application::HasInstance())
			return;
		const uint32_t width = Application::Get().GetWindow().GetWidth();
		const uint32_t height = Application::Get().GetWindow().GetHeight();
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
		glReadBuffer(GL_BACK);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
			GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			WLD_CORE_ERROR("[capture] cannot open {0}", path.string());
			return;
		}
		file << "P6\n" << width << " " << height << "\n255\n";
		for (uint32_t row = 0; row < height; ++row)
		{
			const uint32_t sourceRow = height - 1 - row;
			file.write(reinterpret_cast<const char*>(pixels.data() + static_cast<size_t>(sourceRow) * width * 3),
				static_cast<std::streamsize>(width) * 3);
		}
		WLD_CORE_INFO("[capture] wrote {0} ({1}x{2})", path.string(), width, height);
	}

	void Renderer::CaptureFramebuffer(const std::filesystem::path& path, uint32_t fbo, uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return;
		// 注意:不能用 const 变量 + const_cast 接收 glGetIntegerv(编译器会按常量
		// 折叠优化,导致恢复绑定失效,把后续绘制画到错误的目标上)。
		GLint previous = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);

		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
			GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			WLD_CORE_ERROR("[capture] cannot open {0}", path.string());
			return;
		}
		file << "P6\n" << width << " " << height << "\n255\n";
		for (uint32_t row = 0; row < height; ++row)
		{
			const uint32_t sourceRow = height - 1 - row;
			file.write(reinterpret_cast<const char*>(pixels.data() + static_cast<size_t>(sourceRow) * width * 3),
				static_cast<std::streamsize>(width) * 3);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
		WLD_CORE_INFO("[capture] wrote framebuffer {0} ({1}x{2})", path.string(), width, height);
	}
}
