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
	// ecs:Query 与 ecs:AddSystem 的参数签名也走这里 ——
	// Query: components(必填) + options(可选,认 without 排除表);
	// AddSystem: name + fn + phase(可选,字符串或 { phase, after } 表)。
	// T13:ecs:RequireLib(name) 亦在同一张表里 —— name = 相对 <内容根>/scripts/lib/ 的逻辑路径
	// (不带扩展名,.luau 优先/.lua 回退);路径守卫/模块缓存/循环检测/同沙箱执行见 ScriptEngine。
	WLD_API const ScriptServiceBinding* ScriptEcsBindings(std::size_t* count);
}
