#include "World/Gameplay/GameApp.h"

#include "World/Core/Log.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

namespace World::Gameplay
{
	namespace
	{
		std::unique_ptr<GameApp> s_Instance;

		// 单帧推进上限:断点、窗口拖动或首次加载产生的巨大 dt 不应一次性灌进固定步长循环。
		constexpr double kMaxFrameSeconds = 0.25;

		using Clock = std::chrono::steady_clock;

		double ElapsedMilliseconds(Clock::time_point start)
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		}
	}

	GameApp::GameApp(const GameAppDesc& desc)
		: m_Desc(desc)
	{
		// 参数消毒:0 Hz 会让步长变成无穷,0 步上限会让模拟彻底停摆。
		if (m_Desc.FixedStepHz == 0)
			m_Desc.FixedStepHz = 60;
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
	}
}
