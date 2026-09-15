#pragma once
#include "World.h"
#include "World/Gameplay/GameHost.h"

namespace World
{
	// W1:宿主收敛——场景加载/更新/渲染统一走 Gameplay::GameHost(见 World/Gameplay/GameHost.h)。
	class RuntimeLayer : public Layer
	{
	public:
		RuntimeLayer();
		virtual ~RuntimeLayer() = default;
		virtual void OnAttach() override;
		virtual void OnDetach() override;
		virtual void OnUpdate(Timestep ts) override;
		virtual void OnUiFrame() override;
		virtual void OnEvent(Event& event) override;
	private:
		bool OnWindowResize(WindowResizeEvent& e);
		void LoadLevel();
		void CaptureFrameIfRequested();
	private:
		Gameplay::GameHost m_Host;
		Ref<SceneRenderer> m_SceneRenderer;
		uint64_t m_SceneTextureId = 0;
	};
}
