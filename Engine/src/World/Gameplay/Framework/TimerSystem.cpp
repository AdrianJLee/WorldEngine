#include "wldpch.h"
#include "World/Gameplay/Framework/TimerSystem.h"

#include <utility>

namespace World::Gameplay
{
	namespace
	{
		// 时间推进写场景服务,并行执行会打破确定性:显式独占(主线程)。
		constexpr bool kTimerSystemParallelSafe = false;
	}

	void TimerSystem::BeginVariableFrame(Scene& scene, Timestep variableDt)
	{
		Scene::FrameTimeService& time = scene.MutableTime();
		const float unscaled = variableDt.GetSeconds();
		time.UnscaledDeltaSeconds = unscaled;
		time.DeltaSeconds = unscaled * time.TimeScale;
		// 与既有语义一致:ElapsedSeconds 累积"缩放后"的 DeltaSeconds(TimeScale 停表)。
		time.ElapsedSeconds += static_cast<double>(time.DeltaSeconds);
		++time.FrameCount;
	}

	void TimerSystem::AdvanceFixedStep(Scene& scene, Timestep fixedDt)
	{
		Scene::FrameTimeService& time = scene.MutableTime();
		time.FixedDeltaSeconds = fixedDt.GetSeconds();
		++time.FixedStepCount;
	}

	void TimerSystem::SetFixedDeltaSeconds(Scene& scene, float seconds)
	{
		if (seconds > 0.0f)
			scene.MutableTime().FixedDeltaSeconds = seconds;
	}

	Scene::FrameSystem TimerSystem::MakePreFixedSystem(Scene& scene)
	{
		Scene::FrameSystem system;
		system.Name = kSystemName;
		system.ParallelSafe = kTimerSystemParallelSafe;
		system.Phase = SystemPhase::PreFixed;
		system.Owner = "Builtin";
		system.Update = [&scene](Timestep fixedDt) { AdvanceFixedStep(scene, fixedDt); };
		return system;
	}
}
