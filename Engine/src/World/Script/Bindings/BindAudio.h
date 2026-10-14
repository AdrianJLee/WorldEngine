#pragma once

#include "World/Core/Export.h"
#include <cstddef>
#include <string>

namespace World
{
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	// Luau 音频与音乐全局服务绑定 (只读全局表 Audio 与 Music, 支持 3D/2D 空间声像与 Combo 连击音高递进)
	WLD_API bool RegisterAudioBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// 音频面 API 描述表 (Audio 与 Music 两个只读全局表),供运行时注册与 LuaStubGenerator 存根生成共用
	WLD_API const ScriptServiceBinding* AudioServiceBindings(std::size_t* count);
}
