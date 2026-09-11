#pragma once
#include <sol/sol.hpp>
#include "Scene.h"
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace World
{
	// 描述数学类的一个属性（变量或函数）
	struct LuaPropDesc
	{
		std::string Name;
		std::string LuaType;
		std::string Description; // 注释信息
	};

	struct LuaFunctionDesc
	{
		std::string Name;
		std::vector<LuaPropDesc> Parameters;
		std::string ReturnType;
		std::string Description;
		bool IsMethod = true;
	};

	struct LuaOperatorDesc
	{
		std::string Name;
		std::string OperandType;
		std::string ReturnType;
	};

	// Metadata describes only APIs actually bound into the Lua VM.
	struct LuaTypeReflection
	{
		std::string ClassName;
		std::vector<LuaPropDesc> Properties;  // 属性列表
		std::function<void(sol::state&)> BindFunc; // 核心：闭包包裹的 sol2 编译期绑定逻辑
		std::vector<LuaFunctionDesc> Methods;
		std::vector<LuaFunctionDesc> Constructors;
		std::vector<LuaOperatorDesc> Operators;
	};

	class LuaReflectionRegistry
	{
	public:
		static std::vector<LuaTypeReflection>& GetTable()
		{
			static std::vector<LuaTypeReflection> s_Table;
			return s_Table;
		}
		static bool Register(const LuaTypeReflection& desc)
		{
			auto& table = GetTable();
			if (std::any_of(table.begin(), table.end(), [&](const auto& item) { return item.ClassName == desc.ClassName; }))
				return false;
			table.push_back(desc);
			return true;
		}
	};

	// Lua类型注册器
	struct LuaTypeRegistrar
	{
		LuaTypeRegistrar(const LuaTypeReflection& desc)
		{
			LuaReflectionRegistry::Register(desc);
		}
	};

	// Explicit references also retain the binding objects in static-library hosts.
	void RegisterBuiltinEntityLuaType();
	void RegisterBuiltinMat3LuaType();
	void RegisterBuiltinMat4LuaType();

	struct LuaScriptComponent;
	class ScriptEngine
	{
	public:
		static void Init();      // 在 Application 启动时调用，初始化 sol::state 和注册 API
		static void Shutdown();  // 在 Application 关闭时调用
		static bool IsInitialized();
		static void AssertOwnerThread();

		static void DefineMathType();
		static void RegisterMathTypes();

		static bool GenerateLuaStubs();
		static bool InitScriptForEditor(LuaScriptComponent& component);
		// 核心：处理单个实体的脚本实例化和每帧更新
		static void OnCreateScript(LuaScriptComponent& scriptComponent, Entity entity);
		static void OnUpdateScript(LuaScriptComponent& scriptComponent, Timestep ts);
		static void OnDestroyScript(LuaScriptComponent& scriptComponent);

		// 方便获取全局状态
		static sol::state& GetState();
	};
}
