#include "wldpch.h"
#include "World/Gameplay/Framework/InputTrigger.h"

namespace World::Gameplay
{
	InputTrigger InputTrigger::MakePressed(float threshold)
	{
		InputTrigger t;
		t.Type = TriggerType::Pressed;
		t.ActuationThreshold = threshold;
		return t;
	}

	InputTrigger InputTrigger::MakeReleased(float threshold)
	{
		InputTrigger t;
		t.Type = TriggerType::Released;
		t.ActuationThreshold = threshold;
		return t;
	}

	InputTrigger InputTrigger::MakeHold(float duration, float threshold)
	{
		InputTrigger t;
		t.Type = TriggerType::Hold;
		t.Duration = duration;
		t.ActuationThreshold = threshold;
		return t;
	}

	InputTrigger InputTrigger::MakeTap(float maxDuration, float threshold)
	{
		InputTrigger t;
		t.Type = TriggerType::Tap;
		t.Duration = maxDuration;
		t.ActuationThreshold = threshold;
		return t;
	}

	InputTrigger InputTrigger::MakeDoubleTap(float maxInterval, float tapMaxDuration, float threshold)
	{
		InputTrigger t;
		t.Type = TriggerType::DoubleTap;
		t.Interval = maxInterval;
		t.Duration = tapMaxDuration;
		t.ActuationThreshold = threshold;
		return t;
	}

	InputTrigger InputTrigger::MakePulse(float interval, float threshold)
	{
		InputTrigger t;
		t.Type = TriggerType::Pulse;
		t.Interval = interval;
		t.ActuationThreshold = threshold;
		return t;
	}

	InputTrigger InputTrigger::MakeChord(NameId chordAction, float threshold)
	{
		InputTrigger t;
		t.Type = TriggerType::Chord;
		t.ChordAction = chordAction;
		t.ActuationThreshold = threshold;
		return t;
	}

	TriggerPhase InputTrigger::Evaluate(bool isActuated, float dt, TriggerState& state, bool chordActive) const
	{
		if (Type == TriggerType::Chord && !chordActive)
		{
			state.HeldTime = 0.0f;
			state.WasActuated = false;
			return TriggerPhase::None;
		}

		TriggerPhase phase = TriggerPhase::None;

		switch (Type)
		{
		case TriggerType::Pressed:
			if (isActuated && !state.WasActuated)
				phase = TriggerPhase::Triggered;
			else if (isActuated)
				phase = TriggerPhase::Ongoing;
			break;

		case TriggerType::Released:
			if (!isActuated && state.WasActuated)
				phase = TriggerPhase::Triggered;
			else if (isActuated)
				phase = TriggerPhase::Ongoing;
			break;

		case TriggerType::Hold:
			if (isActuated)
			{
				state.HeldTime += dt;
				if (state.HeldTime >= Duration)
					phase = TriggerPhase::Triggered;
				else
					phase = TriggerPhase::Ongoing;
			}
			else
			{
				state.HeldTime = 0.0f;
			}
			break;

		case TriggerType::Tap:
			if (isActuated)
			{
				state.HeldTime += dt;
				phase = TriggerPhase::Ongoing;
			}
			else if (state.WasActuated)
			{
				if (state.HeldTime <= Duration && state.HeldTime > 0.0f)
					phase = TriggerPhase::Triggered;
				state.HeldTime = 0.0f;
			}
			break;

		case TriggerType::DoubleTap:
			state.LastTapReleaseTime += dt;
			if (isActuated && !state.WasActuated)
			{
				state.HeldTime = 0.0f;
			}
			else if (isActuated)
			{
				state.HeldTime += dt;
				phase = TriggerPhase::Ongoing;
			}
			else if (!isActuated && state.WasActuated)
			{
				if (state.HeldTime <= Duration)
				{
					if (state.TapCount == 1 && state.LastTapReleaseTime <= Interval)
					{
						phase = TriggerPhase::Triggered;
						state.TapCount = 0;
					}
					else
					{
						state.TapCount = 1;
						state.LastTapReleaseTime = 0.0f;
					}
				}
				else
				{
					state.TapCount = 0;
				}
				state.HeldTime = 0.0f;
			}
			if (state.LastTapReleaseTime > Interval)
				state.TapCount = 0;
			break;

		case TriggerType::Pulse:
			if (isActuated)
			{
				if (!state.WasActuated)
				{
					phase = TriggerPhase::Triggered;
					state.PulseTimer = 0.0f;
				}
				else
				{
					state.PulseTimer += dt;
					if (state.PulseTimer >= Interval)
					{
						phase = TriggerPhase::Triggered;
						state.PulseTimer -= Interval;
					}
					else
					{
						phase = TriggerPhase::Ongoing;
					}
				}
			}
			else
			{
				state.PulseTimer = 0.0f;
			}
			break;

		case TriggerType::Chord:
			if (chordActive)
			{
				if (isActuated && !state.WasActuated)
					phase = TriggerPhase::Triggered;
				else if (isActuated)
					phase = TriggerPhase::Ongoing;
			}
			break;
		}

		state.WasActuated = isActuated;
		return phase;
	}
}
