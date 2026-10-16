#pragma once
#include "World.h"
#include "World/Events/KeyEvent.h"
#include "World/Gameplay/Runtime/GameHost.h"
// GameUI(M7b):UiHost 已提升到引擎(Editor 与 Runtime 共用同一实现)。
#include "World/UI/UiHost.h"

#include <map>
#include <memory>
#include <string>

namespace World
{
	namespace Plugins
	{
		class PluginManager;
	}

	void SetRuntimeAiControlPort(int port);
	int GetRuntimeAiControlPort();

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
		bool OnKeyPressed(KeyPressedEvent& e);
		void CaptureFrameIfRequested();
		// W3c:UI 阶段的开发钩子(脚本点击注入 / present 抓图),语义见 RuntimeLayer.cpp。
		void ApplyDevUiActions();
		// W3c:UI 帧末的开发钩子收尾(抓图落盘确认 + 自动退出),必须在 FlushPresentCaptures 之后调用。
		void FinishDevUiFrame();

		// Runtime AI 控制通道 (--ai-control=<port>)
		void StartAiControlServer();
		void StopAiControlServer();
		void PumpAiControlRequests();
		bool ExecuteRuntimeAiCommand(const std::string& cmd,
			const std::map<std::string, std::string>& args,
			std::string& result,
			std::string& error);
	private:
		Gameplay::GameHost m_Host;
		Ref<SceneRenderer> m_SceneRenderer;
		UiHost m_UiHost;
		UiPlatformInputSampler m_UiInputSampler;
		uint64_t m_SceneTextureId = 0;
		std::unique_ptr<Plugins::PluginManager> m_PluginManager;
		struct RuntimeAiServerState;
		std::unique_ptr<RuntimeAiServerState> m_AiServer;
		float m_LastFrameDeltaSeconds = 1.0f / 60.0f;
		uint32_t m_UiFrame = 0;
		bool m_ScriptUiDrawn = false;
		bool m_DevUiCapturePending = false;
		uint32_t m_DevUiArmedFrame = 0;
		uint32_t m_PendingWidth = 0;
		uint32_t m_PendingHeight = 0;
		float m_ResizeDelay = 0.0f;
		bool m_ShowGameHud = false;
	};
}