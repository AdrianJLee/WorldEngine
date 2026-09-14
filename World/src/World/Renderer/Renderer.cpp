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
	static Rhi::Handle<Rhi::RenderPass> s_PresentPass;

	// 每窗口呈现状态(头文件中只前置声明,由 Renderer 创建与销毁)。
	struct PresentTarget
	{
		void* NativeWindow = nullptr;
		bool IsMain = false;
		uint32_t Width = 0, Height = 0;
		bool Dirty = true;
		Rhi::Handle<Rhi::Swapchain> Swapchain;
		Rhi::Handle<Rhi::CommandQueue> Queue;
		Rhi::Handle<Rhi::Semaphore> ImageReady, RenderDone;
		std::vector<Rhi::Handle<Rhi::Framebuffer>> Framebuffers;
		Rhi::Handle<Rhi::Framebuffer> Framebuffer;
		Rhi::Handle<Rhi::Texture> Image;
		uint32_t ImageIndex = 0;
	};

	namespace
	{
		PresentTarget s_MainPresent;
		PresentTarget* s_ActivePresent = &s_MainPresent;
		std::vector<std::unique_ptr<PresentTarget>> s_AuxPresent;

		void ReleasePresentState(PresentTarget& state)
		{
			state.Framebuffers.clear();
			state.Framebuffer = nullptr;
			state.Image = nullptr;
			state.Swapchain = nullptr;
			state.ImageReady = nullptr;
			state.RenderDone = nullptr;
			state.Dirty = true;
		}
	}

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
		s_PresentPass = nullptr;
		ReleasePresentState(s_MainPresent);
		s_AuxPresent.clear();
		s_ActivePresent = &s_MainPresent;
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
		s_MainPresent.Dirty = true;
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

	PresentTarget* Renderer::MainPresentTarget()
	{
		return &s_MainPresent;
	}

	PresentTarget* Renderer::CreatePresentTarget(const PresentTargetDesc& desc)
	{
		auto state = std::make_unique<PresentTarget>();
		state->NativeWindow = desc.NativeWindow;
		state->Width = desc.Width;
		state->Height = desc.Height;
		s_AuxPresent.push_back(std::move(state));
		return s_AuxPresent.back().get();
	}

	void Renderer::DestroyPresentTarget(PresentTarget* target)
	{
		if (!target || target == &s_MainPresent)
			return;
		auto* state = static_cast<PresentTarget*>(target);
		if (s_ActivePresent == state)
			s_ActivePresent = &s_MainPresent;
		ReleasePresentState(*state);
		s_AuxPresent.erase(std::remove_if(s_AuxPresent.begin(), s_AuxPresent.end(),
			[state](const std::unique_ptr<PresentTarget>& candidate) { return candidate.get() == state; }),
			s_AuxPresent.end());
	}

	void Renderer::ResizePresentTarget(PresentTarget* target, uint32_t width, uint32_t height)
	{
		PresentTarget* state = target ? static_cast<PresentTarget*>(target) : &s_MainPresent;
		state->Width = width;
		state->Height = height;
		state->Dirty = true;
	}

	bool Renderer::BeginFramePresent(PresentTarget* target)
	{
		PresentTarget& state = target ? *static_cast<PresentTarget*>(target) : s_MainPresent;
		s_ActivePresent = &state;
		if (!m_Device || s_BackendName != "vulkan")
			return true; // GL:渲染到各自窗口的默认帧缓冲,无需交换链
		if (state.Dirty || !state.Swapchain)
		{
			state.Dirty = false;
			ReleasePresentState(state);
			void* nativeWindow = state.NativeWindow;
			if (!nativeWindow && Application::HasInstance())
				nativeWindow = Application::Get().GetWindow().GetNativeWindow();
			if (!nativeWindow)
				return false;
			Rhi::SwapchainDesc swapDesc;
			swapDesc.NativeWindow = nativeWindow;
			swapDesc.Format = Rhi::Format::B8G8R8A8_UNORM;
			swapDesc.Present = Rhi::PresentMode::Fifo;
			swapDesc.DebugName = state.IsMain ? "MainSwapchain" : "AuxSwapchain";
			state.Swapchain = m_Device->CreateSwapchain(swapDesc);
			state.Queue = m_Device->CreateQueue("Present");
			state.ImageReady = m_Device->CreateSemaphore();
			state.RenderDone = m_Device->CreateSemaphore();
		}
		if (!state.Swapchain)
			return false;
		// acquire 必须携带信号量或栅栏(VUID 01780);该信号由 UI 提交等待消费,
		// UI 提交再发出 RenderDone 供 Present 等待。
		const Rhi::AcquireResult acquired = state.Swapchain->AcquireNext(state.ImageReady);
		if (acquired.OutOfDate)
		{
			state.Dirty = true;
			return false;
		}
		state.Image = acquired.Image;
		state.ImageIndex = acquired.ImageIndex;
		const auto vulkanSwapchain = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanSwapchain>(state.Swapchain);
		if (vulkanSwapchain && state.Image)
			vulkanSwapchain->TransitionImage(state.ImageIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		if (state.Image)
		{
			if (state.Framebuffers.size() <= state.ImageIndex)
				state.Framebuffers.resize(state.ImageIndex + 1);
			if (!state.Framebuffers[state.ImageIndex])
			{
				Rhi::FramebufferDesc framebufferDesc;
				framebufferDesc.RenderPass = GetPresentRenderPass();
				framebufferDesc.Extent = state.Swapchain->GetExtent();
				framebufferDesc.Attachments = { state.Image };
				framebufferDesc.DebugName = "Present";
				state.Framebuffers[state.ImageIndex] = m_Device->CreateFramebuffer(framebufferDesc);
			}
			state.Framebuffer = state.Framebuffers[state.ImageIndex];
		}
		return true;
	}

	void Renderer::EndFramePresent(PresentTarget* target)
	{
		PresentTarget& state = target ? *static_cast<PresentTarget*>(target) : s_MainPresent;
		if (m_Device && state.Swapchain && state.RenderDone)
		{
			const auto vulkanSwapchain = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanSwapchain>(state.Swapchain);
			if (vulkanSwapchain && state.Image)
				vulkanSwapchain->TransitionImage(state.ImageIndex, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			state.Swapchain->Present(state.RenderDone);
			if (state.Queue)
				state.Queue->WaitIdle();
		}
		state.Image = nullptr;
		state.Framebuffer = nullptr;
	}

	Rhi::Handle<Rhi::Framebuffer> Renderer::GetPresentFramebuffer()
	{
		return s_ActivePresent ? s_ActivePresent->Framebuffer : Rhi::Handle<Rhi::Framebuffer>();
	}

	Rhi::Handle<Rhi::Texture> Renderer::GetPresentTarget()
	{
		return s_ActivePresent ? s_ActivePresent->Image : Rhi::Handle<Rhi::Texture>();
	}

	void Renderer::SubmitUi(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer)
	{
		if (!m_Device || !commandBuffer)
			return;
		if (s_BackendName != "vulkan")
			return; // OpenGL 立即模式
		PresentTarget& state = s_ActivePresent ? *s_ActivePresent : s_MainPresent;
		if (!state.Queue || !state.Image)
			return;
		Rhi::SubmitInfo submit;
		submit.CommandBuffers = { commandBuffer };
		if (state.ImageReady)
			submit.WaitSemaphores = { state.ImageReady };
		if (state.RenderDone)
			submit.SignalSemaphores = { state.RenderDone };
		state.Queue->Submit(submit);
		// 帧循环串行化:提交后等待队列空闲,下一帧才可安全复用命令缓冲与资源。
		state.Queue->WaitIdle();
	}

	void Renderer::SubmitScene(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer,
		const Rhi::Handle<Rhi::Texture>& colorTexture)
	{
		if (!m_Device || !commandBuffer)
			return;
		if (s_BackendName != "vulkan")
			return; // OpenGL 立即模式
		PresentTarget& state = s_ActivePresent ? *s_ActivePresent : s_MainPresent;
		if (!state.Queue)
			return;
		Rhi::SubmitInfo submit;
		submit.CommandBuffers = { commandBuffer };
		state.Queue->Submit(submit);
		state.Queue->WaitIdle();
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
