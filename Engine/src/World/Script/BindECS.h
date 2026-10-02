#pragma once

#include "World/Core/Export.h"
#include <cstddef>
#include <string>

namespace World
{
	class ScriptBindingContext;

	struct ScriptServiceBinding;

	// Pure ECS M3: Luau 纯 ECS 绑定层 (只读全局表 ecs 与 world、Query DSL、AddSystem、EntityCount)
	WLD_API bool RegisterEcsBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// ecs 面 API 的唯一描述表:与运行时注册共用同一份(避免"能调的方法"和"存根宣告的方法"漂移)。
	WLD_API const ScriptServiceBinding* ScriptEcsBindings(std::size_t* count);
}
