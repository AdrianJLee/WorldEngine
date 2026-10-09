#include "World/Gameplay/Framework/InputFrame.h"
#include "World/Gameplay/Framework/InputSource.h"
#include "World/Gameplay/Framework/InputReplay.h"
#include "World/Gameplay/Framework/InputRemap.h"
#include "World/Gameplay/Framework/InputGlyphs.h"
#include "World/Gameplay/Framework/InputTouch.h"
#include "World/Platform/Windows/WindowsIme.h"
#include "World/Platform/Windows/WindowsRawInput.h"
#include "World/Gameplay/Framework/InputModifier.h"
#include "World/Gameplay/Framework/InputTrigger.h"
#include "World/Gameplay/Framework/InputContext.h"
// P2a W7 & Enhanced Input M0: 输入映射资产 + InputService (NameId驻留/动作/轴/多玩家槽位/零堆边沿判定)。
#include "World/Gameplay/Framework/InputMap.h"
#include "World/Core/StringPool.h"
#include "World/Gameplay/Framework/GamepadBackend.h"
#include "World/Events/DeviceEvent.h"


#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)
}

int main()
{
	try
	{
		using namespace World::Gameplay;

		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / "worldengine-input-test.weinput";

		// 1. 资产往返:动作/轴/绑定都能存读一致。
		{
			InputMap map;
			InputAction jump;
			jump.Name = "Jump";
			jump.Bindings = { { InputDevice::Key, 32 }, { InputDevice::Gamepad, 0 } };
			InputAction right;
			right.Name = "MoveRight";
			right.Bindings = { { InputDevice::Key, 68 } };
			InputAction left;
			left.Name = "MoveLeft";
			left.Bindings = { { InputDevice::Key, 65 } };
			map.Actions() = { jump, right, left };
			InputAxis move;
			move.Name = "Move";
			move.PositiveAction = "MoveRight";
			move.NegativeAction = "MoveLeft";
			move.GamepadAxis = "LX";
			map.Axes() = { move };

			std::string error;
			CHECK(InputMap::Save(path, map, &error));
			CHECK(error.empty());

			InputMap loaded;
			CHECK(InputMap::Load(path, &loaded, &error));
			CHECK(loaded.Actions().size() == 3);
			CHECK(loaded.Axes().size() == 1);
			const InputAction* loadedJump = loaded.FindAction("Jump");
			CHECK(loadedJump != nullptr && loadedJump->Bindings.size() == 2);
			CHECK(loadedJump->Bindings[1].Device == InputDevice::Gamepad);
			const InputAxis* loadedMove = loaded.FindAxis("Move");
			CHECK(loadedMove != nullptr && loadedMove->PositiveAction == "MoveRight");
			CHECK(loadedMove->GamepadAxis == "LX");
			CHECK(loaded.FindAction("Missing") == nullptr);

			// M0 增强断言: NameId 驻留有效
			World::NameId jumpId = World::StringPool::Get().InternName("Jump");
			World::NameId moveId = World::StringPool::Get().InternName("Move");
			CHECK(loadedJump->Id == jumpId);
			CHECK(loadedMove->Id == moveId);
			CHECK(loaded.FindAction(jumpId) == loadedJump);
			CHECK(loaded.FindAxis(moveId) == loadedMove);

			// M40:上下文往返 —— 映射上的 modifiers / triggers 必须原样存回。
			// 此前 Save 完全不写这两块(整个 Save 里 "Trigger" 出现 0 次),编辑器 Input Map 面板
			// 保存一次就把它们抹掉:实测模板工程只重绑一个键,5 个 trigger 块全部消失。上面那段往返
			// 只建了 actions/axes、没有 contexts,因此看不见这个缺陷 —— 本段即为该缺陷的回归。
			{
				InputMap contextual;
				InputMappingContext walking("Walking", 0, false);
				ActionBindingConfig holdCfg;
				holdCfg.ActionName = "Sprint";
				holdCfg.Binding = { InputDevice::Key, 340 };
				holdCfg.Triggers.push_back(InputTrigger::MakeHold(0.3f));
				holdCfg.Modifiers.push_back(InputModifier::MakeDeadZone(0.1f, 0.9f, true));
				walking.AddMapping(holdCfg);
				ActionBindingConfig tapCfg;
				tapCfg.ActionName = "Interact";
				tapCfg.Binding = { InputDevice::Key, 70 };
				tapCfg.Triggers.push_back(InputTrigger::MakeTap(0.25f));
				tapCfg.Triggers.push_back(InputTrigger::MakeDoubleTap(0.3f, 0.2f));
				tapCfg.Triggers.push_back(InputTrigger::MakePulse(0.12f));
				walking.AddMapping(tapCfg);
				contextual.Contexts() = { walking };

				CHECK(InputMap::Save(path, contextual, &error));
				InputMap reloaded;
				CHECK(InputMap::Load(path, &reloaded, &error));
				CHECK(reloaded.Contexts().size() == 1);
				const InputMappingContext& reloadedCtx = reloaded.Contexts().front();
				CHECK(reloadedCtx.GetName() == "Walking");
				CHECK(reloadedCtx.GetMappings().size() == 2);

				const ActionBindingConfig& hold = reloadedCtx.GetMappings()[0];
				CHECK(hold.ActionName == "Sprint");
				CHECK(hold.Triggers.size() == 1);
				CHECK(hold.Triggers[0].Type == TriggerType::Hold);
				CHECK(std::fabs(hold.Triggers[0].Duration - 0.3f) < 1e-4f);
				CHECK(hold.Modifiers.size() == 1);
				CHECK(hold.Modifiers[0].Type == ModifierType::DeadZone);
				CHECK(std::fabs(hold.Modifiers[0].LowerThreshold - 0.1f) < 1e-4f);
				CHECK(std::fabs(hold.Modifiers[0].UpperThreshold - 0.9f) < 1e-4f);
				CHECK(hold.Modifiers[0].Radial);

				const ActionBindingConfig& tap = reloadedCtx.GetMappings()[1];
				CHECK(tap.ActionName == "Interact");
				CHECK(tap.Triggers.size() == 3);
				CHECK(tap.Triggers[0].Type == TriggerType::Tap);
				CHECK(std::fabs(tap.Triggers[0].Duration - 0.25f) < 1e-4f);
				CHECK(tap.Triggers[1].Type == TriggerType::DoubleTap);
				CHECK(std::fabs(tap.Triggers[1].Interval - 0.3f) < 1e-4f);
				CHECK(std::fabs(tap.Triggers[1].Duration - 0.2f) < 1e-4f);
				CHECK(tap.Triggers[2].Type == TriggerType::Pulse);
				CHECK(std::fabs(tap.Triggers[2].Interval - 0.12f) < 1e-4f);
			}

			// 2. 服务:键状态 -> 动作/轴;边沿判定依赖 EndFrame。
			InputService input;
			input.SetMap(loaded);
			input.SetPlayerCount(2);
			CHECK(input.GetPlayerCount() == 2);

			input.SetKeyState(0, InputDevice::Key, 32, true);   // 玩家0 按下 Jump
			CHECK(input.ActionDown("Jump", 0));
			CHECK(input.ActionPressed("Jump", 0));              // 首次按下 = pressed
			CHECK(input.ActionDown(jumpId, 0));                 // NameId 重载断言
			CHECK(input.ActionPressed(jumpId, 0));
			CHECK(!input.ActionDown("Jump", 1));                // 玩家1 未按
			CHECK(!input.ActionDown(jumpId, 1));

			input.EndFrame();
			CHECK(input.ActionDown("Jump", 0));
			CHECK(input.ActionDown(jumpId, 0));
			CHECK(!input.ActionPressed("Jump", 0));             // 持续按住不再是 pressed
			CHECK(!input.ActionPressed(jumpId, 0));

			input.SetKeyState(0, InputDevice::Key, 32, false);
			CHECK(input.ActionReleased("Jump", 0));             // 松开 = released
			CHECK(input.ActionReleased(jumpId, 0));
			input.EndFrame();
			CHECK(!input.ActionReleased("Jump", 0));
			CHECK(!input.ActionReleased(jumpId, 0));

			// 3. 轴:键盘动作对 + 手柄轴 + 死区。
			input.SetKeyState(0, InputDevice::Key, 68, true);   // MoveRight
			CHECK(std::fabs(input.Axis("Move", 0) - 1.0f) < 1e-5f);
			CHECK(std::fabs(input.Axis(moveId, 0) - 1.0f) < 1e-5f);
			input.SetKeyState(0, InputDevice::Key, 68, false);
			input.SetKeyState(0, InputDevice::Key, 65, true);   // MoveLeft
			CHECK(std::fabs(input.Axis("Move", 0) + 1.0f) < 1e-5f);
			CHECK(std::fabs(input.Axis(moveId, 0) + 1.0f) < 1e-5f);
			input.SetKeyState(0, InputDevice::Key, 65, false);
			CHECK(std::fabs(input.Axis("Move", 0)) < 1e-5f);
			CHECK(std::fabs(input.Axis(moveId, 0)) < 1e-5f);

			input.SetGamepadAxis(0, "LX", 0.05f);               // 死区内
			CHECK(std::fabs(input.Axis("Move", 0)) < 1e-5f);
			CHECK(std::fabs(input.Axis(moveId, 0)) < 1e-5f);
			input.SetGamepadAxis(0, "LX", 0.8f);
			CHECK(std::fabs(input.Axis("Move", 0) - 0.8f) < 1e-5f);
			CHECK(std::fabs(input.Axis(moveId, 0) - 0.8f) < 1e-5f);

			// 4. 未知动作/轴与未初始化玩家都安全返回。
			World::NameId ghostId = World::StringPool::Get().InternName("Ghost");
			CHECK(!input.ActionDown("Ghost", 0));
			CHECK(!input.ActionDown(ghostId, 0));
			CHECK(!input.ActionPressed("Ghost", 0));
			CHECK(!input.ActionPressed(ghostId, 0));
			CHECK(std::fabs(input.Axis("Ghost", 0)) < 1e-5f);
			CHECK(std::fabs(input.Axis(ghostId, 0)) < 1e-5f);
			CHECK(!input.ActionDown("Jump", 7));
			CHECK(!input.ActionDown(jumpId, 7));
			CHECK(std::fabs(input.Axis("Move", 7)) < 1e-5f);
			CHECK(std::fabs(input.Axis(moveId, 7)) < 1e-5f);

			// 5. W7-4 快照:一次性求值全部动作/轴,供并行系统只读。
			input.SetKeyState(0, InputDevice::Key, 32, true);
			input.SetGamepadAxis(0, "LX", 0.6f);
			input.BuildSnapshot(0);
			const InputSnapshot& snapshot = input.GetSnapshot();
			CHECK(snapshot.IsDown("Jump"));
			CHECK(snapshot.IsDown(jumpId));
			CHECK(snapshot.WasPressed("Jump"));
			CHECK(snapshot.WasPressed(jumpId));
			CHECK(!snapshot.WasReleased("Jump"));
			CHECK(!snapshot.WasReleased(jumpId));
			CHECK(std::fabs(snapshot.GetAxis("Move") - 0.6f) < 1e-5f);
			CHECK(std::fabs(snapshot.GetAxis(moveId) - 0.6f) < 1e-5f);
			CHECK(!snapshot.IsDown("Ghost"));
			CHECK(!snapshot.IsDown(ghostId));
			CHECK(std::fabs(snapshot.GetAxis("Ghost")) < 1e-5f);
			CHECK(std::fabs(snapshot.GetAxis(ghostId)) < 1e-5f);

			// 快照是只读副本:之后喂入新状态不会改变已有快照。
			input.SetKeyState(0, InputDevice::Key, 32, false);
			CHECK(snapshot.IsDown("Jump"));
			CHECK(snapshot.IsDown(jumpId));

			// 6. M0 微基准: 验证 NameId 热路径高频查询无性能抖动
			for (int i = 0; i < 50000; ++i)
			{
				volatile bool down = input.ActionDown(jumpId, 0);
				volatile float ax = input.Axis(moveId, 0);
				(void)down;
				(void)ax;
			}

			input.Clear();
			CHECK(input.GetPlayerCount() == 0);
			std::filesystem::remove(path);
		}

		
			// 7. M1 测试: 手柄物理后端、热插拔事件、玩家设备配对、震动控制
			{
				InputService m1Input;
				m1Input.SetPlayerCount(2);

				int connectEvents = 0;
				int disconnectEvents = 0;
				m1Input.SetEventCallback([&](World::Event& ev) {
					if (ev.GetEventType() == World::EventType::DeviceConnected)
						++connectEvents;
					else if (ev.GetEventType() == World::EventType::DeviceDisconnected)
						++disconnectEvents;
				});

				// R2b:Init() 不得踩掉已激活的 mock —— 此前 Init 无条件 ClearMock,
			// 于是「先设 mock、后建会话」的顺序会把夹具状态抹掉;而 Init()
			// 本身在 R2b 之前没有任何生产调用点 ⇒ XInput 路径从未启用。
			{
				GamepadBackend::Get().ClearMock();
				float probeAxes[6] = { 0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
				GamepadBackend::Get().SetMockConnected(0, true);
				GamepadBackend::Get().SetMockState(0, 0x0002, probeAxes);
				CHECK(GamepadBackend::Get().IsMockActive());
				GamepadBackend::Get().Init();
				CHECK(GamepadBackend::Get().IsMockActive());   // 被 Init 踩掉就是回归
				GamepadState probeState;
				CHECK(GamepadBackend::Get().Poll(0, probeState));
				CHECK(probeState.Buttons == 0x0002);
				CHECK(std::fabs(probeState.Axes[0] - 0.25f) < 1e-4f);
				GamepadBackend::Get().ClearMock();
			}

			// (a) 模拟手柄连接与热插拔事件
				GamepadBackend::Get().ClearMock();
				GamepadBackend::Get().SetMockConnected(0, true);
				m1Input.PollDevices();
				CHECK(connectEvents == 1);
				CHECK(disconnectEvents == 0);

				// (b) 模拟手柄断开
				GamepadBackend::Get().SetMockConnected(0, false);
				m1Input.PollDevices();
				CHECK(connectEvents == 1);
				CHECK(disconnectEvents == 1);

				// (c) 重新连接并注入模拟按键/轴
				GamepadBackend::Get().SetMockConnected(0, true);
				float mockAxes[6] = { 0.75f, -0.5f, 0.0f, 0.0f, 1.0f, 0.0f };
				GamepadBackend::Get().SetMockState(0, 0x0001 /* A 键 */, mockAxes);
				m1Input.PollDevices();
				CHECK(connectEvents == 2);

				// 检查原始设备状态
				const RawInputState* raw = m1Input.GetRawState(0);
				CHECK(raw != nullptr);
				CHECK(raw->GamepadConnected[0]);
				CHECK(raw->GamepadButtons[0] == 0x0001);
				CHECK(std::fabs(raw->GamepadAxes[0][0] - 0.75f) < 1e-4f);
				CHECK(std::fabs(raw->GamepadAxes[0][1] - (-0.5f)) < 1e-4f);

				// (d) 玩家设备配对测试 (显式配对与查找)
				DeviceId pad1{ InputDevice::Gamepad, 1 };
				m1Input.BindPlayerDevice(1, pad1);
				CHECK(m1Input.GetPlayerDevice(1) == pad1);
				CHECK(m1Input.FindPlayerForDevice(pad1) == 1);
				CHECK(m1Input.FindPlayerForDevice(DeviceId{ InputDevice::Gamepad, 3 }) == -1);

				// (e) 震动 API 与生命周期衰减测试
				m1Input.SetVibration(0, 0.5f, 0.8f, 1.0f /* 1秒 */);
				float leftVib = 0.0f, rightVib = 0.0f;
				m1Input.GetVibration(0, &leftVib, &rightVib);
				CHECK(std::fabs(leftVib - 0.5f) < 1e-4f);
				CHECK(std::fabs(rightVib - 0.8f) < 1e-4f);

				float backendLeft = 0.0f, backendRight = 0.0f;
				GamepadBackend::Get().GetLastVibration(0, &backendLeft, &backendRight);
				CHECK(std::fabs(backendLeft - 0.5f) < 1e-4f);
				CHECK(std::fabs(backendRight - 0.8f) < 1e-4f);

				// 推进 0.5 秒: 震动依然有效
				m1Input.UpdateVibration(0.5f);
				m1Input.GetVibration(0, &leftVib, &rightVib);
				CHECK(std::fabs(leftVib - 0.5f) < 1e-4f);

				// 再推进 0.6 秒: 震动超时归零
				m1Input.UpdateVibration(0.6f);
				m1Input.GetVibration(0, &leftVib, &rightVib);
				CHECK(leftVib == 0.0f);
				CHECK(rightVib == 0.0f);
				GamepadBackend::Get().GetLastVibration(0, &backendLeft, &backendRight);
				CHECK(backendLeft == 0.0f);
				CHECK(backendRight == 0.0f);

				GamepadBackend::Get().ClearMock();
			}

			
			// 8. M2 测试: 修改器 (Modifiers)、触发器 (Triggers) 与 映射上下文栈 (Context Stack)
			{
				// (a) 修改器单元测试
				// DeadZone
				auto dzAxial = InputModifier::MakeDeadZone(0.2f, 1.0f, false);
				CHECK(std::fabs(dzAxial.Apply(glm::vec3(0.1f, 0.0f, 0.0f)).x) < 1e-5f);
				CHECK(dzAxial.Apply(glm::vec3(0.6f, 0.0f, 0.0f)).x > 0.0f);

				auto dzRadial = InputModifier::MakeDeadZone(0.3f, 1.0f, true);
				CHECK(glm::length(dzRadial.Apply(glm::vec3(0.2f, 0.2f, 0.0f))) < 1e-5f); // length ~0.28 < 0.3
				CHECK(glm::length(dzRadial.Apply(glm::vec3(0.5f, 0.5f, 0.0f))) > 0.0f);

				// Invert & Scale
				auto inv = InputModifier::MakeInvert(true, false, false);
				CHECK(std::fabs(inv.Apply(glm::vec3(1.0f, 2.0f, 0.0f)).x - (-1.0f)) < 1e-5f);
				CHECK(std::fabs(inv.Apply(glm::vec3(1.0f, 2.0f, 0.0f)).y - 2.0f) < 1e-5f);

				auto sc = InputModifier::MakeScale(2.5f);
				CHECK(std::fabs(sc.Apply(glm::vec3(2.0f, 0.0f, 0.0f)).x - 5.0f) < 1e-5f);

				// Swizzle (WASD -> Vec2 XY)
				auto swz = InputModifier::MakeSwizzle(1, 0, 2); // 交换 X 和 Y
				glm::vec3 swzRes = swz.Apply(glm::vec3(1.0f, 3.0f, 5.0f));
				CHECK(std::fabs(swzRes.x - 3.0f) < 1e-5f);
				CHECK(std::fabs(swzRes.y - 1.0f) < 1e-5f);

				// Normalize
				auto norm = InputModifier::MakeNormalize(1.0f);
				glm::vec3 normRes = norm.Apply(glm::vec3(3.0f, 4.0f, 0.0f)); // len 5 -> clamped to 1
				CHECK(std::fabs(glm::length(normRes) - 1.0f) < 1e-4f);

				// ResponseCurve (二次方衰减)
				auto curve = InputModifier::MakeResponseCurve(2.0f);
				CHECK(std::fabs(curve.Apply(glm::vec3(0.5f, 0.0f, 0.0f)).x - 0.25f) < 1e-5f);

				// (b) 触发器单元测试
				TriggerState tState;

				// Hold Trigger
				auto hold = InputTrigger::MakeHold(0.5f);
				CHECK(hold.Evaluate(true, 0.2f, tState) == TriggerPhase::Ongoing);
				CHECK(hold.Evaluate(true, 0.35f, tState) == TriggerPhase::Triggered); // 0.2 + 0.35 = 0.55 >= 0.5
				CHECK(hold.Evaluate(false, 0.1f, tState) == TriggerPhase::None);
				tState.Reset();

				// Tap Trigger
				auto tap = InputTrigger::MakeTap(0.2f);
				CHECK(tap.Evaluate(true, 0.1f, tState) == TriggerPhase::Ongoing);
				CHECK(tap.Evaluate(false, 0.05f, tState) == TriggerPhase::Triggered); // 按下 0.1s <= 0.2s 后抬起
				tState.Reset();

				// Pulse Trigger
				auto pulse = InputTrigger::MakePulse(0.1f);
				CHECK(pulse.Evaluate(true, 0.05f, tState) == TriggerPhase::Triggered); // 首帧触发
				CHECK(pulse.Evaluate(true, 0.05f, tState) == TriggerPhase::Ongoing);   // 0.05s 未达到 0.1s
				CHECK(pulse.Evaluate(true, 0.06f, tState) == TriggerPhase::Triggered); // 累计 0.11s >= 0.1s 脉冲触发
				tState.Reset();

				// Chord Trigger (需要协同动作)
				auto chord = InputTrigger::MakeChord(World::StringPool::Get().InternName("Ctrl"));
				CHECK(chord.Evaluate(true, 0.1f, tState, false /* chord 未激活 */) == TriggerPhase::None);
				CHECK(chord.Evaluate(true, 0.1f, tState, true  /* chord 激活 */) == TriggerPhase::Triggered);
				tState.Reset();

				// (c) 上下文栈 (Mapping Context Stack) 与优先级解算
				InputService m2Input;
				m2Input.SetPlayerCount(1);

				World::NameId fireAction = World::StringPool::Get().InternName("Fire");

				// 基础上下文: 键 32 (空格) 开火, 优先级 0
				InputMappingContext defaultCtx("Default", 0, true);
				ActionBindingConfig baseFire;
				baseFire.Action = fireAction;
				baseFire.Binding = { InputDevice::Key, 32 };
				defaultCtx.AddMapping(baseFire);

				// 载具上下文: 载具内空格改为手刹, 键 70 (F) 开火, 优先级 10 (更高)
				InputMappingContext vehicleCtx("Vehicle", 10, true);
				ActionBindingConfig vehFire;
				vehFire.Action = fireAction;
				vehFire.Binding = { InputDevice::Key, 70 };
				vehFire.Modifiers.push_back(InputModifier::MakeScale(2.0f));
				vehicleCtx.AddMapping(vehFire);

				m2Input.PushContext(defaultCtx, 0);
				CHECK(m2Input.HasContext(defaultCtx.GetId()));

				// 步行状态: 按空格开火
				m2Input.SetKeyState(0, InputDevice::Key, 32, true);
				ActionState evalState = m2Input.EvaluateActionState(fireAction, 0, 0.1f);
				CHECK(evalState.Triggered);
				CHECK(std::fabs(evalState.Value.x - 1.0f) < 1e-5f);

				// 上车: Push 载具上下文 (优先级 10)
				m2Input.PushContext(vehicleCtx, 10);
				CHECK(m2Input.GetContextStack().size() == 2);
				CHECK(m2Input.GetContextStack()[0].GetPriority() == 10); // 顶层优先级最高

				// 此时按空格不会触发开火 (被载具上下文覆盖/消费)
				evalState = m2Input.EvaluateActionState(fireAction, 0, 0.1f);
				CHECK(!evalState.Triggered);

				// 在载具内按 F 键开火 (带 2.0 缩放修改器)
				m2Input.SetKeyState(0, InputDevice::Key, 70, true);
				evalState = m2Input.EvaluateActionState(fireAction, 0, 0.1f);
				CHECK(evalState.Triggered);
				CHECK(std::fabs(evalState.Value.x - 2.0f) < 1e-5f);

				// 下车: Pop 载具上下文
				m2Input.PopContext(vehicleCtx.GetId());
				CHECK(!m2Input.HasContext(vehicleCtx.GetId()));
				CHECK(m2Input.GetContextStack().size() == 1);

				// 恢复步行: 按住的空格重新生效持续按下 (Down)
				evalState = m2Input.EvaluateActionState(fireAction, 0, 0.1f);
				CHECK(evalState.Down);

				// 释放空格再重新按下: 触发新的 Triggered 边沿
				m2Input.SetKeyState(0, InputDevice::Key, 32, false);
				m2Input.EvaluateActionState(fireAction, 0, 0.1f);
				m2Input.SetKeyState(0, InputDevice::Key, 32, true);
				evalState = m2Input.EvaluateActionState(fireAction, 0, 0.1f);
				CHECK(evalState.Triggered);
			}

			
			// 8b. R2:动作级注入 —— 必须能在"宿主每帧轮询覆写原始键位"的前提下把动作驱动起来。
			// 旧的 input.inject 直接写原始键位(SetKeyState),于是在有窗口的宿主里下一帧就被
			// GameHost 的 glfwGetKey 覆盖 ⇒ 注入在 Editor/Runtime 中必定无效(实测立案)。
			// 现在注入是解算结果之上的一层覆盖,与原始设备状态解耦。
			{
				InputService injected;
				InputMap map;
				InputAction fire;
				fire.Name = "Fire";
				fire.Bindings = { { InputDevice::Key, 32 } };
				map.Actions() = { fire };
				injected.SetMap(map);
				injected.SetPlayerCount(1);
				const World::NameId fireId = World::StringPool::Get().InternName("Fire");

				// 反假基线:没注入也没按键 -> 不激活
				CHECK(!injected.ActionDown(fireId, 0));
				CHECK(!injected.ActionPressed(fireId, 0));

				injected.InjectAction(0, fireId, 1.0f, 2);
				CHECK(injected.ActionDown(fireId, 0));
				CHECK(injected.ActionPressed(fireId, 0));    // 注入第一帧给出 Pressed 边沿
				// 同一帧内多次查询必须给同一答案(Pressed 是帧级事实,不是"报一次就消"),
				// 同时解算缓存只算一次 —— 这两件事互不冲突。
				CHECK(injected.ActionPressed(fireId, 0));

				// 宿主把原始键位轮询成"抬起" —— 注入不受影响(这正是旧实现失效的场景)
				injected.SetKeyState(0, InputDevice::Key, 32, false);
				CHECK(injected.ActionDown(fireId, 0));

				injected.EndFrame();
				CHECK(injected.ActionDown(fireId, 0));       // 仍在注入寿命内
				CHECK(!injected.ActionPressed(fireId, 0));   // 但不再是边沿

				injected.EndFrame();                         // 寿命耗尽,自动撤销
				CHECK(!injected.ActionDown(fireId, 0));
				CHECK(injected.ActionReleased(fireId, 0));
				injected.EndFrame();
				CHECK(!injected.ActionReleased(fireId, 0));  // 边沿只报一次

				// 显式清除
				injected.InjectAction(0, fireId, 1.0f, 10);
				CHECK(injected.ActionDown(fireId, 0));
				injected.ClearInjection(0);
				CHECK(!injected.ActionDown(fireId, 0));
			}

			// 8c. R2c/R3: CaptureFrame 与 ApplyFrame 往返, 以及 InputRemapManager 覆盖生效测试
			{
				InputService svc;
				InputMap map;
				InputAction jumpAction;
				jumpAction.Name = "Jump";
				jumpAction.Bindings = { { InputDevice::Key, 32 } };
				InputAction fireAction;
				fireAction.Name = "Fire";
				fireAction.Bindings = { { InputDevice::Key, 70 } };
				map.Actions() = { jumpAction, fireAction };

				InputAxis moveAxis;
				moveAxis.Name = "Move";
				moveAxis.PositiveAction = "Fire";
				map.Axes() = { moveAxis };

				svc.SetMap(map);
				svc.SetPlayerCount(1);
				const World::NameId jumpId = World::StringPool::Get().InternName("Jump");
				const World::NameId fireId = World::StringPool::Get().InternName("Fire");

				// 1) 模拟按下 Jump 并捕获 64B InputFrame
				svc.SetKeyState(0, InputDevice::Key, 32, true);
				CHECK(svc.ActionDown(jumpId, 0));
				InputFrame captured = svc.CaptureFrame(0, 42);
				CHECK(captured.FrameIndex == 42);
				CHECK(captured.PlayerIndex == 0);
				CHECK((captured.ActionButtons & 1ULL) != 0);       // 第 0 个动作 Jump 激活
				CHECK((captured.ActionButtons & 2ULL) == 0);       // 第 1 个动作 Fire 未激活

				// 2) 清空状态, 并通过 ApplyFrame 恢复
				svc.SetKeyState(0, InputDevice::Key, 32, false);
				CHECK(!svc.ActionDown(jumpId, 0));
				svc.ApplyFrame(captured);
				CHECK(svc.ActionDown(jumpId, 0));                 // 通过 InputFrame 成功恢复激活态
				CHECK(svc.ActionPressed(jumpId, 0));              // 第一帧给出上升沿

				// 3) InputRemapManager 覆盖生效测试: 将 Fire 重绑定到 Space(32)
				InputRemapManager& remap = InputRemapManager::Get();
				remap.ClearAllOverrides();
				remap.SetOverride(fireId, { { InputDevice::Key, 32 } });
				// 此时按键 32 会触发 Fire (来自覆盖层), 而不是原绑定的 70
				svc.ClearInjection(0);
				svc.SetKeyState(0, InputDevice::Key, 32, true);
				svc.SetKeyState(0, InputDevice::Key, 70, false);
				CHECK(svc.ActionDown(fireId, 0));
				remap.ClearAllOverrides();
				svc.InvalidateResolve();
				// 撤销覆盖后恢复原绑定 70
				CHECK(!svc.ActionDown(fireId, 0));
				svc.SetKeyState(0, InputDevice::Key, 70, true);
				CHECK(svc.ActionDown(fireId, 0));
				remap.ClearAllOverrides();
			}

			// 9. M3 测试: 定长 InputFrame (64B POD)、录制/回放 (.wreplay) 与 AI 注入源
			{
				// (a) InputFrame 尺寸与偏移静态断言验证
				CHECK(sizeof(InputFrame) == 64);

				// (b) 录制与回放往返
				const std::filesystem::path replayPath =
					std::filesystem::temp_directory_path() / "worldengine-test.wreplay";

				ReplayHeader header;
				header.RandomSeed = 123456789ULL;
				header.FixedDelta = 1.0f / 60.0f;

				std::vector<InputFrame> recordFrames;
				for (uint32_t f = 0; f < 10; ++f)
				{
					InputFrame frame;
					frame.FrameIndex = f;
					frame.PlayerIndex = 0;
					frame.ActionButtons = (1ULL << (f % 64));
					frame.MouseX = static_cast<int16_t>(100 + f * 5);
					frame.MouseY = static_cast<int16_t>(200 + f * 3);
					recordFrames.push_back(frame);
				}

				header.FinalStateHash = InputReplay::ComputeStateHash(recordFrames);
				CHECK(header.FinalStateHash != 0);

				std::string repErr;
				CHECK(InputReplay::Save(replayPath, header, recordFrames, &repErr));
				CHECK(repErr.empty());

				ReplayHeader loadedHeader;
				std::vector<InputFrame> loadedFrames;
				CHECK(InputReplay::Load(replayPath, &loadedHeader, &loadedFrames, &repErr));
				CHECK(loadedHeader.RandomSeed == 123456789ULL);
				CHECK(loadedHeader.FrameCount == 10);
				CHECK(loadedFrames.size() == 10);
				CHECK(InputReplay::ComputeStateHash(loadedFrames) == header.FinalStateHash);
				std::filesystem::remove(replayPath);

				// (c) ReplayInputSource 逐帧消费
				ReplayInputSource replaySource(loadedFrames);
				CHECK(!replaySource.IsFinished());
				RawInputState repRaw;
				replaySource.Poll(0, repRaw, 1.0f / 60.0f);
				CHECK(std::fabs(repRaw.MousePosition.x - 100.0f) < 1e-4f);

				// (d) AiInjectionInputSource 动作注入
				AiInjectionInputSource aiSource;
				World::NameId jumpId = World::StringPool::Get().InternName("Jump");
				aiSource.InjectAction(0, jumpId, 1.0f, 3 /* 3帧 */);
				CHECK(aiSource.HasPendingInjections(0));
				RawInputState aiRaw;
				aiSource.Poll(0, aiRaw, 1.0f / 60.0f); // 消耗第1帧
				aiSource.Poll(0, aiRaw, 1.0f / 60.0f); // 消耗第2帧
				aiSource.Poll(0, aiRaw, 1.0f / 60.0f); // 消耗第3帧
				aiSource.Poll(0, aiRaw, 1.0f / 60.0f); // 结束
				CHECK(!aiSource.HasPendingInjections(0));
			}

			// 10. M4 测试: 运行时重绑定、冲突检测、用户覆盖持久化与键位字形表
			{
				InputRemapManager& remap = InputRemapManager::Get();
				remap.ClearAllOverrides();

				World::NameId fire = World::StringPool::Get().InternName("Fire");
				World::NameId jump = World::StringPool::Get().InternName("Jump");

				remap.SetOverride(fire, { { InputDevice::Key, 32 } }); // 空格开火
				CHECK(remap.GetOverride(fire) != nullptr);

				// 冲突检测: 为 Jump 绑定空格应触发与 Fire 的冲突
				RemapConflict conflict;
				bool hasConflict = remap.HasConflict({ InputDevice::Key, 32 }, jump, &conflict);
				CHECK(hasConflict);
				CHECK(conflict.ConflictingAction == fire);

				// 用户覆盖文件序列化与读取 (user://input.overrides.yaml)
				const std::filesystem::path overridesPath =
					std::filesystem::temp_directory_path() / "input.overrides.yaml";
				std::string remapErr;
				CHECK(remap.SaveOverrides(overridesPath, &remapErr));
				remap.ClearAllOverrides();
				CHECK(remap.GetOverride(fire) == nullptr);

				CHECK(remap.LoadOverrides(overridesPath, &remapErr));
				CHECK(remap.GetOverride(fire) != nullptr);
				std::filesystem::remove(overridesPath);

				// 字形表与键帽映射
				CHECK(InputGlyphs::GetGlyphText(InputDevice::Key, 32) == "Space");
				CHECK(InputGlyphs::GetGlyphText(InputDevice::Gamepad, 0) == "A");
				CHECK(InputGlyphs::GetGlyphText(InputDevice::Mouse, 0) == "LMB");
			}

			// 11. M5 测试: 原始高精鼠标增量、IME 状态与触控点结构
			{
				// (a) 原始高精鼠标增量累积与复位
				World::Platform::WindowsRawInput::ResetAccumulated();
				CHECK(World::Platform::WindowsRawInput::GetAccumulatedRawX() == 0);

				// (b) IME 状态容器
				const auto& ime = World::Platform::WindowsIme::GetCurrentState();
				CHECK(!ime.Active);

				// (c) 触控输入预留结构 POD 验证
				TouchPoint touch;
				touch.FingerId = 1;
				touch.Position = glm::vec2(100.0f, 200.0f);
				touch.Phase = TouchPhase::Began;
				CHECK(touch.Phase == TouchPhase::Began);
			}

			std::printf("World.Input: all checks passed (including M0 NameId enhancements)\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Input FAILED: %s\n", error.what());
		return 1;
	}
}
