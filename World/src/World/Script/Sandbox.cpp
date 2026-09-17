#include "World/Script/Sandbox.h"

#include "World/Script/LuauHeaders.h"

#include <chrono>
#include <cstdint>

namespace World
{
	namespace
	{
		// 时间预算的采样间隔:每命中 N 个安全点才读一次墙钟。steady_clock 的一次读取
		// 会进入每条循环热路径;采样把开销压到可忽略,而紧凑循环里的过冲只有几十次
		// 迭代(微秒级),对毫秒级预算没有实际影响。
		constexpr std::uint64_t kTimeSampleInterval = 64;

		// 每个 VM 一份预算状态。指针存放在 lua_callbacks()->userdata
		// (Luau 注释保证该字段不被它自己覆盖),随 InstallHook/DetachHook 生命周期。
		struct BudgetState
		{
			Sandbox::Policy Default{};
			Sandbox::Policy Active{};
			std::uint64_t Hits = 0;                 // 当前 Scope 的指令命中数
			std::chrono::steady_clock::time_point Start{};
			bool Started = false;                   // 有最外层 Scope 在飞
			bool Sticky = false;                    // 超限后每个安全点继续报错
			int Depth = 0;                          // Scope 嵌套深度
			Sandbox::Status Last{};                 // 最近一次超限的 kind/used/limit
			void (*PreviousInterrupt)(lua_State*, int) = nullptr;
			void* PreviousUserdata = nullptr;
		};

		// 只有"本引擎安装过 hook"的 VM 才认 userdata:靠 interrupt 指针本身做校验,
		// 避免误读其他子系统放在该字段上的数据。
		void SandboxInterrupt(lua_State* state, int gc);

		BudgetState* StateFrom(lua_State* state)
		{
			if (!state)
				return nullptr;
			lua_Callbacks* callbacks = lua_callbacks(state);
			if (!callbacks || callbacks->interrupt != &SandboxInterrupt)
				return nullptr;
			return static_cast<BudgetState*>(callbacks->userdata);
		}

		// 超限诊断:单行文本,包含 kind / used / limit 与中断位置(level 0 = 当前正在执行的函数;
		// hook 不在 Lua 调用栈上)。只用 POD 局部量 —— 本函数以 longjmp(lua_error) 结束,
		// C++ 对象不会被析构。
		[[noreturn]] void RaiseBudgetError(lua_State* state, const BudgetState& budget)
		{
			lua_Debug info{};
			const char* source = "?";
			int line = 0;
			if (lua_getinfo(state, 0, "sl", &info) != 0)
			{
				source = info.short_src ? info.short_src : "?";
				line = info.currentline;
			}
			const char* kind = (budget.Last.Kind && budget.Last.Kind[0]) ? budget.Last.Kind : "instructions";
			lua_pushfstring(state, "script budget exceeded (%s): used=%llu limit=%llu at %s:%d",
				kind,
				static_cast<unsigned long long>(budget.Last.Used),
				static_cast<unsigned long long>(budget.Last.Limit),
				source,
				line);
			lua_error(state);
		}

		void SandboxInterrupt(lua_State* state, int gc)
		{
			BudgetState* budget = StateFrom(state);
			if (!budget)
				return;

			// 组装前的 hook(当前引擎没有别的使用者;保留链式调用不做静默覆盖)。
			if (budget->PreviousInterrupt)
				budget->PreviousInterrupt(state, gc);

			// GC 路径(gc >= 0)绝不报错:longjmp 出 GC 会破坏 VM 的一致性
			//(Luau 自身在 GC 里也以同样的方式调用 interrupt)。
			if (gc >= 0)
				return;

			// 没有活动的宿主受保护调用(例如 VM 初始化期 lua_call 开库):不计数、不中断。
			if (budget->Depth <= 0 || !budget->Started)
				return;

			// 粘住:超限后每个安全点继续报错,直到逃出宿主的 lua_pcall(Scope 析构清除)。
			if (budget->Sticky)
				RaiseBudgetError(state, *budget);

			++budget->Hits;

			// 指令口径:每次 interrupt 命中(gc < 0)计 1;命中数 > limit 才超限。
			const std::uint64_t instructionLimit = budget->Active.Instructions;
			if (instructionLimit != 0 && budget->Hits > instructionLimit)
			{
				budget->Sticky = true;
				budget->Last = Sandbox::Status{ true, "instructions", budget->Hits, instructionLimit };
				RaiseBudgetError(state, *budget);
			}

			// 时间口径:采样读钟。
			const std::uint64_t timeLimitMs = budget->Active.TimeMs;
			if (timeLimitMs != 0 && (budget->Hits % kTimeSampleInterval) == 0)
			{
				const auto elapsed = std::chrono::steady_clock::now() - budget->Start;
				const auto elapsedMs = static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
				if (elapsedMs >= timeLimitMs)
				{
					budget->Sticky = true;
					budget->Last = Sandbox::Status{ true, "time", elapsedMs, timeLimitMs };
					RaiseBudgetError(state, *budget);
				}
			}
		}
	}

	void Sandbox::InstallHook(lua_State* state)
	{
		if (!state)
			return;
		lua_Callbacks* callbacks = lua_callbacks(state);
		if (!callbacks || callbacks->interrupt == &SandboxInterrupt)
			return;   // 幂等

		auto* budget = new BudgetState();
		budget->PreviousInterrupt = callbacks->interrupt;
		budget->PreviousUserdata = callbacks->userdata;
		callbacks->userdata = budget;
		callbacks->interrupt = &SandboxInterrupt;
	}

	void Sandbox::DetachHook(lua_State* state)
	{
		if (!state)
			return;
		lua_Callbacks* callbacks = lua_callbacks(state);
		if (!callbacks || callbacks->interrupt != &SandboxInterrupt)
			return;
		auto* budget = static_cast<BudgetState*>(callbacks->userdata);
		callbacks->interrupt = budget ? budget->PreviousInterrupt : nullptr;
		callbacks->userdata = budget ? budget->PreviousUserdata : nullptr;
		delete budget;
	}

	void Sandbox::SetDefaultPolicy(lua_State* state, const Policy& policy)
	{
		if (BudgetState* budget = StateFrom(state))
			budget->Default = policy;
	}

	Sandbox::Policy Sandbox::GetDefaultPolicy(lua_State* state)
	{
		if (BudgetState* budget = StateFrom(state))
			return budget->Default;
		return Policy{};
	}

	Sandbox::Scope::Scope(lua_State* state, const Policy& policy)
		: m_State(state)
	{
		BudgetState* budget = StateFrom(state);
		if (!budget)
		{
			// 该 VM 未安装 hook:作用域退化为空操作(不猜、不报错)。
			m_State = nullptr;
			return;
		}
		if (budget->Depth == 0)
		{
			// 最外层:归零 + 固定策略 + 起表。
			budget->Active = policy;
			budget->Hits = 0;
			budget->Sticky = false;
			budget->Last = Status{};
			budget->Start = std::chrono::steady_clock::now();
			budget->Started = true;
			m_Outermost = true;
		}
		// 嵌套:继承外层计数与策略,不重置。
		++budget->Depth;
	}

	Sandbox::Scope::~Scope()
	{
		BudgetState* budget = StateFrom(m_State);
		if (!budget)
			return;
		if (budget->Depth > 0)
			--budget->Depth;
		if (m_Outermost && budget->Depth == 0)
		{
			// 已逃出宿主的受保护调用:清除粘住状态,下一次调用从干净状态开始。
			budget->Sticky = false;
			budget->Started = false;
		}
	}

	bool Sandbox::Scope::Exceeded() const
	{
		const BudgetState* budget = StateFrom(m_State);
		return budget && budget->Last.Exceeded;
	}

	Sandbox::Status Sandbox::Scope::Result() const
	{
		Status status;
		const BudgetState* budget = StateFrom(m_State);
		if (!budget)
			return status;
		if (budget->Last.Exceeded)
			return budget->Last;
		status.Used = budget->Hits;
		status.Limit = budget->Active.Instructions;
		return status;
	}
}
