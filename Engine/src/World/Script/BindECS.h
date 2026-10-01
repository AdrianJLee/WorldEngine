#pragma once

#include "World/Core/Export.h"
#include <string>

namespace World
{
	class ScriptBindingContext;

	// Pure ECS M3: Luau 纯 ECS 绑定层 (只读全局表 ecs 与 world、Query DSL、AddSystem、EntityCount)
	WLD_API bool RegisterEcsBindings(ScriptBindingContext& bindings, std::string* error = nullptr);
}
