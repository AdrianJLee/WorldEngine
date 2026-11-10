#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputTypes.h"
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

namespace World::Gameplay
{
	// 鼠标键是**设备状态**，与输入映射表的绑定无关：脚本的
	// `Input.MouseButton(1)` 必须能看到右键，即使输入表只绑了左键
	// （这是"右键拖拽转镜头不响应"的真因）。
	// 抽成纯函数是为了能在无窗口环境里验证：注入一个假的 isPressed 即可。
	// 语义：pointerCaptured=true（UI 吃掉指针）时全部清零；只采样前 5 个物理键。
	WLD_API void SampleRawMouseButtons(RawInputState& outState, bool pointerCaptured,
		const std::function<bool(int)>& isPressed);

	class WLD_API IInputSource
	{
	public:
		virtual ~IInputSource() = default;
		virtual void Poll(uint32_t player, RawInputState& outState, float dt) = 0;
	};

	// 硬件物理输入源 (GLFW 键盘/鼠标 + GamepadBackend XInput/GLFW 手柄 + WindowsRawInput 原始鼠标)
	class WLD_API DeviceInputSource : public IInputSource
	{
	public:
		DeviceInputSource() = default;
		void Poll(uint32_t player, RawInputState& outState, float dt) override;

		void SetPointerCaptured(bool captured) { m_PointerCaptured = captured; }
		bool IsPointerCaptured() const { return m_PointerCaptured; }

	private:
		bool m_PointerCaptured = false;
	};

	// AI 注入输入源
	struct InjectedAction
	{
		NameId Action;
		float Value = 1.0f;
		uint32_t RemainingFrames = 1;
	};

	class WLD_API AiInjectionInputSource : public IInputSource
	{
	public:
		void InjectAction(uint32_t player, NameId action, float value, uint32_t frames);
		void Clear(uint32_t player);
		void Poll(uint32_t player, RawInputState& outState, float dt) override;

		bool HasPendingInjections(uint32_t player) const;

	private:
		std::unordered_map<uint32_t, std::vector<InjectedAction>> m_Injections;
	};
}
