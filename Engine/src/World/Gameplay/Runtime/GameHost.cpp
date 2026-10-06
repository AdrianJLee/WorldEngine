#include "World/Gameplay/Runtime/GameHost.h"

#include "World/Asset/AsyncLoader.h"
#include "World/Asset/ScenePrefetch.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/Core/Input.h"
#include "World/Physics/PhysicsSettings.h"
#include "World/Gameplay/Framework/InputMap.h"
#include "World/Gameplay/Framework/InputSystem.h"
#include "World/Gameplay/Framework/TimerSystem.h"

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
		// 只返回实体句柄:帧相机数据(投影矩阵)由 Scene::GetCameraView 按指纹缓存产出。
		bool ResolvePrimaryCamera(const Ref<Scene>& scene, entt::entity& camera, TransformComponent*& transform)
		{
			camera = entt::null;
			transform = nullptr;
			if (!scene)
				return false;

			Entity entity = scene->GetPrimaryCameraEntity();
			if (!entity || !entity.HasComponent<CameraComponent>() || !entity.HasComponent<TransformComponent>())
				return false;

			camera = static_cast<entt::entity>(entity);
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
		// 固定阶段(物理/移动)与可变阶段(transform/camera/animation/抽取)分别接到场景:
		// 顺序由 GameApp::Tick 保证 = Fixed(0..N) → Update → Late → PreRender。
		app.SetPhaseCallbacks(
			// PURE-ECS(工业口径):**固定步长回调**接场景的固定阶段(物理 + 移动)。
			// 此前这里是空回调、只有可变 update 接了场景 ⇒ 物理拿的是可变帧时间,
			// 同一段真实时间在不同帧率下结果不同。GameApp 的累加器负责一帧跑 0..N 步。
			[this](Timestep ts)
			{
				if (m_Scene && m_RuntimeStarted)
					m_Scene->OnFixedUpdate(ts);
			},
			[this](Timestep ts)
			{
				if (m_Scene && m_RuntimeStarted)
				{
					// P6:把本帧的固定步长余量(0..1)交给场景,渲染抽取用它在上一个固定步位姿与
					// 本帧位姿之间插值 —— 只影响渲染,权威模拟数据不动(高刷屏上的运动更平滑)。
					m_Scene->SetFixedStepAlpha(GameApp::Get().LastFixedStepAlpha());
					m_Scene->OnUpdateRuntime(ts);
				}
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
		// T5c:反序列化一结束就把场景需要的资产交给后台解析 —— 首帧的同步加载退化为缓存命中。
		{
			const ScenePrefetchResult prefetch = PrefetchSceneAssets(*context, *m_Scene);
			WLD_CORE_INFO("[load] '{0}': assets prefetched={1} already-known={2} ({3})",
				scenePath, prefetch.Requested, prefetch.AlreadyKnown, DescribeAssetLoads());
		}

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
		// P6:宿主接管的场景开启固定步长 → 渲染插值(只影响表现层;alpha 每可变帧由上面的回调推)。
		if (scene)
			scene->SetPhysicsInterpolationEnabled(true);
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

		// WP5:把会话的固定步长下发给场景时间服务 —— 第一个固定步之前 GetTime().FixedDeltaSeconds
		// 就应该是本会话真正的步长,而不是结构体默认值(项目可覆盖 FixedStepHz)。
		if (GameApp* app = GameApp::TryGet())
			Gameplay::TimerSystem::SetFixedDeltaSeconds(*m_Scene, static_cast<float>(app->FixedStepSeconds()));
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
		// WP5:运行结束丢掉该场景的输入边沿/鼠标缓存(下次 StartRuntime 从干净状态开始,
		// 也避免编辑器反复 Play/Stop 时按场景地址累积条目)。
		Gameplay::InputSystem::Forget(*m_Scene);
		m_RuntimeStarted = false;
	}

	void GameHost::Tick(Timestep frameTime, bool render)
	{
		// GameUI(M9):宿主在 Tick 前 `SetPointerCaptured(...)`;这里**消费一次即复位** ——
		// 宿主不设置 = 下一帧自动 false,捕获状态不泄漏到后续帧。
		const bool pointerCaptured = m_PointerCaptured;
		m_PointerCaptured = false;

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
			for (const Gameplay::InputAction& action : mapInput.Actions())
				for (const Gameplay::InputBinding& binding : action.Bindings)
				{
					if (binding.Device == Gameplay::InputDevice::Mouse)
					{
						// M9:UI 吃掉指针的那一帧,鼠标键位一律按抬起(不产生 Pressed/Released 边沿);
						// 只在有窗口或"捕获帧"时写入 —— 无窗口且未捕获时保留调用方喂入的状态
						// (既有 headless 口径不变)。
						const bool down = hasEngineWindow && !pointerCaptured
							&& Input::IsMouseButtonPressed(binding.Code);
						if (hasEngineWindow || pointerCaptured)
							input.SetKeyState(0, Gameplay::InputDevice::Mouse, binding.Code, down);
					}
					else if (hasEngineWindow)
					{
						const bool down = binding.Device == Gameplay::InputDevice::Key
							&& Input::IsKeyPressed(binding.Code);
						input.SetKeyState(0, binding.Device, binding.Code, down);
					}
				}
			input.BuildSnapshot(0);
		}

		// T5c:帧首提交后台解析结果。必须在**任何**可能提前 return 的分支之前,
		// 否则"加载未完成 ⇒ 跳过渲染 ⇒ 不提交 ⇒ 永远加载未完成"会自锁。
		PumpAssetLoads();

		// W2:推进排队的关卡加载(读盘 → 反序列化 → 激活;激活时进度回调会把场景交给宿主)。
		GameApp::Get().Levels().Pump();
		// WP5:帧首采样**一次**(可变帧开头、固定步循环之前 —— Tick 内是 Fixed(0..N) → Update)。
		// 同一可变帧内的每个固定步读到的都是这份快照 ⇒ PreFixed/Fixed 只消费、不采样,保 P4/P5 确定性。
		// 放在 Pump 之后:本帧刚激活的场景也能在同一帧拿到快照。
		if (m_Scene && m_RuntimeStarted)
		{
			glm::vec2 mousePosition(0.0f);
			glm::vec2 scrollDelta(0.0f);
			if (Application::HasInstance())
			{
				// 无窗口宿主(测试/专用服务器)绝不轮询平台鼠标(与上面的按键喂入同一守卫)。
				const auto position = Input::GetMousePosition();
				mousePosition = glm::vec2(position.first, position.second);
				// WP5:滚轮同样只在有窗口时读平台累积值。
				// M9:UI 吃掉滚轮的那一帧不传给玩法(相机不得滚动);鼠标位置照常传(瞄准可用)。
				if (!pointerCaptured)
				{
					const auto scroll = Input::GetScrollDelta();
					scrollDelta = glm::vec2(scroll.first, scroll.second);
				}
			}
			Gameplay::InputSystem::Sample(*m_Scene, GameApp::Get().Input(), mousePosition, scrollDelta);
		}
		GameApp::Get().Tick(frameTime);
		// W7-6/P2 W3b:帧末把"当前按下"滚成"上一帧按下",下一帧的 Pressed/Released 才有真实边沿。
		// 必须在本帧脚本/玩法读取之后调用,否则同一帧内刚按下的键会被立刻滚走。
		GameApp::Get().Input().EndFrame();

		// D5c-4a:缓存本帧秒数给 SubmitSceneRender 用(骨骼动画步长)。
		m_LastTickSeconds = frameTime.GetSeconds();
		// 流式加载未完成时**不提交场景**:避免半加载场景一帧一个样地"跳变"(pop-in)。
		// 宿主(见 RuntimeLayer)在这段时间画加载界面;headless 宿主只是少跑几帧渲染。
		if (const std::size_t pending = PendingAssetLoads(); pending > 0)
		{
			// 节流日志:卡住时能在日志里看出来(每 ~120 帧一次)。
			static uint64_t loadingFrame = 0;
			if (++loadingFrame % 120 == 1)
				WLD_CORE_INFO("[load] streaming: {0} asset(s) pending ({1})",
					pending, DescribeAssetLoads());
			return;
		}
		if (render)
			SubmitSceneRender();
	}

	void GameHost::PumpAssetLoads()
	{
		if (!m_Scene)
			return;
		if (AsyncLoader* loader = m_Scene->GetContext().Resources().TryGet<AsyncLoader>())
			loader->PumpCompletions();
		// 纹理驻留是进程级的:提交点同样在帧首(与网格一致,且不依赖场景渲染被调用)。
		TextureLibrary::Get().PumpCompletions();
	}

	std::size_t GameHost::PendingAssetLoads() const
	{
		if (!m_Scene)
			return 0;
		const AsyncLoader* loader = m_Scene->GetContext().Resources().TryGet<AsyncLoader>();
		return (loader ? loader->PendingCount() : 0) + TextureLibrary::Get().PendingCount();
	}

	std::string GameHost::DescribeAssetLoads() const
	{
		if (!m_Scene)
			return {};
		const AsyncLoader* loader = m_Scene->GetContext().Resources().TryGet<AsyncLoader>();
		std::string mesh = loader ? loader->Describe() : std::string();
		const std::string textures = TextureLibrary::Get().DescribeLoads();
		if (!mesh.empty() && !textures.empty())
			return mesh + "  texture[" + textures + "]";
		return mesh.empty() ? textures : mesh;
	}

	void GameHost::SubmitSceneRender()
	{
		// 未注入渲染器 = 无头宿主(测试/服务端);未进运行时 = 编辑器用自己的视口渲染。
		if (!m_Scene || !m_SceneRenderer || !m_RuntimeStarted)
			return;

		entt::entity camera = entt::null;
		TransformComponent* transform = nullptr;
		if (!ResolvePrimaryCamera(m_Scene, camera, transform))
			return;

		m_SceneRenderer->BeginScene(m_Scene.get(), SceneRendererOptions());
		// D5c-4a:骨骼动画步长(与编辑器同一口径:推进组件 Time)。
		m_SceneRenderer->SetDeltaSeconds(m_LastTickSeconds);
		const glm::mat4 camMatrix = transform->GetLocalMatrix();
		const Camera& cameraView = m_Scene->GetCameraView(camera);
		m_SceneRenderer->SubmitScene(cameraView, camMatrix);
		m_SceneRenderer->EndScene();

		// 诊断(WLD_TRACE_HOST=1):确认"每帧都在提交"以及相机矩阵是否退化 ——
		// Runtime 抓图只剩清屏色时,这两者是最可能的断点。
		if (std::getenv("WLD_TRACE_HOST"))
		{
			static uint64_t calls = 0;
			++calls;
			if (calls <= 4 || calls % 120 == 0)
			{
				const glm::mat4& projection = cameraView.GetProjectionMatrix();
				WLD_CORE_INFO("[host] SubmitSceneRender call#{0} renderer={9} target={1}x{2} projDiag=({3},{4},{5}) camPos=({6},{7},{8})",
					calls, m_SceneRenderer->GetWidth(), m_SceneRenderer->GetHeight(),
					projection[0][0], projection[1][1], projection[2][2],
					camMatrix[3][0], camMatrix[3][1], camMatrix[3][2],
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
