#pragma once
#include <sol/sol.hpp>
#include "Scene.h"

namespace World
{
	// 描述数学类的一个属性（变量或函数）
	struct MathPropDesc
	{
		std::string Name;      // 在 Lua 里的名字，如 "x" 或 "Normalize"
		std::string LuaType;   // 在 EmmyLua 里的类型，如 "number" 或 "fun():Vec3"
		std::string Description; // 可选：属性的描述信息，未来可以用来生成更丰富的文档注释
	};

	// 描述一个完整的数学类
	struct MathTypeReflection
	{
		std::string ClassName;                // 类名，如 "Vec3"
		std::vector<MathPropDesc> Properties;  // 属性列表
		std::function<void(sol::state&)> BindFunc; // 核心：闭包包裹的 sol2 编译期绑定逻辑
	};

	class MathReflectionRegistry
	{
	public:
		static std::vector<MathTypeReflection>& GetTable()
		{
			static std::vector<MathTypeReflection> s_Table;
			return s_Table;
		}
	};

	// 辅助结构体：利用构造函数执行注册动作
	struct MathTypeRegistrar
	{
		MathTypeRegistrar(const MathTypeReflection& desc)
		{
			MathReflectionRegistry::GetTable().push_back(desc);
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
		static void OnCreateScript(LuaScriptComponent& scriptComponent);
		static void OnUpdateScript(LuaScriptComponent& scriptComponent, Timestep ts);
		static void OnDestroyScript(LuaScriptComponent& scriptComponent);

		// 方便获取全局状态
		static sol::state& GetState();
	};
}
