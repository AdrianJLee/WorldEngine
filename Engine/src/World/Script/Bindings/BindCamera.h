#pragma once

#include "World/Core/Export.h"
#include <cstddef>
#include <string>

namespace World
{
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	// Luau 摄像机与全屏后处理全局服务绑定 (只读全局表 Camera 与 PostProcess)
	WLD_API bool RegisterCameraBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// 摄像机与后处理服务面 API 描述表 (只读全局表 Camera 与 PostProcess), 供运行时注册与存根生成共用
	WLD_API const ScriptServiceBinding* CameraServiceBindings(std::size_t* count);
}
