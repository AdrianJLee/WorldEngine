#pragma once
#include <sol/sol.hpp>
#include "Scene.h"

namespace World
{
	class ScriptInstance
	{
	public:
		virtual ~ScriptInstance() = default;
		virtual void OnCreate() {}
		virtual void OnDestroy() {}
		virtual void OnUpdate(Timestep ts) {}
	};

	class LuaScriptComponent;
	class ScriptEngine
	{
	public:
		static void Init();      // 在 Application 启动时调用，初始化 sol::state 和注册 API
		static void Shutdown();  // 在 Application 关闭时调用

		// 当关卡点击 Play 时调用，传入当前运行的场景上下文
		static void OnRuntimeStart(Scene* scene);
		static void OnRuntimeStop();

		static void InitScriptForEditor(LuaScriptComponent& component);
		// 核心：处理单个实体的脚本实例化和每帧更新
		static void OnCreateScript(LuaScriptComponent& scriptComponent);
		static void OnUpdateScript(LuaScriptComponent& scriptComponent, Timestep ts);
		static void OnDestroyScript(LuaScriptComponent& scriptComponent);

		// 方便获取全局状态
		static sol::state& GetState();
	};
}
