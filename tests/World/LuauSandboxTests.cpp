// P2 W6:脚本沙箱预算(指令 / 时间 + 中断)的 headless 回归。
//
// 覆盖派工单 w6_sandbox_budget 的 7 条用例:
//   ① 死循环被中断(诊断含 "script budget exceeded" + kind/used/limit + chunk 名/行号);
//   ② 中断后同一 VM 仍可用;
//   ③ 同一 VM 的其它脚本继续(失败后 good 脚本成功且副作用可观测);
//   ④ 粘住语义:pcall 吞不掉中断(必须返回 false,不能挂住);
//   ⑤ 预算内正常 + 不跨调用累计(同一策略连续 3 次小循环全成功);
//   ⑥ 新建 VM 不设策略 → 长循环可跑完(LuauVm 默认 0 = 不限);
//   ⑦ 时间口径:TimeMs = 30 的 spin 返回,诊断为 time 类(宽松上界,不追求精度)。

#include "World/Core/Core.h"
#include "World/Core/Log.h"
#include "World/Script/LuauVm.h"
#include "World/Script/Sandbox.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptValue.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
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

	bool Contains(const std::string& haystack, const std::string& needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point start)
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count());
	}

	// 从诊断文本里取 "used=<number>"(报告要贴实测命中数;测试也要断言计数精确)。
	std::uint64_t ParseUsed(const std::string& text)
	{
		const std::string key = "used=";
		const std::size_t at = text.find(key);
		if (at == std::string::npos)
			return 0;
		std::uint64_t value = 0;
		for (std::size_t index = at + key.size(); index < text.size(); ++index)
		{
			const char digit = text[index];
			if (digit < '0' || digit > '9')
				break;
			value = value * 10 + static_cast<std::uint64_t>(digit - '0');
		}
		return value;
	}

	// ③ 的副作用观测点(宿主注入函数写、C++ 侧读)。
	int s_Recorded = -1;

	// ⑤ 的每次调用计数(外层 Scope 测一次宿主调用的命中数)。
	constexpr std::uint64_t kFrameBudget = 50000;
	constexpr int kFrameIterations = 20000;

	// ①②③④ 用的指令预算:远超夹具自身开销、又足够小到毫秒级完成。
	constexpr std::uint64_t kSpinBudget = 100000;
}

int main()
{
	World::Log::Init();
	try
	{
		World::LuauVm vm;
		std::string error;
		CHECK(vm.Init(&error));
		CHECK(vm.IsInitialized());

		// ⑥ LuauVm 自身默认 0 = 不限:2'000'000 次安全点的循环必须跑完(远超 ScriptEngine 的 1e6)。
		{
			const World::Sandbox::Policy defaults = World::Sandbox::GetDefaultPolicy(vm.State());
			CHECK(defaults.Instructions == 0 && defaults.TimeMs == 0);
			const auto start = std::chrono::steady_clock::now();
			CHECK(vm.RunString("for i = 1, 2000000 do local x = i end", "=sandbox-default-unlimited", &error));
			std::printf("[W6] (6) default policy unlimited: 2000000-step loop completed in %llu ms\n",
				static_cast<unsigned long long>(ElapsedMs(start)));
		}

		// 宿主受保护调用使用的默认策略(脚本引擎会下发自己的值;这里直接设 VM 级预算)。
		World::Sandbox::SetDefaultPolicy(vm.State(), World::Sandbox::Policy{ kSpinBudget, 0 });
		CHECK(World::Sandbox::GetDefaultPolicy(vm.State()).Instructions == kSpinBudget);

		// ① 死循环被中断:false + 诊断含 script budget exceeded / kind / used / limit / chunk 名 + 行号。
		{
			error.clear();
			const auto start = std::chrono::steady_clock::now();
			CHECK(!vm.RunString("while true do end", "scripts/tests/BudgetSpinLoop.lua", &error));
			const auto elapsed = ElapsedMs(start);
			CHECK(elapsed < 5000);   // 挂了就不是"被中断"
			CHECK(Contains(error, "script budget exceeded"));
			CHECK(Contains(error, "instructions"));
			CHECK(Contains(error, "used="));
			CHECK(Contains(error, "limit="));
			CHECK(Contains(error, "BudgetSpinLoop.lua"));
			CHECK(Contains(error, "stack traceback"));
			// 指令口径精确性:命中数 > limit 的第一次命中就中断 → used 恰好是 limit + 1。
			CHECK(ParseUsed(error) == kSpinBudget + 1);
			std::printf("[W6] (1) spin interrupted after %llu ms, used=%llu limit=%llu\n",
				static_cast<unsigned long long>(elapsed),
				static_cast<unsigned long long>(ParseUsed(error)),
				static_cast<unsigned long long>(kSpinBudget));
			std::printf("[W6] (1) diagnostic first line: %s\n", error.substr(0, error.find('\n')).c_str());
		}

		// ② 中断后同一 VM 仍可用(粘住状态已随作用域析构清除)。
		{
			error.clear();
			CHECK(vm.RunString("assert(1 + 1 == 2 and type(pcall) == 'function')", "=sandbox-after-interrupt", &error));
			std::printf("[W6] (2) same VM still usable after an interrupted call\n");
		}

		// ③ 同一 VM 的其它脚本继续:spin 失败 + good 脚本成功且副作用可观测。
		{
			World::ScriptBindingContext bindings(vm);
			CHECK(bindings.IsValid());
			s_Recorded = -1;
			CHECK(vm.SetGlobal("W6Record", bindings.CreateFunction("W6Record",
				[](const World::ScriptValue* args, std::size_t count) -> World::ScriptValue {
					if (count < 1)
						throw std::runtime_error("W6Record expects one number");
					double value = 0.0;
					if (!args[0].AsNumber(&value))
						throw std::runtime_error("W6Record expects one number");
					s_Recorded = static_cast<int>(value);
					return World::ScriptValue::Nil();
				})));

			error.clear();
			CHECK(!vm.RunString("while true do end", "=sandbox-other-bad", &error));
			CHECK(Contains(error, "script budget exceeded"));
			error.clear();
			CHECK(vm.RunString("W6Record(42)", "=sandbox-other-good", &error));
			CHECK(s_Recorded == 42);
			CHECK(vm.ClearGlobal("W6Record"));
			std::printf("[W6] (3) other script on the same VM succeeded, side effect=%d\n", s_Recorded);
		}

		// ④ 粘住语义(本包重点):pcall 吞掉中断后,每个安全点继续报错,最终逃到宿主 lua_pcall。
		{
			error.clear();
			const auto start = std::chrono::steady_clock::now();
			CHECK(!vm.RunString("while true do pcall(function() while true do end end) end",
				"=sandbox-sticky", &error));
			const auto elapsed = ElapsedMs(start);
			CHECK(elapsed < 5000);   // 不能挂住
			CHECK(Contains(error, "script budget exceeded"));
			CHECK(Contains(error, "instructions"));
			// 粘住期间重复报错用的是"第一次超限"的状态(诊断不被后续重新计数污染)。
			CHECK(ParseUsed(error) == kSpinBudget + 1);
			std::printf("[W6] (4) sticky: pcall-wrapped spin still failed after %llu ms, used=%llu\n",
				static_cast<unsigned long long>(elapsed),
				static_cast<unsigned long long>(ParseUsed(error)));
			std::printf("[W6] (4) diagnostic first line: %s\n", error.substr(0, error.find('\n')).c_str());
			error.clear();
			CHECK(vm.RunString("assert(type(pcall) == 'function')", "=sandbox-after-sticky", &error));
		}

		// ⑤ 预算内正常 + 不跨调用累计:一次调用一个 Scope,计数从 0 开始。
		//    单次命中 ≈ 迭代数;三次累计 > kFrameBudget → 若跨调用累计,第三次必然超限。
		{
			std::uint64_t totalHits = 0;
			for (int call = 0; call < 3; ++call)
			{
				World::Sandbox::Scope frame(vm.State(), World::Sandbox::Policy{ kFrameBudget, 0 });
				error.clear();
				CHECK(vm.RunString("for i = 1, 20000 do local x = i + 1 end", "=sandbox-frame", &error));
				CHECK(!frame.Exceeded());
				totalHits += frame.Result().Used;
			}
			CHECK(totalHits > kFrameBudget);
			std::printf("[W6] (5) 3 x %d-iteration calls under a %llu-hit per-call budget: total hits=%llu (no cross-call accumulation)\n",
				kFrameIterations,
				static_cast<unsigned long long>(kFrameBudget),
				static_cast<unsigned long long>(totalHits));
		}

		// ⑦ 时间口径:TimeMs = 30 → spin 在毫秒级返回,诊断为 time 类;上界放宽避免 flaky。
		{
			World::Sandbox::SetDefaultPolicy(vm.State(), World::Sandbox::Policy{ 0, 30 });
			error.clear();
			const auto start = std::chrono::steady_clock::now();
			CHECK(!vm.RunString("while true do end", "=sandbox-time", &error));
			const auto elapsed = ElapsedMs(start);
			CHECK(elapsed >= 30);
			CHECK(elapsed < 2000);
			CHECK(Contains(error, "script budget exceeded"));
			CHECK(Contains(error, "time"));
			CHECK(ParseUsed(error) >= 30);
			std::printf("[W6] (7) time budget: spin returned after %llu ms, used=%llu limit=30\n",
				static_cast<unsigned long long>(elapsed),
				static_cast<unsigned long long>(ParseUsed(error)));
			World::Sandbox::SetDefaultPolicy(vm.State(), World::Sandbox::Policy{});
		}

		vm.Shutdown();
		CHECK(!vm.IsInitialized());
		std::printf("World.LuauSandbox: all checks passed\n");
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "World.LuauSandbox: FAILED: %s\n", exception.what());
		return 1;
	}
}
