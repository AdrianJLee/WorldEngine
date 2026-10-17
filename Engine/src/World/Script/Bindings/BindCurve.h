#pragma once

#include "World/Core/Export.h"
#include <cstddef>
#include <string>

struct lua_State;

namespace World
{
	class Scene;
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	// Luau 曲线系统全局服务绑定 (只读全局表 Curve)
	WLD_API bool RegisterCurveBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// 曲线系统服务面 API 描述表 (只读全局表 Curve), 供运行时注册与存根生成共用
	WLD_API const ScriptServiceBinding* CurveServiceBindings(std::size_t* count);

	// 便捷包装入口
	WLD_API bool BindCurve(lua_State* L, Scene* scene = nullptr);
}
