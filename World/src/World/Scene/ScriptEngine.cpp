#include "wldpch.h"
#include "ScriptEngine.h"
#include "Components.h"
namespace World
{
	static sol::state* s_LuaState = nullptr;

	void ScriptEngine::Init()
	{
		s_LuaState = new sol::state();

		// 1. 打开 Lua 的基础库和数学库
		s_LuaState->open_libraries(sol::lib::base, sol::lib::math, sol::lib::package);

		// 2. 接管 Lua 的 print 函数，让脚本里的 print 直接输出到你的引擎控制台
		s_LuaState->set_function("print", [](sol::variadic_args args)
			{
				std::string result = "[Lua] ";
				for (auto arg : args)
				{
					result += arg.as<std::string>() + " ";
				}
				WLD_TRACE(result);
			});

		// 3. 注册你的 C++ 组件给 Lua 使用
		//s_LuaState->new_usertype<TransformComponent>("TransformComponent",
		//	"Location", &TransformComponent::Location,
		//	"SetLocation", &TransformComponent::SetLocation
		//);

		WLD_CORE_INFO("[Lua] ScriptEngine initialized successfully.");
	}
	void ScriptEngine::Shutdown()
	{
		delete s_LuaState;
		s_LuaState = nullptr;
	}

	sol::state& ScriptEngine::GetState()
	{
		return *s_LuaState;
	}

	void ScriptEngine::InitScriptForEditor(LuaScriptComponent& sc)
	{
		if (sc.ScriptFilePath.empty()) return;

		sc.CachedFields.clear(); // 清空旧数据

		// 1. 开辟一个临时沙盒，防止污染全局
		sol::environment tempEnv(*s_LuaState, sol::create, s_LuaState->globals());

		// 2. 尝试读取文件
		auto result = s_LuaState->script_file(WLD_ASSETPATH + std::string("/") + sc.ScriptFilePath, tempEnv);
		if (result.valid())
		{
			sol::table scriptTable = result;

			// 3. 遍历提取非函数变量
			for (auto& kv : scriptTable)
			{
				std::string key = kv.first.as<std::string>();
				sol::object value = kv.second;
				sol::type type = value.get_type();

				// 跳过内部函数（如 OnCreate, OnUpdate）和隐藏变量（如以下划线开头的 _xxx）
				if (type == sol::type::function || key.empty() || key[0] == '_') continue;

				LuaScriptField field;
				if (type == sol::type::number)
				{
					// Lua 的 number 默认是 double，我们在引擎里简化处理
					// 如果没有小数，我们当成 Int，否则当成 Float
					double val = value.as<double>();
					if (val == std::floor(val))
					{
						field.Type = LuaFieldType::Int;
						field.Value = (int)val;
					}
					else
					{
						field.Type = LuaFieldType::Float;
						field.Value = (float)val;
					}
				}
				else if (type == sol::type::boolean)
				{
					field.Type = LuaFieldType::Bool;
					field.Value = value.as<bool>();
				}
				else if (type == sol::type::string)
				{
					field.Type = LuaFieldType::String;
					field.Value = value.as<std::string>();
				}

				if (field.Type != LuaFieldType::None)
				{
					sc.CachedFields[key] = field; // 存入 C++ 缓存！
				}
			}
		}
	}

	void ScriptEngine::OnCreateScript(LuaScriptComponent& scriptComponent)
	{
		if (!scriptComponent.IsLoaded && !scriptComponent.ScriptFilePath.empty())
		{
			scriptComponent.LuaEnv = sol::environment(*s_LuaState, sol::create, s_LuaState->globals());


			auto result = s_LuaState->script_file(WLD_ASSETPATH + std::string("/") + scriptComponent.ScriptFilePath, scriptComponent.LuaEnv);

			if (result.valid())
			{
				scriptComponent.ScriptTable = result;

				for (const auto& [name, field] : scriptComponent.CachedFields)
				{
					switch (field.Type)
					{
						case LuaFieldType::Float:  scriptComponent.ScriptTable[name] = std::any_cast<float>(field.Value); break;
						case LuaFieldType::Int:    scriptComponent.ScriptTable[name] = std::any_cast<int>(field.Value); break;
						case LuaFieldType::Bool:   scriptComponent.ScriptTable[name] = std::any_cast<bool>(field.Value); break;
						case LuaFieldType::String: scriptComponent.ScriptTable[name] = std::any_cast<std::string>(field.Value); break;
					}
				}

				scriptComponent.OnCreateFunc = scriptComponent.ScriptTable["OnCreate"];
				scriptComponent.OnUpdateFunc = scriptComponent.ScriptTable["OnUpdate"];
				scriptComponent.OnDestroyFunc = scriptComponent.ScriptTable["OnDestroy"];


				if (scriptComponent.OnCreateFunc.valid())
				{
					scriptComponent.OnCreateFunc(scriptComponent.ScriptTable);
				}
			}
			else
			{
				sol::error err = result;
				WLD_CORE_ERROR("[Lua] Script Error: {0}", err.what());
			}

			// 标记为已加载，防止每帧都去读硬盘
			scriptComponent.IsLoaded = true;
		}
	}

	void ScriptEngine::OnUpdateScript(LuaScriptComponent& scriptComponent, Timestep ts)
	{
		if (scriptComponent.IsLoaded && scriptComponent.OnUpdateFunc.valid())
		{
			auto result = scriptComponent.OnUpdateFunc(scriptComponent.ScriptTable, (float)ts);
			if (!result.valid())
			{
				sol::error err = result;

				WLD_CORE_ERROR("[Lua] Runtime Error in {0}:\n{1}", scriptComponent.ScriptFilePath, err.what());


				OnDestroyScript(scriptComponent);
			}
		}
	}

	void ScriptEngine::OnDestroyScript(LuaScriptComponent& scriptComponent)
	{
		// 如果脚本加载过，并且包含 OnDestroy 方法，则执行它
		if (scriptComponent.IsLoaded && scriptComponent.OnDestroyFunc.valid())
		{
			scriptComponent.OnDestroyFunc(scriptComponent.ScriptTable);
		}

		// 彻底清空环境，防止内存泄漏
		scriptComponent.IsLoaded = false;
		scriptComponent.LuaEnv = sol::lua_nil;
		scriptComponent.OnCreateFunc = sol::lua_nil;
		scriptComponent.OnUpdateFunc = sol::lua_nil;
		scriptComponent.OnDestroyFunc = sol::lua_nil;
	}
}