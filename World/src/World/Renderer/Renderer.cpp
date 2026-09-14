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
		// 呈现信号量按**交换链图像**配对(Vulkan 规范要求):acquire 信号由渲染该图像的
		// 提交等待,渲染完成的信号由 Present 等待。按帧槽位配对在帧深 >1 时会互相踩。
		std::vector<Rhi::Handle<Rhi::Semaphore>> ImageReady, RenderDone;
		std::vector<Rhi::Handle<Rhi::Framebuffer>> Framebuffers;
		Rhi::Handle<Rhi::Framebuffer> Framebuffer;
		Rhi::Handle<Rhi::Texture> Image;
		uint32_t ImageIndex = 0;
		void* QueueDevice = nullptr;   // 队列所属设备(切换后端时校验)
	};

	namespace
	{
		PresentTarget s_MainPresent;
		PresentTarget* s_ActivePresent = &s_MainPresent;
		std::vector<std::unique_ptr<PresentTarget>> s_AuxPresent;

		// 刻意用堆分配且不析构:避免静态析构顺序导致退出期访问已销毁的容器。
		std::vector<std::pair<void*, std::function<void()>>>& DeviceReleaseHooks()
		{
			static auto* hooks = new std::vector<std::pair<void*, std::function<void()>>>();
			return *hooks;
		}

		// ---- 帧槽位 / 栅栏 / 延迟释放(B0) ----
		// B0.2:场景/UI 的命令缓冲、UBO/描述符集、顶点索引缓冲按槽位环形化;
		// 呈现信号量按"acquire 按帧槽位 + render-finished 按交换链图像"配对(Vulkan 规范做法)。
		constexpr uint32_t kFramesInFlight = 2;
		uint64_t s_FrameNumber = 0;
		Rhi::Handle<Rhi::Fence> s_FrameFences[kFramesInFlight];
		bool s_FrameFenceSubmitted[kFramesInFlight] = {};
		bool s_FrameHadSubmission = false;
		std::vector<std::pair<uint64_t, std::function<void()>>> s_DeferredReleases;   // (到期帧号, 回收动作)

		void RunDeferredReleases(uint64_t frameNumber)
		{
			auto& releases = s_DeferredReleases;
			for (auto it = releases.begin(); it != releases.end();)
			{
				if (it->first <= frameNumber)
				{
					if (it->second)
						it->second();
					it = releases.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		// WLD_VK_PRESENT_TRACE=1:逐帧打印 acquire/present 的关键状态(排查"窗口空白")。
		bool PresentTraceEnabled()
		{
			static const bool enabled = std::getenv("WLD_VK_PRESENT_TRACE") != nullptr;
			return enabled;
		}

		template <typename... Args>
		void PresentTrace(const char* format, Args&&... args)
		{
			if (PresentTraceEnabled())
				WLD_CORE_INFO(format, std::forward<decltype(args)>(args)...);
		}

		void ReleasePresentState(PresentTarget& state)
		{
			state.Framebuffers.clear();
			state.Framebuffer = nullptr;
			state.Image = nullptr;
			state.Swapchain = nullptr;
			// 队列/信号量都属于设备:漏掉任何一个都会在切换后端后用到已销毁设备。
			state.Queue = nullptr;
			state.ImageReady.clear();
			state.RenderDone.clear();
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
		Rhi::DeviceDesc deviceDesc;
		// 开发期可选:WLD_VULKAN_VALIDATION=1 打开验证层(仅当层可用)。
		if (const char* validation = std::getenv("WLD_VULKAN_VALIDATION"))
			deviceDesc.EnableValidation = validation[0] != '0';
		m_Device = Rhi::CreateDevice(requestedBackend, deviceDesc, &error);
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
		// 设备销毁前的兜底:所有延迟释放先跑完(它们的资源属于当前设备)。
		RunDeferredReleases(std::numeric_limits<uint64_t>::max());
		for (uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			s_FrameFences[i] = nullptr;
			s_FrameFenceSubmitted[i] = false;
		}
		// 设备销毁前先让持有句柄的子系统释放资源(否则它们的析构会用到已销毁设备)。
		{
			auto hooks = DeviceReleaseHooks();
			for (auto& [owner, hook] : hooks)
			{
				if (hook)
					hook();
			}
		}
		Renderer2D::Shutdown();
		s_GlobalDescriptorSetLayout = nullptr;
		s_PresentPass = nullptr;
		// 主窗口与独立窗口的呈现目标保留对象本身(宿主持有 PresentTarget*),
		// 只释放随设备生存的交换链/画面/信号量,并标记为需要重建。
		ReleasePresentState(s_MainPresent);
		s_MainPresent.Dirty = true;
		for (const std::unique_ptr<PresentTarget>& target : s_AuxPresent)
		{
			ReleasePresentState(*target);
			target->Dirty = true;
		}
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

	uint32_t Renderer::FrameSlot()
	{
		return static_cast<uint32_t>(s_FrameNumber % kFramesInFlight);
	}

	uint64_t Renderer::FrameNumber()
	{
		return s_FrameNumber;
	}

	void Renderer::BeginFrame()
	{
		if (!m_Device)
			return;
		const uint32_t slot = FrameSlot();
		if (s_BackendName == "vulkan" && s_FrameFenceSubmitted[slot])
		{
			// 该槽位上一轮提交的 GPU 工作完成后才开始复用其资源(替代整队列 WaitIdle)。
			if (!s_FrameFences[slot])
				s_FrameFences[slot] = m_Device->CreateFence(true);
			s_FrameFences[slot]->Wait();
			s_FrameFences[slot]->Reset();
			s_FrameFenceSubmitted[slot] = false;
		}
		RunDeferredReleases(s_FrameNumber);
	}

	void Renderer::EndFrame()
	{
		// 兜底:本帧有提交但没有任何提交携带帧栅栏(例如无 UI 的纯场景帧)时,
		// 直接等一次队列空闲,保证延迟释放不会回收仍在使用的资源。
		const uint32_t slot = FrameSlot();
		if (s_FrameHadSubmission && !s_FrameFenceSubmitted[slot] &&
			m_Device && s_BackendName == "vulkan" && s_ActivePresent && s_ActivePresent->Queue)
			s_ActivePresent->Queue->WaitIdle();
		s_FrameHadSubmission = false;
		++s_FrameNumber;
	}

	void Renderer::QueueRelease(std::function<void()> release)
	{
		if (!release)
			return;
		if (!m_Device || s_BackendName != "vulkan")
		{
			// GL 立即模式:调用点已经保证安全,直接执行。
			release();
			return;
		}
		// kFramesInFlight 轮之后、且该槽位的 fence 通过后执行。
		s_DeferredReleases.emplace_back(s_FrameNumber + kFramesInFlight, std::move(release));
	}

	void Renderer::RegisterDeviceReleaseHook(void* owner, std::function<void()> hook)
	{
		auto& hooks = DeviceReleaseHooks();
		for (auto& [key, existing] : hooks)
		{
			if (key == owner)
			{
				existing = std::move(hook);
				return;
			}
		}
		hooks.emplace_back(owner, std::move(hook));
	}

	void Renderer::UnregisterDeviceReleaseHook(void* owner)
	{
		auto& hooks = DeviceReleaseHooks();
		for (auto it = hooks.begin(); it != hooks.end(); ++it)
		{
			if (it->first == owner)
			{
				hooks.erase(it);
				return;
			}
		}
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
		// 销毁独立窗口的交换链前先等 GPU 空闲:该窗口可能刚提交过帧,
		// 直接释放会让 Vulkan 在仍有在途工作时销毁资源(访问违例)。
		if (m_Device)
			m_Device->WaitIdle();
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
			state.QueueDevice = m_Device.get();
			// acquire 信号量按帧槽位环(它只被同帧的提交消费);
			// render-finished 信号量按交换链图像配对(Present 等待它,同图像下次 acquire 前必须已被消费)。
			const uint32_t imageCount = std::max(1u, state.Swapchain->GetImageCount());
			state.ImageReady.clear();
			state.RenderDone.clear();
			state.ImageReady.reserve(kFramesInFlight);
			state.RenderDone.reserve(imageCount);
			for (uint32_t i = 0; i < kFramesInFlight; ++i)
				state.ImageReady.push_back(m_Device->CreateSemaphore());
			for (uint32_t i = 0; i < imageCount; ++i)
				state.RenderDone.push_back(m_Device->CreateSemaphore());
		}
		if (!state.Swapchain)
			return false;
		// acquire 必须携带信号量或栅栏(VUID 01780);该信号由 UI 提交等待消费,
		// UI 提交再发出 RenderDone 供 Present 等待。
		const Rhi::AcquireResult acquired = state.Swapchain->AcquireNext(
			state.ImageReady.empty() ? nullptr : state.ImageReady[FrameSlot() % state.ImageReady.size()]);
		PresentTrace("[present] frame={0} acquire image={1} outOfDate={2} swapchain={3}",
			s_FrameNumber, acquired.ImageIndex, acquired.OutOfDate ? 1 : 0,
			static_cast<int>(state.Swapchain ? 1 : 0));
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
		const size_t presentIndex = std::min<size_t>(state.ImageIndex, state.RenderDone.empty() ? 0 : state.RenderDone.size() - 1);
		if (m_Device && state.Swapchain && !state.RenderDone.empty() && state.RenderDone[presentIndex])
		{
			const auto vulkanSwapchain = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanSwapchain>(state.Swapchain);
			if (vulkanSwapchain && state.Image)
				vulkanSwapchain->TransitionImage(state.ImageIndex, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			state.Swapchain->Present(state.RenderDone[presentIndex]);
			PresentTrace("[present] presented frame={0} imageIndex={1}", s_FrameNumber, state.ImageIndex);
		}
		state.Image = nullptr;
		state.Framebuffer = nullptr;
		// 独立窗口在编辑器帧内渲染:呈现结束后把"当前呈现目标"切回主窗口,
		// 否则随后主窗口 UI 的提交会作用到独立窗口的 framebuffer(主窗口黑屏)。
		if (target)
			s_ActivePresent = &s_MainPresent;
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
		s_FrameHadSubmission = true;
		PresentTarget& state = s_ActivePresent ? *s_ActivePresent : s_MainPresent;
		if (!state.Queue || !state.Image)
		{
			PresentTrace("[present] skip UI submit frame={0} queue={1} image={2}",
				s_FrameNumber, static_cast<int>(state.Queue ? 1 : 0), static_cast<int>(state.Image ? 1 : 0));
			return;
		}
		Rhi::SubmitInfo submit;
		submit.CommandBuffers = { commandBuffer };
		const uint32_t slot = FrameSlot();
		if (!state.ImageReady.empty())
			submit.WaitSemaphores = { state.ImageReady[slot % state.ImageReady.size()] };
		const size_t imageIndex = std::min<size_t>(state.ImageIndex, state.RenderDone.empty() ? 0 : state.RenderDone.size() - 1);
		if (!state.RenderDone.empty())
			submit.SignalSemaphores = { state.RenderDone[imageIndex] };
		// 帧栅栏挂在"本帧最后一次提交"上:场景提交(若有)与本次提交同队列有序,
		// 因此该 fence 信号即代表整帧 GPU 工作完成。
		if (!s_FrameFences[slot])
			s_FrameFences[slot] = m_Device->CreateFence(true);
		submit.Fence = s_FrameFences[slot];
		s_FrameFenceSubmitted[slot] = true;
		state.Queue->Submit(submit);
		PresentTrace("[present] UI submitted frame={0} slot={1} imageIndex={2} fence=1",
			s_FrameNumber, slot, state.ImageIndex);
	}

	void Renderer::SubmitScene(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer,
		const Rhi::Handle<Rhi::Texture>& colorTexture)
	{
		if (!m_Device || !commandBuffer)
			return;
		if (s_BackendName != "vulkan")
			return; // OpenGL 立即模式
		s_FrameHadSubmission = true;
		PresentTarget& state = s_ActivePresent ? *s_ActivePresent : s_MainPresent;
		// 设备切换后队列属于旧设备:必须等 BeginFramePresent 重建后再提交。
		if (!state.Queue || state.QueueDevice != m_Device.get())
			return;
		Rhi::SubmitInfo submit;
		submit.CommandBuffers = { commandBuffer };
		// 帧栅栏挂在 UI 提交上(本帧最后一次提交),场景提交与它同队列有序。
		// 若本帧没有 UI 提交(纯场景帧),这里也负责挂栅栏。
		state.Queue->Submit(submit);
		// 场景纹理的 ShaderRead 转换已记录进场景命令缓冲(SceneRenderer::RecordSubmit),
		// 这里不再 WaitIdle、也不再做外部转换。
		(void)colorTexture;
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

	void Renderer::CaptureDefaultFramebuffer(const std::filesystem::path& path, uint32_t width, uint32_t height)
	{
		// 默认帧缓冲读取必须用 GL_BACK(GL_COLOR_ATTACHMENT0 只对 FBO 有效)。
		if (width == 0 || height == 0)
			return;
		GLint previous = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glReadBuffer(GL_BACK);

		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
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
		WLD_CORE_INFO("[capture] wrote default framebuffer {0} ({1}x{2})", path.string(), width, height);
	}
}


