#pragma once
#include <sol/sol.hpp>
#include "Scene.h"

namespace World
{
	// 描述数学类的一个属性（变量或函数）
	struct LuaPropDesc
	{
		std::string Name;
		std::string LuaType;
		std::string Description; // 注释信息
	};

	// 描述一个完整的数学类
	struct LuaTypeReflection
	{
		std::string ClassName;
		std::vector<LuaPropDesc> Properties;  // 属性列表
		std::function<void(sol::state&)> BindFunc; // 核心：闭包包裹的 sol2 编译期绑定逻辑
	};

	class LuaReflectionRegistry
	{
	public:
		static std::vector<LuaTypeReflection>& GetTable()
		{
			static std::vector<LuaTypeReflection> s_Table;
			return s_Table;
		}
	};

	// Lua类型注册器
	struct LuaTypeRegistrar
	{
		LuaTypeRegistrar(const LuaTypeReflection& desc)
		{
			LuaReflectionRegistry::GetTable().push_back(desc);
		}
	};

	class LuaScriptComponent;
	class ScriptEngine
	{
	public:
		static void Init();      // 在 Application 启动时调用，初始化 sol::state 和注册 API
		static void Shutdown();  // 在 Application 关闭时调用

		static void DefineMathType();
		static void RegisterMathTypes();

		static void GenerateLuaStubs();
		static void InitScriptForEditor(LuaScriptComponent& component);
		// 核心：处理单个实体的脚本实例化和每帧更新
		static void OnCreateScript(LuaScriptComponent& scriptComponent, Entity entity);
		static void OnUpdateScript(LuaScriptComponent& scriptComponent, Timestep ts);
		static void OnDestroyScript(LuaScriptComponent& scriptComponent);

		// 方便获取全局状态
		static sol::state& GetState();
	};
}
