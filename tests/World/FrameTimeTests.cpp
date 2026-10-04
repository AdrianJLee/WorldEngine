// WP5:帧首输入采样(InputSystem)与场景时间服务(TimerSystem / FrameTimeService)的反假验收。
//
//   A 确定性:同一可变帧内 N(>=3)个固定步读到的输入快照逐位相同;
//   B 一次采样:一帧只采样一次(计数断言)+ 跨帧边沿正确;
//   C 时间:FixedStepCount 与固定步数逐次相等、TimeScale 只缩放 Delta、暂停不累积 Elapsed;
//   E Lua:只读 Time 服务的读数与 C++ 逐位一致、表不可写。
//  (D 是手工突变检查:把采样从 GameHost 帧首挪进固定步路径时 A/A2 必须变红;见任务报告,不在这里自动跑。)
#include "wldpch.h"
#include "World/Core/Core.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Framework/InputMap.h"
#include "World/Gameplay/Framework/InputSystem.h"
#include "World/Gameplay/Framework/TimerSystem.h"
#include "World/Gameplay/Runtime/GameHost.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Scene/Scene.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptValue.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace World;
	using namespace World::Gameplay;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	constexpr float kFixedStep = 1.0f / 60.0f;

	// 快照比较要"逐位":浮点按位模式比较,不用近似。
	uint32_t Bits(float value)
	{
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	// 快照指纹:unordered_map 迭代顺序不定,先按键排序再序列化;
	// 同样的输入内容必然得到同一个字符串 ⇒ "逐位相同"的断言才有意义。
	std::string SnapshotDigest(const Scene::InputSnapshot& snapshot)
	{
		std::vector<std::pair<std::string, bool>> buttons(snapshot.Buttons.begin(), snapshot.Buttons.end());
		std::sort(buttons.begin(), buttons.end(),
			[](const auto& left, const auto& right) { return left.first < right.first; });
		std::vector<std::pair<std::string, float>> axes(snapshot.Axes.begin(), snapshot.Axes.end());
		std::sort(axes.begin(), axes.end(),
			[](const auto& left, const auto& right) { return left.first < right.first; });

		std::string digest = "buttons:";
		for (const auto& entry : buttons)
			digest += entry.first + (entry.second ? "=1;" : "=0;");
		digest += "|axes:";
		for (const auto& entry : axes)
			digest += entry.first + "=" + std::to_string(Bits(entry.second)) + ";";
		digest += "|mouse:" + std::to_string(Bits(snapshot.MousePosition.x)) + "," +
			std::to_string(Bits(snapshot.MousePosition.y));
		digest += "|mdelta:" + std::to_string(Bits(snapshot.MouseDelta.x)) + "," +
			std::to_string(Bits(snapshot.MouseDelta.y));
		digest += "|scroll:" + std::to_string(Bits(snapshot.ScrollDelta));
		digest += "|frame:" + std::to_string(snapshot.SampledFrame);
		digest += "|valid:" + std::to_string(snapshot.Valid ? 1 : 0);
		return digest;
	}

	InputMap MakeProbeMap()
	{
		InputMap map;
		InputAction jump; jump.Name = "Jump"; jump.Bindings = { { InputDevice::Key, 32 } };
		InputAction right; right.Name = "MoveRight"; right.Bindings = { { InputDevice::Key, 68 } };
		InputAction left; left.Name = "MoveLeft"; left.Bindings = { { InputDevice::Key, 65 } };
		map.Actions() = { jump, right, left };
		InputAxis move; move.Name = "Move"; move.PositiveAction = "MoveRight"; move.NegativeAction = "MoveLeft";
		map.Axes() = { move };
		return map;
	}

	void RunLua(const char* source, const char* chunk)
	{
		std::string error;
		if (!ScriptEngine::GetState().RunString(source, chunk, &error))
			throw std::runtime_error(std::string(chunk) + " failed: " + error);
	}

	double NumberGlobal(const char* name)
	{
		double number = 0.0;
		if (!ScriptEngine::GetState().GetGlobal(name).AsNumber(&number))
			throw std::runtime_error(std::string("global ") + name + " is not a number");
		return number;
	}

	bool BoolGlobal(const char* name)
	{
		bool value = false;
		if (!ScriptEngine::GetState().GetGlobal(name).AsBool(&value))
			throw std::runtime_error(std::string("global ") + name + " is not a boolean");
		return value;
	}
}

int main()
{
	try
	{
		WorldContext context;

		// =====================================================================
		// A. 同一可变帧内 N 个固定步读到的输入快照必须逐位相同(保 P4/P5 确定性)
		// =====================================================================
		{
			Scene scene(context);
			scene.EnsureDefaultFrameSystems();   // 内置系统(含 PreFixed 的 timer-system)先就位

			InputService input;
			input.SetMap(MakeProbeMap());
			input.SetPlayerCount(1);
			input.SetKeyState(0, InputDevice::Key, 32, true);   // Jump
			input.SetKeyState(0, InputDevice::Key, 68, true);   // MoveRight
			input.BuildSnapshot(0);                             // 宿主:原始状态 -> 动作/轴
			InputSystem::Sample(scene, input, glm::vec2(10.0f, 20.0f));   // 帧首恰好一次
			CHECK(InputSystem::GetSampleCount(scene) == 1);

			std::vector<std::string> digests;
			std::vector<bool> pressedPerStep;
			std::vector<uint64_t> stepCounts;
			scene.RegisterFrameSystem({
				"wp5-prefixed-probe", false,
				[&](Timestep) {
					digests.push_back(SnapshotDigest(scene.GetInputSnapshot()));
					pressedPerStep.push_back(InputSystem::WasPressed(scene, "Jump"));
					stepCounts.push_back(scene.GetTime().FixedStepCount);
				},
				SystemPhase::PreFixed, {} });

			constexpr int kSteps = 3;
			for (int step = 0; step < kSteps; ++step)
				scene.OnFixedUpdate(Timestep(kFixedStep));

			CHECK(digests.size() == static_cast<std::size_t>(kSteps));
			for (int step = 1; step < kSteps; ++step)
				CHECK(digests[step] == digests[0]);          // 逐位相同(动作/轴/鼠标/帧号全参与)
			CHECK(InputSystem::GetSampleCount(scene) == 1);  // 反假:固定步里没有第二次采样
			CHECK(digests[0] == SnapshotDigest(scene.GetInputSnapshot()));
			CHECK(InputSystem::IsDown(scene, "Jump"));
			CHECK(InputSystem::IsDown(scene, "MoveRight"));
			CHECK(!InputSystem::IsDown(scene, "Ghost"));
			CHECK(InputSystem::WasPressed(scene, "Jump"));
			CHECK(!InputSystem::WasReleased(scene, "Jump"));
			CHECK(pressedPerStep.size() == static_cast<std::size_t>(kSteps));
			for (bool pressed : pressedPerStep)
				CHECK(pressed);                              // 同帧每个固定步都是"本帧首次按下"
			CHECK(std::fabs(InputSystem::GetAxis(scene, "Move") - 1.0f) < 1e-6f);
			CHECK(InputSystem::GetAxis(scene, "Ghost") == 0.0f);
			CHECK((stepCounts == std::vector<uint64_t>{ 1, 2, 3 }));   // 每步恰好 +1

			// 运行停止:丢掉边沿缓存并清空快照(停止后不应再读到上一场运行的输入)。
			InputSystem::Forget(scene);
			CHECK(InputSystem::GetSampleCount(scene) == 0);
			CHECK(!InputSystem::WasPressed(scene, "Jump"));
			CHECK(!InputSystem::IsDown(scene, "Jump"));
			CHECK(!scene.GetInputSnapshot().Valid);
		}

		// =====================================================================
		// A2. 宿主接线:GameHost 在**固定步循环之前**每帧只采样一次(真实调用点)
		// =====================================================================
		{
			WorldContext hostContext;
			Ref<Scene> scene = CreateRef<Scene>(hostContext);
			scene->EnsureDefaultFrameSystems();
			std::vector<std::string> digests;
			scene->RegisterFrameSystem({
				"wp5-host-probe", false,
				[&](Timestep) { digests.push_back(SnapshotDigest(scene->GetInputSnapshot())); },
				SystemPhase::PreFixed, {} });

			GameHost host;
			GameAppDesc hostDesc;
			hostDesc.FixedStepHz = 60;
			hostDesc.MaxFixedStepsPerFrame = 8;
			host.Init(hostDesc);
			host.SetScene(scene, true);
			CHECK(host.IsRuntimeStarted());
			// 会话固定步长已下发:第一个固定步之前就应读到 60Hz 的步长。
			CHECK(std::fabs(scene->GetTime().FixedDeltaSeconds - kFixedStep) < 1e-9f);

			// headless 宿主不轮询窗口:测试直接喂 InputService,宿主只负责建快照/采样。
			InputService& injected = GameApp::Get().Input();
			injected.SetMap(MakeProbeMap());
			injected.SetPlayerCount(1);
			injected.SetKeyState(0, InputDevice::Key, 32, true);   // Jump
			injected.BuildSnapshot(0);

			host.Tick(Timestep(0.05f), false);   // 一个可变帧 = 3 个固定步
			CHECK(digests.size() == 3);
			CHECK(digests[1] == digests[0] && digests[2] == digests[0]);
			CHECK(InputSystem::GetSampleCount(*scene) == 1);   // 一帧一次(不是每固定步一次)
			CHECK(InputSystem::IsDown(*scene, "Jump"));
			CHECK(scene->GetTime().FixedStepCount == 3);

			// 停止运行:边沿缓存与快照都清掉。
			host.StopRuntime();
			CHECK(InputSystem::GetSampleCount(*scene) == 0);
			CHECK(!scene->GetInputSnapshot().Valid);
			host.Shutdown();
			CHECK(!GameApp::Exists());
		}

		// =====================================================================
		// B. 一帧只采样一次(计数断言)+ 跨帧 Pressed/Released 边沿与鼠标增量
		// =====================================================================
		{
			Scene scene(context);
			scene.EnsureDefaultFrameSystems();

			InputService input;
			input.SetMap(MakeProbeMap());
			input.SetPlayerCount(1);
			input.SetKeyState(0, InputDevice::Key, 32, true);   // Jump 按下
			input.BuildSnapshot(0);
			InputSystem::Sample(scene, input, glm::vec2(1.0f, 1.0f));
			CHECK(InputSystem::GetSampleCount(scene) == 1);
			CHECK(InputSystem::WasPressed(scene, "Jump"));      // 本帧首次按下
			CHECK(scene.GetInputSnapshot().MouseDelta == glm::vec2(0.0f));   // 首帧不造假位移

			// 同一可变帧内的固定步读到的边沿与帧首一致(快照未被固定步改写)。
			scene.OnFixedUpdate(Timestep(kFixedStep));
			CHECK(InputSystem::WasPressed(scene, "Jump"));
			CHECK(InputSystem::GetSampleCount(scene) == 1);
			scene.OnUpdateRuntime(Timestep(kFixedStep));
			// SampledFrame = 采样时的可变帧号;本帧采样后可变帧推进到 FrameCount=1。
			CHECK(InputSystem::GetLastSampledFrame(scene) == scene.GetTime().FrameCount - 1);

			// 帧 2:按住不放(EndFrame 把"当前按下"滚成"上一帧按下")。
			input.EndFrame();
			input.BuildSnapshot(0);
			InputSystem::Sample(scene, input, glm::vec2(2.0f, 2.0f));
			CHECK(InputSystem::GetSampleCount(scene) == 2);     // 一帧一次,累计两次
			CHECK(InputSystem::IsDown(scene, "Jump"));
			CHECK(!InputSystem::WasPressed(scene, "Jump"));     // 持续按住不再是 pressed
			CHECK(scene.GetInputSnapshot().MouseDelta == glm::vec2(1.0f, 1.0f));
			scene.OnUpdateRuntime(Timestep(kFixedStep));

			// 帧 3:松开 ⇒ Released 只在那一帧为真。
			input.EndFrame();
			input.SetKeyState(0, InputDevice::Key, 32, false);
			input.BuildSnapshot(0);
			InputSystem::Sample(scene, input, glm::vec2(2.0f, 2.0f));
			CHECK(!InputSystem::IsDown(scene, "Jump"));
			CHECK(InputSystem::WasReleased(scene, "Jump"));
			CHECK(!InputSystem::WasPressed(scene, "Jump"));
			CHECK(scene.GetInputSnapshot().MouseDelta == glm::vec2(0.0f));
			CHECK(InputSystem::GetSampleCount(scene) == 3);
		}

		// =====================================================================
		// C1. 固定步计数/步长逐次相等;TimeScale 只缩放 DeltaSeconds
		// =====================================================================
		{
			Scene scene(context);
			scene.EnsureDefaultFrameSystems();
			CHECK(scene.GetTime().FixedStepCount == 0);
			for (uint64_t step = 1; step <= 5; ++step)
			{
				scene.OnFixedUpdate(Timestep(kFixedStep));
				CHECK(scene.GetTime().FixedStepCount == step);
				CHECK(scene.GetTime().FixedDeltaSeconds == kFixedStep);
			}

			scene.MutableTime().TimeScale = 0.5f;
			const float fixedBefore = scene.GetTime().FixedDeltaSeconds;
			scene.OnUpdateRuntime(Timestep(0.02f));
			CHECK(scene.GetTime().UnscaledDeltaSeconds == 0.02f);
			CHECK(std::fabs(scene.GetTime().DeltaSeconds - 0.01f) < 1e-7f);
			CHECK(scene.GetTime().FixedDeltaSeconds == fixedBefore);   // TimeScale 不改固定步长
			CHECK(scene.GetTime().FrameCount == 1);

			const double elapsed = scene.GetTime().ElapsedSeconds;
			scene.OnUpdateRuntime(Timestep(0.02f));
			CHECK(scene.GetTime().ElapsedSeconds > elapsed);           // 单调递增
			CHECK(scene.GetTime().FrameCount == 2);
			CHECK(std::fabs(scene.GetTime().ElapsedSeconds - 0.02) < 1e-6);
			CHECK(scene.GetTime().FixedStepCount == 5);                // 可变帧不改固定步计数
		}

		// =====================================================================
		// C2. 暂停不累积 ElapsedSeconds;恢复后单调(宿主 RunFixedWhenPaused=false)
		// =====================================================================
		{
			Scene scene(context);
			GameAppDesc desc;
			desc.FixedStepHz = 60;
			desc.MaxFixedStepsPerFrame = 8;
			desc.RunFixedWhenPaused = false;
			GameApp::Create(desc);
			GameApp& app = GameApp::Get();
			app.SetPhaseCallbacks(
				[&scene](Timestep ts) { scene.OnFixedUpdate(ts); },
				[&scene](Timestep ts) { scene.OnUpdateRuntime(ts); },
				GameApp::PhaseCallback());

			app.Tick(Timestep(0.05f));   // 1/60 固定步 x3 + 1 个可变帧
			const double elapsedRunning = scene.GetTime().ElapsedSeconds;
			const uint64_t stepsRunning = scene.GetTime().FixedStepCount;
			CHECK(elapsedRunning > 0.0);
			CHECK(stepsRunning == 3);

			app.SetPaused(true);
			app.Tick(Timestep(0.05f));
			CHECK(scene.GetTime().ElapsedSeconds == elapsedRunning);   // 暂停不涨
			CHECK(scene.GetTime().FixedStepCount == stepsRunning);

			app.SetPaused(false);
			app.Tick(Timestep(0.05f));
			CHECK(scene.GetTime().ElapsedSeconds > elapsedRunning);    // 恢复后单调
			CHECK(scene.GetTime().FixedStepCount > stepsRunning);
			GameApp::Shutdown();
		}

		// =====================================================================
		// E. Lua 只读 Time 服务:读数与 C++ 逐位一致,且表不可写
		// =====================================================================
		{
			ScriptEngine::Init();
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);
			Scene::FrameTimeService& time = scene.MutableTime();
			time.ElapsedSeconds = 12.5;
			time.DeltaSeconds = 0.0166666675f;   // 1/60f
			time.FixedDeltaSeconds = kFixedStep;
			time.FrameCount = 42;
			time.FixedStepCount = 7;
			time.TimeScale = 0.75f;

			RunLua(R"(
				PROBE_ELAPSED = Time.Elapsed()
				PROBE_DELTA = Time.Delta()
				PROBE_FIXED = Time.FixedDelta()
				PROBE_FRAMES = Time.FrameCount()
				PROBE_STEPS = Time.FixedStepCount()
				PROBE_SCALE = Time.TimeScale()
				PROBE_SHAPE = type(Time.Elapsed) == "function" and type(Time.Delta) == "function"
					and type(Time.FixedDelta) == "function" and type(Time.FrameCount) == "function"
					and type(Time.FixedStepCount) == "function" and type(Time.TimeScale) == "function"
				-- 服务表用 __newindex 锁住"新键"(与既有 Input/Level/Save 同一口径)。
				PROBE_WRITE = pcall(function() Time.GhostField = 1 end)
			)", "TimeServiceProbe");

			CHECK(NumberGlobal("PROBE_ELAPSED") == time.ElapsedSeconds);
			CHECK(NumberGlobal("PROBE_DELTA") == static_cast<double>(time.DeltaSeconds));
			CHECK(NumberGlobal("PROBE_FIXED") == static_cast<double>(time.FixedDeltaSeconds));
			CHECK(NumberGlobal("PROBE_FRAMES") == static_cast<double>(time.FrameCount));
			CHECK(NumberGlobal("PROBE_STEPS") == static_cast<double>(time.FixedStepCount));
			CHECK(NumberGlobal("PROBE_SCALE") == static_cast<double>(time.TimeScale));
			CHECK(!BoolGlobal("PROBE_WRITE"));   // 服务表只读(__newindex 拒绝)
			CHECK(BoolGlobal("PROBE_SHAPE"));    // 6 个只读方法都在

			ScriptEngine::SetActiveScene(nullptr);
			ScriptEngine::Shutdown();
		}

		std::printf("[World.FrameTime] WP5 input sampling + frame time service checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "[World.FrameTime] failed: %s\n", error.what());
		if (ScriptEngine::IsInitialized())
			ScriptEngine::Shutdown();
		return 1;
	}
}
