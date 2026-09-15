#include "wldpch.h"
#include "Renderer.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Application.h"
#include "World/Renderer/RenderCommand.h"
#include "World/Renderer/VertexArray.h"
#include "World/Renderer/Shader.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Renderer/Renderer3D.h"
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
		// acquire 信号量由"帧起始的布局转换提交"消费,该提交再发出 FrameStart,
		// 帧内渲染提交等待 FrameStart:同一个信号量不会被等待两次(二值信号量语义)。
		std::vector<Rhi::Handle<Rhi::Semaphore>> FrameStart;
		bool FrameStartPending = false;    // FrameStart 已发出、尚无提交等待
		bool AcquireConsumed = false;      // acquire 信号量已被帧起始转换消费
		// 呈现前的布局转换也走"提交后不等待":该信号量由转换提交发出,Present 等它,
		// 从而既保证图像已处于 PRESENT 布局,又不用整队列排空(旧实现在这里 WaitIdle)。
		std::vector<Rhi::Handle<Rhi::Semaphore>> PresentReady;
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
		// 本帧第一个提交要等待的二进制信号量:优先 FrameStart(帧起始转换完成),
		// 回退 acquire 信号量(转换提交失败时);二值信号量不能被等待两次,故取用即清标志。
		Rhi::Handle<Rhi::Semaphore> TakePresentWait(PresentTarget& state, uint32_t slot)
		{
			if (state.FrameStartPending && !state.FrameStart.empty())
			{
				state.FrameStartPending = false;
				return state.FrameStart[slot % state.FrameStart.size()];
			}
			if (!state.AcquireConsumed && !state.ImageReady.empty())
			{
				state.AcquireConsumed = true;
				return state.ImageReady[slot % state.ImageReady.size()];
			}
			return nullptr;
		}

		// B0.2:场景/UI 的命令缓冲、UBO/描述符集、顶点索引缓冲按槽位环形化;
		// 呈现信号量按"acquire 按帧槽位 + render-finished 按交换链图像"配对(Vulkan 规范做法)。
		// B0 遗留评估:帧深 2 → 3(CPU/GPU 重叠更充分)。资源按槽位环形,信号量按
		// 帧槽位/交换链图像配对,延迟释放窗口随之放宽。
		// 槽位数量的唯一来源是 Renderer::FramesInFlight:SceneRenderer / WUI 的
		// 槽位数组同源于它,避免"渲染帧深 3、子系统仍按 2 取模"导致的越界/在飞复用。
		constexpr uint32_t kFramesInFlight = Renderer::FramesInFlight;
		uint64_t s_FrameNumber = 0;
		Rhi::Handle<Rhi::Fence> s_FrameFences[kFramesInFlight];
		bool s_FrameFenceSubmitted[kFramesInFlight] = {};
		bool s_FrameHadSubmission = false;
		Rhi::Handle<Rhi::CommandQueue> s_GlQueue;   // GL:提交点回放命令列表用的队列
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
		state.FrameStart.clear();
		state.RenderDone.clear();
		state.PresentReady.clear();
		state.FrameStartPending = false;
		state.AcquireConsumed = false;
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
		Renderer3D::Init();
		WLD_CORE_INFO("RHI backend initialized: {0}", s_BackendName);
	}

	void Renderer::Shutdown()
	{
		WLD_PROFILE_FUNCTION();
		// 设备销毁前的兜底:所有延迟释放先跑完(它们的资源属于当前设备)。
		RunDeferredReleases(std::numeric_limits<uint64_t>::max());
		// 释放交换链画面/信号量/描述符池前必须让 GPU 工作结束,
		// 否则触发 VUID-vkDestroyFramebuffer-00892 / vkDestroySemaphore-05149 /
		// vkDestroyDescriptorPool-00303(销毁仍在被提交命令引用的对象)。
		if (m_Device)
			m_Device->WaitIdle();
		for (uint32_t i = 0; i < kFramesInFlight; ++i)
		{
			s_FrameFences[i] = nullptr;
			s_FrameFenceSubmitted[i] = false;
		}
		s_GlQueue = nullptr;
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
		Renderer3D::Shutdown();
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
				s_FrameFences[slot] = m_Device->CreateFence(false);
			s_FrameFences[slot]->Wait();
			s_FrameFences[slot]->Reset();
			s_FrameFenceSubmitted[slot] = false;
		}
		RunDeferredReleases(s_FrameNumber);
	}

	void Renderer::EndFrame()
	{
		const uint32_t slot = FrameSlot();
		// 帧末挂帧栅栏:一次空提交(同队列有序,因此它的完成即代表本帧所有提交完成)。
		// 这样一帧内无论有多少次 UI 提交(主窗口 + N 个独立窗口),fence 都只提交一次,
		// 既保持"帧栅栏 = 整帧 GPU 工作"的语义,也不会撞上 VUID-vkQueueSubmit-fence-00063。
		if (s_FrameHadSubmission && !s_FrameFenceSubmitted[slot]
			&& m_Device && s_BackendName == "vulkan" && s_ActivePresent && s_ActivePresent->Queue)
		{
			// 提交栅栏必须未信号(创建时用 SIGNALED 会触发同一条 VUID);等待前由 BeginFrame
			// 统一 Wait + Reset。
			if (!s_FrameFences[slot])
				s_FrameFences[slot] = m_Device->CreateFence(false);
			Rhi::SubmitInfo submit;
			submit.Fence = s_FrameFences[slot];
			s_ActivePresent->Queue->Submit(submit);
			s_FrameFenceSubmitted[slot] = true;
		}
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

	void Renderer::WaitForGpu()
	{
		// GL 语义下没有跨帧 GPU 队列:调用点自身已同步。
		if (!m_Device || s_BackendName != "vulkan")
			return;
		m_Device->WaitIdle();
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
			// 重建交换链前先等 GPU 空闲:旧的画面/信号量可能仍被在飞命令引用。
			m_Device->WaitIdle();
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
			state.PresentReady.clear();
			state.ImageReady.reserve(kFramesInFlight);
			state.RenderDone.reserve(imageCount);
			state.PresentReady.reserve(imageCount);
			state.FrameStart.reserve(kFramesInFlight);
			for (uint32_t i = 0; i < kFramesInFlight; ++i)
			{
				state.ImageReady.push_back(m_Device->CreateSemaphore());
				state.FrameStart.push_back(m_Device->CreateSemaphore());
			}
			for (uint32_t i = 0; i < imageCount; ++i)
			{
				state.RenderDone.push_back(m_Device->CreateSemaphore());
				state.PresentReady.push_back(m_Device->CreateSemaphore());
			}
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
		state.FrameStartPending = false;
		state.AcquireConsumed = false;
		const auto vulkanSwapchain = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanSwapchain>(state.Swapchain);
		if (vulkanSwapchain && state.Image)
		{
			// 转换必须等 acquire 信号量(否则可能在呈现引擎仍持有图像时就改写布局),
			// 完成后再发出 FrameStart 供本帧渲染提交等待。
			const uint32_t startSlot = FrameSlot();
			const Rhi::Handle<Rhi::Semaphore> acquireSemaphore = state.ImageReady.empty()
				? nullptr : state.ImageReady[startSlot % state.ImageReady.size()];
			const Rhi::Handle<Rhi::Semaphore> frameStartSemaphore = state.FrameStart.empty()
				? nullptr : state.FrameStart[startSlot % state.FrameStart.size()];
			state.FrameStartPending = vulkanSwapchain->TransitionImage(state.ImageIndex,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, frameStartSemaphore, acquireSemaphore);
			state.AcquireConsumed = state.FrameStartPending;
		}
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
		// 空帧兜底:本帧没有任何提交时 FrameStart 会停在"已发出未被等待",
		// 二值信号量不能连续发出两次;用一个只带等待的空提交把它消费掉。
		if (state.FrameStartPending && !state.FrameStart.empty() && state.Queue && s_BackendName == "vulkan")
		{
			const uint32_t slot = FrameSlot();
			Rhi::SubmitInfo drain;
			drain.WaitSemaphores = { state.FrameStart[slot % state.FrameStart.size()] };
			state.Queue->Submit(drain);
			state.FrameStartPending = false;
			PresentTrace("[present] drained unused frame-start semaphore frame={0} slot={1}", s_FrameNumber, slot);
		}
		const size_t presentIndex = std::min<size_t>(state.ImageIndex, state.RenderDone.empty() ? 0 : state.RenderDone.size() - 1);
		if (m_Device && state.Swapchain && !state.RenderDone.empty() && state.RenderDone[presentIndex])
		{
			const auto vulkanSwapchain = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanSwapchain>(state.Swapchain);
			// 布局转换提交在 UI 提交之后入队(同队列有序),完成时发出 PresentReady,
			// Present 等待它即可,不需要整队列排空。
			Rhi::Handle<Rhi::Semaphore> presentWait = state.RenderDone[presentIndex];
			if (vulkanSwapchain && state.Image)
			{
				const size_t readyIndex = std::min<size_t>(state.ImageIndex,
					state.PresentReady.empty() ? 0 : state.PresentReady.size() - 1);
				if (!state.PresentReady.empty())
					presentWait = state.PresentReady[readyIndex];
				vulkanSwapchain->TransitionImage(state.ImageIndex, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, presentWait);
			}
			state.Swapchain->Present(presentWait);
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
		{
			// GL:命令列表在提交点回放(调用线程持有上下文,且早于 WUI 的 blit 与窗口交换)。
			if (!s_GlQueue)
				s_GlQueue = m_Device->CreateQueue("GL");
			Rhi::SubmitInfo submit;
			submit.CommandBuffers = { commandBuffer };
			s_GlQueue->Submit(submit);
			return;
		}
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
		// 帧起始转换已消费 acquire 信号量并发出 FrameStart(见 TakePresentWait)。
		if (const Rhi::Handle<Rhi::Semaphore> wait = TakePresentWait(state, slot))
			submit.WaitSemaphores = { wait };
		const size_t imageIndex = std::min<size_t>(state.ImageIndex, state.RenderDone.empty() ? 0 : state.RenderDone.size() - 1);
		if (!state.RenderDone.empty())
			submit.SignalSemaphores = { state.RenderDone[imageIndex] };
		// 帧栅栏不在这里挂:一帧可能有多次 UI 提交(主窗口 + 每个独立窗口各一次),
		// 重复提交同一个 fence 会触发 VUID-vkQueueSubmit-fence-00063(fence 已被信号)。
		// 统一由 Renderer::EndFrame 在帧末用一个空提交挂上,语义仍是"整帧 GPU 工作完成"。
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
		{
			// GL:场景命令列表在提交点回放(早于 UI 提交,保持场景→UI 顺序)。
			if (!s_GlQueue)
				s_GlQueue = m_Device->CreateQueue("GL");
			Rhi::SubmitInfo submit;
			submit.CommandBuffers = { commandBuffer };
			s_GlQueue->Submit(submit);
			return;
		}
		s_FrameHadSubmission = true;
		PresentTarget& state = s_ActivePresent ? *s_ActivePresent : s_MainPresent;
		// 设备切换后队列属于旧设备:必须等 BeginFramePresent 重建后再提交。
		if (!state.Queue || state.QueueDevice != m_Device.get())
			return;
		Rhi::SubmitInfo submit;
		submit.CommandBuffers = { commandBuffer };
		// 场景提交通常是本帧第一个提交:由它消费 FrameStart/acquire 等待,避免二值信号量被等两次。
		if (const Rhi::Handle<Rhi::Semaphore> wait = TakePresentWait(state, FrameSlot()))
			submit.WaitSemaphores = { wait };
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

	// 后端无关的纹理读回:CopyTextureToBuffer(RHI) → Map → PPM。
	// 与上面三个 GL 专用函数不同,这条路径在 Vulkan 下同样有效,是"双后端截图基线"的基础。
	bool Renderer::CaptureTexture(const std::filesystem::path& path,
		const Rhi::Handle<Rhi::Texture>& texture, uint32_t width, uint32_t height)
	{
		if (!m_Device || !texture || width == 0 || height == 0)
			return false;

		Rhi::BufferDesc readbackDesc;
		readbackDesc.Size = static_cast<uint64_t>(width) * height * 4;
		readbackDesc.Usage = Rhi::BufferUsageTransferDst;
		readbackDesc.Memory = Rhi::MemoryHint::HostVisible;
		readbackDesc.DebugName = "CaptureReadback";
		Rhi::Handle<Rhi::Buffer> readback = m_Device->CreateBuffer(readbackDesc);
		if (!readback)
		{
			WLD_CORE_ERROR("[capture] failed to create readback buffer");
			return false;
		}
		// 抓取用队列缓存复用:每次抓图都建队列会泄漏后端对象。
		static Rhi::Handle<Rhi::CommandQueue> s_CaptureQueue;
		if (!s_CaptureQueue)
			s_CaptureQueue = m_Device->CreateQueue("Capture");
		if (!s_CaptureQueue)
			return false;

		// 场景颜色附件在帧末处于 ShaderReadOnly;拷贝前后各做一次转换,
		// 保证拷完仍可被 UI 采样(否则下一帧读到的是一张"被拷走"的布局)。
		s_CaptureQueue->ExecuteImmediate([&](Rhi::CommandBuffer& cmd)
		{
			Rhi::ResourceBarrier toCopy;
			toCopy.Texture = texture;
			toCopy.Before = Rhi::ResourceState::ShaderReadOnly;
			toCopy.After = Rhi::ResourceState::CopySrc;
			cmd.PipelineBarrier({ toCopy });
			cmd.CopyTextureToBuffer(texture, readback, 0);
			Rhi::ResourceBarrier back;
			back.Texture = texture;
			back.Before = Rhi::ResourceState::CopySrc;
			back.After = Rhi::ResourceState::ShaderReadOnly;
			cmd.PipelineBarrier({ back });
		});

		const uint8_t* pixels = static_cast<const uint8_t*>(readback->Map());
		if (!pixels)
		{
			WLD_CORE_ERROR("[capture] readback buffer is not mappable on this backend");
			return false;
		}
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			readback->Unmap();
			WLD_CORE_ERROR("[capture] cannot open {0}", path.string());
			return false;
		}
		file << "P6\n" << width << " " << height << "\n255\n";
		// GL 的纹理原点在左下(读回是自下而上),Vulkan 复制出来自顶向下 —— 统一成 PPM 的顶向下。
		const bool flipRows = s_BackendName != "vulkan";
		std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
		for (uint32_t y = 0; y < height; ++y)
		{
			const uint32_t sourceRow = flipRows ? (height - 1 - y) : y;
			const uint8_t* source = pixels + static_cast<size_t>(sourceRow) * width * 4;
			for (uint32_t x = 0; x < width; ++x)
			{
				row[x * 3 + 0] = source[x * 4 + 0];
				row[x * 3 + 1] = source[x * 4 + 1];
				row[x * 3 + 2] = source[x * 4 + 2];
			}
			file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
		}
		readback->Unmap();
		WLD_CORE_INFO("[capture] wrote texture {0} ({1}x{2})", path.string(), width, height);
		return true;
	}
}


