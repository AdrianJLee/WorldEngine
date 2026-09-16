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
		void CaptureFrameIfRequested();
	private:
		Gameplay::GameHost m_Host;
		Ref<SceneRenderer> m_SceneRenderer;
		uint64_t m_SceneTextureId = 0;
		// 窗口尺寸跟随(0.1s 节流):尺寸稳定后再重建场景渲染目标。
		uint32_t m_PendingWidth = 0;
		uint32_t m_PendingHeight = 0;
		float m_ResizeDelay = 0.0f;
	};
}
