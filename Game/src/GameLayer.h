#pragma once
#include "World.h"

namespace World
{
	class GameLayer : public Layer
	{
	public:
		GameLayer();
		virtual ~GameLayer() = default;
		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnUpdate(Timestep ts) override;
		virtual void OnImGuiRender() override;
		virtual void OnEvent(Event& event) override;
	private:
		bool OnWindowResize(WindowResizeEvent& e);
		void LoadScene();
	private:
		Ref<Scene> m_ActiveScene;
		Ref<SceneRenderer> m_SceneRenderer;
	};
}