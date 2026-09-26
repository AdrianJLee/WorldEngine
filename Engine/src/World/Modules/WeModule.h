#pragma once

#include <cstdint>

namespace World
{
	class WorldContext;
}

namespace World::Modules
{
	// P0 模块契约(同构建工具链下的内部 C++ 契约;版本化 C ABI 在 P5)。
	// 字段只增不改号;宿主与模块双方都校验 struct_size 与 abi_version。
	// 2 = 2026-09-26 脚本组件重写:`Schema::ScriptBinding` 从 `{ void(*Bind)(void*) }` 变成
	//     `{ Create, Destroy }` + 组件类型改名 ⇒ 旧 Game.dll 必须被**干净拒绝**,不能按旧形状解释
	//     (旧 DLL 的 Bind 会被当 Create 调用)。宿主侧改成等值校验(见 ModuleManager.cpp)。
	constexpr uint32_t WE_MODULE_ABI_VERSION = 2;

	struct WeModule
	{
		uint32_t StructSize = sizeof(WeModule);
		uint32_t AbiVersion = WE_MODULE_ABI_VERSION;
		const char* Id = nullptr;
		const char* Name = nullptr;
		uint32_t Version = 0;
		bool (*Register)(WorldContext& context) = nullptr;
		void (*Unregister)(WorldContext& context) = nullptr;
	};
}
