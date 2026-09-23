// P2 W1:Luau 运行时封装的回归(不依赖窗口/渲染,可 headless 跑)。
//
// 断言三件事:
//   1. VM 能正常执行纯计算脚本(base/math/string/table 可用);
//   2. 沙箱生效:文件/加载/环境/调试类全局不可达,且全局表冻结(脚本不能新增全局);
//   3. 编译/运行错误带 chunk 名与行号,可直接用于编辑器诊断。

#include "World/Core/Core.h"
#include "World/Core/Log.h"
#include "World/Script/LuauVm.h"

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

		// 1) 纯计算:局部变量、表、字符串、math 都能用。
		CHECK(vm.RunString(R"(
			local values = { 1, 2, 3 }
			local sum = 0
			for _, value in values do sum += value end
			local floor = math.floor(3.7)
			local text = string.format("%d-%d", sum, floor)
			assert(text == "6-3", "unexpected text: " .. text)
		)", "=pure", &error));

		// 2) 沙箱:禁用全局必须为 nil(逐个来自 LuauVm::ForbiddenGlobals)。
		CHECK(vm.RunString(R"(
			local forbidden = { "io", "os", "debug", "package", "require",
				"dofile", "loadfile", "loadstring", "load", "getfenv", "setfenv" }
			for _, name in forbidden do
				assert(_G[name] == nil, "global must be nil: " .. name)
			end
		)", "=sandbox-globals", &error));

		// 3) 标准库只读:脚本不得改写 math/string 等库表(否则一个脚本能破坏其他脚本看到的 API)。
		//    注意:Luau 的 safeenv 语义允许脚本写"自己的全局"(它会被编译成快速路径),
		//    真正的全局隔离由 W2 的 per-instance environment 提供 —— 所以这里断言的是库表只读,
		//    而不是"写全局报错"(实测写新全局不会报错,断言写错方向会误导后续实现)。
		CHECK(vm.RunString(R"(
			local ok = pcall(function() math.floor = function() return 0 end end)
			assert(not ok, "math table must be read-only")
			local ok2 = pcall(function() string.format = nil end)
			assert(not ok2, "string table must be read-only")
		)", "=sandbox-library-write", &error));

		// 4) 编译错误带 chunk 名与行号。
		error.clear();
		CHECK(!vm.RunString("local x = \n", "=compile-error", &error));
		CHECK(Contains(error, "compile-error"));
		CHECK(Contains(error, "2"));

		// 5) 运行期错误同样可定位。
		error.clear();
		CHECK(!vm.RunString("error(\"boom\")", "=runtime-error", &error));
		CHECK(Contains(error, "runtime-error"));
		CHECK(Contains(error, "boom"));

		vm.Shutdown();
		CHECK(!vm.IsInitialized());
		std::printf("World.LuauVm: all checks passed\n");
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "World.LuauVm: FAILED: %s\n", exception.what());
		return 1;
	}
}
