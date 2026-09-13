#include "wldpch.h"
#include "ScriptEngine.h"
#include "Components.h"
#include "LuaStubGenerator.h"
#include "World/Core/Application.h"
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace World
{
	std::vector<LuaTypeReflection>& LuaReflectionRegistry::GetTable()
	{
		static std::vector<LuaTypeReflection> s_Table;
		return s_Table;
	}

	namespace
	{
		sol::state* s_LuaState = nullptr;
		std::thread::id s_OwnerThread;

		void ReportLuaError(LuaScriptComponent& script, const char* phase, const std::string& error)
		{
			const std::string message = "[Lua] " + script.ScriptFilePath + " entity=" +
				std::to_string(static_cast<uint32_t>(script.RuntimeEntity)) + " phase=" + phase + ":\n" + error;
			if (!script.LastError.empty()) script.LastError += "\n";
			script.LastError += message;
			script.State = ScriptInstanceState::Faulted;
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("{0}", message);
		}

		void ClearLuaReferences(LuaScriptComponent& script)
		{
			script.OnCreateFunc = sol::protected_function{};
			script.OnUpdateFunc = sol::protected_function{};
			script.OnDestroyFunc = sol::protected_function{};
			script.ScriptTable = sol::table{};
			script.LuaEnv = sol::environment{};
			script.RuntimeEntity = {};
			script.IsLoaded = false;
			script.CreateEntered = false;
		}

		// VFS 优先读脚本源码;未命中回退磁盘;找不到抛 logic_error。
		std::string ReadScriptSource(const std::string& scriptFilePath)
		{
			if (Application::HasInstance())
			{
				std::error_code ec;
				std::vector<uint8_t> bytes;
				if (Application::Get().GetContext().Vfs().Read(scriptFilePath, bytes, ec) && !bytes.empty())
					return std::string(bytes.begin(), bytes.end());
			}
			const std::filesystem::path diskPath =
				WLD_ASSETPATH + std::string("/") + scriptFilePath;
			std::ifstream file(diskPath, std::ios::binary);
			if (file.is_open())
			{
				std::stringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			}
			throw std::logic_error("Script not found: " + scriptFilePath);
		}

		void CheckResult(const sol::protected_function_result& result)
		{
			if (!result.valid())
			{
				sol::error error = result;
				throw std::runtime_error(error.what());
			}
		}

		int LookupCallback(lua_State* state)
		{
			// This C function runs inside a protected Lua call. Ordinary indexing preserves
			// __index inheritance, and Lua errors cannot unwind through C++ local objects.
			lua_gettable(state, 1);
			return 1;
		}

		sol::protected_function ReadCallback(const sol::table& table, const char* name)
		{
			auto lookup = sol::make_object(*s_LuaState, static_cast<lua_CFunction>(&LookupCallback)).as<sol::protected_function>();
			auto result = lookup(table, name);
			CheckResult(result);
			sol::object value = result.get<sol::object>();
			if (!value.valid() || value == sol::lua_nil) return {};
			if (value.get_type() != sol::type::function) throw std::logic_error(std::string(name) + " must be a function or nil");
			return value.as<sol::protected_function>();
		}

		LuaTypeReflection VectorDescription(const char* name, size_t dimensions)
		{
			LuaTypeReflection type;
			type.ClassName = name;
			const char* axes[] = { "x", "y", "z", "w" };
			std::vector<LuaPropDesc> coordinates;
			for (size_t i = 0; i < dimensions; ++i)
			{
				type.Properties.push_back({ axes[i], "number", "Vector coordinate" });
				coordinates.push_back({ axes[i], "number", "Vector coordinate" });
			}
			type.Methods = { { "length", {}, "number", "Euclidean vector length", true } };
			type.Constructors = {
				{ "new", {}, name, "Create a vector", false },
				{ "new", {{ "value", "number", "Value for every coordinate" }}, name, "Fill every coordinate", false },
				{ "new", coordinates, name, "Create a vector from coordinates", false }
			};
			return type;
		}

		std::unordered_map<std::string, std::string> ParseFieldAnnotationsInternal(const std::string& text)
		{
			std::unordered_map<std::string, std::string> schema;
			std::istringstream stream(text);
			std::string line;
			while (std::getline(stream, line))
			{
				const size_t start = line.find_first_not_of(" \t\r\n");
				if (start == std::string::npos)
					continue;
				const std::string trimmed = line.substr(start);
				if (trimmed.rfind("---@field", 0) != 0)
					continue;
				std::istringstream rest(trimmed.substr(9));
				std::string name, type;
				if (!(rest >> name))
					continue;
				if (!(rest >> type))
					continue;
				schema[name] = type;
			}
			return schema;
		}

		LuaFieldType AnnotationToFieldTypeInternal(const std::string& typeName, const sol::object& value)
		{
			if (typeName == "number")
			{
				if (value.get_type() == sol::type::number)
				{
					const double number = value.as<double>();
					if (std::isfinite(number) && number == std::floor(number) &&
						number >= (std::numeric_limits<int>::min)() && number <= (std::numeric_limits<int>::max)())
						return LuaFieldType::Int;
					return LuaFieldType::Float;
				}
				return LuaFieldType::None;
			}
			if (typeName == "integer" || typeName == "int")
				return value.get_type() == sol::type::number ? LuaFieldType::Int : LuaFieldType::None;
			if (typeName == "boolean" || typeName == "bool")
				return value.get_type() == sol::type::boolean ? LuaFieldType::Bool : LuaFieldType::None;
			if (typeName == "string")
				return value.get_type() == sol::type::string ? LuaFieldType::String : LuaFieldType::None;
			return LuaFieldType::None;
		}
	}

	bool ScriptEngine::IsInitialized() { return s_LuaState != nullptr; }

	void ScriptEngine::AssertOwnerThread()
	{
		if (!s_LuaState) throw std::logic_error("ScriptEngine is not initialized");
		if (s_OwnerThread != std::this_thread::get_id()) throw std::logic_error("Lua access must run on the ScriptEngine owner thread");
	}

	void ScriptEngine::Init()
	{
		if (s_LuaState) { AssertOwnerThread(); return; }
		s_OwnerThread = std::this_thread::get_id();
		s_LuaState = new sol::state();
		try
		{
			s_LuaState->open_libraries(sol::lib::base, sol::lib::math, sol::lib::package);
			s_LuaState->set_function("print", [](sol::variadic_args args) {
				AssertOwnerThread();
				std::string text = "[Lua]";
				sol::protected_function tostring = GetState()["tostring"];
				for (auto argument : args)
				{
					sol::object value = argument;
					auto result = tostring(value);
					CheckResult(result);
					text += " " + result.get<std::string>();
				}
				if (Log::GetClientLogger()) WLD_TRACE("{0}", text);
			});
			RegisterMathTypes();
		}
		catch (...) { delete s_LuaState; s_LuaState = nullptr; s_OwnerThread = {}; throw; }
		if (Log::GetCoreLogger()) WLD_CORE_INFO("[Lua] ScriptEngine initialized successfully.");
	}

	void ScriptEngine::Shutdown()
	{
		if (!s_LuaState) return;
		AssertOwnerThread();
		// Hosts release all scene/preview references before entering this function.
		delete s_LuaState;
		s_LuaState = nullptr;
		s_OwnerThread = {};
	}

	sol::state& ScriptEngine::GetState() { AssertOwnerThread(); return *s_LuaState; }

	void ScriptEngine::DefineMathType()
	{
		if (IsInitialized()) AssertOwnerThread();
		auto vec2 = VectorDescription("vec2", 2);
		vec2.Operators = { { "add", "vec2", "vec2" }, { "sub", "vec2", "vec2" },
			{ "mul", "number", "vec2" }, { "mul", "vec2", "vec2" }, { "div", "number", "vec2" }, { "div", "vec2", "vec2" } };
		vec2.BindFunc = [](sol::state& lua) {
			lua.new_usertype<glm::vec2>("vec2", sol::constructors<glm::vec2(), glm::vec2(float), glm::vec2(float, float)>(),
				"x", &glm::vec2::x, "y", &glm::vec2::y,
				"length", [](const glm::vec2& value) { return glm::length(value); },
				sol::meta_function::addition, [](const glm::vec2& a, const glm::vec2& b) { return a + b; },
				sol::meta_function::subtraction, [](const glm::vec2& a, const glm::vec2& b) { return a - b; },
				sol::meta_function::multiplication, sol::overload(
					[](const glm::vec2& a, float b) { return a * b; },
					[](float a, const glm::vec2& b) { return a * b; },
					[](const glm::vec2& a, const glm::vec2& b) { return a * b; }),
				sol::meta_function::division, sol::overload(
					[](const glm::vec2& a, float b) { return a / b; },
					[](const glm::vec2& a, const glm::vec2& b) { return a / b; }));
		};
		LuaReflectionRegistry::Register(vec2);

		auto vec3 = VectorDescription("vec3", 3);
		vec3.Methods.push_back({ "normalize", {}, "vec3", "Return a normalized vector", true });
		vec3.Methods.push_back({ "dot", {{ "other", "vec3", "Other vector" }}, "number", "Dot product", true });
		vec3.Methods.push_back({ "cross", {{ "other", "vec3", "Other vector" }}, "vec3", "Cross product", true });
		vec3.Operators = { { "add", "vec3", "vec3" }, { "sub", "vec3", "vec3" },
			{ "mul", "number", "vec3" }, { "mul", "vec3", "vec3" }, { "div", "number", "vec3" }, { "div", "vec3", "vec3" } };
		vec3.BindFunc = [](sol::state& lua) {
			lua.new_usertype<glm::vec3>("vec3", sol::constructors<glm::vec3(), glm::vec3(float), glm::vec3(float, float, float)>(),
				"x", &glm::vec3::x, "y", &glm::vec3::y, "z", &glm::vec3::z,
				"length", [](const glm::vec3& value) { return glm::length(value); },
				"normalize", [](const glm::vec3& value) { return glm::normalize(value); },
				"dot", [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); },
				"cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); },
				sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
				sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
				sol::meta_function::multiplication, sol::overload(
					[](const glm::vec3& a, float b) { return a * b; },
					[](float a, const glm::vec3& b) { return a * b; },
					[](const glm::vec3& a, const glm::vec3& b) { return a * b; }),
				sol::meta_function::division, sol::overload(
					[](const glm::vec3& a, float b) { return a / b; },
					[](const glm::vec3& a, const glm::vec3& b) { return a / b; }));
		};
		LuaReflectionRegistry::Register(vec3);

		auto vec4 = VectorDescription("vec4", 4);
		vec4.BindFunc = [](sol::state& lua) {
			lua.new_usertype<glm::vec4>("vec4", sol::constructors<glm::vec4(), glm::vec4(float), glm::vec4(float, float, float, float)>(),
				"x", &glm::vec4::x, "y", &glm::vec4::y, "z", &glm::vec4::z, "w", &glm::vec4::w,
				"length", [](const glm::vec4& value) { return glm::length(value); });
		};
		LuaReflectionRegistry::Register(vec4);
	}

	void ScriptEngine::RegisterMathTypes()
	{
		AssertOwnerThread();
		DefineMathType();
		RegisterBuiltinEntityLuaType();
		RegisterBuiltinMat3LuaType();
		RegisterBuiltinMat4LuaType();
		for (const auto& type : LuaReflectionRegistry::GetTable())
			if (type.BindFunc) type.BindFunc(*s_LuaState);
	}

	bool ScriptEngine::GenerateLuaStubs()
	{
		AssertOwnerThread();
		std::string error;
		const std::filesystem::path path(WLD_ASSETPATH + std::string("/scripts/intermediate/WorldEngineAPI.lua"));
		if (!LuaStubGenerator::Generate(path, error))
		{
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("[Lua] {0}", error);
			return false;
		}
		return true;
	}

	std::unordered_map<std::string, std::string> ScriptEngine::ParseFieldAnnotations(const std::string& scriptText)
	{
		return ParseFieldAnnotationsInternal(scriptText);
	}

	bool ScriptEngine::InitScriptForEditor(LuaScriptComponent& script)
	{
		AssertOwnerThread();
		if (script.ScriptFilePath.empty() || script.IsLoaded || script.State == ScriptInstanceState::Creating ||
			script.State == ScriptInstanceState::Running || script.State == ScriptInstanceState::Destroying) return false;
		try
		{
			// 1. 读取脚本源码，静态解析 ---@field 注解（不执行脚本顶层代码）。
			std::unordered_map<std::string, std::string> schema;
			schema = ParseFieldAnnotationsInternal(ReadScriptSource(script.ScriptFilePath));

			// 2. 执行脚本获取默认值表。
			sol::environment environment(*s_LuaState, sol::create, s_LuaState->globals());
			auto result = s_LuaState->safe_script(ReadScriptSource(script.ScriptFilePath), environment, sol::script_pass_on_error);
			CheckResult(result);
			if (result.return_count() == 0 || result.get_type() != sol::type::table) throw std::logic_error("Script must return a table");
			sol::table table = result.get<sol::table>();

			// 3. 构建字段：类型优先取注解，缺失注解走旧值推断兼容路径。
			std::unordered_map<std::string, LuaScriptField> fields;
			for (const auto& [keyObject, value] : table)
			{
				if (keyObject.get_type() != sol::type::string) continue;
				const std::string name = keyObject.as<std::string>();
				if (name.empty() || name[0] == '_' || name == "entity") continue;

				LuaScriptField field;
				const auto schemaIt = schema.find(name);
				if (schemaIt != schema.end())
				{
					field.Type = AnnotationToFieldTypeInternal(schemaIt->second, value);
					if (field.Type == LuaFieldType::None) continue;
					switch (field.Type)
					{
						case LuaFieldType::Int: field.Value = value.as<int>(); break;
						case LuaFieldType::Float: field.Value = value.as<float>(); break;
						case LuaFieldType::Bool: field.Value = value.as<bool>(); break;
						case LuaFieldType::String: field.Value = value.as<std::string>(); break;
						default: break;
					}
				}
				else
				{
					if (value.get_type() == sol::type::number)
					{
						const double number = value.as<double>();
						if (std::isfinite(number) && number == std::floor(number) && number >= (std::numeric_limits<int>::min)() && number <= (std::numeric_limits<int>::max)())
							field = { LuaFieldType::Int, static_cast<int>(number) };
						else field = { LuaFieldType::Float, static_cast<float>(number) };
					}
					else if (value.get_type() == sol::type::boolean) field = { LuaFieldType::Bool, value.as<bool>() };
					else if (value.get_type() == sol::type::string) field = { LuaFieldType::String, value.as<std::string>() };
				}
				if (field.Type == LuaFieldType::None) continue;
				const auto old = script.CachedFields.find(name);
				if (old != script.CachedFields.end() && old->second.Type == field.Type) field.Value = old->second.Value;
				fields.emplace(name, std::move(field));
			}
			script.CachedFields = std::move(fields);
			script.LastError.clear();
			script.State = ScriptInstanceState::Stopped;
			return true;
		}
		catch (const std::exception& error) { script.LastError.clear(); ReportLuaError(script, "EditorLoad", error.what()); }
		catch (...) { script.LastError.clear(); ReportLuaError(script, "EditorLoad", "Unknown exception"); }
		return false;
	}

	void ScriptEngine::OnCreateScript(LuaScriptComponent& script, Entity entity)
	{
		AssertOwnerThread();
		if (script.State != ScriptInstanceState::Pending && script.State != ScriptInstanceState::Stopped) return;
		ClearLuaReferences(script);
		script.LastError.clear();
		script.RuntimeEntity = entity;
		if (script.ScriptFilePath.empty()) { script.State = ScriptInstanceState::Stopped; return; }
		script.State = ScriptInstanceState::Creating;
		const char* phase = "Load";
		try
		{
			script.LuaEnv = sol::environment(*s_LuaState, sol::create, s_LuaState->globals());
			auto result = s_LuaState->safe_script(ReadScriptSource(script.ScriptFilePath), script.LuaEnv, sol::script_pass_on_error);
			CheckResult(result);
			if (result.return_count() == 0 || result.get_type() != sol::type::table) throw std::logic_error("Script must return a table");
			script.ScriptTable = result.get<sol::table>();
			for (const auto& [name, field] : script.CachedFields)
			{
				switch (field.Type)
				{
					case LuaFieldType::Float: script.ScriptTable.raw_set(name, std::any_cast<float>(field.Value)); break;
					case LuaFieldType::Int: script.ScriptTable.raw_set(name, std::any_cast<int>(field.Value)); break;
					case LuaFieldType::Bool: script.ScriptTable.raw_set(name, std::any_cast<bool>(field.Value)); break;
					case LuaFieldType::String: script.ScriptTable.raw_set(name, std::any_cast<std::string>(field.Value)); break;
					default: break;
				}
			}
			// All names carry the same non-owning, generation-checked entity handle.
			script.ScriptTable.raw_set("entity", entity, "__Entity", entity, "__EntityID", entity);
			script.OnCreateFunc = ReadCallback(script.ScriptTable, "OnCreate");
			script.OnUpdateFunc = ReadCallback(script.ScriptTable, "OnUpdate");
			script.OnDestroyFunc = ReadCallback(script.ScriptTable, "OnDestroy");
			script.IsLoaded = true;
			script.CreateEntered = true;
			phase = "OnCreate";
			if (script.OnCreateFunc.valid()) CheckResult(script.OnCreateFunc(script.ScriptTable));
			script.State = ScriptInstanceState::Running;
		}
		catch (const std::exception& error) { ReportLuaError(script, phase, error.what()); }
		catch (...) { ReportLuaError(script, phase, "Unknown exception"); }
	}

	void ScriptEngine::OnUpdateScript(LuaScriptComponent& script, Timestep ts)
	{
		AssertOwnerThread();
		if (script.State != ScriptInstanceState::Running || !script.IsLoaded) return;
		try { if (script.OnUpdateFunc.valid()) CheckResult(script.OnUpdateFunc(script.ScriptTable, ts.GetSeconds())); }
		catch (const std::exception& error) { ReportLuaError(script, "OnUpdate", error.what()); }
		catch (...) { ReportLuaError(script, "OnUpdate", "Unknown exception"); }
	}

	void ScriptEngine::OnDestroyScript(LuaScriptComponent& script)
	{
		// Empty/stopped components can outlive the VM; live references cannot.
		if (IsInitialized()) AssertOwnerThread();
		else if (script.IsLoaded || script.LuaEnv.valid() || script.ScriptTable.valid())
			throw std::logic_error("Script references must be released before ScriptEngine::Shutdown");
		if (script.State == ScriptInstanceState::Destroying) return;
		bool faulted = script.State == ScriptInstanceState::Faulted;
		script.State = ScriptInstanceState::Destroying;
		try
		{
			const bool entered = script.CreateEntered;
			script.CreateEntered = false;
			if (entered && script.OnDestroyFunc.valid()) CheckResult(script.OnDestroyFunc(script.ScriptTable));
		}
		catch (const std::exception& error) { ReportLuaError(script, "OnDestroy", error.what()); faulted = true; }
		catch (...) { ReportLuaError(script, "OnDestroy", "Unknown exception"); faulted = true; }
		ClearLuaReferences(script);
		script.State = faulted ? ScriptInstanceState::Faulted : ScriptInstanceState::Stopped;
	}
}
