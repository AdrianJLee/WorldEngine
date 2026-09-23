#include "World/Gameplay/GameFlow.h"

#include "World/Core/Log.h"

#include <cstddef>
#include <utility>

namespace World::Gameplay
{
	namespace
	{
		// GameFlow.h 里 m_States 的长度必须与枚举项数一致。
		constexpr std::size_t kStateCount = 7;
		static_assert(static_cast<std::size_t>(FlowState::Shutdown) + 1 == kStateCount,
			"FlowState 新增状态时必须同步 GameFlow::m_States 数组长度");

		constexpr std::size_t Index(FlowState state) { return static_cast<std::size_t>(state); }
	}

	const char* FlowStateName(FlowState state)
	{
		switch (state)
		{
		case FlowState::Boot:          return "Boot";
		case FlowState::MainMenu:      return "MainMenu";
		case FlowState::LevelLoading:  return "LevelLoading";
		case FlowState::Playing:       return "Playing";
		case FlowState::Paused:        return "Paused";
		case FlowState::LevelComplete: return "LevelComplete";
		case FlowState::Shutdown:      return "Shutdown";
		}
		return "Unknown";
	}

	void GameFlow::RegisterState(FlowState state, StateHandler onEnter, StateHandler onExit)
	{
		if (Index(state) >= kStateCount)
			return;

		StateEntry& entry = m_States[Index(state)];
		// 同一状态重复注册是覆盖,不叠加:编辑器重新绑定回调时不会触发双重回调。
		entry.OnEnter = std::move(onEnter);
		entry.OnExit = std::move(onExit);
	}

	void GameFlow::Start(FlowState initial, uint64_t frame)
	{
		(void)frame;
		if (m_Started)
		{
			WLD_CORE_WARN("GameFlow::Start ignored: flow already started in state {0}", FlowStateName(m_Current));
			return;
		}

		m_Started = true;
		m_Current = initial;
		m_Pending = initial;
		m_HasPending = false;
		m_History.clear();

		// 初始状态只触发 onEnter(没有"上一个状态"的退出语义),也不产生迁移历史。
		if (m_States[Index(initial)].OnEnter)
			m_States[Index(initial)].OnEnter(initial);
		WLD_CORE_INFO("GameFlow started in state {0}", FlowStateName(initial));
	}

	void GameFlow::Request(FlowState next)
	{
		// 排队而非立即切换:保证切换只发生在 GameApp::Tick 的安全点。
		m_Pending = next;
		m_HasPending = true;
	}

	bool GameFlow::ApplyPending(uint64_t frame)
	{
		if (!m_Started || !m_HasPending)
			return false;

		const FlowState next = m_Pending;
		m_HasPending = false;
		if (next == m_Current)
			return false;

		Enter(next, frame);
		return true;
	}

	void GameFlow::Enter(FlowState next, uint64_t frame)
	{
		const FlowState previous = m_Current;

		if (m_States[Index(previous)].OnExit)
			m_States[Index(previous)].OnExit(next);

		m_Current = next;
		m_History.push_back(FlowTransition{ previous, next, frame });
		WLD_CORE_TRACE("GameFlow: {0} -> {1} (frame {2})",
			FlowStateName(previous), FlowStateName(next), frame);

		if (m_States[Index(next)].OnEnter)
			m_States[Index(next)].OnEnter(previous);
	}
}
