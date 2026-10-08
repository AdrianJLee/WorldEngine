#include "World/Gameplay/Runtime/GameApp.h"

#include "World/Core/Log.h"
#include "World/Gameplay/Framework/GamepadBackend.h"
#include "World/Utils/Paths.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace World::Gameplay
{
	namespace
	{
		std::unique_ptr<GameApp> s_Instance;
		// 会话身份计数器:脚本事件桥用它识别"总线被重建"(指针可能落在同一地址)。
		uint64_t s_NextSessionId = 0;

		// 单帧推进上限:断点、窗口拖动或首次加载产生的巨大 dt 不应一次性灌进固定步长循环。
		constexpr double kMaxFrameSeconds = 0.25;

		using Clock = std::chrono::steady_clock;

		double ElapsedMilliseconds(Clock::time_point start)
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		}

		// 会话的输入映射 = 内容根下的 `input.weinput`(动作/轴/绑定)。必须在第一次 Tick 之前
		// 装载:**不装载时 `InputService::FindAction/FindAxis` 恒返回空**,脚本侧
		// `Input.Down/Pressed/Released/Axis` 一律 false/0,`GameHost::Tick` 也没有任何绑定可喂
		// (`mapInput.Actions()` 为空)—— 而引擎全程不报错,是纯静默失效。
		// 内容根口径与 `UiHost::Initialize` 同一条:显式 desc 优先,空则回落到 `Paths::AssetRoot()`
		// (编辑器 Play 的 desc 不带 ContentRoot)。
		void LoadInputMapForSession(InputService& input, const std::filesystem::path& contentRoot)
		{
			const std::filesystem::path root = contentRoot.empty() ? Paths::AssetRoot() : contentRoot;
			if (root.empty())
			{
				WLD_CORE_WARN("[input] no content root: '<content root>/input.weinput' was not loaded, "
					"so every Input.Down/Pressed/Released/Axis read returns false/0");
				return;
			}
			const std::filesystem::path mapPath = root / "input.weinput";
			std::error_code existsError;
			if (!std::filesystem::exists(mapPath, existsError))
			{
				WLD_CORE_WARN("[input] '{0}' not found: every Input.Down/Pressed/Released/Axis read "
					"returns false/0 (add the file or check the project content root)",
					mapPath.string());
				return;
			}
			InputMap map;
			std::string error;
			if (!InputMap::Load(mapPath, &map, &error))
			{
				WLD_CORE_ERROR("[input] cannot load '{0}': {1}", mapPath.string(), error);
				return;
			}
			input.SetMap(std::move(map));
			WLD_CORE_INFO("[input] input map loaded from '{0}' ({1} action(s), {2} axis/axes)",
				mapPath.string(), input.GetMap().Actions().size(), input.GetMap().Axes().size());
		}
	}

	GameApp::GameApp(const GameAppDesc& desc)
		: m_Desc(desc)
	{
		// 参数消毒:0 Hz 会让步长变成无穷,0 步上限会让模拟彻底停摆。
		// P4-1:固定步长频率的合法区间是 [1, 240](与清单校验同口径);这里再钳一次,
		// 防止宿主/测试直接构造越界 desc。0 保持旧的"回落到 60 Hz"语义(既有测试依赖)。
		m_Desc.FixedStepHz = m_Desc.FixedStepHz == 0 ? 60u : std::clamp(m_Desc.FixedStepHz, 1u, 240u);
		if (m_Desc.MaxFixedStepsPerFrame == 0)
			m_Desc.MaxFixedStepsPerFrame = 1;

		m_FixedStepSeconds = 1.0 / static_cast<double>(m_Desc.FixedStepHz);
		m_FramePhases.reserve(4);
	}

	void GameApp::CreateSaveService(SaveService::SceneProvider sceneProvider)
	{
		// 项目 id 来自会话描述;用户根默认 %LOCALAPPDATA%/<ProjectId>,测试可用 WLD_SAVE_DIR 覆盖。
		m_Saves = std::make_unique<SaveService>(m_Desc.ProjectId, std::move(sceneProvider));
	}

	void GameApp::Create(const GameAppDesc& desc)
	{
		if (s_Instance)
		{
			WLD_CORE_WARN("GameApp::Create ignored: session for project '{0}' already exists",
				s_Instance->m_Desc.ProjectId);
			return;
		}

		s_Instance = std::unique_ptr<GameApp>(new GameApp(desc));
		s_Instance->m_SessionId = ++s_NextSessionId;
		// 输入映射随会话创建装载:Editor Play 与 Runtime 都经这里(见 GameHost::Init),
		// 是"脚本能读到输入"的唯一前置条件。
		LoadInputMapForSession(s_Instance->m_Input, desc.ContentRoot);
		// R2b:手柄后端随会话初始化。此前 Init() 没有任何生产调用点 ⇒ LoadXInput() 从未执行
		// ⇒ XInput 轮询路径永不启用(只剩 GLFW joystick 回退),而 `Input.Rumble` 在真实手柄上
		// **静默无效**(m_XInputSetState 为空,没有调用对象)。
		GamepadBackend::Get().Init();
		// 会话创建即进入 Boot:宿主随后按需 Request(Playing)/Request(MainMenu)。
		s_Instance->m_Flow.Start(FlowState::Boot, 0);

		WLD_CORE_INFO("GameApp session created: project '{0}', {1} Hz fixed step, content root '{2}'",
			s_Instance->m_Desc.ProjectId, s_Instance->m_Desc.FixedStepHz,
			s_Instance->m_Desc.ContentRoot.generic_string());
	}

	void GameApp::Shutdown()
	{
		if (!s_Instance)
			return;

		WLD_CORE_INFO("GameApp session shutdown after {0} frame(s)", s_Instance->m_FrameNumber);
		// R2b:与 Create 对称收尾(释放 XInput 模块句柄)。
		GamepadBackend::Get().Shutdown();
		s_Instance.reset();
	}

	bool GameApp::Exists()
	{
		return s_Instance != nullptr;
	}

	GameApp& GameApp::Get()
	{
		WLD_CORE_ASSERT(s_Instance != nullptr, "GameApp::Get() without an active session; call Create() first");
		if (!s_Instance)
			throw std::logic_error("World::Gameplay::GameApp session does not exist");
		return *s_Instance;
	}

	GameApp* GameApp::TryGet()
	{
		return s_Instance.get();
	}

	void GameApp::SetPhaseCallbacks(PhaseCallback fixedUpdate, PhaseCallback update, PhaseCallback lateUpdate)
	{
		m_FixedUpdate = std::move(fixedUpdate);
		m_Update = std::move(update);
		m_LateUpdate = std::move(lateUpdate);
	}

	void GameApp::Tick(Timestep frameTime)
	{
		m_FramePhases.clear();
		++m_FrameNumber;

		// 1. 流程安全点:排队的状态切换只在这里生效,阶段执行中途不会换状态。
		{
			const Clock::time_point start = Clock::now();
			m_Flow.ApplyPending(m_FrameNumber);
			m_FramePhases.push_back({ "Flow", ElapsedMilliseconds(start) });
		}

		double frameSeconds = static_cast<double>(frameTime.GetSeconds());
		if (!(frameSeconds > 0.0))
			frameSeconds = 0.0;
		else if (frameSeconds > kMaxFrameSeconds)
			frameSeconds = kMaxFrameSeconds;

		const bool fixedRuns = !m_Paused || m_Desc.RunFixedWhenPaused;
		const bool variableRuns = !m_Paused;

		// 2. 固定步长:累积到整数倍才推进,并按上限截断(防死亡螺旋)。
		m_LastFixedSteps = 0;
		if (fixedRuns)
		{
			const Clock::time_point start = Clock::now();
			m_Accumulator += frameSeconds;
			while (m_Accumulator >= m_FixedStepSeconds && m_LastFixedSteps < m_Desc.MaxFixedStepsPerFrame)
			{
				// P2 W4:计时器只跟随固定步长推进(确定性);回调里的 After/Every/Cancel/Clear
				// 与回调异常都由 TimerService 自己隔离(见 EventBus.h 契约)。
				m_Timers.Advance(m_FixedStepSeconds);
				if (m_FixedUpdate)
					m_FixedUpdate(Timestep(static_cast<float>(m_FixedStepSeconds)));
				// W5:系统注册表的固定步长阶段(权威模拟)与回调同拍执行。
				m_Systems.RunPhase(SystemPhase::PreFixed, Timestep(static_cast<float>(m_FixedStepSeconds)));
				m_Systems.RunPhase(SystemPhase::Fixed, Timestep(static_cast<float>(m_FixedStepSeconds)));

				m_Accumulator -= m_FixedStepSeconds;
				++m_LastFixedSteps;
			}
			// 触到上限说明帧率跟不上:丢弃积压,否则延迟会永久累积(表现与模拟越拉越远)。
			if (m_LastFixedSteps >= m_Desc.MaxFixedStepsPerFrame)
				m_Accumulator = std::min(m_Accumulator, m_FixedStepSeconds);
			m_FramePhases.push_back({ "FixedUpdate", ElapsedMilliseconds(start) });
		}
		// P6:本帧余量占固定步的比例 ⇒ 渲染插值系数。固定步块之后算(此时 m_Accumulator 是本帧
		// 真正剩下的部分步);暂停且不跑固定步时余量没有意义,置 0。
		m_LastFixedStepAlpha = (fixedRuns && m_FixedStepSeconds > 0.0)
			? static_cast<float>(std::clamp(m_Accumulator / m_FixedStepSeconds, 0.0, 1.0))
			: 0.0f;

		// 3. 可变步长阶段(暂停时不推进:表现层与权威模拟一起停)。
		if (variableRuns)
		{
			if (m_Update)
			{
				const Clock::time_point start = Clock::now();
				m_Update(frameTime);
				m_Systems.RunPhase(SystemPhase::Update, frameTime);
				m_FramePhases.push_back({ "Update", ElapsedMilliseconds(start) });
			}

			if (m_LateUpdate)
			{
				const Clock::time_point start = Clock::now();
				m_LateUpdate(frameTime);
				m_Systems.RunPhase(SystemPhase::Late, frameTime);
				m_FramePhases.push_back({ "LateUpdate", ElapsedMilliseconds(start) });
			}
		}

		// 4. 帧末事件派发:队列式事件在 Update/Late 之后统一投递。
		//    放在暂停判定之外 —— 事件是"事实通知",暂停也要送达;派发中 emit 的新事件留到下一批。
		{
			const Clock::time_point start = Clock::now();
			m_Events.DispatchPending();
			m_FramePhases.push_back({ "Events", ElapsedMilliseconds(start) });
		}
	}
}
