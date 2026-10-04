#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Scene/Scene.h"

namespace World::Gameplay
{
	// WP5:场景级时间服务(Scene::GetTime())的唯一推进者。
	//
	// 分工(工业口径,保 P4/P5 的确定性):
	//   * BeginVariableFrame:可变帧每帧一次(宿主未暂停时)—— FrameCount / Delta / Elapsed;
	//   * AdvanceFixedStep:固定步每次一次 —— FixedDeltaSeconds / FixedStepCount;
	//     由 PreFixed 阶段的 `timer-system` 驱动(每帧 0..N 次 = 本帧固定步次数);
	//   * 只读查询走 Scene::GetTime()(语义注释见 Scene.h)。
	//
	// 硬约束:
	//   * 暂停(RunFixedWhenPaused=false)时宿主不调可变/固定回调 ⇒ ElapsedSeconds 不累积;
	//   * TimeScale 只缩放 DeltaSeconds(ElapsedSeconds 随缩放后的 dt 累积,保持既有语义),
	//     不改 FixedDeltaSeconds;
	//   * FixedStepCount 与固定步次数严格一致(每个固定步恰好 +1 一次)。
	class WLD_API TimerSystem
	{
	public:
		static constexpr const char* kSystemName = "timer-system";

		// 可变帧推进(每可变帧一次;暂停时宿主不调用)。
		static void BeginVariableFrame(Scene& scene, Timestep variableDt);
		// 固定步推进(每固定步一次)。
		static void AdvanceFixedStep(Scene& scene, Timestep fixedDt);
		// 会话固定步长(进运行态时由宿主下发;只影响"第一个固定步之前"的读数)。
		static void SetFixedDeltaSeconds(Scene& scene, float seconds);
		// 可注册的 PreFixed 帧系统:绑定到 scene;Owner 由注册处显式标 Builtin。
		static Scene::FrameSystem MakePreFixedSystem(Scene& scene);
	};
}
