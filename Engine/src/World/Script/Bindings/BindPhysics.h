#pragma once

#include "World/Core/Export.h"
#include <cstddef>
#include <string>

namespace World
{
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	// Luau 物理服务全局绑定 (只读全局表 Physics, 提供统一射线/扫掠/重叠几何探测与运动学角色控制 Move-and-Slide)
	WLD_API bool RegisterPhysicsBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// 物理服务面 API 描述表 (只读全局表 Physics), 供运行时注册与 LuaStubGenerator 存根生成共用
	WLD_API const ScriptServiceBinding* PhysicsServiceBindings(std::size_t* count);
}
