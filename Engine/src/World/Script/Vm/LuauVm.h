#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace World
{
	class ScriptValue;
	class ScriptRef;
	class ScriptFunctionRef;
	class ScriptTableRef;
	class ScriptBindingContext;

	namespace LuauDetail
	{
		// userdata tag 池:tag 0 被 lua_newuserdata 占用,1..127(LUA_UTAG_LIMIT-1)留给脚本绑定层。
		constexpr int kFirstScriptUserdataTag = 1;
		constexpr int kLastScriptUserdataTag = 127;

		// VM 生命周期令牌:ScriptRef / ScriptValue / 绑定层共享它判定"VM 是否还活着"。
		// Shutdown() 先清空 State 再 lua_close,之后的引用析构与 IsValid() 都不会再碰 registry。
		// 令牌不延长 VM 的寿命,只让"VM 已关闭"这件事在引用侧可查询。
		class LuauVmState
		{
		public:
			lua_State* State = nullptr;
			int NextUserdataTag = kFirstScriptUserdataTag;
			// registry 里只存裸指针(light userdata),反查共享所有权靠这个 weak_ptr。
			std::weak_ptr<LuauVmState> Self;
		};

		// 引擎内部:从任意 lua_State 取回它所属 VM 的生命周期令牌;不是本引擎的 VM → 空。
		// 实现见 LuauVm.cpp:令牌以 tagged light userdata 存在 registry 的私有键下。
		std::shared_ptr<LuauVmState> VmStateFromLuaState(lua_State* state);
	}

	// P2 脚本前端的第一层:Luau 运行时封装。
	//
	// 职责边界(后续工作包在此基础上叠加,不要在这里塞玩法逻辑):
	//   - 建立 VM、只开**受控标准库**、沙箱化(全局表冻结 + 禁用文件/加载/环境 API);
	//   - 提供"跑一段源码并返回错误(chunk 名 + 行号)"的最小入口(工具/测试用);
	//   - W1b 起:per-instance environment、宿主全局注入、编译成函数、受保护调用(含 traceback);
	//   - W7 起:LoadChunk 是源码/字节码容器的**唯一**装载入口(luau_load 只留一处);
	//   - W2+ 的行为注册、绑定、句柄、事件、热重载都建在它之上。
	//
	// 为什么不用 luaL_openlibs:它会开 os/debug 并保留 getfenv/setfenv 等逃逸面;
	// Mod 脚本是**不可信**输入,标准库必须在封装层收口,而不是靠调用方自律。
	class WLD_API LuauVm
	{
	public:
		LuauVm() = default;
		~LuauVm();
		LuauVm(const LuauVm&) = delete;
		LuauVm& operator=(const LuauVm&) = delete;

		bool Init(std::string* error = nullptr);
		void Shutdown();
		bool IsInitialized() const { return m_State != nullptr; }
		lua_State* State() const { return m_State; }

		// 执行一段 Luau 源码。失败时把错误信息(含 chunk 名与行号)写进 error。
		bool RunString(const std::string& source, const char* chunkName, std::string* error = nullptr);

		// ---- W1b:绑定层基础 ----

		// 宿主/测试用的全局注入。注入的是**线程全局表**(luaL_sandboxthread 建的可写代理),
		// 因此 C++ 注册的库表保持只读,而注入的全局对之后加载的脚本可见;
		// 每实例 environment 通过 __index 回退到这里,所以注入对实例脚本同样可见。
		// 注意:注入发生在脚本加载**之前**才保证可见 —— 见 T1 报告的"导入解析"一节。
		bool SetGlobal(const char* name, const ScriptValue& value);
		bool ClearGlobal(const char* name);
		ScriptValue GetGlobal(const char* name) const;

		// 空表(带 registry 引用,可跨调用持有)。
		ScriptTableRef CreateTable();

		// 每实例 environment:一张**可写**表,元表 __index 回退到线程全局表
		// (即沙箱代理 → 只读真全局);脚本写全局落在自己这张表里,实例之间互不影响。
		ScriptTableRef CreateEnvironment();

		// 把源码编译成函数(不执行)。environment 的有效引用决定闭包的 env;
		// 传空引用时用线程全局(等价于 RunString 的行为)。
		// W7-1 起是 LoadChunk(源码) 的薄壳,环境语义不变。
		ScriptFunctionRef CompileFunction(const std::string& source, const char* chunkName,
			const ScriptTableRef& environment, std::string* error = nullptr);

		// 编译并在指定 environment 里执行一段 chunk。
		bool RunStringInEnvironment(const std::string& source, const char* chunkName,
			const ScriptTableRef& environment, std::string* error = nullptr);

		// ---- W7-1:统一装载入口(源码 / 容器) ----

		// 从字节装载体加载函数(不执行),环境语义与 CompileFunction 完全一致:
		//   - 前 4 字节 == "WSL1" → 容器分支:ScriptArtifact::Unpack 逐项校验;
		//     头命中但任一校验失败 → **硬失败**,绝不回退按源码编译;
		//   - 其它字节 → 源码分支:按 Luau 源码编译(与下面的 string_view 重载同一路径)。
		ScriptFunctionRef LoadChunk(const std::vector<uint8_t>& bytes, const char* chunkName,
			const ScriptTableRef& environment, std::string* error = nullptr);

		// 从源码加载函数(不执行)。CompileFunction / RunString / RunStringInEnvironment
		// 都经由它落到同一处 luau_load;W6 预算仍由执行路径(ProtectedCall)施加。
		ScriptFunctionRef LoadChunk(std::string_view source, const char* chunkName,
			const ScriptTableRef& environment, std::string* error = nullptr);

		// 被禁用的全局名(沙箱收口清单,测试与文档共用一份事实源)。
		static const char* const* ForbiddenGlobals(size_t* count);
		// 允许的标准库(按 Luau 的 library 名)。
		static const char* const* AllowedLibraries(size_t* count);

	private:
		friend class ScriptBindingContext;

		void OpenAllowedLibraries();
		void ApplySandbox();
		// 绑定层用的 VM 状态令牌(见 LuauDetail::LuauVmState)。
		const std::shared_ptr<LuauDetail::LuauVmState>& SharedState() const { return m_Token; }

		lua_State* m_State = nullptr;
		std::shared_ptr<LuauDetail::LuauVmState> m_Token;
	};
}
