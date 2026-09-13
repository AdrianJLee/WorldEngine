#include "RuntimeLayer.h"
#include "GameHud.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Renderer/Renderer.h"
#include "World/ImGui/ImGuiLayer.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/WUI/WuiImGuiBackend.h"

#include <cstdlib>

namespace World
{
	RuntimeLayer::RuntimeLayer()
		:Layer("RuntimeLayer")
	{

	}
	void RuntimeLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();

		std::string moduleError;
		if (!Modules::GameModuleHost::LoadDefault(Application::Get().GetContext(), &moduleError))
			WLD_CORE_ERROR("Failed to load Game module: {0}", moduleError);

		m_SceneRenderer = CreateRef<SceneRenderer>();

		m_SceneRenderer->Init();


		LoadScene();

	}
	void RuntimeLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();
		if (m_ActiveScene)
			m_ActiveScene->OnRuntimeStop();
		m_ActiveScene.reset();
		if (m_SceneRenderer)
			m_SceneRenderer->Shutdown();
		m_SceneRenderer.reset();
	}
	void RuntimeLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
		if (m_ActiveScene)
		{
			Renderer2D::ResetStats();

			m_ActiveScene->OnUpdateRuntime(ts);
			// Scripts may remove the camera or its entity during the update.
			auto entity = m_ActiveScene->GetPrimaryCameraEntity();
			if (m_SceneRenderer && entity && entity.HasComponent<CameraComponent>() && entity.HasComponent<TransformComponent>())
			{
				auto& camera = entity.GetComponent<CameraComponent>().Camera;
				auto& transform = entity.GetComponent<TransformComponent>().Transform;

				m_SceneRenderer->BeginScene(m_ActiveScene.get(), SceneRendererOptions());
				m_SceneRenderer->SubmitScene(camera, transform);
				m_SceneRenderer->EndScene();

			}


		}

		// 渲染基线捕获(仅开发验证):WLD_CAPTURE_FRAMES=N 后读默认帧缓冲写 PPM。
		CaptureFrameIfRequested();
	}

	void RuntimeLayer::CaptureFrameIfRequested()
	{
		static int countdown = -1;
		if (countdown == -1)
		{
			const char* frames = std::getenv("WLD_CAPTURE_FRAMES");
			countdown = frames ? std::atoi(frames) : -2;
		}
		if (countdown > 0)
		{
			--countdown;
			return;
		}
		if (countdown != 0)
			return;
		countdown = -2;

		const char* pathEnv = std::getenv("WLD_CAPTURE_PATH");
		if (!pathEnv || !pathEnv[0])
			return;
		Renderer::CaptureFrame(pathEnv);
	}
	void RuntimeLayer::OnImGuiRender()
	{
		static Wui::WuiContext wuiContext;
		static Wui::WuiImGuiBackend wuiBackend;
		static bool fontsInitialized = false;
		if (!fontsInitialized)
		{
			wuiBackend.SetFonts(ImGuiLayer::GetDefaultFont(), ImGuiLayer::GetBoldFont(), ImGuiLayer::GetCjkFont());
			fontsInitialized = true;
		}
		Wui::WuiInputState input;
		if (wuiBackend.BeginFrame(input))
		{
			wuiContext.BeginFrame(input);
			// 只读查询必须走 const 路径:Running 场景上非 const GetRegistry()
			// 会触发结构写断言并抛异常。
			const Scene* activeScene = m_ActiveScene.get();
			const size_t entityCount = activeScene ? activeScene->GetRegistry().view<UUIDComponent>().size() : 0;
			DrawGameHud(wuiContext, entityCount);
			wuiContext.EndFrame();
			wuiBackend.Render(wuiContext.Commands(), wuiContext.OverlayCommands());
		}
		wuiBackend.EndFrame(wuiContext.Cursor());
	}
	void RuntimeLayer::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		// 拦截窗口 Resize 事件以动态更新相机投影矩阵
		dispatcher.Dispatch<WindowResizeEvent>(WLD_BIND_EVENT_FN(RuntimeLayer::OnWindowResize));
	}
	bool RuntimeLayer::OnWindowResize(WindowResizeEvent& e)
	{
		// 当独立游戏窗口缩放时，必须同步缩放 Scene 的摄像机 Aspect Ratio
		if (e.GetWidth() == 0 || e.GetHeight() == 0)
			return false; // 最小化时跳过

		if (m_ActiveScene)
			m_ActiveScene->OnViewportResize(e.GetWidth(), e.GetHeight());
		return false;
	}

	void RuntimeLayer::LoadScene()
	{
		Ref<Scene> tempScene = CreateRef<Scene>(Application::Get().GetContext());
		SceneSerializer serializer(tempScene);
		// 启动场景:项目 manifest 的 start_scene;无 manifest 时回退默认。
		std::string scenePath = "scenes/test.wd";
		std::filesystem::path manifestPath;
		if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			std::string error;
			World::Asset::ProjectManifest manifest;
			if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
			{
				scenePath = manifest.StartScene;
				World::Renderer::SetRequestedRenderer(manifest.Renderer);
			}
		}
		if (serializer.Deserialize(scenePath))
		{
			WLD_CORE_INFO("Scene loaded successfully from VFS or Disk!");
			bool hasCamera = false;
			for (auto handle : tempScene->GetRegistry().view<CameraComponent>())
			{
				(void)handle;
				hasCamera = true;
				break;
			}
			if (!hasCamera)
				WLD_CORE_WARN("Scene has no CameraComponent entity; nothing will be rendered. Add a camera in the editor (Hierarchy -> Create Camera).");
			m_ActiveScene = tempScene;

			uint32_t width = Application::Get().GetWindow().GetWidth();
			uint32_t height = Application::Get().GetWindow().GetHeight();
			m_ActiveScene->OnViewportResize(width, height);
			m_ActiveScene->OnRuntimeStart();
		}
		else
		{
			WLD_CORE_ERROR("Failed to load scene from VFS or Disk: {0}", scenePath);
		}
	}
}
