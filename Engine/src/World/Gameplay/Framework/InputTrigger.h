#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputTypes.h"

namespace World::Gameplay
{
	enum class TriggerType : uint8_t
	{
		Pressed = 0, // 单帧按下触发
		Released,    // 单帧释放触发
		Hold,        // 达到持续时间后触发
		Tap,         // 短促按下并在限定时间内释放时触发
		DoubleTap,   // 双击触发
		Pulse,       // 持续按下期间按固定频率脉冲触发
		Chord        // 伴随组合动作处于激活态时才允许触发
	};

	struct TriggerState
	{
		float HeldTime = 0.0f;
		float LastTapReleaseTime = 0.0f;
		int TapCount = 0;
		float PulseTimer = 0.0f;
		bool WasActuated = false;

		void Reset()
		{
			HeldTime = 0.0f;
			LastTapReleaseTime = 0.0f;
			TapCount = 0;
			PulseTimer = 0.0f;
			WasActuated = false;
		}
	};

	struct WLD_API InputTrigger
	{
		TriggerType Type = TriggerType::Pressed;
		float ActuationThreshold = 0.5f;
		float Duration = 0.5f;        // Hold 阈值或 Tap 最大时长
		float Interval = 0.3f;        // DoubleTap 间隔或 Pulse 周期
		NameId ChordAction;           // Chord 所依赖的辅助动作

		TriggerPhase Evaluate(bool isActuated, float dt, TriggerState& state, bool chordActive = true) const;

		static InputTrigger MakePressed(float threshold = 0.5f);
		static InputTrigger MakeReleased(float threshold = 0.5f);
		static InputTrigger MakeHold(float duration = 0.5f, float threshold = 0.5f);
		static InputTrigger MakeTap(float maxDuration = 0.2f, float threshold = 0.5f);
		static InputTrigger MakeDoubleTap(float maxInterval = 0.3f, float tapMaxDuration = 0.2f, float threshold = 0.5f);
		static InputTrigger MakePulse(float interval = 0.1f, float threshold = 0.5f);
		static InputTrigger MakeChord(NameId chordAction, float threshold = 0.5f);
	};
}
