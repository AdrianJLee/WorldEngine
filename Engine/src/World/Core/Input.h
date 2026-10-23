#pragma once
#include "World/Core/Core.h"

#include <utility>

namespace World
{
	class WLD_API Input
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

		// 捕获态（指针锁定）下的指针位移兜底。正常路径用 Raw Input；当原始输入
		// 不可用（远程桌面/安全软件拦截/设备注册失败）时，用 GLFW 在 disabled
		// 模式下报告的虚拟指针位移，保证鼠标仍能控制镜头。
		static void AccumulateMouseDelta(float dx, float dy);
		static std::pair<float, float> ConsumeMouseDelta();
	};
}
