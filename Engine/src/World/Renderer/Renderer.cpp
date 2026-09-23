#include "wldpch.h"
#include "Renderer.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Application.h"
#include "World/Renderer/RenderCommand.h"
#include "World/Renderer/VertexArray.h"
#include "World/Renderer/Shader.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"
#include "World/RHI/Vulkan/VulkanSwapchain.h"
#include "World/RHI/Vulkan/VulkanResources.h"
#include "World/RHI/Vulkan/VulkanDevice.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <fstream>
#include <chrono>
#include <cstring>

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
		std::vector<Rhi::Handle<Rhi::Semaphore>> ImageReady;
		// acquire 信号量由"帧起始的布局转换提交"消费,该提交再发出 FrameStart,
		// 帧内渲染提交等待 FrameStart:同一个信号量不会被等待两次(二值信号量语义)。
		std::vector<Rhi::Handle<Rhi::Semaphore>> FrameStart;
		bool FrameStartPending = false;    // FrameStart 已发出、尚无提交等待
		bool AcquireConsumed = false;      // acquire 信号量已被帧起始转换消费
		// UI 提交发出 PresentReady[imageIndex],Present 等它(见 SubmitUi/EndFramePresent)。
		// 二值信号量必须"发一次、等一次":UI 提交发,Present 等,不再经过中间提交,
		// 否则未被等待的信号会让下一次 signal 触发 VUID-vkQueueSubmit-pSignalSemaphores-00067
		// (2026-09-20 修复"每帧重建交换链"后由验证层实测暴露)。
		std::vector<Rhi::Handle<Rhi::Semaphore>> PresentReady;
		bool UiSubmitted = false;          // 本帧 UI 提交是否发出过 PresentReady
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

		// ---- P4-perf:垂直同步 / 呈现模式 ----
		// 优先级:环境变量 `WLD_VK_PRESENT_MODE`(本次运行强制)> 运行期切换(Renderer::SetVsync)
		//        > 清单 `rendering.vsync`。
		int s_VsyncOverride = -1;   // -1 = 未覆盖,0 = 强制关,1 = 强制开
		const char* PresentModeEnv()
		{
			static const char* value = std::getenv("WLD_VK_PRESENT_MODE");
			return value && value[0] ? value : nullptr;
		}

		bool DesiredVsync()
		{
			// immediate 是"不等 vblank",在 GL 侧同样表现为关闭垂直同步。
			if (const char* mode = PresentModeEnv())
				if (std::strcmp(mode, "immediate") == 0)
					return false;
			return s_VsyncOverride >= 0 ? (s_VsyncOverride == 1) : RenderSettings::VsyncEnabled();
		}

		Rhi::PresentMode DesiredPresentMode()
		{
			if (const char* mode = PresentModeEnv())
			{
				if (std::strcmp(mode, "immediate") == 0)
					return Rhi::PresentMode::Immediate;
				if (std::strcmp(mode, "mailbox") == 0)
					return Rhi::PresentMode::Mailbox;
				if (std::strcmp(mode, "fifo") == 0)
					return Rhi::PresentMode::Fifo;
			}
			return DesiredVsync() ? Rhi::PresentMode::Fifo : Rhi::PresentMode::Immediate;
		}

		// ---- P4-perf:每帧阶段耗时(WLD_FRAME_TIMING=1;默认关,零开销) ----
		// 只做诊断:把"每帧 ~20ms 去哪了"拆成驱动调用/录制两类可比数字。
		// 注意:累计口径是"整帧"(主窗口 + 独立窗口的提交都在内)。
		struct FrameTiming
		{
			bool Enabled = false;
			double BeginStamp = 0.0;
			double Fence = 0, Acquire = 0, TransitionIn = 0, Scene = 0, Ui = 0,
				TransitionOut = 0, Present = 0, FenceSubmit = 0, Rebuild = 0, Total = 0;
			uint32_t Frames = 0;
			uint32_t Submits = 0;
			uint32_t Rebuilds = 0;
		};

		FrameTiming& Timing()
		{
			static FrameTiming timing = [] {
				FrameTiming out;
				const char* env = std::getenv("WLD_FRAME_TIMING");
				out.Enabled = env && env[0] != '\0' && env[0] != '0';
				return out;
			}();
			return timing;
		}

		double TimingNowMs()
		{
			return std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// 把一段时间记进某个阶段(未开启计时时完全不进 steady_clock)。
		struct TimingScope
		{
			explicit TimingScope(double& sink) : m_Sink(&sink), m_Start(0.0)
			{
				if (Timing().Enabled)
					m_Start = TimingNowMs();
			}
			~TimingScope()
			{
				if (m_Sink && Timing().Enabled)
					*m_Sink += TimingNowMs() - m_Start;
			}
			double* m_Sink;
			double m_Start;
		};

		void TimingCountSubmit()
		{
			FrameTiming& timing = Timing();
			if (timing.Enabled)
				++timing.Submits;
		}

		void TimingBeginFrame()
		{
			FrameTiming& timing = Timing();
			if (timing.Enabled)
				timing.BeginStamp = TimingNowMs();
		}

		void TimingEndFrame()
		{
			FrameTiming& timing = Timing();
			if (!timing.Enabled || timing.BeginStamp <= 0.0)
				return;
			timing.Total += TimingNowMs() - timing.BeginStamp;
			timing.BeginStamp = 0.0;
			if (++timing.Frames < 120)
				return;
			const double frames = static_cast<double>(timing.Frames);
			WLD_CORE_INFO("[frame-timing] total={0:.2f} fence={1:.2f} acquire={2:.2f} transIn={3:.2f} "
				"scene={4:.2f} ui={5:.2f} transOut={6:.2f} present={7:.2f} fenceSubmit={8:.2f} "
				"rebuild={9:.2f} submits/frame={10:.1f} rebuilds/frame={11:.2f} (ms/frame, n={12})",
				timing.Total / frames, timing.Fence / frames, timing.Acquire / frames,
				timing.TransitionIn / frames, timing.Scene / frames, timing.Ui / frames,
				timing.TransitionOut / frames, timing.Present / frames, timing.FenceSubmit / frames,
				timing.Rebuild / frames, static_cast<double>(timing.Submits) / frames,
				static_cast<double>(timing.Rebuilds) / frames, timing.Frames);
			timing.Fence = timing.Acquire = timing.TransitionIn = timing.Scene = timing.Ui = 0.0;
			timing.TransitionOut = timing.Present = timing.FenceSubmit = timing.Rebuild = timing.Total = 0.0;
			timing.Submits = 0;
			timing.Rebuilds = 0;
			timing.Frames = 0;
		}

		template <typename... Args>
		void PresentTrace(const char* format, Args&&... args)
		{
			if (PresentTraceEnabled())
				WLD_CORE_INFO(format, std::forward<decltype(args)>(args)...);
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
			state.PresentReady.clear();
			state.FrameStartPending = false;
			state.AcquireConsumed = false;
			state.UiSubmitted = false;
			// 兜底标记:调用方(销毁/释放)之后必须重建。**重建路径**要在调用本函数
			// 之后再清一次 Dirty,否则会每帧都重建交换链(实测 18ms/帧、FPS 40)。
			state.Dirty = true;
		}

		// ---- P4-CLEANUP:设备已丢失(TDR/reset)时的退出硬化 ----
		// "设备已丢失"只在 Vulkan 设备上有语义(OpenGL 后端没有这个概念,恒为 false)。
		// 返回 true 时:该设备上的"等 GPU 空闲 / 提交"都不会成功(规范:返回
		// VK_ERROR_DEVICE_LOST),退出路径按"跳过 GPU 交互、只释放 CPU 侧句柄"处理。
		bool DeviceLost(const Rhi::Handle<Rhi::Device>& device)
		{
			if (!device)
				return false;
			const auto vulkanDevice = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanDevice>(device);
			return vulkanDevice && vulkanDevice->IsDeviceLost();
		}

		// 跳过 GPU 交互的提示**只报一次**:退出会依次经过 WaitForGpu → 各独立窗口的
		// DestroyPresentTarget → Renderer::Shutdown,三处跳过的是同一件事,逐处报会刷屏。
		void WarnDeviceLostSkipOnce(const char* action)
		{
			static bool warned = false;
			if (warned)
				return;
			warned = true;
			WLD_CORE_WARN("[shutdown] 设备已丢失:跳过 GPU 等待/交互({0});"
				"按原顺序继续释放 CPU 侧句柄(释放钩子与资源销毁不受影响)",
				action ? action : "?");
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
		// P4-UX2f:Debug 构建**默认打开** Vulkan 验证层 —— 偶发 device lost 只有验证层能给出
		// 具体 VUID 与对象;WLD_VULKAN_VALIDATION=0 可关(Release 默认关)。
		bool enableValidation = false;
#if defined(WLD_DEBUG)
		enableValidation = true;
#endif
		if (const char* validation = std::getenv("WLD_VULKAN_VALIDATION"))
			enableValidation = validation[0] != '0';
		deviceDesc.EnableValidation = enableValidation;
		if (enableValidation)
			WLD_CORE_INFO("[rhi] Vulkan 验证层已请求(层不可用时设备创建会忽略)");
		m_Device = Rhi::CreateDevice(requestedBackend, deviceDesc, &error);
		if (!m_Device)
		{
			WLD_CORE_WARN("Requested backend '{0}' unavailable ({1}); falling back to OpenGL",
				backend, error);
			error.clear();
			m_Device = Rhi::CreateDevice(Rhi::Backend::OpenGL, {}, &error);
		}
		WLD_CORE_ASSERT(m_Device, "Failed to create RHI device: {0}", error);

		// ShaderCompiler 依据 API 选 SPIR-V 目标(Vulkan profile / GL profile)。
		RendererAPI::SetAPI(requestedBackend == Rhi::Backend::Vulkan && m_Device->GetCapabilities().BackendName == "Vulkan"
			? RendererAPI::API::Vulkan : RendererAPI::API::OpenGL);
		s_BackendName = RendererAPI::GetAPI() == RendererAPI::API::Vulkan ? "vulkan" : "opengl";

		RenderCommand::Init();
		Renderer2D::Init();
		Renderer3D::Init();
		// P4-perf:清单里的 rendering.vsync 在这里落到窗口(GL 侧 swap interval)。
		// Vulkan 侧不在这里建交换链(懒创建),BeginFramePresent 会用同一份设置。
		const bool vsync = DesiredVsync();
		if (Application::HasInstance())
		{
			Window& window = Application::Get().GetWindow();
			if (window.IsVsync() != vsync)
				window.SetVsync(vsync);
		}
		WLD_CORE_INFO("[render] vsync={0} present_mode={1}", vsync ? "on" : "off",
			vsync ? "fifo" : "immediate");
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
		// P4-CLEANUP:设备已丢失时这一步等不到任何东西(vkDeviceWaitIdle 立刻返回
		// VK_ERROR_DEVICE_LOST,驱动的在途工作已被终止),按"设备丢失 = 跳过 GPU 交互、
		// 只释放 CPU 侧句柄"的口径跳过;**下面的释放钩子/子系统/呈现状态清理顺序不变**。
		if (m_Device && !DeviceLost(m_Device))
			m_Device->WaitIdle();
		else if (m_Device)
			WarnDeviceLostSkipOnce("Renderer::Shutdown 的整设备等待(WaitIdle)");
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

	void Renderer::SetVsync(bool enabled)
	{
		s_VsyncOverride = enabled ? 1 : 0;
		WLD_CORE_INFO("[render] vsync toggled: {0}", enabled ? "on" : "off");
		// GL:swap interval 立即改(窗口/上下文还在);Vulkan:交换链重建,下一帧生效。
		if (Application::HasInstance())
		{
			Window& window = Application::Get().GetWindow();
			if (window.IsVsync() != enabled)
				window.SetVsync(enabled);
		}
		if (s_MainPresent.Swapchain)
			s_MainPresent.Dirty = true;
	}

	bool Renderer::IsVsyncEnabled()
	{
		return DesiredVsync();
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
		// P4-CLEANUP(2026-09-21):诊断/回归钩子 —— 人为把设备标记为"已丢失",用来验证
		// "丢失之后安全退出"这条路径(真实 TDR 不可控,之前的回归只能靠碰运气复现)。
		// 用法:`WLD_VK_SIMULATE_DEVICE_LOST=<帧号>` → 该帧起所有提交/等待按"设备已死"处理,
		// 退出应仍然干净(0xC0000005 不再出现)。只影响被显式打开该变量的进程。
		if (s_BackendName == "vulkan")
		{
			if (const char* simulate = std::getenv("WLD_VK_SIMULATE_DEVICE_LOST"))
			{
				const uint64_t targetFrame = std::strtoull(simulate, nullptr, 10);
				if (targetFrame != 0 && s_FrameNumber == targetFrame)
				{
					if (const auto vulkanDevice = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanDevice>(m_Device))
						vulkanDevice->NotifyDeviceLost("simulated (WLD_VK_SIMULATE_DEVICE_LOST)");
				}
			}
		}
		TimingBeginFrame();
		const uint32_t slot = FrameSlot();
		if (s_BackendName == "vulkan" && s_FrameFenceSubmitted[slot])
		{
			// 该槽位上一轮提交的 GPU 工作完成后才开始复用其资源(替代整队列 WaitIdle)。
			// P4-CLEANUP(2026-09-21,实测挂起事故):设备已丢失时**必须跳过这次等待** ——
			// 丢失之后 VulkanCommandQueue::Submit 会早退,帧末那次"挂栅栏的空提交"根本没发出去,
			// 栅栏永远不会 signal,而 EndFrame 已经把它标成已提交 → 本函数等到 3 帧后回到同一槽位时
			// 就永久阻塞(实测:主线程卡死、窗口无响应、AI 通道 10s 超时)。
			// 丢失时"等 GPU"没有语义(驱动已终止在途工作),清标志继续走,让宿主决定退出/恢复。
			if (DeviceLost(m_Device))
			{
				WarnDeviceLostSkipOnce("Renderer::BeginFrame 的帧栅栏等待");
				s_FrameFenceSubmitted[slot] = false;
			}
			else
			{
				TimingScope fenceScope(Timing().Fence);
				if (!s_FrameFences[slot])
					s_FrameFences[slot] = m_Device->CreateFence(false);
				s_FrameFences[slot]->Wait();
				s_FrameFences[slot]->Reset();
				s_FrameFenceSubmitted[slot] = false;
			}
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
			{
				TimingScope submitScope(Timing().FenceSubmit);
				s_ActivePresent->Queue->Submit(submit);
				TimingCountSubmit();
			}
			s_FrameFenceSubmitted[slot] = true;
		}
		s_FrameHadSubmission = false;
		++s_FrameNumber;
		TimingEndFrame();
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
		// P4-CLEANUP:设备已丢失时"等 GPU 空闲"没有语义 —— 驱动的在途工作已被终止。
		// 宿主退出序列第一步就调到这里(Application::Shutdown),跳过并说明。
		if (DeviceLost(m_Device))
		{
			WarnDeviceLostSkipOnce("Renderer::WaitForGpu 的整设备等待(WaitIdle)");
			return;
		}
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
			// D4:set0 全局布局新增两个 binding(所有用该布局的管线共享,未用到的 binding
			// 由各自调用方决定是否写入):
			//   binding 2 = 灯光 UBO(顶点阶段读阴影矩阵,片元阶段做光照/阴影);
			//   binding 3 = 方向光阴影贴图(仅片元)。
			// GL 后端的 UBO/纹理绑定单元 = binding,2/3 与既有 0/1 不冲突。
			desc.Bindings.push_back({ 2, Rhi::DescriptorType::UniformBuffer,
				Rhi::ShaderStageFlag(Rhi::ShaderStage::Vertex) | Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
			desc.Bindings.push_back({ 3, Rhi::DescriptorType::CombinedImageSampler,
				Rhi::ShaderStageFlag(Rhi::ShaderStage::Fragment), 1 });
			desc.DebugName = "Global.Set0";
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
		// P4-CLEANUP:设备已丢失时在途工作已被驱动终止,这里的等待同样跳过;
		// 交换链/画面/信号量仍按原顺序释放(都是 CPU 侧句柄)。
		if (m_Device && !DeviceLost(m_Device))
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
		// 设备丢失后不再提交任何工作:继续提交只会产生成串的验证层报错(实测 126 条 VUID),
		// 而 GPU 已经无法执行。宿主可据此走"设备重建/提示退出"策略。
		if (const auto vulkanDevice = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanDevice>(m_Device);
			vulkanDevice && vulkanDevice->IsDeviceLost())
			return false;
		if (state.Dirty || !state.Swapchain)
		{
			TimingScope rebuildScope(Timing().Rebuild);
			if (Timing().Enabled)
				++Timing().Rebuilds;
			// 重建交换链前先等 GPU 空闲:旧的画面/信号量可能仍被在飞命令引用。
			m_Device->WaitIdle();
			ReleasePresentState(state);
			// ReleasePresentState 会把 Dirty 置起来(销毁路径的兜底语义);这里紧接着
			// 就重建,必须在这里清掉 —— 早期版本把它清在调用之前,于是每帧都重建交换链
			// (实测 18.4ms/帧、编辑器 40 FPS、Runtime 同样慢,GL 不受影响)。
			state.Dirty = false;
			void* nativeWindow = state.NativeWindow;
			if (!nativeWindow && Application::HasInstance())
				nativeWindow = Application::Get().GetWindow().GetNativeWindow();
			if (!nativeWindow)
				return false;
			Rhi::SwapchainDesc swapDesc;
			swapDesc.NativeWindow = nativeWindow;
			swapDesc.Format = Rhi::Format::B8G8R8A8_UNORM;
			// P4-perf:呈现模式来自清单 rendering.vsync(默认 FIFO),环境变量/运行期可覆盖。
			swapDesc.Present = DesiredPresentMode();
			swapDesc.DebugName = state.IsMain ? "MainSwapchain" : "AuxSwapchain";
			state.Swapchain = m_Device->CreateSwapchain(swapDesc);
			state.Queue = m_Device->CreateQueue("Present");
			state.QueueDevice = m_Device.get();
			// acquire 信号量按帧槽位环(它只被同帧的提交消费);
			// render-finished(PresentReady)按交换链图像配对:UI 提交发、Present 等,
			// 同图像下次 acquire 前必然已被消费(二值信号量"发一次等一次"配对)。
			const uint32_t imageCount = std::max(1u, state.Swapchain->GetImageCount());
			state.ImageReady.clear();
			state.PresentReady.clear();
			state.ImageReady.reserve(kFramesInFlight);
			state.PresentReady.reserve(imageCount);
			state.FrameStart.reserve(kFramesInFlight);
			for (uint32_t i = 0; i < kFramesInFlight; ++i)
			{
				state.ImageReady.push_back(m_Device->CreateSemaphore());
				state.FrameStart.push_back(m_Device->CreateSemaphore());
			}
			for (uint32_t i = 0; i < imageCount; ++i)
			{
				state.PresentReady.push_back(m_Device->CreateSemaphore());
			}
			state.UiSubmitted = false;
		}
		if (!state.Swapchain)
			return false;
		// acquire 必须携带信号量或栅栏(VUID 01780);该信号由帧起始转换提交消费,
		// 转换提交再发出 FrameStart 供本帧第一次渲染提交等待。
		Rhi::AcquireResult acquired;
		{
			TimingScope acquireScope(Timing().Acquire);
			acquired = state.Swapchain->AcquireNext(
				state.ImageReady.empty() ? nullptr : state.ImageReady[FrameSlot() % state.ImageReady.size()]);
		}
		PresentTrace("[present] frame={0} acquire image={1} outOfDate={2} swapchain={3}",
			s_FrameNumber, acquired.ImageIndex, acquired.OutOfDate ? 1 : 0,
			static_cast<int>(state.Swapchain ? 1 : 0));
		PresentTrace("[present] semaphore handles: imageReady={0} frameStart={1}",
			state.ImageReady.empty() ? 0ull : std::static_pointer_cast<Rhi::Vulkan::VulkanSemaphore>(
				state.ImageReady[FrameSlot() % state.ImageReady.size()])->DebugHandle(),
			state.FrameStart.empty() ? 0ull : std::static_pointer_cast<Rhi::Vulkan::VulkanSemaphore>(
				state.FrameStart[FrameSlot() % state.FrameStart.size()])->DebugHandle());
		// acquire 失败(SURFACE_LOST 等)或交换链过期:没有可继续渲染的图像,且规范保证
		// acquire 的信号量**不会被 signal**。放弃本帧、不做任何提交(尤其不能提交对它的
		// 等待,否则触发 VUID-vkQueueSubmit-pWaitSemaphores-03238),下一帧重建交换链。
		if (acquired.Failed || acquired.OutOfDate)
		{
			state.FrameStartPending = false;
			state.AcquireConsumed = true;
			state.UiSubmitted = false;
			state.Image = nullptr;
			state.Framebuffer = nullptr;
			state.Dirty = true;
			return false;
		}
		// 次优图像可用:照常渲染,但下一帧重建交换链。
		if (acquired.Suboptimal)
			state.Dirty = true;
		state.Image = acquired.Image;
		state.ImageIndex = acquired.ImageIndex;
		state.FrameStartPending = false;
		state.AcquireConsumed = false;
		state.UiSubmitted = false;   // 本帧还没提交 UI,PresentReady 未发出
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
			{
				// acquire→ColorAttachment 的布局转换提交(每帧一次驱动调用)。
				TimingScope transitionScope(Timing().TransitionIn);
				state.FrameStartPending = vulkanSwapchain->TransitionImage(state.ImageIndex,
					VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, frameStartSemaphore, acquireSemaphore);
				if (state.FrameStartPending)
					TimingCountSubmit();
			}
			PresentTrace("[present] transition submitted={0} acquireSem={1} frameStartSem={2}",
				static_cast<int>(state.FrameStartPending),
				acquireSemaphore ? static_cast<uint64_t>(std::static_pointer_cast<Rhi::Vulkan::VulkanSemaphore>(acquireSemaphore)->DebugHandle()) : 0ull,
				frameStartSemaphore ? static_cast<uint64_t>(std::static_pointer_cast<Rhi::Vulkan::VulkanSemaphore>(frameStartSemaphore)->DebugHandle()) : 0ull);
			state.AcquireConsumed = state.FrameStartPending;
			if (!state.FrameStartPending)
			{
				// 转换提交失败:本帧没有可用的布局保证,不能继续渲染(渲染通道声明了
				// 附件初始布局,若图像实际仍在 PRESENT 布局就是非法用法)。放弃本帧,
				// 不做任何提交(此时 acquire 信号量的状态无法确认,再提交等待可能撞上
				// VUID-vkQueueSubmit-pWaitSemaphores-03238),下一帧重建交换链。
				state.Dirty = true;
				state.Image = nullptr;
				state.Framebuffer = nullptr;
				return false;
			}
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
			{
				TimingScope submitScope(Timing().TransitionOut);
				state.Queue->Submit(drain);
				TimingCountSubmit();
			}
			state.FrameStartPending = false;
			PresentTrace("[present] drained unused frame-start semaphore frame={0} slot={1}", s_FrameNumber, slot);
		}
		const size_t presentIndex = std::min<size_t>(state.ImageIndex,
			state.PresentReady.empty() ? 0 : state.PresentReady.size() - 1);
		if (m_Device && state.Swapchain && !state.PresentReady.empty() && state.PresentReady[presentIndex])
		{
			const auto vulkanSwapchain = std::dynamic_pointer_cast<Rhi::Vulkan::VulkanSwapchain>(state.Swapchain);
			// 本帧走了 UI 通道时:那个 render pass(InitialLayout=ColorAttachment →
			// FinalLayout=Present)已经在通道末尾把图像转换到 PRESENT 布局,并由本次 UI 提交
			// 发出 PresentReady —— Present 直接等它,不需要中间提交,也不需要整队列排空。
			// 没走 UI 通道时(纯场景帧/空帧):补一次纯屏障转换(同队列有序,不需要信号量),
			// 否则 acquired 图像可能还停在 ColorAttachment。
			if (vulkanSwapchain && state.Image && !state.UiSubmitted)
			{
				TimingScope transitionScope(Timing().TransitionOut);
				if (vulkanSwapchain->TransitionImage(state.ImageIndex, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
					nullptr, nullptr))
					TimingCountSubmit();
			}
			const Rhi::Handle<Rhi::Semaphore> presentWait =
				state.UiSubmitted ? state.PresentReady[presentIndex] : nullptr;
			{
				TimingScope presentScope(Timing().Present);
				state.Swapchain->Present(presentWait);
			}
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
		// 渲染完成 → PresentReady[图像下标]:二值信号量"本提交发、Present 等",
		// 中间不再经过转换提交(否则信号无人等待,下次 signal 触发 VUID-00067)。
		const size_t imageIndex = std::min<size_t>(state.ImageIndex,
			state.PresentReady.empty() ? 0 : state.PresentReady.size() - 1);
		if (!state.PresentReady.empty() && state.PresentReady[imageIndex])
		{
			submit.SignalSemaphores = { state.PresentReady[imageIndex] };
			state.UiSubmitted = true;
		}
		// 帧栅栏不在这里挂:一帧可能有多次 UI 提交(主窗口 + 每个独立窗口各一次),
		// 重复提交同一个 fence 会触发 VUID-vkQueueSubmit-fence-00063(fence 已被信号)。
		// 统一由 Renderer::EndFrame 在帧末用一个空提交挂上,语义仍是"整帧 GPU 工作完成"。
		{
			TimingScope submitScope(Timing().Ui);
			state.Queue->Submit(submit);
			TimingCountSubmit();
		}
		PresentTrace("[present] UI submitted frame={0} slot={1} imageIndex={2} signalsPresentReady=1",
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
		{
			// 诊断:场景提交被丢弃时静默失败会让"画面只有清屏色"变成谜案(实测 Runtime 侧)。
			static int dropLogged = 0;
			if (dropLogged < 3)
			{
				++dropLogged;
				WLD_CORE_WARN("[scene] submission dropped: queue={0} deviceMatch={1}",
					state.Queue ? 1 : 0, state.Queue && state.QueueDevice == m_Device.get() ? 1 : 0);
			}
			return;
		}
		Rhi::SubmitInfo submit;
		submit.CommandBuffers = { commandBuffer };
		// 场景提交通常是本帧第一个提交:由它消费 FrameStart/acquire 等待,避免二值信号量被等两次。
		if (const Rhi::Handle<Rhi::Semaphore> wait = TakePresentWait(state, FrameSlot()))
			submit.WaitSemaphores = { wait };
		// 帧栅栏挂在 UI 提交上(本帧最后一次提交),场景提交与它同队列有序。
		// 若本帧没有 UI 提交(纯场景帧),这里也负责挂栅栏。
		{
			TimingScope submitScope(Timing().Scene);
			state.Queue->Submit(submit);
			TimingCountSubmit();
		}
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
	namespace
	{
		struct PendingPresentCapture
		{
			void* Target = nullptr;             // PresentTarget*;nullptr = 主窗口
			std::filesystem::path Path;
			uint32_t Width = 0;
			uint32_t Height = 0;
		};

		std::vector<PendingPresentCapture>& PendingCaptures()
		{
			static auto* pending = new std::vector<PendingPresentCapture>();
			return *pending;
		}
	}

	void Renderer::RequestPresentCapture(PresentTarget* target, const std::filesystem::path& path,
		uint32_t width, uint32_t height)
	{
		if (path.empty() || width == 0 || height == 0)
			return;
		PendingCaptures().push_back({ static_cast<void*>(target), path, width, height });
	}

	void Renderer::FlushPresentCaptures()
	{
		if (PendingCaptures().empty() || !m_Device)
			return;
		PresentTarget& active = s_ActivePresent ? *s_ActivePresent : s_MainPresent;
		const bool mainTarget = (&active == &s_MainPresent);
		for (size_t i = 0; i < PendingCaptures().size();)
		{
			const PendingPresentCapture pending = PendingCaptures()[i];
			const bool matches = (pending.Target == nullptr) ? mainTarget
				: (pending.Target == static_cast<void*>(&active));
			if (!matches)
			{
				++i;
				continue;
			}
			PendingCaptures().erase(PendingCaptures().begin() + static_cast<std::ptrdiff_t>(i));
			// OpenGL:默认帧缓冲读回(此时 UI 已经提交/交换前)。
			if (s_BackendName != "vulkan")
			{
				CaptureFrame(pending.Path);
				continue;
			}
			if (!active.Image || !active.Queue || active.QueueDevice != m_Device.get())
			{
				WLD_CORE_WARN("[capture] present target not ready for capture");
				continue;
			}
			Rhi::BufferDesc readbackDesc;
			readbackDesc.Size = static_cast<uint64_t>(pending.Width) * pending.Height * 4;
			readbackDesc.Usage = Rhi::BufferUsageTransferDst;
			readbackDesc.Memory = Rhi::MemoryHint::HostVisible;
			readbackDesc.DebugName = "PresentCaptureReadback";
			Rhi::Handle<Rhi::Buffer> readback = m_Device->CreateBuffer(readbackDesc);
			if (!readback)
				continue;
			// 与帧渲染**同队列**提交(队列顺序保证拷贝在 UI 提交之后),不加额外等待信号量:
			// FrameStart 是二值信号量,已被本帧渲染提交消费,再等一次会破坏语义(实测设备丢失)。
			//
			// 布局契约(2026-09-20 修,DEF-2):本函数跑在"UI 已提交、尚未呈现"之间,
			// 而 EndFramePresent 在本帧走过 UI 通道时**直接 present**(它假定 UI 通道的
			// FinalLayout=Present 已经把图像留在 PRESENT 布局,不再补转换)。所以抓图这里
			// 借走布局必须**还回去**:
			//   Present → CopySrc(显式屏障)→ 拷贝 → CopySrc → Present(显式屏障)。
			// 旧实现只做了前半段,拷完把交换链图留在 TRANSFER_SRC_OPTIMAL → 每次
			// vkQueuePresentKHR 报一条 VUID-VkPresentInfoKHR-pImageIndices-01430
			// (实测 7 次抓图 7 条)。屏障走 RHI 的跟踪布局,不还会让跟踪值与真实值一起跑偏。
			// 注:CopyTextureToBuffer 自带"进入前是什么布局就还原成什么布局",这里进入前是
			// CopySrc,所以它本身不会替我们还 —— 必须显式补第二条屏障。
			active.Queue->ExecuteImmediate([&](Rhi::CommandBuffer& cmd)
			{
				Rhi::ResourceBarrier toCopy;
				toCopy.Texture = active.Image;
				toCopy.Before = Rhi::ResourceState::ShaderReadOnly;   // 说明性字段:后端以跟踪布局为准
				toCopy.After = Rhi::ResourceState::CopySrc;
				cmd.PipelineBarrier({ toCopy });
				cmd.CopyTextureToBuffer(active.Image, readback, 0);
				Rhi::ResourceBarrier backToPresent;
				backToPresent.Texture = active.Image;
				backToPresent.Before = Rhi::ResourceState::CopySrc;   // 说明性字段:后端以跟踪布局为准
				backToPresent.After = Rhi::ResourceState::Present;
				cmd.PipelineBarrier({ backToPresent });
			});

			const uint8_t* pixels = static_cast<const uint8_t*>(readback->Map());
			if (!pixels)
			{
				WLD_CORE_ERROR("[capture] present readback buffer is not mappable");
				continue;
			}
			std::ofstream file(pending.Path, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				readback->Unmap();
				WLD_CORE_ERROR("[capture] cannot open {0}", pending.Path.string());
				continue;
			}
			file << "P6\n" << pending.Width << " " << pending.Height << "\n255\n";
			// 通道顺序:Vulkan 交换链是 B8G8R8A8(实测面板底色被抓成 (33,32,31),
			// 正确值是 (31,32,33)),必须按源格式把 B/R 换回 PPM 的 R,G,B。
			const Rhi::Format sourceFormat = active.Image->GetDesc().Format;
			const bool sourceIsBgra = sourceFormat == Rhi::Format::B8G8R8A8_UNORM
				|| sourceFormat == Rhi::Format::B8G8R8A8_SRGB;
			std::vector<uint8_t> row(static_cast<size_t>(pending.Width) * 3);
			for (uint32_t y = 0; y < pending.Height; ++y)
			{
				// 行序:Vulkan 的 vkCmdCopyImageToBuffer 第 0 行就是图像**顶部**
				// (与 OpenGL glReadPixels 的"第 0 行是底部"相反),这里不能翻转。
				// 历史 bug:此前照 GL 约定多翻一次,导致 Vulkan 抓图整幅纵向镜像
				// (曾误判为 WUI/投影问题;实测"翻转行序后与 OpenGL 只差 0.013%")。
				const uint8_t* source = pixels + static_cast<size_t>(y) * pending.Width * 4;
				for (uint32_t x = 0; x < pending.Width; ++x)
				{
					row[x * 3 + 0] = sourceIsBgra ? source[x * 4 + 2] : source[x * 4 + 0];
					row[x * 3 + 1] = source[x * 4 + 1];
					row[x * 3 + 2] = sourceIsBgra ? source[x * 4 + 0] : source[x * 4 + 2];
				}
				file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
			}
			readback->Unmap();
			WLD_CORE_INFO("[capture] wrote present target {0} ({1}x{2})",
				pending.Path.string(), pending.Width, pending.Height);
		}
	}

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
		// 截图是开发期低频操作:先把在飞工作等干净,避免"拷贝跑到本帧绘制之前"
		// (实测 Runtime 侧会因此抓到只有清屏色的旧内容)。
		m_Device->WaitIdle();

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
		// 行序:统一输出成**显示朝向**。场景纹理由 WUI 以 UV {0,1,1,-1} 贴到视口(上下翻转),
		// 所以附件第 0 行实际显示在画面底部;这里翻一次,截图就是用户看到的方向。
		// 两个后端用同一个约定(实测 GL/Vulkan 附件逐像素一致),因此双后端基线对比不受影响。
		std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
		for (uint32_t y = 0; y < height; ++y)
		{
			const uint8_t* source = pixels + static_cast<size_t>(height - 1 - y) * width * 4;
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

	int Renderer::DrainGLErrors(const char* tag)
	{
		if (s_BackendName == "vulkan")
			return 0;
		int count = 0;
		for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError())
		{
			++count;
			if (count <= 8)
				WLD_CORE_WARN("[gl-error] tag={0} code=0x{1}", tag ? tag : "?", error);
		}
		if (count > 8)
			WLD_CORE_WARN("[gl-error] tag={0} 共 {1} 个错误(只列出前 8 个)", tag ? tag : "?", count);
		return count;
	}
}


