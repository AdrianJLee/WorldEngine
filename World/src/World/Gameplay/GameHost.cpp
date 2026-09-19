#include "World/Gameplay/GameHost.h"
#include "World/Core/Input.h"
#include "World/Core/PhysicsSettings.h"
#include "World/Gameplay/InputMap.h"

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

		// P4-1:物理固定步长来自项目清单(physics.fixed_step_hz)。这里按工作目录重新装载一次,
		// 保证"改了清单 → 下次进入 Play/Runtime 生效"(Renderer3D::Init 只在启动/设备重建时装载)。
		// 区间钳制在 GameApp 构造函数里统一做(1..240)。
		PhysicsSettings::LoadFromProject(std::filesystem::current_path());
		GameAppDesc effectiveDesc = desc;
		effectiveDesc.FixedStepHz = PhysicsSettings::Get().FixedStepHz;

		// 有 Application 时复用它的 WorldContext;无 Application(测试/工具)时自持一份。
		if (!Application::HasInstance() && !m_OwnedContext)
			m_OwnedContext = std::make_unique<WorldContext>();

		if (!GameApp::Exists())
		{
			GameApp::Create(effectiveDesc);
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

		InstallLevelServices(desc);

		// W8-3:会话级存档服务接线 —— 场景来源就是本宿主持有的当前场景(编辑器/Runtime 共用)。
		if (!app.Saves())
			app.CreateSaveService([this] { return m_Scene.get(); });

		m_Initialized = true;
	}

	void GameHost::InstallLevelServices(const GameAppDesc& desc)
	{
		LevelService& levels = GameApp::Get().Levels();

		// 场景构造留在宿主侧(Scene 的 WorldContext 与注册表都必须在主线程使用)。
		levels.SetSceneLoader([this](const std::string& scenePath, std::string* error) -> Ref<Scene>
		{
			WorldContext* context = Application::HasInstance() ? &Application::Get().GetContext() : m_OwnedContext.get();
			if (!context)
			{
				if (error) *error = "no WorldContext available for level loading";
				return nullptr;
			}
			Ref<Scene> scene = CreateRef<Scene>(*context);
			SceneSerializer serializer(scene);
			if (!serializer.Deserialize(scenePath))
			{
				if (error) *error = serializer.GetLastError().empty()
					? ("failed to deserialize '" + scenePath + "'") : serializer.GetLastError();
				return nullptr;
			}
			return scene;
		});

		levels.SetProgressCallback([this](const LevelLoadProgress& report)
		{
			m_LastLevelProgress = report;
			if (report.State == LevelLoadState::Failed)
			{
				WLD_CORE_ERROR("GameHost: level '{0}' failed: {1}", report.LevelId, report.Error);
				return;
			}
			// 关卡就绪:宿主接管场景并启动运行时(替换或叠加都由 LevelService 的场景栈决定)。
			if (report.State == LevelLoadState::Idle && report.Progress >= 1.0f)
			{
				if (Ref<Scene> scene = GameApp::Get().Levels().FindScene(report.LevelId))
					SetScene(scene, true);
			}
		});

		// 清单查找顺序:内容根父目录(项目根)→ 内容根 → 当前工作目录;找不到就只用路径加载。
		LevelList list;
		std::string error;
		const std::filesystem::path candidates[] = {
			desc.ContentRoot.parent_path() / "levels.welevel",
			desc.ContentRoot / "levels.welevel",
			std::filesystem::current_path() / "levels.welevel",
		};
		for (const std::filesystem::path& candidate : candidates)
		{
			if (candidate.empty() || !std::filesystem::exists(candidate))
				continue;
			if (LevelList::Load(candidate, &list, &error))
			{
				WLD_CORE_INFO("GameHost: level list '{0}' loaded ({1} level(s))",
					candidate.generic_string(), list.Entries().size());
				levels.SetLevelList(std::move(list));
				return;
			}
			WLD_CORE_WARN("GameHost: level list '{0}' rejected: {1}", candidate.generic_string(), error);
		}
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

		// W2:清单里存在同一场景的关卡时优先走关卡服务(统一走加载状态机/进度),
		// 由 Tick 内的 Pump 完成激活;清单缺失或不匹配时退回直接路径加载。
		if (startRuntime)
		{
			const LevelService& levels = GameApp::Get().Levels();
			for (const LevelEntry& entry : levels.GetLevelList().Entries())
			{
				if (entry.ScenePath != scenePath)
					continue;
				if (LoadLevelById(entry.Id))
					return true;
				break;
			}
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

	bool GameHost::LoadLevelById(const std::string& levelId, bool additive)
	{
		if (!m_Initialized)
		{
			WLD_CORE_ERROR("GameHost::LoadLevelById('{0}') called before Init()", levelId);
			return false;
		}
		// 只登记请求:下一次 Tick 的 Pump 完成读取/反序列化/激活(进度经 GetLastLevelProgress 可查)。
		return GameApp::Get().Levels().RequestLoad(levelId, additive);
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

		// W7-3:把引擎轮询到的键盘/鼠标状态喂给 InputService(只喂映射里真正用到的绑定),
		// 再生成只读快照供并行安全系统读取(W7-4)。
		{
			Gameplay::InputService& input = GameApp::Get().Input();
			const Gameplay::InputMap& mapInput = input.GetMap();
			if (input.GetPlayerCount() == 0)
				input.SetPlayerCount(1);
			// 无 Application/窗口的宿主(测试、工具、专用服务器)没有 GLFW 句柄:
			// 这里必须保留调用方直接喂进 InputService 的状态,不能去轮询一个不存在的窗口
			// (真实故障:headless 下 Input::IsKeyPressed 走 Application::Get() 空实例 →
			//  glfwGetKey 拿到野指针,0xC0000005;在"有实例但无真实窗口"时也会把注入状态覆盖成抬起)。
			const bool hasEngineWindow = Application::HasInstance();
			if (hasEngineWindow)
			{
				for (const Gameplay::InputAction& action : mapInput.Actions())
					for (const Gameplay::InputBinding& binding : action.Bindings)
					{
						const bool down = binding.Device == Gameplay::InputDevice::Mouse
							? Input::IsMouseButtonPressed(binding.Code)
							: (binding.Device == Gameplay::InputDevice::Key
								? Input::IsKeyPressed(binding.Code) : false);
						input.SetKeyState(0, binding.Device, binding.Code, down);
					}
			}
			input.BuildSnapshot(0);
		}

		// W2:推进排队的关卡加载(读盘 → 反序列化 → 激活;激活时进度回调会把场景交给宿主)。
		GameApp::Get().Levels().Pump();
		GameApp::Get().Tick(frameTime);
		// W7-6/P2 W3b:帧末把"当前按下"滚成"上一帧按下",下一帧的 Pressed/Released 才有真实边沿。
		// 必须在本帧脚本/玩法读取之后调用,否则同一帧内刚按下的键会被立刻滚走。
		GameApp::Get().Input().EndFrame();

		// D5c-4a:缓存本帧秒数给 SubmitSceneRender 用(骨骼动画步长)。
		m_LastTickSeconds = frameTime.GetSeconds();
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
		// D5c-4a:骨骼动画步长(与编辑器同一口径:推进组件 Time)。
		m_SceneRenderer->SetDeltaSeconds(m_LastTickSeconds);
		m_SceneRenderer->SubmitScene(camera->Camera, transform->Transform);
		m_SceneRenderer->EndScene();

		// 诊断(WLD_TRACE_HOST=1):确认"每帧都在提交"以及相机矩阵是否退化 ——
		// Runtime 抓图只剩清屏色时,这两者是最可能的断点。
		if (std::getenv("WLD_TRACE_HOST"))
		{
			static uint64_t calls = 0;
			++calls;
			if (calls <= 4 || calls % 120 == 0)
			{
				const glm::mat4& projection = camera->Camera.GetProjectionMatrix();
				WLD_CORE_INFO("[host] SubmitSceneRender call#{0} renderer={9} target={1}x{2} projDiag=({3},{4},{5}) camPos=({6},{7},{8})",
					calls, m_SceneRenderer->GetWidth(), m_SceneRenderer->GetHeight(),
					projection[0][0], projection[1][1], projection[2][2],
					transform->Transform[3][0], transform->Transform[3][1], transform->Transform[3][2],
					static_cast<const void*>(m_SceneRenderer.get()));
			}
		}
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
