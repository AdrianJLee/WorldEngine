// P2a W7:输入映射资产 + InputService(动作/轴/多玩家槽位/边沿判定)。
#include "World/Gameplay/InputMap.h"

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

			// 2. 服务:键状态 -> 动作/轴;边沿判定依赖 EndFrame。
			InputService input;
			input.SetMap(loaded);
			input.SetPlayerCount(2);
			CHECK(input.GetPlayerCount() == 2);

			input.SetKeyState(0, InputDevice::Key, 32, true);   // 玩家0 按下 Jump
			CHECK(input.ActionDown("Jump", 0));
			CHECK(input.ActionPressed("Jump", 0));              // 首次按下 = pressed
			CHECK(!input.ActionDown("Jump", 1));                // 玩家1 未按

			input.EndFrame();
			CHECK(input.ActionDown("Jump", 0));
			CHECK(!input.ActionPressed("Jump", 0));             // 持续按住不再是 pressed

			input.SetKeyState(0, InputDevice::Key, 32, false);
			CHECK(input.ActionReleased("Jump", 0));             // 松开 = released
			input.EndFrame();
			CHECK(!input.ActionReleased("Jump", 0));

			// 3. 轴:键盘动作对 + 手柄轴 + 死区。
			input.SetKeyState(0, InputDevice::Key, 68, true);   // MoveRight
			CHECK(std::fabs(input.Axis("Move", 0) - 1.0f) < 1e-5f);
			input.SetKeyState(0, InputDevice::Key, 68, false);
			input.SetKeyState(0, InputDevice::Key, 65, true);   // MoveLeft
			CHECK(std::fabs(input.Axis("Move", 0) + 1.0f) < 1e-5f);
			input.SetKeyState(0, InputDevice::Key, 65, false);
			CHECK(std::fabs(input.Axis("Move", 0)) < 1e-5f);

			input.SetGamepadAxis(0, "LX", 0.05f);               // 死区内
			CHECK(std::fabs(input.Axis("Move", 0)) < 1e-5f);
			input.SetGamepadAxis(0, "LX", 0.8f);
			CHECK(std::fabs(input.Axis("Move", 0) - 0.8f) < 1e-5f);

			// 4. 未知动作/轴与未初始化玩家都安全返回。
			CHECK(!input.ActionDown("Ghost", 0));
			CHECK(!input.ActionPressed("Ghost", 0));
			CHECK(std::fabs(input.Axis("Ghost", 0)) < 1e-5f);
			CHECK(!input.ActionDown("Jump", 7));
			CHECK(std::fabs(input.Axis("Move", 7)) < 1e-5f);

			input.Clear();
			CHECK(input.GetPlayerCount() == 0);
			std::filesystem::remove(path);
		}

		std::printf("World.Input: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Input FAILED: %s\n", error.what());
		return 1;
	}
}
