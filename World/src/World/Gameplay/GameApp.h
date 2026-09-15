#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Gameplay/GameFlow.h"
#include "World/Gameplay/LevelService.h"
#include "World/Gameplay/SystemRegistry.h"
#include "World/Gameplay/InputMap.h"
#include "World/Gameplay/SaveService.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace World::Gameplay
{
	// 每帧阶段的耗时读数(可观测,进统计面板/日志)。
	struct FramePhaseTiming
	{
		std::string Name;
		double Milliseconds = 0.0;
	};

	struct GameAppDesc
	{
		std::string ProjectId;
		std::filesystem::path ContentRoot;
		std::string StartLevel;                 // W1 未接关卡清单前等价于"启动场景路径"
		uint32_t FixedStepHz = 60;              // 固定步长频率
		uint32_t MaxFixedStepsPerFrame = 4;     // 防死亡螺旋的上限
		bool RunFixedWhenPaused = false;
	};

	// 游戏会话(P2a §3.1):Editor 的 Play/Simulate 与 Runtime 走同一条 Tick 路径。
	//
	// Tick 内的固定顺序(对外契约,不得随意调整):
	//   Flow 安全点 -> FixedUpdate(0..MaxSteps) -> Update -> LateUpdate -> (宿主渲染) -> 阶段计时归集
	// 阶段回调由宿主/子系统注册;W5 起由 SystemRegistry 托管,但顺序不变。
	class WLD_API GameApp
	{
	public:
		using PhaseCallback = std::function<void(Timestep)>;

		static void Create(const GameAppDesc& desc);
		static void Shutdown();
		static bool Exists();
		static GameApp& Get();
		static GameApp* TryGet();

		void Tick(Timestep frameTime);

		GameFlow& Flow() { return m_Flow; }
		const GameFlow& Flow() const { return m_Flow; }
		// W2:关卡服务(清单/加载状态机/场景栈)由会话持有,宿主只负责注入 SceneLoader。
		LevelService& Levels() { return m_Levels; }
		const LevelService& Levels() const { return m_Levels; }
		// W5:系统注册表(阶段/顺序/并行标记/耗时)。Tick 会按阶段顺序派发它。
		// W7:输入服务(动作/轴/帧快照)。宿主负责喂原始状态,玩法只读动作与轴。
		InputService& Input() { return m_Input; }
		const InputService& Input() const { return m_Input; }
		SystemRegistry& Systems() { return m_Systems; }
		const SystemRegistry& Systems() const { return m_Systems; }
		// W8:存档服务。场景来源由宿主注入(GameHost 持有当前场景):
		// 未注入前 Saves() 返回 nullptr,宿主可在拿到场景后调用 CreateSaveService。
		void CreateSaveService(SaveService::SceneProvider sceneProvider);
		SaveService* Saves() { return m_Saves.get(); }
		const SaveService* Saves() const { return m_Saves.get(); }

		// 阶段回调:未设置时该阶段为空转。fixed 可能在一帧内被调用 0..MaxFixedStepsPerFrame 次。
		void SetPhaseCallbacks(PhaseCallback fixedUpdate, PhaseCallback update, PhaseCallback lateUpdate);
		// 固定步长阶段每次调用的步长(秒),供物理/确定性逻辑使用。
		double FixedStepSeconds() const { return m_FixedStepSeconds; }
		uint32_t LastFixedSteps() const { return m_LastFixedSteps; }
		uint64_t FrameNumber() const { return m_FrameNumber; }

		void SetPaused(bool paused) { m_Paused = paused; }
		bool IsPaused() const { return m_Paused; }

		const std::vector<FramePhaseTiming>& LastFramePhases() const { return m_FramePhases; }
		const GameAppDesc& Desc() const { return m_Desc; }

	private:
		explicit GameApp(const GameAppDesc& desc);

		GameAppDesc m_Desc;
		GameFlow m_Flow;
		LevelService m_Levels;
		SystemRegistry m_Systems;
		std::unique_ptr<SaveService> m_Saves;
		InputService m_Input;
		double m_FixedStepSeconds = 1.0 / 60.0;
		double m_Accumulator = 0.0;
		uint32_t m_LastFixedSteps = 0;
		uint64_t m_FrameNumber = 0;
		bool m_Paused = false;
		PhaseCallback m_FixedUpdate;
		PhaseCallback m_Update;
		PhaseCallback m_LateUpdate;
		std::vector<FramePhaseTiming> m_FramePhases;
	};
}
