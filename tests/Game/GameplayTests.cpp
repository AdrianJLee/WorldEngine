// P2a W1 回归:会话(GameApp)、流程(GameFlow)与宿主粘合层(GameHost)。
// 不依赖窗口/GPU:GameHost 在无 Application 时自持 WorldContext,渲染器可缺省。
#include "World/Gameplay/GameApp.h"
#include "World/Gameplay/GameFlow.h"
#include "World/Gameplay/GameHost.h"

#include "World/Core/WorldContext.h"
#include "World/Scene/Scene.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	using World::Timestep;
	using World::Gameplay::FlowState;
	using World::Gameplay::FlowStateName;
	using World::Gameplay::GameApp;
	using World::Gameplay::GameAppDesc;
	using World::Gameplay::GameFlow;
	using World::Gameplay::GameHost;

	constexpr float kStep60 = 1.0f / 60.0f;
}

int main()
{
	try
	{
		// 1. GameFlow:启动只触发 onEnter;切换在安全点生效且 enter/exit 成对。
		{
			GameFlow flow;
			std::vector<std::string> log;
			const auto record = [&log](const char* phase, FlowState other)
			{
				log.push_back(std::string(phase) + ":" + FlowStateName(other));
			};

			CHECK(flow.Current() == FlowState::Boot);
			CHECK(!flow.ApplyPending(1)); // 未 Start 时不产生切换

			// 显式标签:onEnter 收到的是"上一个状态",用 FlowStateName 拼日志会歧义。
			flow.RegisterState(FlowState::Boot,
				[&log](FlowState) { log.push_back("enter:Boot"); });
			flow.RegisterState(FlowState::Playing,
				[&log](FlowState) { log.push_back("enter:Playing"); },
				[&log](FlowState) { log.push_back("exit:Playing"); });
			flow.RegisterState(FlowState::Paused,
				[&log](FlowState) { log.push_back("enter:Paused"); },
				[&log](FlowState) { log.push_back("exit:Paused"); });

			flow.Start(FlowState::Boot, 0);
			CHECK(flow.Current() == FlowState::Boot);
			CHECK(flow.History().empty()); // 初始状态不算迁移
			CHECK(log.size() == 1);
			CHECK(log[0] == "enter:Boot");

			// 重复 Start 被忽略(不重置 history、不重复 onEnter)。
			flow.Start(FlowState::MainMenu, 5);
			CHECK(flow.Current() == FlowState::Boot);
			CHECK(log.size() == 1);

			flow.Request(FlowState::Playing);
			CHECK(flow.HasPending());
			CHECK(flow.Current() == FlowState::Boot); // 请求不立即生效

			CHECK(flow.ApplyPending(10));
			CHECK(flow.Current() == FlowState::Playing);
			CHECK(!flow.HasPending());
			// Boot 只注册了 onEnter(无 onExit),因此这里只多一条 enter:Playing。
			CHECK(log.size() == 2);
			CHECK(log[1] == "enter:Playing");
			CHECK(flow.History().size() == 1);
			CHECK(flow.History()[0].From == FlowState::Boot);
			CHECK(flow.History()[0].To == FlowState::Playing);
			CHECK(flow.History()[0].Frame == 10);

			// 无排队时 ApplyPending 不产生迁移。
			CHECK(!flow.ApplyPending(11));
			CHECK(flow.History().size() == 1);

			// 重复请求以最后一次为准:Paused -> Playing 被 Playing 覆盖,等于当前状态,不切换。
			flow.Request(FlowState::Paused);
			flow.Request(FlowState::Playing);
			CHECK(!flow.ApplyPending(12));
			CHECK(flow.Current() == FlowState::Playing);
			CHECK(log.size() == 2);

			// 真正的切换:exit(Playing) 先于 enter(Paused)。
			flow.Request(FlowState::Paused);
			CHECK(flow.ApplyPending(13));
			CHECK(flow.Current() == FlowState::Paused);
			CHECK(log.size() == 4);
			CHECK(log[2] == "exit:Playing");
			CHECK(log[3] == "enter:Paused");
			CHECK(flow.History().size() == 2);
			CHECK(flow.History()[1].Frame == 13);
		}

		// 2. GameApp:固定步长累积、单帧步数上限、暂停与阶段计时。
		{
			GameAppDesc desc;
			desc.ProjectId = "gameplay-test";
			desc.FixedStepHz = 60;
			desc.MaxFixedStepsPerFrame = 4;

			if (GameApp::Exists())
				GameApp::Shutdown();
			GameApp::Create(desc);
			CHECK(GameApp::Exists());
			CHECK(GameApp::TryGet() != nullptr);
			CHECK(std::fabs(GameApp::Get().FixedStepSeconds() - 1.0 / 60.0) < 1e-9);

			int fixedCount = 0;
			int updateCount = 0;
			int lateCount = 0;
			double fixedSeconds = 0.0;
			GameApp::Get().SetPhaseCallbacks(
				[&](Timestep ts) { ++fixedCount; fixedSeconds += ts.GetSeconds(); },
				[&](Timestep) { ++updateCount; },
				[&](Timestep) { ++lateCount; });

			GameApp::Get().Tick(Timestep(kStep60));
			CHECK(GameApp::Get().LastFixedSteps() == 1);
			CHECK(fixedCount == 1);
			CHECK(updateCount == 1);
			CHECK(lateCount == 1);
			CHECK(std::fabs(fixedSeconds - 1.0 / 60.0) < 1e-6);

			// 累积:一帧 2 步的输入产生 2 次固定更新,可变阶段仍只有 1 次。
			GameApp::Get().Tick(Timestep(2.0f * kStep60));
			CHECK(GameApp::Get().LastFixedSteps() == 2);
			CHECK(fixedCount == 3);
			CHECK(updateCount == 2);

			// 巨大 dt 被截断到 0.25s,步数受 MaxFixedStepsPerFrame 限制,不出现死亡螺旋。
			GameApp::Get().Tick(Timestep(1.0f));
			CHECK(GameApp::Get().LastFixedSteps() == 4);
			CHECK(fixedCount == 7);

			// 暂停:固定与可变阶段都停,步数读数归零。
			GameApp::Get().SetPaused(true);
			CHECK(GameApp::Get().IsPaused());
			GameApp::Get().Tick(Timestep(kStep60));
			CHECK(GameApp::Get().LastFixedSteps() == 0);
			CHECK(fixedCount == 7);
			CHECK(updateCount == 3);

			// 恢复:上限截断只保留一个步长的积压,因此恢复帧恰好推进 2 步。
			GameApp::Get().SetPaused(false);
			GameApp::Get().Tick(Timestep(kStep60));
			CHECK(GameApp::Get().LastFixedSteps() == 2);
			CHECK(fixedCount == 9);
			CHECK(updateCount == 4);

			CHECK(GameApp::Get().FrameNumber() == 5);
			CHECK(!GameApp::Get().LastFramePhases().empty());
			CHECK(GameApp::Get().Flow().Current() == FlowState::Boot);

			// 会话唯一:重复 Create 不覆盖已有会话(FixedStepHz 保持 60)。
			GameAppDesc other;
			other.FixedStepHz = 120;
			GameApp::Create(other);
			CHECK(GameApp::Get().Desc().FixedStepHz == 60);

			GameApp::Get().Shutdown();
			CHECK(!GameApp::Exists());
			CHECK(GameApp::TryGet() == nullptr);
		}

		// 3. GameApp:RunFixedWhenPaused 允许固定步长在暂停时继续(暂停菜单里的服务器模拟)。
		{
			GameAppDesc desc;
			desc.ProjectId = "paused-fixed-test";
			desc.FixedStepHz = 30;
			desc.RunFixedWhenPaused = true;

			GameApp::Create(desc);
			int fixedCount = 0;
			int updateCount = 0;
			GameApp::Get().SetPhaseCallbacks(
				[&](Timestep) { ++fixedCount; },
				[&](Timestep) { ++updateCount; },
				nullptr);

			GameApp::Get().SetPaused(true);
			GameApp::Get().Tick(Timestep(1.0f / 30.0f));
			CHECK(fixedCount == 1);
			CHECK(updateCount == 0);
			CHECK(std::fabs(GameApp::Get().FixedStepSeconds() - 1.0 / 30.0) < 1e-9);

			// 非法参数消毒:0 Hz 回落到 60 Hz。
			GameApp::Get().Shutdown();
			GameAppDesc sanitized;
			sanitized.FixedStepHz = 0;
			sanitized.MaxFixedStepsPerFrame = 0;
			GameApp::Create(sanitized);
			CHECK(GameApp::Get().Desc().FixedStepHz == 60);
			CHECK(std::fabs(GameApp::Get().FixedStepSeconds() - 1.0 / 60.0) < 1e-9);
			GameApp::Get().Shutdown();
		}

		// 4. GameHost:Init 幂等、加载失败不换场景、Simulate 语义、启动/停止运行时。
		{
			World::WorldContext context;
			World::Ref<World::Scene> scene = World::CreateRef<World::Scene>(context);

			GameAppDesc desc;
			desc.ProjectId = "host-test";
			desc.FixedStepHz = 60;

			GameHost host;
			host.Init(desc);
			CHECK(GameApp::Exists());
			GameApp* session = GameApp::TryGet();

			// Init 幂等:重复调用复用同一会话,不重建。
			host.Init(desc);
			CHECK(GameApp::TryGet() == session);

			// 加载失败:返回 false,保持当前(空)场景,不抛异常。
			CHECK(!host.LoadLevel("scenes/__worldengine_missing_scene__.wd"));
			CHECK(host.GetScene() == nullptr);
			host.Tick(Timestep(kStep60), true); // 无场景 + 无渲染器:不崩

			// Simulate:注入场景但不启动运行时,宿主不驱动 OnUpdateRuntime。
			host.SetScene(scene, false);
			CHECK(host.GetScene() == scene);
			CHECK(!host.IsRuntimeStarted());
			host.Tick(Timestep(kStep60), true);
			CHECK(!scene->IsRunning());

			// Play:启动运行时,场景进入 Running,宿主接管更新与渲染提交(无渲染器时只更新)。
			host.StartRuntime();
			CHECK(host.IsRuntimeStarted());
			CHECK(scene->IsRunning());
			host.Tick(Timestep(kStep60), true);

			host.StopRuntime();
			CHECK(!host.IsRuntimeStarted());
			CHECK(!scene->IsRunning());
			host.Tick(Timestep(kStep60), false);

			// Shutdown 释放场景与会话。
			host.Shutdown();
			CHECK(host.GetScene() == nullptr);
			CHECK(!GameApp::Exists());
		}

		std::printf("World.Gameplay: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::printf("World.Gameplay FAILED: %s\n", error.what());
		return 1;
	}
}
