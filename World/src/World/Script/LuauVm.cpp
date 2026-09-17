#include "World/Script/LuauVm.h"

#include "World/Core/Log.h"

#include "World/Script/LuauHeaders.h"

#include <Luau/Common.h>
#include <Luau/Compiler.h>

#include <cstring>

namespace World
{
	namespace
	{
		// Luau 的断言走 Luau::assertCallHandler;默认只写 stderr(Debug 下还带 DebugBreak)。
		// 接入引擎日志:脚本/沙箱层的断言必须能被编辑器诊断面板看到。
		int LuauAssertHandler(const char* expression, const char* file, int line, const char* function)
		{
			WLD_CORE_ERROR("[luau] assert failed: {0} at {1}:{2} ({3})",
				expression ? expression : "?", file ? file : "?", line, function ? function : "?");
			return true;   // 与 Luau 默认行为一致:记录后仍进入 DebugBreak/Debug 诊断路径
		}

		// 允许的标准库:纯计算/数据结构,不含文件、进程、加载、反射逃逸面。
		// 注意:Luau 的 os/debug/vector 等**默认不在**这里,不许顺手加回来。
		struct LibraryEntry
		{
			const char* Name;
			int (*Open)(lua_State*);
		};
		const LibraryEntry kAllowedLibraries[] = {
			{ "", luaopen_base },
			{ "math", luaopen_math },
			{ "string", luaopen_string },
			{ "table", luaopen_table },
			{ "bit32", luaopen_bit32 },
			{ "coroutine", luaopen_coroutine },
			{ "utf8", luaopen_utf8 },
		};

		// 对外报名字用(与 kAllowedLibraries 一一对应;base 报 "base" 而不是空串)。
		const char* const kAllowedLibraryNames[] = {
			"base", "math", "string", "table", "bit32", "coroutine", "utf8",
		};

		// 必须为 nil 的全局名(逐个显式置空 + 沙箱冻结,双保险):
		// 文件/加载/环境/调试类 API 一旦可达,Mod 脚本就能越出沙箱。
		const char* const kForbiddenGlobals[] = {
			"io",
			"os",
			"debug",
			"package",
			"require",
			"dofile",
			"loadfile",
			"loadstring",
			"load",
			"getfenv",
			"setfenv",
			"rawget",
			"rawset",
			"rawequal",
			"rawlen",
			"newproxy",
		};
	}

	LuauVm::~LuauVm()
	{
		Shutdown();
	}

	const char* const* LuauVm::AllowedLibraries(size_t* count)
	{
		if (count)
			*count = sizeof(kAllowedLibraries) / sizeof(kAllowedLibraries[0]);
		return kAllowedLibraryNames;
	}

	const char* const* LuauVm::ForbiddenGlobals(size_t* count)
	{
		if (count)
			*count = sizeof(kForbiddenGlobals) / sizeof(kForbiddenGlobals[0]);
		return kForbiddenGlobals;
	}

	bool LuauVm::Init(std::string* error)
	{
		if (m_State)
			return true;
		Luau::assertHandler() = &LuauAssertHandler;
		m_State = luaL_newstate();
		if (!m_State)
		{
			if (error)
				*error = "luaL_newstate failed";
			return false;
		}
		OpenAllowedLibraries();
		ApplySandbox();
		WLD_CORE_INFO("[luau] vm initialized (libs=base/math/string/table/bit32/coroutine/utf8)");
		return true;
	}

	void LuauVm::Shutdown()
	{
		if (!m_State)
			return;
		lua_close(m_State);
		m_State = nullptr;
	}

	void LuauVm::OpenAllowedLibraries()
	{
		// 逐个打开(而不是 luaL_openlibs):少一个库就少一片逃逸面。
		// 这是 Luau 的标准做法(见 VM/src/linit.cpp):库的 open 函数接收库名参数并自己注册全局。
		// 注意 Luau 没有 luaL_requiref(那是 PUC 5.2+ 的 API)。
		for (const LibraryEntry& library : kAllowedLibraries)
		{
			const char* name = library.Name[0] ? library.Name : "base";
			lua_pushcfunction(m_State, library.Open, name);
			lua_pushstring(m_State, name);
			lua_call(m_State, 1, 0);
		}
	}

	void LuauVm::ApplySandbox()
	{
		// 1) 显式移除禁用全局(luaL_openlibs 或 requiref 可能带入的)。
		//    注意每个 lua_setglobal 都会弹出一个值:必须在循环里 push(否则第二个就栈下溢,
		//    Luau 断言会直接报 "1 <= L->top - L->base")。
		for (const char* name : kForbiddenGlobals)
		{
			lua_pushnil(m_State);
			lua_setglobal(m_State, name);
		}

		// 2) 冻结全局表:脚本不得新增/改写全局(否则两个 Mod 可以通过全局名互相影响,
		//    也无法保证"沙箱外的东西不会被脚本替换")。
		luaL_sandbox(m_State);
		// 3) 主线程同样进入沙箱(否则主线程仍可用 getfenv 等绕过)。
		luaL_sandboxthread(m_State);
	}

	bool LuauVm::RunString(const std::string& source, const char* chunkName, std::string* error)
	{
		if (!m_State)
		{
			if (error)
				*error = "vm not initialized";
			return false;
		}
		const char* name = chunkName ? chunkName : "chunk";
		// 注意:luau_load 吃的是**字节码**,不是源码;源码要先经 Luau::compile
		// (直接用源码调用会得到 "bytecode version mismatch" 一类错误)。
		std::string bytecode;
		try
		{
			bytecode = Luau::compile(source, {});
		}
		catch (const std::exception& exception)
		{
			if (error)
				*error = std::string("compile error: ") + exception.what();
			return false;
		}
		const int loadResult = luau_load(m_State, name, bytecode.data(), bytecode.size(), 0);
		if (loadResult != 0)
		{
			if (error)
			{
				const char* message = lua_tostring(m_State, -1);
				*error = message ? message : "compile error";
			}
			lua_pop(m_State, 1);
			return false;
		}
		if (lua_pcall(m_State, 0, 0, 0) != 0)
		{
			if (error)
			{
				const char* message = lua_tostring(m_State, -1);
				*error = message ? message : "runtime error";
			}
			lua_pop(m_State, 1);
			return false;
		}
		return true;
	}
}
