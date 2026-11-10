// World.UiPointerGate — GameUI(M9)UI 输入路由闭环,headless。
//
// 覆盖(派工单验收):
//   A. `GameHost::SetPointerCaptured` 语义:
//      * 捕获=true ⇒ 鼠标键位一律按抬起(InputSystem 快照里**不产生按下**,也没有按下沿);
//      * 捕获=false ⇒ 鼠标动作正常进入快照;
//      * 只影响指针:同一帧的键盘动作不受影响;
//      * "消费一次即复位":宿主下一帧不再设置 ⇒ 鼠标恢复(捕获不跨帧泄漏)。
//   B. `UiHost::RouteInput` 未启用(没有 `.wui`)⇒ 返回 false 且**零命令、零副作用**。
//   C. `UiPlatformInputSampler` 无窗口(headless)⇒ 零状态、零边沿(绝不轮询不存在的窗口)。
//
// 说明:headless 没有 Application/窗口 ⇒ GameHost 的平台轮询分支不执行,这里的"注入"直接喂
// InputService(与 FrameTimeTests A2 同一路径)。真实窗口才成立的"平台轮询 + 滚轮归零"分支
// 本文件覆盖不到(报告已注明,不谎报)。

#include "World/Core/Core.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Framework/InputMap.h"
#include "World/Gameplay/Framework/InputSystem.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Gameplay/Runtime/GameHost.h"
#include "World/Scene/Scene.h"
#include "World/UI/UiHost.h"
#include "World/UI/UiInputRouter.h"
#include "World/WUI/WuiContext.h"

#include <cstdio>
#include <stdexcept>
#include <string>

#include <glm/glm.hpp>

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

	// 探针映射:一个鼠标动作(Fire = 左键)+ 一个键盘动作(Jump = 空格)。
	InputMap MakePointerProbeMap()
	{
		InputMap map;
		InputAction fire;
		fire.Name = "Fire";
		fire.Bindings = { { InputDevice::Mouse, 0 } };
		InputAction jump;
		jump.Name = "Jump";
		jump.Bindings = { { InputDevice::Key, 32 } };
		map.Actions() = { fire, jump };
		return map;
	}

	// ---- A. GameHost 指针捕获语义 ----

	void TestHostPointerCapture()
	{
		WorldContext context;
		Ref<Scene> scene = CreateRef<Scene>(context);
		scene->EnsureDefaultFrameSystems();

		GameHost host;
		GameAppDesc desc;
		desc.ProjectId = "worldengine-test-ui-pointer-gate";
		desc.FixedStepHz = 60;
		host.Init(desc);
		host.SetScene(scene, /*startRuntime=*/true);
		CHECK(host.IsRuntimeStarted());

		InputService& input = GameApp::Get().Input();
		input.SetMap(MakePointerProbeMap());
		input.SetPlayerCount(1);

		const Timestep frame(1.0f / 60.0f);

		// A1:未捕获 ⇒ 注入的鼠标按下进入快照(对照组;证明后面的"没有按下"不是假阴性)。
		input.SetKeyState(0, InputDevice::Mouse, 0, true);
		host.Tick(frame, false);
		CHECK(InputSystem::IsDown(*scene, "Fire"));
		CHECK(InputSystem::WasPressed(*scene, "Fire"));

		// A2:捕获 ⇒ 同一注入被压成抬起 ⇒ 快照里没有按下,也没有按下沿。
		host.SetPointerCaptured(true);
		host.Tick(frame, false);
		CHECK(!host.IsPointerCaptured());   // "消费一次即复位"
		CHECK(!InputSystem::IsDown(*scene, "Fire"));
		CHECK(!InputSystem::WasPressed(*scene, "Fire"));
		CHECK(!InputSystem::IsDown(*scene, "Jump"));

		// A3:捕获只影响指针 —— 同一帧键盘动作不受影响。
		input.SetKeyState(0, InputDevice::Key, 32, true);
		host.SetPointerCaptured(true);
		host.Tick(frame, false);
		CHECK(InputSystem::IsDown(*scene, "Jump"));
		CHECK(!InputSystem::IsDown(*scene, "Fire"));

		// A4:"消费一次即复位" —— 宿主不再设置 ⇒ 下一帧鼠标恢复,捕获不跨帧泄漏。
		input.SetKeyState(0, InputDevice::Mouse, 0, true);
		host.Tick(frame, false);
		CHECK(InputSystem::IsDown(*scene, "Fire"));

		host.StopRuntime();
		host.Shutdown();
		CHECK(!GameApp::Exists());
	}

	// ---- B. UiHost::RouteInput 未启用 ⇒ false + 零命令 ----

	void TestRouteInputDisabled()
	{
		UiHost host;   // 未 Initialize = 没有 `.wui`
		CHECK(!host.Enabled());
		CHECK(host.DocumentPath().empty());
		CHECK(host.Commands().Empty());

		Wui::WuiInputState click;
		click.MousePos = { 40.0f, 30.0f };
		click.MouseDown[0] = true;
		click.MouseClicked[0] = true;
		click.Wheel = -1.0f;
		click.ViewportSize = { 1280.0f, 720.0f };

		CHECK(!host.RouteInput(click));   // 未启用 ⇒ 不消费指针
		CHECK(host.Commands().Empty());   // 零命令(零副作用)
		CHECK(host.Commands().Size() == 0);

		// ClearCommands 幂等;未启用时重复路由仍然零副作用。
		host.ClearCommands();
		CHECK(host.Commands().Empty());
		CHECK(!host.RouteInput(click));
		CHECK(host.Commands().Empty());
	}

	// ---- C. 平台采样器 headless 回退 ----

	void TestSamplerHeadlessFallback()
	{
		UiPlatformInputSampler sampler;
		const Wui::WuiInputState state = sampler.Sample(glm::vec2 { 1280.0f, 720.0f });
		CHECK(state.MousePos.x == 0.0f && state.MousePos.y == 0.0f);
		CHECK(!state.MouseDown[0] && !state.MouseDown[1] && !state.MouseDown[2]);
		CHECK(!state.MouseClicked[0] && !state.MouseClicked[1] && !state.MouseClicked[2]);
		CHECK(state.Wheel == 0.0f);
		CHECK(state.ViewportSize.x == 1280.0f && state.ViewportSize.y == 720.0f);
	}
}

int main()
{
	try
	{
		TestHostPointerCapture();
		TestRouteInputDisabled();
		TestSamplerHeadlessFallback();
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.UiPointerGate FAILED: %s\n", error.what());
		return 1;
	}
	std::printf("World.UiPointerGate OK\n");
	return 0;
}
