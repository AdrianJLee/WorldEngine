#pragma once

#include "World/Core/Export.h"
#include <cstddef>
#include <string>

namespace World
{
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	// Luau 大世界流送与空间全局状态服务绑定 (只读全局表 Streaming 与 GlobalState)
	WLD_API bool RegisterStreamingBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// 服务描述表, 供运行时注册与存根生成共用
	WLD_API const ScriptServiceBinding* StreamingServiceBindings(std::size_t* count);
}
