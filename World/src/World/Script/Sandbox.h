#pragma once

#include "World/Core/Export.h"

#include <cstdint>

struct lua_State;

namespace World
{
	// P2 W6:脚本沙箱的预算与中断(指令数 / 墙钟)。
	//
	// 契约(与 tools/codex/tasks/20260914-2300-p2-scripting-hotreload/plan.md §11 的
	// "W6 勘察结论" 一致):
	//   - 每个 VM 一份状态,挂在 lua_callbacks()->userdata 上;hook 是 lua_callbacks()->interrupt;
	//   - 只在安全点计数(gc < 0 的 interrupt 命中);GC 路径(gc >= 0)直接返回,绝不 longjmp;
	//   - 指令口径:每次命中计 1;命中数 > Instructions 才超限(0 = 不限);
	//   - 时间口径:在 hook 内按固定间隔采样 steady_clock,超过 TimeMs 即超限(0 = 关);
	//   - 粘住(sticky):一旦超限,此后每个安全点继续报错,直到逃出宿主的受保护调用
	//     (最外层 Scope 析构)才清除 —— 这是防"脚本用 pcall 吞掉中断继续死循环"的唯一手段;
	//   - 嵌套 Scope 继承外层计数与策略、不重置(受保护调用里的受保护调用仍算同一次调用);
	//   - 全部宿主→Lua 入口(RunString / CompileFunction / ScriptFunctionRef::Call / 将来的
	//     LoadChunk)都走 ScriptValue.cpp 里唯一的 ProtectedCall,因此在那里开 Scope 即可覆盖。
	//
	// LuauVm 层默认策略 = 0(不限),保持现有 World.LuauVm / World.LuauBinding 语义;
	// 实际预算由 ScriptEngine 通过 SetDefaultPolicy 下发。
	namespace Sandbox
	{
		// 预算。0 = 该维度不限/关闭。
		struct Policy
		{
			std::uint64_t Instructions = 0;   // 安全点命中数上限(0 = 不限)
			std::uint64_t TimeMs = 0;         // 墙钟毫秒上限(0 = 关)
		};

		// 最近一次超限的状态。
		struct Status
		{
			bool Exceeded = false;
			const char* Kind = "";            // "instructions" / "time";未超限时为空串
			std::uint64_t Used = 0;           // 指令 = 命中数;时间 = 已耗时毫秒
			std::uint64_t Limit = 0;
		};

		// 安装/拆除 hook。必须在 VM 建立后、lua_close 之前成对调用
		//(LuauVm::Init / LuauVm::Shutdown 已接线)。重复安装/拆除是幂等的。
		WLD_API void InstallHook(lua_State* state);
		WLD_API void DetachHook(lua_State* state);

		// 每次宿主受保护调用使用的默认策略(未安装 hook 的 state 上调用是无操作/返回 0)。
		WLD_API void SetDefaultPolicy(lua_State* state, const Policy& policy);
		WLD_API Policy GetDefaultPolicy(lua_State* state);

		// 受保护调用作用域(RAII):
		//   - 最外层 Scope 归零计数、固定策略、起表(时间预算的起点);
		//   - 嵌套 Scope 只抬深度:继承外层计数与策略,不重置;
		//   - 最外层析构清除粘住状态(下一次调用从干净状态开始)。
		class WLD_API Scope
		{
		public:
			Scope(lua_State* state, const Policy& policy);
			~Scope();
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;

			// 本作用域(含其嵌套调用)是否触发了预算。
			bool Exceeded() const;
			// 超限时返回最近一次超限的 kind/used/limit;未超限时 Kind 为空、
			// Used = 当前指令命中数、Limit = 指令上限(0 = 不限)。
			Status Result() const;

		private:
			lua_State* m_State = nullptr;
			bool m_Outermost = false;
		};
	}
}
