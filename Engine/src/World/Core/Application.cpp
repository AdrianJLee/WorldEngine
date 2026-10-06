#include "wldpch.h"
#include "World/Core/Application.h"

#include "World/Core/Log.h"
#include "World/Core/Input.h"
#include "World/Core/Timestep.h"
#include "World/Asset/ProjectMount.h"
#include "World/Renderer/Renderer.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Events/KeyEvent.h"
#include "World/Events/MouseEvent.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/WUI/WuiRhiBackend.h"

#include <GLFW/glfw3.h>


namespace World
{
	Application* Application::s_Instance = nullptr;

	namespace
	{
		// JOBSYS:WLD_JOB_STATS=1 时周期性打印任务系统统计(默认关,零开销)。
		const bool s_JobStatsEnabled = []
		{
			const char* value = std::getenv("WLD_JOB_STATS");
			return value != nullptr && value[0] != '\0' && value[0] != '0';
		}();
		uint64_t s_LastJobStatsStamp = ~0ull;
	}

	Application& Application::Get()
	{
		return *s_Instance;
	}
	Application::Application(const std::string& name, WorldContext& context)
		: m_Context(context)
	{
		WLD_TRACE_FUNCTION();

		WLD_CORE_ASSERT(!s_Instance, "Appliicatiion already exists!");
		s_Instance = this;
		m_EngineAllocator = std::unique_ptr<DualTrackAllocator>(new DualTrackAllocator("EngineAllocator", 1024 * 1024 * 50)); // 50 MB
		// 开发/验证钩子:WLD_WINDOW_SIZE="宽x高" 覆盖主窗口尺寸。编辑器 Play 与打包 Runtime 的
		// 一致性验收需要两边同分辨率(编辑器的场景目标尺寸由视口面板决定,不是窗口尺寸)。
		uint32_t windowWidth = 1280, windowHeight = 720;
		if (const char* sizeEnv = std::getenv("WLD_WINDOW_SIZE"))
		{
			unsigned int w = 0, h = 0;
			if (std::sscanf(sizeEnv, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64)
			{
				windowWidth = w;
				windowHeight = h;
			}
			else
				WLD_CORE_WARN("WLD_WINDOW_SIZE ignored (expected \"WxH\", got '{0}')", sizeEnv);
		}
		m_Window = std::unique_ptr<Window>(Window::Create(WindowProps(name, windowWidth, windowHeight)));
		m_Title = name;

		m_Window->SetEventCallback(WLD_BIND_EVENT_FN(Application::OnEvent));

		// 内容挂载必须早于渲染器初始化:发行形态下管线创建即解析着色器烘焙产物。
		Asset::MountProjectContent(m_Context);

		Renderer::Init();

		ScriptEngine::Init();
	}

	Application::~Application()
	{
		WLD_TRACE_FUNCTION();
		Shutdown();
	}

	void Application::RecreateWindow()
	{
		if (!m_Window)
			return;
		int x = 0, y = 0;
		m_Window->GetPosition(&x, &y);
		const uint32_t width = m_Window->GetWidth();
		const uint32_t height = m_Window->GetHeight();
		const bool frameless = m_Window->IsFrameless();
		const bool vsync = m_Window->IsVsync();

		// 旧窗口连同其 GL 上下文/原生句柄一起销毁:后端切换后重新按当前后端创建。
		m_Window.reset();
		m_Window = std::unique_ptr<Window>(Window::Create(
			WindowProps(m_Title, width, height, frameless)));
		m_Window->SetEventCallback(WLD_BIND_EVENT_FN(Application::OnEvent));
		m_Window->SetVsync(vsync);
		if (frameless)
			m_Window->SetFrameless(true);
		m_Window->SetPosition(x, y);
		m_Window->MakeCurrent();
		Renderer::OnWindowResize(width, height);
		WLD_CORE_INFO("Main window recreated for backend '{0}'", Renderer::GetBackendName());
	}

	void Application::Shutdown()
	{
		if (m_Shutdown) return;
		m_Shutdown = true;
		m_Running = false;
		// 关闭流程第一步:排空 GPU,避免 OnDetach/资源释放销毁仍在被在飞命令引用的对象
		// (VUID-vkDestroyFramebuffer-00892 / vkFreeCommandBuffers-00047 等)。
		Renderer::WaitForGpu();
		// OnDetach releases scene instances and joins layer-owned work first.
		m_LayerStack.DetachAll();
		// Run the allocator resets while their Lua state and graphics context
		// are still alive.
		if (m_EngineAllocator) m_EngineAllocator->Reset();
		FrameArena::Shutdown();
		// 泄漏/归因报告由 Profiling::Telemetry::Shutdown() -> MemoryTrack::Shutdown() 输出:
		// 它跑在 Reset/Shutdown **之前**,所以真泄漏不会被清空掩盖(旧实现顺序是反的)。
		Profiling::Telemetry::Shutdown();
		ScriptEngine::Shutdown();
		JobSystem::Shutdown();
		// GL 语义:窗口(=GL 上下文)一旦销毁,RHI 里所有 GL 资源析构都会在没有上下文的
		// 情况下调用 glDelete*(访问违例,实测退出码 0xC0000005)。因此必须在 m_Window.reset()
		// 之前释放渲染器/设备;Vulkan 侧同样受益(设备与呈现目标先于窗口释放)。
		Renderer::Shutdown();
		m_Window.reset();
		s_Instance = nullptr;
	}

	void Application::Run()
	{
		WLD_TRACE_FUNCTION();
		if (m_Shutdown) return;
		JobSystem::Init();    // 再启动线程池
		Profiling::Telemetry::Init();    // 遥测:帧统计常开、作用域追踪按需

		while (m_Running)
		{
			WLD_TRACE_SCOPE("RunLoop");
			// 帧边界:帧统计(常开)与采集状态机都由这一对驱动。
			Profiling::Telemetry::BeginFrame();

			float time = (float)glfwGetTime();
			Timestep timestep = time - m_LastFrameTime;

			m_LastFrameTime = time;

			if (!m_Minimized)
			{
				// P4-CLEANUP:帧相位追踪(诊断用,默认关)。用途:主线程"卡住但不是崩溃"时,
				// 日志最后一条相位就是凶手(2026-09-21 device-lost 挂起事故就是靠它定位的)。
				// `WLD_FRAME_TRACE=1` 打开。
				static const bool frameTrace = [] {
					const char* env = std::getenv("WLD_FRAME_TRACE");
					return env && env[0] != '\0' && env[0] != '0';
				}();
				const auto tracePhase = [](const char* phase) {
					if (frameTrace)
						WLD_CORE_INFO("[frame-trace] {0}", phase);
				};
				tracePhase("Renderer::BeginFrame");
				Renderer::BeginFrame();
				tracePhase("Renderer::BeginFramePresent");
				Renderer::BeginFramePresent();
				{
					WLD_TRACE_SCOPE("LayerStack OnUpdate");
					for (Layer* layer : m_LayerStack)
					{
						tracePhase(("OnUpdate " + std::string(layer->GetName())).c_str());
						layer->OnUpdate(timestep);
					}
				}

				{
					WLD_TRACE_SCOPE("LayerStack UiFrame");
					for (Layer* layer : m_LayerStack)
					{
						tracePhase(("OnUiFrame " + std::string(layer->GetName())).c_str());
						layer->OnUiFrame();
					}
				}
				// WP5:本帧滚轮已被玩法输入采样消费(采样在层更新的 OnUpdate 里),
				// 清掉累积值;平台轮询(OnUpdate)随后只累积下一帧的增量。
				Input::ResetScrollDelta();
				tracePhase("Renderer::EndFramePresent");
				Renderer::EndFramePresent();
				tracePhase("Renderer::EndFrame");
				Renderer::EndFrame();
			}


			m_Window->OnUpdate();

			// 帧内临时分配(每线程 arena):工作线程已空闲,统一回卷。
			FrameArena::ResetAll();

			// 帧收尾(采集满额时在此落盘:落盘只发生在采集结束之后)。
			Profiling::Telemetry::EndFrame();

			// JOBSYS:`WLD_JOB_STATS=1` 时每 ~2 秒打一行任务系统统计 —— 用于回答
			// "并行到底有没有发生"(Executed 不涨 = 没有任何并行路径被触发)。
			if (s_JobStatsEnabled)
			{
				const uint64_t stamp = static_cast<uint64_t>(time) / 2;
				if (stamp != s_LastJobStatsStamp)
				{
					s_LastJobStatsStamp = stamp;
					WLD_CORE_INFO("[jobs] {0}", JobSystem::DescribeStats());
				}
			}
		}
		Shutdown();
	}

	void Application::OnEvent(Event& e)
	{
		WLD_TRACE_FUNCTION();
		EventDispatcher dispatcher(e);

		// WUI RHI 后端输入:GLFW 事件先喂给输入收集器。
		dispatcher.Dispatch<KeyPressedEvent>([](KeyPressedEvent& ev)
			{ Wui::WuiRhiBackend::FeedKey(static_cast<uint32_t>(ev.GetKeyCode()), true, ev.GetRepeatCount() > 0); return false; });
		dispatcher.Dispatch<KeyReleasedEvent>([](KeyReleasedEvent& ev)
			{ Wui::WuiRhiBackend::FeedKey(static_cast<uint32_t>(ev.GetKeyCode()), false, false); return false; });
		dispatcher.Dispatch<KeyTypedEvent>([](KeyTypedEvent& ev)
			{ Wui::WuiRhiBackend::FeedChar(static_cast<uint32_t>(ev.GetKeyCode())); return false; });
		dispatcher.Dispatch<MouseButtonPressedEvent>([](MouseButtonPressedEvent& ev)
			{ Wui::WuiRhiBackend::FeedMouseButton(ev.GetMouseButton(), true); return false; });
		dispatcher.Dispatch<MouseButtonReleasedEvent>([](MouseButtonReleasedEvent& ev)
			{ Wui::WuiRhiBackend::FeedMouseButton(ev.GetMouseButton(), false); return false; });
		dispatcher.Dispatch<MouseMovedEvent>([](MouseMovedEvent& ev)
			{ Wui::WuiRhiBackend::FeedMouseMove(ev.GetX(), ev.GetY()); return false; });
		dispatcher.Dispatch<MouseScrolledEvent>([](MouseScrolledEvent& ev)
			{
				Wui::WuiRhiBackend::FeedMouseScroll(ev.GetXOffset(), ev.GetYOffset());
				// WP5:同一份平台事件也喂给玩法输入(滚轮此前只到 UI,游戏侧读不到)。
				Input::AccumulateScroll(ev.GetXOffset(), ev.GetYOffset());
				return false;
			});

		dispatcher.Dispatch<WindowResizeEvent>(WLD_BIND_EVENT_FN(Application::OnWindowResize));
		for (auto it = m_LayerStack.end(); it != m_LayerStack.begin(); )
		{
			//调用Layer层中的OnEvent
			(*--it)->OnEvent(e);
			if (e.m_Handled)
				break;
		}

		// 窗口关闭后置：先让 Layer 有机会否决（标记 handled），未被处理才真正退出主循环。
		if (e.GetEventType() == WindowCloseEvent::GetStaticType() && !e.m_Handled)
		{
			dispatcher.Dispatch<WindowCloseEvent>(WLD_BIND_EVENT_FN(Application::OnWindowClose));
		}

		//WLD_CORE_TRACE("{0}", e.ToString());
	}

	bool Application::OnWindowClose(WindowCloseEvent& e)
	{
		WLD_TRACE_FUNCTION();

		m_Running = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& e)
	{
		WLD_TRACE_FUNCTION();

		if (e.GetWidth() == 0 || e.GetHeight() == 0)
		{
			m_Minimized = true;
			return false;
		}
		m_Minimized = false;
		Renderer::OnWindowResize(e.GetWidth(), e.GetHeight());
		return false;
	}

	void Application::PushLayer(Layer* layer)
	{
		WLD_TRACE_FUNCTION();

		m_LayerStack.PushLayer(layer);
	}
	void Application::PushOverlay(Layer* layer)
	{
		WLD_TRACE_FUNCTION();

		m_LayerStack.PushOverLay(layer);
	}
	void Application::Close()
	{
		m_Running = false;
	}

}
