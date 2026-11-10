#pragma once

#include "World/Core/Export.h"
#include <cstddef>
#include <string>

struct lua_State;

namespace World
{
	class Scene;
	class ScriptBindingContext;
	class ParticleSystem;
	struct ScriptServiceBinding;

	// Luau 粒子系统全局服务绑定 (只读全局表 Particle)
	WLD_API bool RegisterParticleBindings(ScriptBindingContext& bindings, std::string* error = nullptr);

	// 粒子系统服务面 API 描述表 (只读全局表 Particle), 供运行时注册与存根生成共用
	WLD_API const ScriptServiceBinding* ParticleServiceBindings(std::size_t* count);

	// VFX 服务面 API 描述表 (只读全局表 VFX), 供运行时注册与存根生成共用
	WLD_API const ScriptServiceBinding* VfxServiceBindings(std::size_t* count);

	// 全局粒子模拟系统实例
	WLD_API ParticleSystem& GetGlobalParticleSystem();

	// 便捷包装入口
	WLD_API bool BindParticle(lua_State* L, Scene* scene = nullptr);
}
