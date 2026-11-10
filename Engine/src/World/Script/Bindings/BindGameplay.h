#pragma once

#include "World/Core/Export.h"
#include <cstddef>
#include <string>

namespace World
{
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	// Luau 通用 Gameplay 全局服务绑定 (只读全局表 Health 与 SmartObject)
	WLD_API bool RegisterGameplayBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// 服务面 API 描述表 (只读全局表 Health 与 SmartObject), 供运行时注册与存根生成共用
	WLD_API const ScriptServiceBinding* GameplayCoreServiceBindings(std::size_t* count);
}
