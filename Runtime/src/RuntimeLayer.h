#pragma once
#include "World.h"
#include "World/Gameplay/Runtime/GameHost.h"
// GameUI(M7b):UiHost 已提升到引擎(Editor 与 Runtime 共用同一实现)。
#include "World/UI/UiHost.h"

#include <memory>

namespace World
{
	namespace Plugins
	{
		class PluginManager;
	}

	// W1:宿主收敛——场景加载/更新/渲染统一走 Gameplay::GameHost(见 World/Gameplay/Runtime/GameHost.h)。
	class RuntimeLayer : public Layer
	{
	public:
		RuntimeLayer();
		// PLUG-T5:析构在 .cpp 定义(unique_ptr<Plugins::PluginManager> 的删除器需要完整类型)。
		virtual ~RuntimeLayer();
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
		// GameUI(M4):.wui 驱动的游戏 UI(加载 + 每帧布局/绘制/无障碍)。未启用时全部空操作。
		UiHost m_UiHost;
		// GameUI(M9):OnUpdate 里(Tick 之前)用平台输入组最小状态并路由 —— 跨帧记住按键沿。
		UiPlatformInputSampler m_UiInputSampler;
		uint64_t m_SceneTextureId = 0;
		// PLUG-T5:发行形态的插件管理器(<exe>/bin/plugins/*.dll,按发行清单的 shipped 顺序加载)。
		std::unique_ptr<Plugins::PluginManager> m_PluginManager;
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
