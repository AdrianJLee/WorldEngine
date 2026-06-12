#include "wldpch.h"
#include "ScriptEngine.h"
#include "Components.h"

namespace World
{
	// 全局 Lua 状态指针，整个引擎只会有这一个实例
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
		RegisterMathTypes();
		WLD_CORE_INFO("[Lua] ScriptEngine initialized successfully.");
	}
	void ScriptEngine::Shutdown()
	{
		delete s_LuaState;
		s_LuaState = nullptr;
	}

	std::string GetLuaTypeName(const PropertyDesc& prop)
	{
		switch (prop.Type)
		{
			case DataType::Bool: return "boolean";
			case DataType::Char: return "string"; // Lua 没有 char 类型，使用 string 代替
			case DataType::Int8:
			case DataType::UInt8:
			case DataType::Int16:
			case DataType::UInt16:
			case DataType::Int32:
			case DataType::UInt32:
			case DataType::Int64:
			case DataType::UInt64:
			case DataType::Float:
			case DataType::Double:
				return "number";
			case DataType::String: return "string";
			case DataType::Vec2: return "vec2";
			case DataType::Vec3: return "vec3";
			case DataType::Vec4: return "vec4";
			case DataType::Mat3: return "mat3";
			case DataType::Mat4: return "mat4";
			case DataType::Enum: return "number"; // 枚举在 Lua 中通常表示为数字
			case DataType::Object:
			{
				const ObjectDesc& objDesc = std::any_cast<ObjectDesc>(prop.UserData);

				const TypeDesc* objTypeDesc = TypeRegistry::Get().GetTypeDesc(objDesc.Name);
				if (objTypeDesc)
				{
					return objTypeDesc->Name;
				}
				return "any";
			}

			default: return "any";
		}
	}
	void ScriptEngine::DefineMathType()
	{

		LuaReflectionRegistry::GetTable().push_back(
			{
				"vec2",
				{
					{"x", "number"},
					{"y", "number"},
					{"length", "fun():number","Get the length of the vector"},
				},
				[](sol::state& lua)
				{
					lua.new_usertype<glm::vec2>(
						"vec2",sol::constructors<glm::vec2(),glm::vec2(float), glm::vec2(float, float)>(),
						"x", &glm::vec2::x,
						"y", &glm::vec2::y,
						"length", [](const glm::vec2& v) -> float { return glm::length(v); },

						sol::meta_function::addition, [](const glm::vec2& a, const glm::vec2& b) { return a + b; },

						sol::meta_function::subtraction, [](const glm::vec2& a, const glm::vec2& b) { return a - b; },

						sol::meta_function::multiplication, sol::overload(
							// 形态 1：向量 * 标量 (vec2 * float)
							[](const glm::vec2& a, float b) -> glm::vec2 { return a * b; },

							// 形态 2：标量 * 向量 (float * vec2)
							[](float a, const glm::vec2& b) -> glm::vec2 { return a * b; },

							// 形态 3：向量 * 向量 (vec2 * vec2)
							[](const glm::vec2& a, const glm::vec2& b) -> glm::vec2 { return glm::vec2(a.x * b.x, a.y * b.y); }
						),

						sol::meta_function::division,sol::overload(
							// 形态 1：向量 / 标量 (vec2 / float)
							[](const glm::vec2& a, float b) -> glm::vec2 { return a / b; },
							// 形态 2：向量 / 向量 (vec2 / vec2)
							[](const glm::vec2& a, const glm::vec2& b) -> glm::vec2 { return glm::vec2(a.x / b.x, a.y / b.y); }
						)
					);

				}
			});

		LuaReflectionRegistry::GetTable().push_back(
			{
				"vec3",
				{
					{"x", "number"},
					{"y", "number"},
					{"z", "number"},
					{"length", "fun():number","Get the length of the vector"},
					{"normalize", "fun():vec3","Normalize the vector"},
					{"dot", "fun(vec3):number","Calculate the dot product with another vector"},
					{"cross", "fun(vec3):vec3","Calculate the cross product with another vector"},
				},

				[](sol::state& lua)
				{
					lua.new_usertype<glm::vec3>(
						"vec3",sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(),
						"x", &glm::vec3::x,
						"y", &glm::vec3::y,
						"z", &glm::vec3::z,
						"length", [](const glm::vec3& v) -> float { return glm::length(v); },
						"normalize", [](const glm::vec3& v) -> glm::vec3 { return glm::normalize(v); },
						"dot", [](const glm::vec3& a, const glm::vec3& b) -> float { return glm::dot(a, b); },
						"cross", [](const glm::vec3& a, const glm::vec3& b) -> glm::vec3 { return glm::cross(a, b); },

						sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
						sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
						sol::meta_function::multiplication, sol::overload(
							// 形态 1：向量 * 标量 (vec3 * float)
							[](const glm::vec3& a, float b) -> glm::vec3 { return a * b; },
							// 形态 2：标量 * 向量 (float * vec3)
							[](float a, const glm::vec3& b) -> glm::vec3 { return a * b; },
							// 形态 3：向量 * 向量 (vec3 * vec3)
							[](const glm::vec3& a, const glm::vec3& b) -> glm::vec3 { return glm::vec3(a.x * b.x, a.y * b.y, a.z * b.z); }
						),

						sol::meta_function::division,sol::overload(
							// 形态 1：向量 / 标量 (vec3 / float)
							[](const glm::vec3& a, float b) -> glm::vec3 { return a / b; },
							// 形态 2：向量 / 向量 (vec3 / vec3)
							[](const glm::vec3& a, const glm::vec3& b) -> glm::vec3 { return glm::vec3(a.x / b.x, a.y / b.y, a.z / b.z); }
						)
					);
				}
			});
		LuaReflectionRegistry::GetTable().push_back(
			{
				"vec4",
				{
					{"x", "number"},
					{"y", "number"},
					{"z", "number"},
					{"w", "number"},
					{"length", "fun():number","Get the length of the vector"},
				},
				[](sol::state& lua)
				{
					lua.new_usertype<glm::vec4>(
						"vec4", sol::constructors<glm::vec4(), glm::vec4(float), glm::vec4(float, float, float, float)>(),
						"x", &glm::vec4::x,
						"y", &glm::vec4::y,
						"z", &glm::vec4::z,
						"w", &glm::vec4::w,
						"length", [](const glm::vec4& v) -> float { return glm::length(v); }
					);
				}
			});
	}
	void ScriptEngine::RegisterMathTypes()
	{
		auto& lua = GetState(); // 拿到你的 sol::state
		DefineMathType();
		// 遍历描述表，直接执行各自的 C++ 绑定 Lambda！
		for (const auto& mathType : LuaReflectionRegistry::GetTable())
		{
			mathType.BindFunc(lua);
		}

	}
	void ScriptEngine::GenerateLuaStubs()
	{
		std::filesystem::path stubPath(WLD_ASSETPATH + std::string("/scripts/intermediate/WorldEngineAPI.lua"));
		std::filesystem::create_directories(stubPath.parent_path()); // 确保目录存在

		std::ofstream out(stubPath);
		out << "---WorldEngineAPI\n\n"; // 告诉插件这是个提示文件

		// 先输出数学类的 Lua 注释
		for (const auto& mathType : LuaReflectionRegistry::GetTable())
		{
			out << "---@class " << mathType.ClassName << "\n";
			for (const auto& prop : mathType.Properties)
			{
				out << "---@field " << prop.Name << " " << prop.LuaType;
				if (!prop.Description.empty())
				{
					out << " " << prop.Description;
				}
				out << "\n";
			}
			out << mathType.ClassName << " = {}\n\n";
		}

		// 遍历你的 C++ 反射系统
		for (const auto& [name, type] : TypeRegistry::Get().GetTemplateMap())
		{
			out << "---@class " << type.Name << "\n";
			for (const auto& prop : type.Properties)
			{
				// 将 C++ 类型映射为 Lua 类型字符串 (如 int -> number)
				out << "---@field " << prop.Name << " " << GetLuaTypeName(prop) << "\n";
			}

			out << type.Name << " = {}\n\n";
		}

		out.close();
		WLD_CORE_INFO("Lua API Stubs generated successfully!");
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

	void ScriptEngine::OnCreateScript(LuaScriptComponent& scriptComponent, Entity entity)
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

				scriptComponent.ScriptTable["__EntityID"] = entity;
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