#pragma once
#include "Scene.h"
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	class LuauVm;
	class ScriptBindingContext;
	class BehaviorRegistry;

	namespace Schema { class SchemaRegistry; }

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
		// 核心：把该类型注册进当前 VM 的绑定上下文（W1b 起不再依赖 sol2）。
		std::function<void(ScriptBindingContext&)> BindFunc;
		std::vector<LuaFunctionDesc> Methods;
		std::vector<LuaFunctionDesc> Constructors;
		std::vector<LuaOperatorDesc> Operators;
	};

	class LuaReflectionRegistry
	{
	public:
		static std::vector<LuaTypeReflection>& GetTable();
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
	// vec2/vec3/vec4 的绑定实现位于 LuaType/Vec2.cpp、Vec3.cpp、Vec4.cpp。
	void RegisterBuiltinVec2Binding(ScriptBindingContext& bindings);
	void RegisterBuiltinVec3Binding(ScriptBindingContext& bindings);
	void RegisterBuiltinVec4Binding(ScriptBindingContext& bindings);

	struct LuaScriptComponent;
	class ScriptEngine
	{
	public:
		static void Init();      // 在 Application 启动时调用：建立 Luau VM、沙箱、绑定层与 API 注册
		static void Shutdown();  // 在 Application 关闭时调用
		static bool IsInitialized();
		static void AssertOwnerThread();

		static void DefineMathType();
		static void RegisterMathTypes();

		static bool GenerateLuaStubs();
		static bool InitScriptForEditor(LuaScriptComponent& component);
		// 从 Lua 脚本源码文本静态解析 `---@field Name Type` 注解，返回字段名到 Lua 类型名的映射（不执行脚本）。
		static std::unordered_map<std::string, std::string> ParseFieldAnnotations(const std::string& scriptText);
		// 核心：处理单个实体的脚本实例化和每帧更新
		static void OnCreateScript(LuaScriptComponent& scriptComponent, Entity entity);
		static void OnUpdateScript(LuaScriptComponent& scriptComponent, Timestep ts);
		static void OnDestroyScript(LuaScriptComponent& scriptComponent);

		// 方便获取全局状态（W1b 起返回 Luau VM 门面；未初始化时抛 logic_error）
		static LuauVm& GetState();
		// 当前 VM 的绑定上下文（类型注册/宿主函数装箱；未初始化时抛 logic_error）
		static ScriptBindingContext& GetBindingContext();

		// ---- P2 W2a:行为注册层（只登记/查询，不参与调度）----
		// 进程内行为注册表：与 VM 生命周期无关（Init/Shutdown 不清空），宿主与测试共用。
		static BehaviorRegistry& Behaviors();
		// 幂等登记/刷新一个 Luau 行为（字段来自 CachedFields；路径为空 → false + error）。
		// 同模块 id 描述一致 → 直接返回 true；描述变化（脚本编辑/热重载）→ Replace 刷新。
		static bool EnsureLuaBehavior(LuaScriptComponent& script, std::string* error = nullptr);
		// 幂等登记/刷新 schema 注册表里全部 Category==Script 的 C++ 行为；
		// 返回已登记/已确认的模块数，失败项写入 errors（可为 null）。
		static std::size_t EnsureSchemaBehaviors(const Schema::SchemaRegistry& schemas,
			std::vector<std::string>* errors = nullptr);
	};
}
