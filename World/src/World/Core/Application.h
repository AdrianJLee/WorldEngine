#pragma once
#include "World/Core/Window.h"
#include "World/Core/LayerStack.h"
#include "World/Events/ApplicationEvent.h"
#include "World/Events/Event.h"
#include "World/ImGui/ImGuiLayer.h"
#include "World/Core/Memory/DualTrackAllocator.h"

namespace World
{
	class Application
	{
	public:
		Application(const std::string& name);
		virtual ~Application();

		void Run();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);

		inline static Application& Get() { return *s_Instance; };
		inline Window& GetWindow() { return *m_Window; };

		void Close();

		ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }

		DualTrackAllocator& GetFrameAllocator() { return *m_FrameAllocator; }
		DualTrackAllocator& GetEngineAllocator() { return *m_EngineAllocator; }
	private:
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
	private:
		std::unique_ptr<DualTrackAllocator> m_FrameAllocator;
		std::unique_ptr<DualTrackAllocator> m_EngineAllocator;

		std::unique_ptr<Window> m_Window;
		ImGuiLayer* m_ImGuiLayer;

		float m_LastFrameTime = 0.0f;

		bool m_Running = true;
		bool m_Minimized = false;

		LayerStack m_LayerStack;

		static Application* s_Instance;


	};

	// To be defined in CLIENT
	Application* CreateApplication();
}
