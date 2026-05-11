#include "wldpch.h"
#include "Application.h"

#include "World/Core/Log.h"
#include "World/Core/Timestep.h"
#include "World/Renderer/Renderer.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/Memory/MemoryTracker.h"

#include <GLFW/glfw3.h>


namespace World
{
	Application* Application::s_Instance = nullptr;
	Application::Application(const std::string& name)
	{
		WLD_PROFILE_FUNCTION();

		WLD_CORE_ASSERT(!s_Instance, "Appliicatiion already exists!");
		s_Instance = this;
		m_FrameAllocator = std::unique_ptr<DualTrackAllocator>(new DualTrackAllocator("FrameAllocator", 1024 * 1024 * 10)); // 10 MB
		m_EngineAllocator = std::unique_ptr<DualTrackAllocator>(new DualTrackAllocator("EngineAllocator", 1024 * 1024 * 50)); // 50 MB
		m_Window = std::unique_ptr<Window>(Window::Create(WindowProps(name)));

		m_Window->SetEventCallback(WLD_BIND_EVENT_FN(Application::OnEvent));

		Renderer::Init();
		m_ImGuiLayer = WLD_ENGINE_NEW(ImGuiLayer);

		PushOverlay(m_ImGuiLayer);

	}

	Application::~Application()
	{
		WLD_PROFILE_FUNCTION();
		JobSystem::Shutdown();
	}

	void Application::Run()
	{
		WLD_PROFILE_FUNCTION();

		while (m_Running)
		{
			WLD_PROFILE_SCOPE("RunLoop");

			MemoryTracker::Get(); // 先启动监控
			JobSystem::Init();    // 再启动线程池

			float time = (float)glfwGetTime();
			Timestep timestep = time - m_LastFrameTime;

			m_LastFrameTime = time;

			if (!m_Minimized)
			{
				{
					WLD_PROFILE_SCOPE("LayerStack OnUpdate");
					for (Layer* layer : m_LayerStack)
					{
						layer->OnUpdate(timestep);
					}
				}

				m_ImGuiLayer->Begin();
				{
					WLD_PROFILE_SCOPE("LayerStack OnImGuiRender");
					for (Layer* layer : m_LayerStack)
					{
						layer->OnImGuiRender();
					}
				}
				m_ImGuiLayer->End();
			}


			m_Window->OnUpdate();

			// Clean up frame allocator after each frame
			m_FrameAllocator->Reset();
		}
	}

	void Application::OnEvent(Event& e)
	{
		WLD_PROFILE_FUNCTION();
		EventDispatcher dispatcher(e);

		//Application::OnWindowClose(e)
		dispatcher.Dispatch<WindowCloseEvent>(WLD_BIND_EVENT_FN(Application::OnWindowClose));
		dispatcher.Dispatch<WindowResizeEvent>(WLD_BIND_EVENT_FN(Application::OnWindowResize));
		for (auto it = m_LayerStack.end(); it != m_LayerStack.begin(); )
		{
			//调用Layer层中的OnEvent
			(*--it)->OnEvent(e);
			if (e.m_Handled)
				break;
		}

		//WLD_CORE_TRACE("{0}", e.ToString());
	}

	bool Application::OnWindowClose(WindowCloseEvent& e)
	{
		WLD_PROFILE_FUNCTION();

		m_Running = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& e)
	{
		WLD_PROFILE_FUNCTION();

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
		WLD_PROFILE_FUNCTION();

		m_LayerStack.PushLayer(layer);
		layer->OnAttach();
	}
	void Application::PushOverlay(Layer* layer)
	{
		WLD_PROFILE_FUNCTION();

		m_LayerStack.PushOverLay(layer);
		layer->OnAttach();
	}
	void Application::Close()
	{
		m_Running = false;
	}

}