#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace World::Gameplay
{
	// 游戏流程状态(P2a §3.1)。流程是**显式状态机**,不塞进 ECS,也不挂在某个实体上。
	enum class FlowState : uint8_t
	{
		Boot = 0,
		MainMenu,
		LevelLoading,
		Playing,
		Paused,
		LevelComplete,
		Shutdown,
	};

	const char* FlowStateName(FlowState state);

	struct FlowTransition
	{
		FlowState From = FlowState::Boot;
		FlowState To = FlowState::Boot;
		uint64_t Frame = 0;
	};

	// 状态机:切换请求排队到"安全点"生效(默认每帧 GameApp::Tick 内一次),
	// 保证 exit/enter 回调成对、且不会在阶段执行中途改变流程。
	class WLD_API GameFlow
	{
	public:
		using StateHandler = std::function<void(FlowState previous)>;

		// 注册状态进入/退出回调(可对同一状态重复调用以覆盖,不叠加)。
		void RegisterState(FlowState state, StateHandler onEnter, StateHandler onExit = {});
		// 启动时立即进入某状态(会触发该状态 onEnter)。
		void Start(FlowState initial, uint64_t frame = 0);
		// 请求切换:排队,下一次 ApplyPending 生效;重复请求以最后一次为准。
		void Request(FlowState next);
		// 在安全点应用排队的切换;返回本次是否发生了切换。
		bool ApplyPending(uint64_t frame);

		FlowState Current() const { return m_Current; }
		FlowState Pending() const { return m_Pending; }
		bool HasPending() const { return m_HasPending; }
		const std::vector<FlowTransition>& History() const { return m_History; }

	private:
		struct StateEntry
		{
			StateHandler OnEnter;
			StateHandler OnExit;
		};

		void Enter(FlowState next, uint64_t frame);

		StateEntry m_States[7];
		FlowState m_Current = FlowState::Boot;
		FlowState m_Pending = FlowState::Boot;
		bool m_HasPending = false;
		bool m_Started = false;
		std::vector<FlowTransition> m_History;
	};
}
