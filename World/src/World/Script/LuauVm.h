#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <string>

struct lua_State;

namespace World
{
	// P2 脚本前端的第一层:Luau 运行时封装。
	//
	// 职责边界(后续工作包在此基础上叠加,不要在这里塞玩法逻辑):
	//   - 建立 VM、只开**受控标准库**、沙箱化(全局表冻结 + 禁用文件/加载/环境 API);
	//   - 提供"跑一段源码并返回错误(chunk 名 + 行号)"的最小入口(工具/测试用);
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

		// 被禁用的全局名(沙箱收口清单,测试与文档共用一份事实源)。
		static const char* const* ForbiddenGlobals(size_t* count);
		// 允许的标准库(按 Luau 的 library 名)。
		static const char* const* AllowedLibraries(size_t* count);

	private:
		void OpenAllowedLibraries();
		void ApplySandbox();

		lua_State* m_State = nullptr;
	};
}
