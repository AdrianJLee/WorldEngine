#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"

#include <entt.hpp>

#include <cstdint>
#include <deque>
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
		float Interval = 0.0f;            // 0 = 每步推进; > 0 = 定时间隔节流推进(秒)
		std::function<bool()> Condition;  // 条件门禁谓词:返回 false 则跳过本轮执行
		// 归属(面板显示"系统来自谁")。内置 = Builtin;项目 C++ = Project:<类型名>;
		// Lua = Lua:<脚本路径>;插件 = Plugin:<名>。
		std::string Owner = "Builtin";
		// 启用开关:false = 仍在册(面板可见、可重新启用)但本轮不执行。
		bool Enabled = true;
		// 声明式并行:声明本系统会读/写哪些组件类型(entt::id_type)。
		// AccessDeclared = false 表示未声明 ⇒ 冲突判定退化为"保守串行 + 尊重 ParallelSafe"。
		std::vector<entt::id_type> Reads;
		std::vector<entt::id_type> Writes;
		bool AccessDeclared = false;
	};

	struct SystemTiming
	{
		std::string Name;
		SystemPhase Phase = SystemPhase::Update;
		bool ParallelSafe = false;
		double Milliseconds = 0.0;
		std::string Owner = "Builtin";
		bool Enabled = true;
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

		// 启用/禁用(具名)。false = 留在册内但不执行;名字不存在返回 false。
		bool SetEnabled(const std::string& name, bool enabled);
		bool IsEnabled(const std::string& name) const;
		// 归属标签(面板显示)。名字不存在返回 false。
		bool SetOwner(const std::string& name, std::string owner);

		const std::vector<SystemTiming>& GetLastTimings() const { return m_Timings; }
		uint64_t GetRunCount() const { return m_RunCount; }
		const std::string& GetLastError() const { return m_LastError; }

	private:
		struct Entry
		{
			SystemDesc Desc;
			UpdateFn Update;
			size_t Order = 0;
			float Accumulator = 0.0f;
			// 遥测用名:指向 m_NameStorage 里的稳定存储(见该成员注释)。
			const char* TraceName = nullptr;
		};

		// RunPhase 的复用工作区。
		// 为什么是成员:原先 RunPhase 每帧在栈上构造 5 个容器(vector×3 + unordered_map
		// + vector<double>),每个系统每帧还要构造一次 SystemTiming::Name —— 直接违反
		// docs/dev/performance-and-data-layout.md §4.4「每帧堆分配 0」。
		// 复用后稳态每帧 0 次分配(仅首次/规模变化时增长)。
		std::vector<Entry> m_Systems;
		std::vector<Entry*> m_ScratchPhaseEntries;
		std::vector<Entry*> m_ScratchTimingOrder;
		std::vector<std::vector<Entry*>> m_BatchPool; // 只增不减;每帧按 m_BatchCount 复用
		size_t m_BatchCount = 0;
		std::unordered_map<Entry*, double> m_ScratchDurations;
		std::vector<double> m_ScratchBatchDurations;

		// 系统名的稳定存储:trace 事件只存 const char*,而 m_Systems 扩容会搬移 Entry
		// (其 Desc.Name 的堆缓冲随之失效)。deque 元素地址稳定,且这些字符串从不修改
		// ⇒ 其中的 c_str() 在系统注销前一直有效。
		std::deque<std::string> m_NameStorage;

		// 复用批次槽(返回的向量保留容量;调用方追加后不应长期持有引用)。
		std::vector<Entry*>& NextBatch();

		std::vector<SystemTiming> m_Timings;
		std::string m_LastError;
		uint64_t m_RunCount = 0;
		size_t m_NextOrder = 0;
	};
}
