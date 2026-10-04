#pragma once
#include "World/Core/Core.h"

#include <utility>

namespace World
{
	class Input
	{
	public:
		static bool IsKeyPressed(int keycode);
		static bool IsMouseButtonPressed(int button);
		static std::pair<float, float> GetMousePosition();
		static float GetMouseX();
		static float GetMouseY();
		// WP5:滚轮增量。平台事件把它**累积**起来,宿主在每帧采样输入后调用 ResetScrollDelta,
		// 因此读到的永远是"本帧"的增量;窗口事件只喂给 UI 时它保持 {0,0}。
		static void AccumulateScroll(float xOffset, float yOffset);
		static std::pair<float, float> GetScrollDelta();
		static void ResetScrollDelta();
	};
}
