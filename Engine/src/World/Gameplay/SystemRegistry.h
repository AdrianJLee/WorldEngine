#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace World::Gameplay
{
	// 更新阶段(P2a W5,顺序即执行顺序):
	//   PreFixed -> Fixed(固定步长,权威模拟) -> Update(可变步长) -> Late -> PreRender
	enum class SystemPhase : uint8_t
	{
		PreFixed = 0,
		Fixed,
		Update,
		Late,
		PreRender,
		Count,
	};

	const char* SystemPhaseName(SystemPhase phase);

	struct SystemDesc
	{
		std::string Name;                 // 唯一名(注册重复会被拒绝)
		SystemPhase Phase = SystemPhase::Update;
		bool ParallelSafe = false;        // 声明"只读写自己独占的数据";调度器据此决定是否并行派发
		std::vector<std::string> After;   // 同阶段内顺序依赖:本系统排在这些系统之后
	};

	struct SystemTiming
	{
		std::string Name;
		SystemPhase Phase = SystemPhase::Update;
		bool ParallelSafe = false;
		double Milliseconds = 0.0;
	};

	// 系统注册表:具名系统 + 阶段 + 同阶段顺序依赖 + 并行标记 + 逐系统耗时。
	// 派发时按阶段顺序执行;同阶段内先做拓扑排序(After 依赖),成环时拒绝执行并在日志中报告。
	class WLD_API SystemRegistry
	{
	public:
		using UpdateFn = std::function<void(Timestep)>;

		// 返回 false 表示:名字为空/重复、阶段非法、依赖成环或依赖不存在。
		bool Register(const SystemDesc& desc, UpdateFn update);
		bool Unregister(const std::string& name);
		void Clear();

		size_t GetSystemCount() const { return m_Systems.size(); }
		bool HasSystem(const std::string& name) const;

		// 执行某一阶段的全部系统;返回实际执行数量(0 表示该阶段无系统或被拒绝)。
		uint32_t RunPhase(SystemPhase phase, Timestep dt);

		const std::vector<SystemTiming>& GetLastTimings() const { return m_Timings; }
		uint64_t GetRunCount() const { return m_RunCount; }
		const std::string& GetLastError() const { return m_LastError; }

	private:
		struct Entry
		{
			SystemDesc Desc;
			UpdateFn Update;
			size_t Order = 0;
		};

		std::vector<Entry> m_Systems;
		std::vector<SystemTiming> m_Timings;
		std::string m_LastError;
		uint64_t m_RunCount = 0;
		size_t m_NextOrder = 0;
	};
}
