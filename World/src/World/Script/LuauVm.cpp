#include "World/Script/LuauVm.h"

#include "World/Core/Asset/ScriptArtifact.h"
#include "World/Core/Log.h"

#include "World/Script/LuauHeaders.h"
#include "World/Script/Sandbox.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <Luau/Common.h>
#include <Luau/Compiler.h>

#include <cstring>
#include <memory>

namespace World
{
	namespace
	{
		// registry 私有键:用静态变量地址当 key,同一个 VM 生命周期内唯一。
		// 值是指向 LuauDetail::LuauVmState 的 tagged light userdata,供
		// "只有 lua_State*" 的场合(绑定 trampoline、值装箱)反查 VM 状态。
		char kStateTokenKey = 0;
		constexpr int kStateTokenTag = 1;   // light userdata tag,与 tag 0(默认)区分开

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

		// W7-1:environment 校验(原 CompileFunction 的语义原样收敛到这里)。
		// 区分"没有 environment"(空引用 → 线程全局)与"environment 已失效"(必须报错);
		// 否则宿主以为脚本跑在隔离环境里,实际静默落到线程全局。
		bool ValidateEnvironment(lua_State* state, const ScriptTableRef& environment, std::string* error)
		{
			if (environment.Payload() && !environment.IsValid())
			{
				if (error)
					*error = "environment reference is no longer valid";
				return false;
			}
			if (environment.IsValid())
			{
				if (environment.State() != state)
				{
					if (error)
						*error = "environment belongs to a different vm";
					return false;
				}
				if (environment.Type() != ScriptValueType::Table)
				{
					if (error)
						*error = "environment must be a table";
					return false;
				}
			}
			return true;
		}

		// W7-1:全工程**唯一**的 luau_load 出口。bytecode 必须是字节码
		// (ScriptArtifact 的 payload 或 Luau::compile 的产物);源码在调用方先编译。
		ScriptFunctionRef LoadBytecodeFunction(lua_State* state, const ScriptTableRef& environment,
			const uint8_t* bytecode, size_t bytecodeSize, const char* chunkName, std::string* error)
		{
			ScriptFunctionRef function;
			const int base = lua_gettop(state);
			int environmentIndex = 0;
			if (environment.IsValid())
			{
				if (!environment.Push(state))
				{
					if (error)
						*error = "failed to push environment";
					lua_settop(state, base);
					return function;
				}
				environmentIndex = lua_gettop(state);
			}

			const char* name = chunkName ? chunkName : "chunk";
			const int loadResult = luau_load(state, name,
				reinterpret_cast<const char*>(bytecode), bytecodeSize, environmentIndex);
			if (loadResult != 0 || lua_type(state, -1) != LUA_TFUNCTION)
			{
				if (error)
				{
					const char* message = lua_tostring(state, -1);
					*error = message ? message : "compile error";
				}
				lua_settop(state, base);
				return function;
			}

			ScriptValue value = ScriptValue::FromStack(state, -1);
			lua_settop(state, base);
			if (!value.AsFunction(&function) && error)
				*error = "compiled chunk is not a function";
			return function;
		}
	}

	namespace LuauDetail
	{
		std::shared_ptr<LuauVmState> VmStateFromLuaState(lua_State* state)
		{
			if (!state)
				return nullptr;
			if (lua_rawgetptagged(state, LUA_REGISTRYINDEX, &kStateTokenKey, kStateTokenTag) != LUA_TLIGHTUSERDATA)
			{
				lua_pop(state, 1);
				return nullptr;
			}
			auto* token = static_cast<LuauVmState*>(lua_tolightuserdatatagged(state, -1, kStateTokenTag));
			lua_pop(state, 1);
			if (!token)
				return nullptr;
			// 令牌自己持有 weak_ptr:VM 对象销毁后拿不回 shared_ptr,调用方据此判定"不是活着的 VM"。
			return token->Self.lock();
		}
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
		lua_State* state = luaL_newstate();
		if (!state)
		{
			if (error)
				*error = "luaL_newstate failed";
			return false;
		}

		// 先登记状态令牌:沙箱化之后 registry 仍是 C++ 侧可写的(脚本够不到 registry)。
		m_State = state;
		m_Token = std::make_shared<LuauDetail::LuauVmState>();
		m_Token->State = state;
		m_Token->Self = m_Token;
		lua_pushlightuserdatatagged(state, m_Token.get(), kStateTokenTag);
		lua_rawsetptagged(state, LUA_REGISTRYINDEX, &kStateTokenKey, kStateTokenTag);

		// W6:预算/中断 hook 随 VM 一起建立(默认策略 0 = 不限,由 ScriptEngine 下发实际预算)。
		// 必须在任何受保护调用之前安装,否则首次调用不受约束。
		Sandbox::InstallHook(m_State);

		OpenAllowedLibraries();
		ApplySandbox();
		WLD_CORE_INFO("[luau] vm initialized (libs=base/math/string/table/bit32/coroutine/utf8)");
		return true;
	}

	void LuauVm::Shutdown()
	{
		if (!m_State)
			return;
		lua_State* state = m_State;
		m_State = nullptr;
		// W6:hook 状态挂在 lua_callbacks()->userdata 上,必须在 lua_close 之前拆除。
		Sandbox::DetachHook(state);
		// 关键顺序:先把令牌里的 State 清空,再 lua_close。
		// 这样即使还有 ScriptRef/ScriptValue 活着,它们的析构/查询也不会碰已释放的 registry。
		if (m_Token)
			m_Token->State = nullptr;
		lua_close(state);
		WLD_CORE_INFO("[luau] vm shutdown");
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
		// W7-1:薄壳 —— 编译/装载统一在 LoadChunk;执行仍走 ScriptFunctionRef::Call
		// (内部是唯一的受保护调用 ProtectedCall,错误带 chunk 名 + 行号 + stack traceback)。
		ScriptFunctionRef function = LoadChunk(std::string_view(source), chunkName, ScriptTableRef(), error);
		if (!function.IsValid())
			return false;
		return function.Call(nullptr, 0, nullptr, error);
	}

	bool LuauVm::SetGlobal(const char* name, const ScriptValue& value)
	{
		if (!m_State || !name || !name[0])
			return false;
		std::string pushError;
		if (!LuauDetail::PushValueToStack(m_State, value, &pushError))
		{
			WLD_CORE_WARN("[luau] SetGlobal('{0}') failed: {1}", name, pushError);
			return false;
		}
		lua_setfield(m_State, LUA_GLOBALSINDEX, name);
		return true;
	}

	bool LuauVm::ClearGlobal(const char* name)
	{
		if (!m_State || !name || !name[0])
			return false;
		lua_pushnil(m_State);
		lua_setfield(m_State, LUA_GLOBALSINDEX, name);
		return true;
	}

	ScriptValue LuauVm::GetGlobal(const char* name) const
	{
		if (!m_State || !name)
			return ScriptValue();
		lua_State* state = m_State;
		lua_getfield(state, LUA_GLOBALSINDEX, name);
		ScriptValue value = ScriptValue::FromStack(state, -1);
		lua_pop(state, 1);
		return value;
	}

	ScriptTableRef LuauVm::CreateTable()
	{
		ScriptTableRef table;
		if (!m_State)
			return table;
		lua_State* state = m_State;
		lua_createtable(state, 0, 0);
		ScriptValue value = ScriptValue::FromStack(state, -1);
		lua_pop(state, 1);
		value.AsTable(&table);
		return table;
	}

	ScriptTableRef LuauVm::CreateEnvironment()
	{
		ScriptTableRef environment;
		if (!m_State)
			return environment;

		lua_State* state = m_State;
		const int base = lua_gettop(state);

		lua_newtable(state);                // 实例 environment:可写
		lua_newtable(state);                // metatable
		// __index 必须回退到**线程全局表**:luaL_sandboxthread 之后它是
		// "可写代理 → 只读真全局",C++ 注册的库/类型与宿主注入的全局都在那一条链上。
		lua_pushvalue(state, LUA_GLOBALSINDEX);
		lua_setfield(state, -2, "__index");
		lua_setreadonly(state, -1, true);   // 元表只读:脚本不能改掉回退目标
		lua_setmetatable(state, -2);

		ScriptValue value = ScriptValue::FromStack(state, -1);
		lua_settop(state, base);
		value.AsTable(&environment);
		return environment;
	}

	ScriptFunctionRef LuauVm::CompileFunction(const std::string& source, const char* chunkName,
		const ScriptTableRef& environment, std::string* error)
	{
		// W7-1:薄壳 —— 装载统一走 LoadChunk 的源码重载(环境校验/错误文本不变)。
		return LoadChunk(std::string_view(source), chunkName, environment, error);
	}

	ScriptFunctionRef LuauVm::LoadChunk(std::string_view source, const char* chunkName,
		const ScriptTableRef& environment, std::string* error)
	{
		ScriptFunctionRef function;
		if (!m_State)
		{
			if (error)
				*error = "vm not initialized";
			return function;
		}
		if (!ValidateEnvironment(m_State, environment, error))
			return function;

		// luau_load 吃字节码;源码先经 Luau::compile。编译失败会以版本 0 的错误装载体返回,
		// 由 luau_load 解码成带 chunk 名与行号的错误文本(与 RunString 的同一约定)。
		std::string bytecode;
		try
		{
			bytecode = Luau::compile(std::string(source), {});
		}
		catch (const std::exception& exception)
		{
			if (error)
				*error = std::string("compile error: ") + exception.what();
			return function;
		}
		return LoadBytecodeFunction(m_State, environment,
			reinterpret_cast<const uint8_t*>(bytecode.data()), bytecode.size(), chunkName, error);
	}

	ScriptFunctionRef LuauVm::LoadChunk(const std::vector<uint8_t>& bytes, const char* chunkName,
		const ScriptTableRef& environment, std::string* error)
	{
		ScriptFunctionRef function;
		if (!m_State)
		{
			if (error)
				*error = "vm not initialized";
			return function;
		}
		if (!ValidateEnvironment(m_State, environment, error))
			return function;

		const char* name = chunkName ? chunkName : "chunk";
		if (Asset::ScriptArtifact::IsArtifactBytes(bytes.data(), bytes.size()))
		{
			// 容器分支:逐项校验;头命中后的任何校验失败都是硬失败(绝不回退按源码编译)。
			std::vector<uint8_t> payload;
			if (!Asset::ScriptArtifact::Unpack(name, bytes.data(), bytes.size(), payload, error))
				return function;
			return LoadBytecodeFunction(m_State, environment, payload.data(), payload.size(), name, error);
		}

		// 非容器 → 源码分支(开发树里的 .lua/.luau 原样走这条路径)。
		const char* data = bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data());
		return LoadChunk(std::string_view(data, bytes.size()), name, environment, error);
	}

	bool LuauVm::RunStringInEnvironment(const std::string& source, const char* chunkName,
		const ScriptTableRef& environment, std::string* error)
	{
		ScriptFunctionRef function = CompileFunction(source, chunkName, environment, error);
		if (!function.IsValid())
			return false;
		return function.Call(nullptr, 0, nullptr, error);
	}
}
