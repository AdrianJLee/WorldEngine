#include "World/Gameplay/GameHost.h"

#include "World/Core/Application.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/SceneSerializer.h"

#include <memory>
#include <utility>

namespace World::Gameplay
{
	namespace
	{
		// 运行期脚本可能删掉相机实体或相机组件,所以每帧重新取,不做缓存。
		bool ResolvePrimaryCamera(const Ref<Scene>& scene, CameraComponent*& camera, TransformComponent*& transform)
		{
			camera = nullptr;
			transform = nullptr;
			if (!scene)
				return false;

			Entity entity = scene->GetPrimaryCameraEntity();
			if (!entity || !entity.HasComponent<CameraComponent>() || !entity.HasComponent<TransformComponent>())
				return false;

			camera = &entity.GetComponent<CameraComponent>();
			transform = &entity.GetComponent<TransformComponent>();
			return true;
		}
	}

	GameHost::GameHost() = default;

	GameHost::~GameHost()
	{
		Shutdown();
	}

	void GameHost::Init(const GameAppDesc& desc)
	{
		if (m_Initialized)
			return;

		// 有 Application 时复用它的 WorldContext;无 Application(测试/工具)时自持一份。
		if (!Application::HasInstance() && !m_OwnedContext)
			m_OwnedContext = std::make_unique<WorldContext>();

		if (!GameApp::Exists())
		{
			GameApp::Create(desc);
			m_CreatedSession = true;
		}

		GameApp& app = GameApp::Get();
		// W1:权威模拟仍是 Scene::OnUpdateRuntime(脚本 + 物理)。W5 把物理/脚本搬进固定步长,
		// 因此这里先按"可变步长驱动场景"接线,行为与改造前 RuntimeLayer 完全一致。
		app.SetPhaseCallbacks(
			GameApp::PhaseCallback(),
			[this](Timestep ts)
			{
				if (m_Scene && m_RuntimeStarted)
					m_Scene->OnUpdateRuntime(ts);
			},
			GameApp::PhaseCallback());

		m_Initialized = true;
	}

	void GameHost::Shutdown()
	{
		StopRuntime();
		m_Scene.reset();
		m_SceneRenderer.reset();
		m_LoadedPath.clear();

		if (m_Initialized)
		{
			if (GameApp* app = GameApp::TryGet())
				app->SetPhaseCallbacks({}, {}, {});
			m_Initialized = false;
		}

		// 会话由宿主持有:只销毁本宿主创建的会话。
		if (m_CreatedSession)
		{
			GameApp::Shutdown();
			m_CreatedSession = false;
		}
		m_OwnedContext.reset();
	}

	void GameHost::SetRenderer(const Ref<SceneRenderer>& renderer)
	{
		m_SceneRenderer = renderer;
	}

	bool GameHost::LoadLevel(const std::string& scenePath, bool startRuntime)
	{
		if (scenePath.empty())
		{
			WLD_CORE_ERROR("GameHost::LoadLevel: empty scene path");
			return false;
		}

		WorldContext* context = Application::HasInstance() ? &Application::Get().GetContext() : m_OwnedContext.get();
		if (!context)
		{
			WLD_CORE_ERROR("GameHost::LoadLevel('{0}') needs a WorldContext; call Init() first", scenePath);
			return false;
		}

		// 先在新场景里加载:失败时保持当前场景不变,调用方可直接重试。
		Ref<Scene> scene = CreateRef<Scene>(*context);
		SceneSerializer serializer(scene);
		if (!serializer.Deserialize(scenePath))
		{
			WLD_CORE_ERROR("GameHost::LoadLevel failed: '{0}' ({1})", scenePath, serializer.GetLastError());
			return false;
		}

		StopRuntime();
		m_Scene = scene;
		m_LoadedPath = scenePath;
		ApplyViewportToScene();

		bool hasCamera = false;
		{
			const Scene& constScene = *m_Scene;
			for (const auto entity : constScene.GetRegistry().view<CameraComponent>())
			{
				(void)entity;
				hasCamera = true;
				break;
			}
		}
		if (!hasCamera)
		{
			WLD_CORE_WARN("Scene '{0}' has no CameraComponent entity; nothing will be rendered", scenePath);
		}

		if (startRuntime)
			StartRuntime();

		WLD_CORE_INFO("GameHost loaded level '{0}' (runtime {1})", scenePath, startRuntime ? "started" : "not started");
		return true;
	}

	void GameHost::SetScene(const Ref<Scene>& scene, bool startRuntime)
	{
		StopRuntime();
		m_Scene = scene;
		m_LoadedPath.clear();
		ApplyViewportToScene();

		if (startRuntime)
			StartRuntime();
	}

	void GameHost::StartRuntime()
	{
		if (!m_Scene || m_RuntimeStarted)
			return;

		m_Scene->OnRuntimeStart();
		m_RuntimeStarted = true;
	}

	void GameHost::StopRuntime()
	{
		if (!m_Scene || !m_RuntimeStarted)
			return;

		m_Scene->OnRuntimeStop();
		m_RuntimeStarted = false;
	}

	void GameHost::Tick(Timestep frameTime, bool render)
	{
		if (!m_Initialized)
			return;

		GameApp::Get().Tick(frameTime);

		if (render)
			SubmitSceneRender();
	}

	void GameHost::SubmitSceneRender()
	{
		// 未注入渲染器 = 无头宿主(测试/服务端);未进运行时 = 编辑器用自己的视口渲染。
		if (!m_Scene || !m_SceneRenderer || !m_RuntimeStarted)
			return;

		CameraComponent* camera = nullptr;
		TransformComponent* transform = nullptr;
		if (!ResolvePrimaryCamera(m_Scene, camera, transform))
			return;

		m_SceneRenderer->BeginScene(m_Scene.get(), SceneRendererOptions());
		m_SceneRenderer->SubmitScene(camera->Camera, transform->Transform);
		m_SceneRenderer->EndScene();
	}

	void GameHost::ApplyViewportToScene()
	{
		if (!m_Scene)
			return;

		uint32_t width = 0;
		uint32_t height = 0;
		if (Application::HasInstance())
		{
			width = Application::Get().GetWindow().GetWidth();
			height = Application::Get().GetWindow().GetHeight();
		}
		else if (m_SceneRenderer)
		{
			width = m_SceneRenderer->GetWidth();
			height = m_SceneRenderer->GetHeight();
		}

		if (width == 0 || height == 0)
		{
			// 无窗口宿主(单测/工具)的默认视口,避免相机宽高比变成 0。
			width = 1280;
			height = 720;
		}

		m_Scene->OnViewportResize(width, height);
	}
}
