namespace World { class Event; }
#include <functional>
#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputTypes.h"
#include "World/Gameplay/Framework/InputContext.h"


#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace World::Gameplay
{
	enum class DevicePairingMode : uint8_t
	{
		AutoClaim = 0, // 首次按键自动认领空闲玩家槽
		Explicit       // 显式由代码分配
	};

	struct VibrationState
	{
		float LeftMotor = 0.0f;
		float RightMotor = 0.0f;
		float RemainingDuration = 0.0f;
	};

	// Input.weinput 资产(YAML):动作/轴/绑定。与 project.we.yaml 同级放置,Mod 可覆盖。
	class WLD_API InputMap
	{
	public:
		static bool Load(const std::filesystem::path& path, InputMap* out, std::string* error = nullptr);
		static bool Save(const std::filesystem::path& path, const InputMap& map, std::string* error = nullptr);

		const std::vector<InputAction>& Actions() const { EnsureIndices(); return m_Actions; }
		std::vector<InputAction>& Actions() { m_IndicesDirty = true; return m_Actions; }
		const std::vector<InputAxis>& Axes() const { EnsureIndices(); return m_Axes; }
		const std::vector<InputMappingContext>& Contexts() const { return m_Contexts; }
		std::vector<InputMappingContext>& Contexts() { return m_Contexts; }

		std::vector<InputAxis>& Axes() { m_IndicesDirty = true; return m_Axes; }

		const InputAction* FindAction(NameId id) const;
		const InputAction* FindAction(const std::string& name) const;
		const InputAxis* FindAxis(NameId id) const;
		const InputAxis* FindAxis(const std::string& name) const;

		void RebuildIndices() const;

	private:
		void EnsureIndices() const { if (m_IndicesDirty) RebuildIndices(); }

		mutable std::vector<InputAction> m_Actions;
		mutable std::vector<InputAxis> m_Axes;
		std::vector<InputMappingContext> m_Contexts;

		mutable std::unordered_map<uint32_t, std::size_t> m_ActionIdToIndex;
		mutable std::unordered_map<std::string, std::size_t> m_ActionNameToIndex;
		mutable std::unordered_map<uint32_t, std::size_t> m_AxisIdToIndex;
		mutable std::unordered_map<std::string, std::size_t> m_AxisNameToIndex;
		mutable bool m_IndicesDirty = true;
	};

	// 输入服务(P2a W7):宿主每帧把原始设备状态喂进来,玩法/脚本只问动作与轴。
	// W7-4:每帧可生成只读快照,供并行安全系统读取(避免系统直接读实时服务)。
	struct WLD_API InputSnapshot
	{
		// 字符串字典 (兼容已有测试与反射)
		std::unordered_map<std::string, bool> Down;
		std::unordered_map<std::string, bool> Pressed;
		std::unordered_map<std::string, bool> Released;
		std::unordered_map<std::string, float> Axes;

		// 驻留 NameId 快速查找
		std::unordered_map<uint32_t, bool> DownById;
		std::unordered_map<uint32_t, bool> PressedById;
		std::unordered_map<uint32_t, bool> ReleasedById;
		std::unordered_map<uint32_t, float> AxesById;

		bool IsDown(NameId action) const;
		bool IsDown(const std::string& action) const;
		bool WasPressed(NameId action) const;
		bool WasPressed(const std::string& action) const;
		bool WasReleased(NameId action) const;
		bool WasReleased(const std::string& action) const;
		float GetAxis(NameId axis) const;
		float GetAxis(const std::string& axis) const;
	};

	// 多玩家输入服务
	class WLD_API InputService
	{
	public:
		void SetMap(InputMap map);
		const InputMap& GetMap() const { return m_Map; }

		void SetPlayerCount(uint32_t count);
		uint32_t GetPlayerCount() const { return static_cast<uint32_t>(m_Players.size()); }

		// 宿主每帧喂原始状态
		void SetKeyState(uint32_t player, InputDevice device, int code, bool down);
		void SetGamepadAxis(uint32_t player, const std::string& axis, float value);
		void SetGamepadAxis(uint32_t player, uint32_t axisIndex, float value);

		// 帧末调用:把"当前按下"滚成"上一帧按下",供 Pressed/Released 边沿判定。
		void EndFrame();

		// 把本帧所有动作/轴求值成快照
		void BuildSnapshot(uint32_t player = 0);
		const InputSnapshot& GetSnapshot() const { return m_Snapshot; }
		void Clear();

		// 查询动作/轴 (支持 NameId 与 string)
		bool ActionDown(NameId action, uint32_t player = 0) const;
		bool ActionDown(const std::string& action, uint32_t player = 0) const;
		bool ActionPressed(NameId action, uint32_t player = 0) const;
		bool ActionPressed(const std::string& action, uint32_t player = 0) const;
		bool ActionReleased(NameId action, uint32_t player = 0) const;
		bool ActionReleased(const std::string& action, uint32_t player = 0) const;
		float Axis(NameId axis, uint32_t player = 0) const;
		float Axis(const std::string& axis, uint32_t player = 0) const;

		// 逐帧统计(可观测)
		uint64_t GetEndFrameCount() const { return m_EndFrameCount; }

		// 原始状态
		const RawInputState* GetRawState(uint32_t player = 0) const;

		// M1: 手柄热插拔、设备配对与震动 API
		void SetPairingMode(DevicePairingMode mode) { m_PairingMode = mode; }
		DevicePairingMode GetPairingMode() const { return m_PairingMode; }

		void BindPlayerDevice(uint32_t player, DeviceId device);
		DeviceId GetPlayerDevice(uint32_t player) const;
		int32_t FindPlayerForDevice(DeviceId device) const;

		void SetVibration(uint32_t player, float leftMotor, float rightMotor, float durationSec = 0.0f);
		void GetVibration(uint32_t player, float* outLeft, float* outRight) const;
		void UpdateVibration(float dt);

		void PollDevices();

		// M2: 上下文栈管理与高级动作求值
		void PushContext(InputMappingContext context, int32_t priority = 0);
		void PopContext(NameId contextId);
		void PopContext(const std::string& contextName);
		bool HasContext(NameId contextId) const;
		void ClearContexts();
		const std::vector<InputMappingContext>& GetContextStack() const { return m_ContextStack; }
		ActionState EvaluateActionState(NameId action, uint32_t player, float dt = 0.0f) const;

		// R1:每帧解算用的时间步。宿主在帧首喂一次(可变帧 dt);触发器(hold/tap/double_tap/pulse)
		// 的时长判定依赖它。未喂时退化为 1/60,保证 headless 夹具与单测不依赖宿主。
		void SetFrameDelta(float dt) { m_FrameDelta = dt > 0.0f ? dt : (1.0f / 60.0f); }
		float GetFrameDelta() const { return m_FrameDelta; }

		void SetEventCallback(const std::function<void(World::Event&)>& callback) { m_EventCallback = callback; }


	private:
		struct PlayerState
		{
			RawInputState Current;
			RawInputState Previous;
			std::unordered_map<std::string, float> NamedGamepadAxes;
			DeviceId BoundDevice = { InputDevice::Key, 0 };
			VibrationState Vibration;

			// R1:触发器状态机,按动作 NameId 索引(保持 M2 口径 —— 允许"只 Push 上下文、
			// 动作并不在当前映射表里"的用法,因此不能用槽位索引)。
			mutable std::unordered_map<uint32_t, TriggerState> TriggerStates;

			// R1:每帧解算缓存,按动作**槽位**索引(= m_Map.Actions() 下标)。
			//   Resolved       本帧解算结果(Value/Phase/Down/Triggered + 已算好的 Pressed/Released 边沿)
			//   PrevDown       上一帧激活(Down);用于 Released 的下降沿
			//   PrevTriggered  上一帧已触发(Triggered);用于 Pressed 的上升沿
			//   ResolvedEpoch  本缓存对应的帧纪元(= m_EndFrameCount);不等即脏,下次查询惰性重算
			// 用 mutable 是因为 ActionDown/Pressed/Released/Axis 都是 const 查询,而触发器本身**有状态**
			// (hold 要累计时长)⇒ 解算必须落在查询路径上,宿主即使从不显式解算也能得到正确结果。
			mutable std::vector<ActionState> Resolved;
			mutable std::vector<uint8_t> PrevDown;
			mutable std::vector<uint8_t> PrevTriggered;
			mutable uint64_t ResolvedEpoch = ~0ull;

		};

		const PlayerState* GetPlayer(uint32_t player) const;
		PlayerState* GetOrCreatePlayer(uint32_t player);

		// R1:单条 binding 的当前按下状态(键/鼠标/手柄三态)。
		bool RawBindingDown(const PlayerState& player, const InputBinding& binding) const;
		// R1:动作 id -> 稠密槽位;不在当前映射表里返回 m_Map.Actions().size()。
		std::size_t ActionSlotOf(NameId action) const;
		// R1:按"显式上下文栈(高 priority 先) -> 动作自带 bindings(最低优先)"解算单个动作。
		//     dt 由调用方给:缓存路径用 m_FrameDelta,EvaluateActionState 用显式参数(单测可控)。
		//     返回的 ActionState 里 Triggered / Down / Value 已就绪;Pressed / Released 由缓存层按帧历史补。
		ActionState EvaluateActionOnStack(const PlayerState& player, NameId action, float dt) const;
		// R1:输入一变就作废解算缓存。用修订号而不是帧号:边沿必须对"同一帧内先 SetKeyState 再查询"立刻可见(旧实现从 Current/Previous 原始状态算边沿,
		// 天然有这条性质;单测与 AI 注入都依赖它)。同一帧内没有人写输入时仍只解算一次。
		void InvalidateResolve() { ++m_ResolveRevision; }
		void EnsurePlayerCaches(const PlayerState& player) const;
		// R1:取某个动作本帧的解算结果(按需惰性解算);动作不在映射表里 / 玩家不存在返回 nullptr。
		const ActionState* ResolvedAction(NameId action, uint32_t player) const;
		void ResolvePlayerIfStale(const PlayerState& player) const;

		InputMap m_Map;
		std::vector<PlayerState> m_Players;
		InputSnapshot m_Snapshot;
		uint64_t m_EndFrameCount = 0;
		DevicePairingMode m_PairingMode = DevicePairingMode::AutoClaim;
		std::function<void(World::Event&)> m_EventCallback;
		bool m_PrevGamepadConnected[4] = { false };
		std::vector<InputMappingContext> m_ContextStack;
		float m_FrameDelta = 1.0f / 60.0f;
		uint64_t m_ResolveRevision = 1;

	};
}
