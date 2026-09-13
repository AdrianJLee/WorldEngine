#pragma once
#include "World.h"

namespace World
{
	class RuntimeLayer : public Layer
	{
	public:
		RuntimeLayer();
		virtual ~RuntimeLayer() = default;
		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnUpdate(Timestep ts) override;
		virtual void OnImGuiRender() override;
		virtual void OnEvent(Event& event) override;
	private:
		bool OnWindowResize(WindowResizeEvent& e);
		void LoadScene();
		void CaptureFrameIfRequested();
	private:
		Ref<Scene> m_ActiveScene;
		Ref<SceneRenderer> m_SceneRenderer;
		uint64_t m_SceneTextureId = 0;
	};
}
