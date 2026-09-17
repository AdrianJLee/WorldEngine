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
		// W3c:UI 阶段的开发钩子(脚本点击注入 / present 抓图),语义见 RuntimeLayer.cpp。
		void ApplyDevUiActions();
		// W3c:UI 帧末的开发钩子收尾(抓图落盘确认 + 自动退出),必须在 FlushPresentCaptures 之后调用。
		void FinishDevUiFrame();
	private:
		Gameplay::GameHost m_Host;
		Ref<SceneRenderer> m_SceneRenderer;
		uint64_t m_SceneTextureId = 0;
		// UI 阶段帧计数器:开发钩子(WLD_UI_CLICK_FRAME / WLD_CAPTURE_PRESENT_FRAME)的
		// 帧号口径就是它(第 1 帧 = 1),与 Editor 侧"帧内钩子"同一读法。
		uint32_t m_UiFrame = 0;
		// 本次运行的脚本 UI 是否至少成功绘制过一次(报告与自动化断言用)。
		bool m_ScriptUiDrawn = false;
		// 抓图挂起标记与自动退出的生效帧号(抓图确认落盘后,在 >= 该帧的 UI 帧末退出)。
		bool m_DevUiCapturePending = false;
		uint32_t m_DevUiArmedFrame = 0;
		// 窗口尺寸跟随(0.1s 节流):尺寸稳定后再重建场景渲染目标。
		uint32_t m_PendingWidth = 0;
		uint32_t m_PendingHeight = 0;
		float m_ResizeDelay = 0.0f;
	};
}
