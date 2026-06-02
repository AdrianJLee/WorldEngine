#include "RuntimeLayer.h"
#include "World/Renderer/SceneRenderer.h"

namespace World
{
	RuntimeLayer::RuntimeLayer()
		:Layer("RuntimeLayer")
	{

	}
	void RuntimeLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();

		std::string dllPath;
		// 获取当前 Runtime.exe 所在的绝对路径
		char exePathBuf[MAX_PATH];
		GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
		std::filesystem::path exePath = exePathBuf;
		std::filesystem::path exeDir = exePath.parent_path(); // 提取 exe 所在目录

		// 拼接打包后的预期路径：exe 同级目录下的 Game.dll
		std::filesystem::path packagedDllPath = exeDir / "Game.dll";
		WLD_INFO("Looking for Game.dll at: {0}", packagedDllPath.string());
		// 检查打包版路径是否存在
		if (std::filesystem::exists(packagedDllPath))
		{
			dllPath = packagedDllPath.string();
		}
		else
		{
			// 4. 兜底：如果在开发环境运行，退回 CMake 生成的工作区路径
			dllPath = std::string(WLD_OUTPUT_DIR) + "bin/" + WLD_BUILD_TYPE + "Game/" + WLD_BUILD_TYPE + "Game.dll";
		}

		HMODULE gameModule = LoadLibraryA(dllPath.c_str());

		if (gameModule)
		{
			WLD_CORE_INFO("Successfully loaded Game.dll from {0}", dllPath);
			typedef void(*InitGameDLLFunc)(World::Application*);
			InitGameDLLFunc initFunc = (InitGameDLLFunc)GetProcAddress(gameModule, "OnInitGameDLL");

			if (initFunc)
			{
				initFunc(&World::Application::Get());
			}
			else
			{
				WLD_CORE_ERROR("Failed to find InitGameDLL function in Game.dll!");
			}

			typedef void* (*GetRegistryFunc)();
			GetRegistryFunc getGameTypeRegistry = (GetRegistryFunc)GetProcAddress(gameModule, "GetGameTypeRegistry");

			if (getGameTypeRegistry)
			{
				// 拿到对面的 TypeRegistry 指针
				World::TypeRegistry* gameRegistry = static_cast<World::TypeRegistry*>(getGameTypeRegistry());

				// 将对面的所有脚本、属性数据，倒灌到当前 Editor 的单例中！
				World::TypeRegistry::Get().MergeFrom(*gameRegistry);
			}
			else
			{
				WLD_CORE_ERROR("Failed to find GetGameTypeRegistry function in Game.dll!");
			}
		}
		else
		{
			WLD_CORE_ERROR("Failed to load Game.dll!");
		}

		m_SceneRenderer = CreateRef<SceneRenderer>();

		m_SceneRenderer->Init();


		LoadScene();

	}
	void RuntimeLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();
		m_SceneRenderer->Shutdown();
	}
	void RuntimeLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
		if (m_ActiveScene)
		{
			Renderer2D::ResetStats();

			auto entity = m_ActiveScene->GetPrimaryCameraEntity();
			if (entity)
			{
				m_ActiveScene->OnUpdateRuntime(ts);
				auto& camera = entity.GetComponent<CameraComponent>().Camera;
				auto& transform = entity.GetComponent<TransformComponent>().Transform;

				m_SceneRenderer->BeginScene(m_ActiveScene.get(), SceneRendererOptions());
				m_SceneRenderer->SubmitScene(camera, transform);
				m_SceneRenderer->EndScene();

			}
			else
			{
				m_ActiveScene->OnUpdateRuntime(ts);
			}


		}
	}
	void RuntimeLayer::OnImGuiRender()
	{

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

		m_ActiveScene->OnViewportResize(e.GetWidth(), e.GetHeight());
		return false;
	}

	void RuntimeLayer::LoadScene()
	{
		Ref<Scene> tempScene = CreateRef<Scene>();
		SceneSerializer serializer(tempScene);
		// During the cook process, scenes could be packed or placed in content folder.
		// Assuming "Resource/Scenes/TestScene.wdscene" relative path is maintained or packed in pak.
		std::string scenePath = "scenes/PhysicalTest.wd";
		if (serializer.Deserialize(scenePath))
		{
			WLD_CORE_INFO("Scene loaded successfully from VFS or Disk!");
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