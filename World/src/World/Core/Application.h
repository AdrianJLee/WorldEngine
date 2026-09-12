#pragma once
#include "World/Core/Window.h"
#include "World/Core/WorldContext.h"
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
		Application(const std::string& name, WorldContext& context);
		virtual ~Application();
		static void SetInstance(Application* instance) { s_Instance = instance; }
		void Run();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);

		static Application& Get();
		WorldContext& GetContext() { return m_Context; }
		const WorldContext& GetContext() const { return m_Context; }
		inline Window& GetWindow() { return *m_Window; };

		void Close();

		ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer; }

		DualTrackAllocator& GetFrameAllocator() { return *m_FrameAllocator; }
		DualTrackAllocator& GetEngineAllocator() { return *m_EngineAllocator; }
	private:
		void Shutdown();
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);
	private:
		WorldContext& m_Context;
		std::unique_ptr<DualTrackAllocator> m_FrameAllocator;
		std::unique_ptr<DualTrackAllocator> m_EngineAllocator;

		std::unique_ptr<Window> m_Window;
		ImGuiLayer* m_ImGuiLayer = nullptr;

		float m_LastFrameTime = 0.0f;

		bool m_Running = true;
		bool m_Minimized = false;
		bool m_Shutdown = false;

		LayerStack m_LayerStack;

		static Application* s_Instance;


	};

	// To be defined in CLIENT
	Application* CreateApplication(WorldContext& context);
}
