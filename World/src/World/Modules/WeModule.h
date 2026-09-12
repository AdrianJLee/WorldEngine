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
	constexpr uint32_t WE_MODULE_ABI_VERSION = 1;

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
